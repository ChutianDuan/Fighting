#include <lab/net/UdpSocket.h>
#include <lab/net/NetCodec.h>

#include <lab/sim/InputBuffer.h>
#include <lab/sim/World.h>
#include <lab/sim/Hasher.h>

#include <lab/time/Clock.h>
#include <lab/util/log.h>

#include <unordered_map>
#include <vector>
#include <cmath>
#include <cstdint>

// libevent
#include <event2/event.h>
#include <event2/util.h>

using Tick = uint32_t; // 若你工程里已有 Tick 类型，删掉这行即可

struct ClientConn {
    lab::net::UdpAddr addr{};
    InputBuffer inputBuf{4096};
    Tick lastInputTick = 0;
    Tick lastAppliedTick = 0;

    bool hasLastApplied = false;
    InputCmd lastApplied{};

    bool assigned = false;
    uint8_t playerId = 0; // 1 or 2

    double lastHeardSec = 0.0; // 用于超时踢人（可选）
};

struct ServerCtx {
    static constexpr uint8_t kMaxPlayers = 2; // 扩展多人时只需调大，并调整 StatePacket 编码
    static constexpr uint8_t kRequiredPlayers = 2; // 收齐后开局
    static constexpr Tick kStartDelayTicks = 30;    // 收齐后延迟若干帧再开局
    static constexpr Tick kHoldInputTicks = 6;      // 缺帧时最多 Hold

    lab::net::UdpSocket sock;
    std::unordered_map<uint64_t, ClientConn> clients;

    // slot -> key（key = addr.Key()），index 1..kMaxPlayers
    uint64_t playerKey[kMaxPlayers + 1] = {0};

    lab::sim::World world{kMaxPlayers};
    Tick tick = 0;
    Tick startTick = 0;
    bool started = false;

    // 固定时间步 accumulator
    double prev = 0.0;
    double acc  = 0.0;

    static constexpr double dt = 1.0 / 60.0;
    static constexpr double maxFrame = 0.25;
    static constexpr Tick kStateEvery = 2;

    // 可选：连接超时（秒）
    static constexpr double kClientTimeoutSec = 10.0;

    uint32_t mazeSeed = 20240625u;
};

// ---------- helpers：slot 分配/查询 ----------
static uint8_t AssignSlot(ServerCtx* ctx, uint64_t key) {
    for (uint8_t pid = 1; pid <= ServerCtx::kMaxPlayers; ++pid) {
        if (ctx->playerKey[pid] == key) return pid;
    }
    for (uint8_t pid = 1; pid <= ServerCtx::kMaxPlayers; ++pid) {
        if (ctx->playerKey[pid] == 0) { ctx->playerKey[pid] = key; return pid; }
    }
    return 0; // 满了
}

static ClientConn* GetPlayer(ServerCtx* ctx, uint8_t pid) {
    if (pid == 0 || pid > ServerCtx::kMaxPlayers) return nullptr;
    uint64_t key = ctx->playerKey[pid];
    if (key == 0) return nullptr;
    auto it = ctx->clients.find(key);
    if (it == ctx->clients.end()) return nullptr;
    return &it->second;
}

static void KickPlayer(ServerCtx* ctx, uint8_t pid) {
    if (pid == 0 || pid > ServerCtx::kMaxPlayers) return;
    uint64_t key = ctx->playerKey[pid];
    if (key == 0) return;

    auto it = ctx->clients.find(key);
    if (it != ctx->clients.end()) {
        LOGI("Kick player%u: %s", pid, it->second.addr.ToString().c_str());
        ctx->clients.erase(it);
    }
    ctx->playerKey[pid] = 0;
}

static size_t OnlineCount(const ServerCtx& ctx) {
    size_t n = 0;
    for (uint8_t pid = 1; pid <= ServerCtx::kMaxPlayers; ++pid) {
        if (ctx.playerKey[pid] != 0) ++n;
    }
    return n;
}

// 每个玩家：按 tick 取输入；缺失则 HoldLast；仍缺则 Default
static InputCmd GetCmdForTick(ClientConn& cc, Tick tick) {
    auto opt = cc.inputBuf.Get(tick);
    if (opt) {
        InputCmd cmd = *opt;
        cc.hasLastApplied = true;
        cc.lastApplied = cmd;
        cc.lastAppliedTick = tick;
        return cmd;
    }

    if (cc.hasLastApplied) {
        if ((tick - cc.lastAppliedTick) <= ServerCtx::kHoldInputTicks) {
            InputCmd cmd = cc.lastApplied;
            cmd.tick = tick; // 对齐当前 tick
            return cmd;
        }
    }

    return InputBuffer::DefaultForTick(tick);
}

// -------------------- Recv：收包 -> 写入各自 InputBuffer --------------------
static void OnUdp(void* user,
                  const lab::net::UdpAddr& from,
                  const uint8_t* data,
                  size_t len) {
    auto* ctx = static_cast<ServerCtx*>(user);

    auto in = lab::net::DecodeInput(data, len);
    if (!in) return;

    const uint64_t key = from.Key();
    auto& c = ctx->clients[key]; // 默认构造 OK

    // 首次见到该 key：尝试分配 slot
    if (!c.assigned) {
        uint8_t pid = AssignSlot(ctx, key);
        if (pid == 0) {
            // 已有两人：拒绝第三人（也可做排队/观战）
            // 注意：这里 erase 是可选的；不 erase 也行，但会留垃圾项
            ctx->clients.erase(key);
            LOGW("Reject client (slots full): %s", from.ToString().c_str());
            return;
        }
        c.assigned = true;
        c.playerId = pid;
        LOGI("Assign %s -> player%u", from.ToString().c_str(), pid);
    }

    // 每次都更新 addr（NAT 端口变化时至少能看到最新地址）
    c.addr = from;
    c.lastHeardSec = Clock::NowSeconds();

    // 写入输入缓冲
    for (const auto& cmd : in->cmds) {
        c.inputBuf.Put(cmd);
        if (cmd.tick > c.lastInputTick) c.lastInputTick = cmd.tick;
    }
}

static void MaybeStartMatch(ServerCtx* ctx) {
    if (ctx->started) return;
    if (OnlineCount(*ctx) < ServerCtx::kRequiredPlayers) return;

    ctx->startTick = ctx->tick + ServerCtx::kStartDelayTicks;
    ctx->tick = ctx->startTick;
    ctx->acc = 0.0;
    ctx->started = true;

    for (uint8_t pid = 1; pid <= ServerCtx::kMaxPlayers; ++pid) {
        ClientConn* pc = GetPlayer(ctx, pid);
        if (!pc) continue;
        lab::net::StartPacket sp{};
        sp.playerId = pid;
        sp.totalPlayers = ServerCtx::kMaxPlayers;
        sp.startTick = ctx->startTick;
        auto bytes = lab::net::EncodeStart(sp);
        ctx->sock.SendTo(pc->addr, bytes);
    }

    LOGI("Server start scheduled at tick=%u (players=%zu)",
         ctx->startTick, OnlineCount(*ctx));
}

// -------------------- Tick：推进世界 + 给两边发 State/Ack --------------------
static void OnTick(evutil_socket_t, short, void* user) {
    auto* ctx = static_cast<ServerCtx*>(user);

    const double now = Clock::NowSeconds();
    double frame = now - ctx->prev;
    ctx->prev = now;
    if (frame > ServerCtx::maxFrame) frame = ServerCtx::maxFrame;
    ctx->acc += frame;

    // 可选：超时踢人
    for (uint8_t pid = 1; pid <= ServerCtx::kMaxPlayers; ++pid) {
        ClientConn* pc = GetPlayer(ctx, pid);
        if (!pc) continue;
        if ((now - pc->lastHeardSec) > ServerCtx::kClientTimeoutSec) {
            KickPlayer(ctx, pid);
        }
    }

    MaybeStartMatch(ctx);

    while (ctx->acc >= ServerCtx::dt) {
        if (!ctx->started) {
            // 未开局，等待下一轮
            ctx->acc -= ServerCtx::dt;
            continue;
        }

        std::vector<InputCmd> cmds;
        cmds.reserve(ServerCtx::kMaxPlayers);

        for (uint8_t pid = 1; pid <= ServerCtx::kMaxPlayers; ++pid) {
            ClientConn* p = GetPlayer(ctx, pid);
            cmds.push_back(p ? GetCmdForTick(*p, ctx->tick)
                             : InputBuffer::DefaultForTick(ctx->tick));
        }

        ctx->world.Step(cmds, float(ServerCtx::dt));

        auto snap = ctx->world.Snapshot();
        uint64_t h = Hasher::Hash(snap);

        const PlayerState p1s = (snap.players.size() > 0) ? snap.players[0] : PlayerState{};
        const PlayerState p2s = (snap.players.size() > 1) ? snap.players[1] : PlayerState{};

        if (ctx->tick % 60 == 0) {
            LOGI("Server tick=%u p1x=%.3f p2x=%.3f hash=%llu",
                snap.tick,
                p1s.x, p2s.x,
                (unsigned long long)h);
        }

        const bool sendState = (ctx->tick % ServerCtx::kStateEvery == 0);

        for (uint8_t pid = 1; pid <= ServerCtx::kMaxPlayers; ++pid) {
            ClientConn* pc = GetPlayer(ctx, pid);
            if (!pc) continue;

            if (sendState) {
                lab::net::StatePacket st{};
                st.playerId = pid;
                st.tick = snap.tick;
                st.mazeSeed = snap.mazeSeed;
                st.playerCount = static_cast<uint8_t>(std::min<size_t>(ServerCtx::kMaxPlayers, snap.players.size()));
                st.projectileCount = static_cast<uint8_t>(std::min<size_t>(snap.projectiles.size(), 255));
                st.players.clear();
                st.players.reserve(st.playerCount);
                for (uint8_t i = 0; i < st.playerCount; ++i) {
                    lab::net::PackedPlayerState ps{};
                    const auto& wp = snap.players[i];
                    ps.x_mm = (int32_t)std::lround(wp.x * 1000.0f);
                    ps.v_mm = (int32_t)std::lround(wp.v * 1000.0f);
                    ps.y_mm = (int32_t)std::lround(wp.y * 1000.0f);
                    ps.vy_mm = (int32_t)std::lround(wp.vy * 1000.0f);
                    ps.hp = wp.hp;
                    ps.action = static_cast<uint8_t>(wp.action);
                    ps.facing = wp.facing;
                    ps.stateTimer = wp.stateTimer;
                    ps.atkActive = wp.atkActive;
                    ps.attackConnected = wp.attackConnected;
                    ps.onGround = wp.onGround;
                    ps.shotCooldown = wp.shotCooldown;
                    ps.aimX = wp.aimX;
                    ps.aimY = wp.aimY;
                    st.players.push_back(ps);
                }

                st.projectiles.clear();
                st.projectiles.reserve(st.projectileCount);
                for (uint8_t i = 0; i < st.projectileCount; ++i) {
                    const auto& wp = snap.projectiles[i];
                    lab::net::PackedProjectile pr{};
                    pr.x_mm = (int32_t)std::lround(wp.x * 1000.0f);
                    pr.y_mm = (int32_t)std::lround(wp.y * 1000.0f);
                    pr.vx_mm = (int32_t)std::lround(wp.vx * 1000.0f);
                    pr.vy_mm = (int32_t)std::lround(wp.vy * 1000.0f);
                    pr.owner = wp.owner;
                    pr.life = wp.life;
                    st.projectiles.push_back(pr);
                }

                st.stateHash = h;

                auto bytes = lab::net::EncodeState(st);
                ctx->sock.SendTo(pc->addr, bytes);
            }

            lab::net::AckPacket ack{};
            ack.playerId = pid;
            ack.serverTickProcessed = ctx->tick;
            ack.serverLastInputTick = pc->lastInputTick;
            ack.serverStateHash = h;

            auto bytes = lab::net::EncodeAck(ack);
            ctx->sock.SendTo(pc->addr, bytes);
        }

        ctx->tick++;
        ctx->acc -= ServerCtx::dt;
    }

}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    uint16_t port = 40000;

    event_base* base = event_base_new();
    if (!base) {
        LOGE("Server: event_base_new failed");
        return 1;
    }

    ServerCtx ctx;
    ctx.prev = Clock::NowSeconds();
    ctx.acc = 0.0;
    ctx.world.SetMazeSeed(ctx.mazeSeed, true);

    if (!ctx.sock.Open() || !ctx.sock.Bind(port) || !ctx.sock.SetNonBlocking(true)) {
        LOGE("Server: failed to open/bind UDP port=%u", port);
        event_base_free(base);
        return 1;
    }
    ctx.sock.SetRecvBuf(1 << 20);
    ctx.sock.SetSendBuf(1 << 20);

    LOGI("Server listening on 0.0.0.0:%u", port);

    if (!ctx.sock.StartEventRead(base, &OnUdp, &ctx)) {
        LOGE("Server: StartEventRead failed");
        event_base_free(base);
        return 1;
    }

    event* evTick = event_new(base, -1, EV_PERSIST, &OnTick, &ctx);
    if (!evTick) {
        LOGE("Server: event_new(tick) failed");
        event_base_free(base);
        return 1;
    }
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 1000; // 建议 1ms 触发 + accumulator 控 60Hz，抗抖动更好
    event_add(evTick, &tv);

    event_base_dispatch(base);

    event_free(evTick);
    event_base_free(base);
    return 0;
}
