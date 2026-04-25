// apps/client_main.cpp
#include <lab/net/UdpSocket.h>
#include <lab/net/NetCodec.h>

#include <lab/app/GameConfig.h>
#include <lab/app/InputPrediction.h>
#include <lab/app/ClientRender.h>

#include <lab/sim/InputBuffer.h>
#include <lab/sim/StateHistory.h>
#include <lab/sim/World.h>
#include <lab/sim/Hasher.h>

#include <lab/time/Clock.h>
#include <lab/util/log.h>

#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <algorithm>

// libevent
#include <event2/event.h>
#include <event2/util.h>

// SDL2
#include <SDL.h>
#include <SDL_ttf.h>

static inline float FromMM(int32_t mm) { return float(mm) / 1000.0f; }

struct ClientCtx {
    static constexpr uint8_t kMaxPlayers = lab::app::GameConfig::kMaxPlayers; // 扩展多人时调大，并同步状态包结构

    lab::app::RenderCtx render{};
    bool running = true;
    event_base* base = nullptr;

    lab::net::UdpSocket sock;
    lab::net::UdpAddr server{};

    static constexpr double dt = lab::app::GameConfig::kDt;
    static constexpr double maxFrame = lab::app::GameConfig::kMaxFrame;

    double prev = 0.0;
    double acc  = 0.0;

    Tick tick = 0; // localNextTick

    lab::sim::World worldPred{kMaxPlayers};

    // 本地输入历史（只存“我自己的输入”，不区分 P1/P2）
    InputBuffer localHist{4096};

    // 远端玩家输入预测历史（按 slot 存，包含占位，slot 从 1 开始）
    std::vector<InputBuffer> remoteHist;
    std::vector<uint8_t> remoteHasLast; // bool proxy-free
    std::vector<InputCmd> remoteLast;

    // 历史状态环（预测+权威混存，用于回滚/对账）
    lab::sim::StateHistory stateHist{4096};

    Tick lastServerTick = 0;   // 服务器 ACK 的已处理 tick
    uint64_t lastServerHash = 0;
    Tick startTick = 0;
    bool hasStart = false;
    double nextHelloSec = 0.0;

    uint32_t rollbackCount = 0;
    uint32_t hashMismatchCount = 0;
    Tick lastRollbackTick = 0;
    Tick lastHashMismatchTick = 0;
    Tick lastAuthoritativeTick = 0; // stateHist 中最新的权威写入 tick

    // server 分配的玩家槽位：1..kMaxPlayers（0 表示未知）
    uint8_t localPlayerId = 0;

    uint32_t inputSeq = 0;

    static constexpr int kRedundancy = lab::app::GameConfig::kInputRedundancy;

    struct InputState {
        int8_t moveX = 0;
        int8_t moveY = 0;
        bool attack = false;
    } input;
};

static std::vector<InputCmd> BuildCmdVec(uint8_t localPid,
                                         const InputCmd& localCmd,
                                         const std::vector<InputCmd>& remoteCmds) {
    std::vector<InputCmd> cmds(ClientCtx::kMaxPlayers, InputBuffer::DefaultForTick(localCmd.tick));
    const uint8_t lp = (localPid == 0) ? 1 : localPid;
    if (lp >= 1 && lp <= ClientCtx::kMaxPlayers) {
        cmds[lp - 1] = localCmd;
    }
    for (size_t i = 0; i < remoteCmds.size() && i < cmds.size(); ++i) {
        // remoteCmds 里包含全部玩家槽位的预测（含占位）
        if ((i + 1) == lp) continue;
        cmds[i] = remoteCmds[i];
    }
    return cmds;
}

static InputCmd GetRemoteCmdForTick(ClientCtx& ctx, uint8_t pid, Tick t) {
    if (pid == 0 || pid > ClientCtx::kMaxPlayers) return InputBuffer::DefaultForTick(t);
    auto& buf = ctx.remoteHist[pid - 1];
    auto hasLast = ctx.remoteHasLast[pid - 1] != 0;
    auto& last = ctx.remoteLast[pid - 1];
    constexpr Tick kHoldTicks = 6;

    if (auto opt = buf.Get(t)) {
        ctx.remoteHasLast[pid - 1] = 1;
        last = *opt;
        return *opt;
    }
    if (hasLast) {
        if (t > last.tick && (t - last.tick) > kHoldTicks) {
            return InputBuffer::DefaultForTick(t);
        }
        InputCmd c = last;
        c.tick = t;
        return c; // HoldLast
    }
    return InputBuffer::DefaultForTick(t);
}

static void RestoreAndReplay(ClientCtx& ctx, const WorldSnapshot& auth) {
    ctx.worldPred.Restore(auth);

    for (Tick t = auth.tick + 1; t < ctx.tick; ++t) {
        InputCmd localCmd = ctx.localHist.Get(t).value_or(InputBuffer::DefaultForTick(t));

        std::vector<InputCmd> remoteCmds(ClientCtx::kMaxPlayers, InputBuffer::DefaultForTick(t));
        for (uint8_t pid = 1; pid <= ClientCtx::kMaxPlayers; ++pid) {
            if (pid == ctx.localPlayerId) continue;
            remoteCmds[pid - 1] = GetRemoteCmdForTick(ctx, pid, t);
        }

        auto cmds = BuildCmdVec(ctx.localPlayerId, localCmd, remoteCmds);
        ctx.worldPred.Step(cmds, float(ClientCtx::dt));
        ctx.stateHist.Put(ctx.worldPred.Snapshot());
    }
}

static void ApplyAuthoritativeState(
    ClientCtx& ctx,
    const lab::net::StatePacket& st)
{
    if (ctx.lastAuthoritativeTick != 0 && st.tick <= ctx.lastAuthoritativeTick) {
        return;
    }

    auto toAction = [](uint8_t a) {
        switch (a) {
            case 1: return Action::Attack;
            case 2: return Action::Hitstun;
            default: return Action::Idle;
        }
    };

    // 构造权威快照（带上 maze / projectile 状态）
    WorldSnapshot auth = ctx.worldPred.Snapshot();
    if (auth.mazeSeed != st.mazeSeed || auth.maze.empty()) {
        ctx.worldPred.SetMazeSeed(st.mazeSeed);
        auth = ctx.worldPred.Snapshot();
    }
    auth.tick = st.tick;
    auth.mazeSeed = st.mazeSeed;
    auth.players.resize(ClientCtx::kMaxPlayers);
    const size_t count = std::min<size_t>(st.players.size(), ClientCtx::kMaxPlayers);
    for (size_t i = 0; i < count; ++i) {
        const auto& ps = st.players[i];
        auth.players[i].x = FromMM(ps.x_mm);
        auth.players[i].v = FromMM(ps.v_mm);
        auth.players[i].y = FromMM(ps.y_mm);
        auth.players[i].vy = FromMM(ps.vy_mm);
        auth.players[i].hp = ps.hp;
        auth.players[i].action = toAction(ps.action);
        auth.players[i].facing = ps.facing;
        auth.players[i].stateTimer = ps.stateTimer;
        auth.players[i].atkActive = ps.atkActive;
        auth.players[i].attackConnected = ps.attackConnected;
        auth.players[i].onGround = ps.onGround;
        auth.players[i].shotCooldown = ps.shotCooldown;
        auth.players[i].aimX = ps.aimX;
        auth.players[i].aimY = ps.aimY;
    }
    auth.projectiles.clear();
    auth.projectiles.reserve(st.projectiles.size());
    for (const auto& pr : st.projectiles) {
        ProjectileState prd{};
        prd.x = FromMM(pr.x_mm);
        prd.y = FromMM(pr.y_mm);
        prd.vx = FromMM(pr.vx_mm);
        prd.vy = FromMM(pr.vy_mm);
        prd.life = pr.life;
        prd.owner = pr.owner;
        prd.alive = pr.life > 0 ? 1 : 0;
        auth.projectiles.push_back(prd);
    }

    const uint64_t authHash = Hasher::Hash(auth);
    if (authHash != st.stateHash && ctx.lastHashMismatchTick != st.tick) {
        ctx.hashMismatchCount++;
        ctx.lastHashMismatchTick = st.tick;
        LOGW("State hash mismatch at tick=%u local=%llu server=%llu",
             st.tick,
             (unsigned long long)authHash,
             (unsigned long long)st.stateHash);
    }

    // 判断是否需要回滚：只比较“我自己的玩家”即可（否则因为对手预测不准会一直回滚）
    uint8_t pid = ctx.localPlayerId ? ctx.localPlayerId : 1;
    size_t idx = (pid == 2) ? 1 : 0;

    bool needRollback = false;
    if (auto localOpt = ctx.stateHist.Get(st.tick)) {
        const auto& local = *localOpt;

        if (local.players.size() > idx && auth.players.size() > idx) {
            float dx = std::fabs(local.players[idx].x - auth.players[idx].x);
            float dy = std::fabs(local.players[idx].y - auth.players[idx].y);
            constexpr float kPosEps = 0.15f; // 放宽回滚阈值，减少本地抖动
            constexpr float kPosYEps = 0.15f;
            const bool hpDiff = local.players[idx].hp != auth.players[idx].hp;
            const bool actionDiff = local.players[idx].action != auth.players[idx].action;
            const bool groundDiff = local.players[idx].onGround != auth.players[idx].onGround;
            needRollback = (dx > kPosEps) || (dy > kPosYEps) || hpDiff || actionDiff || groundDiff;
        }
    }

    ctx.stateHist.Put(auth); // 覆盖存权威快照，供后续 hash 对账使用
    ctx.lastAuthoritativeTick = st.tick;

    if (needRollback) {
        ctx.rollbackCount++;
        ctx.lastRollbackTick = st.tick;
    }
    RestoreAndReplay(ctx, auth);
}

static void ApplyStart(ClientCtx& ctx, const lab::net::StartPacket& sp) {
    if (ctx.localPlayerId == 0) ctx.localPlayerId = sp.playerId;
    ctx.startTick = sp.startTick;
    ctx.hasStart = true;
    ctx.tick = sp.startTick;
    ctx.acc = 0.0;
    ctx.nextHelloSec = 0.0;

    WorldSnapshot init = ctx.worldPred.Snapshot();
    init.tick = sp.startTick;
    if (init.players.size() < ClientCtx::kMaxPlayers) {
        init.players.resize(ClientCtx::kMaxPlayers);
    }
    ctx.worldPred.Restore(init);
    ctx.stateHist.Put(init);

    LOGI("Start received: playerId=%u total=%u startTick=%u",
         sp.playerId, sp.totalPlayers, sp.startTick);
}

static void PrintHud(const ClientCtx& ctx) {
    WorldSnapshot snap = ctx.worldPred.Snapshot();
    LOGI("HUD tick=%u rollbacks=%u hashMismatch=%u",
         snap.tick, ctx.rollbackCount, ctx.hashMismatchCount);
    for (size_t i = 0; i < snap.players.size(); ++i) {
        const auto& p = snap.players[i];
        LOGI("  P%zu x=%.2f y=%.2f v=%.2f vy=%.2f hp=%d act=%s t=%u atk=%u gnd=%u",
             i + 1,
             p.x, p.y,
             p.v, p.vy,
             int(p.hp),
             lab::app::ActionName(p.action),
             p.stateTimer,
             p.atkActive,
             p.onGround);
    }
}

// -------------------- Recv：只负责收包 -> 更新 ack/state -> 更新对手预测/触发回滚 --------------------
static void OnUdp(void* user,
                  const lab::net::UdpAddr& from,
                  const uint8_t* data,
                  size_t len) {
    auto* ctx = static_cast<ClientCtx*>(user);

    if (from.Key() != ctx->server.Key()) return;

    if (auto sp = lab::net::DecodeStart(data, len)) {
        ApplyStart(*ctx, *sp);
        return;
    }

    // 1) ACK
    if (auto ack = lab::net::DecodeAck(data, len)) {
        ctx->lastServerTick = ack->serverTickProcessed;
        ctx->lastServerHash = ack->serverStateHash;

        if (ctx->localPlayerId == 0) {
            ctx->localPlayerId = ack->playerId; // server 分配的槽位
            LOGI("Assigned localPlayerId=%u (from ACK)", ctx->localPlayerId);
        }

        if (ctx->lastServerTick % 60 == 0) {
            LOGI("ACK: serverTick=%u lastInput=%u hash=%llu",
                 ack->serverTickProcessed, ack->serverLastInputTick,
                 (unsigned long long)ack->serverStateHash);
        }

        // hash 对账（仅当本地已存同 tick 的权威快照时才比对）
        if (ctx->lastAuthoritativeTick == ack->serverTickProcessed) {
            if (auto snap = ctx->stateHist.Get(ack->serverTickProcessed)) {
                uint64_t h = Hasher::Hash(*snap);
                if (h != ack->serverStateHash && ctx->lastHashMismatchTick != ack->serverTickProcessed) {
                    ctx->hashMismatchCount++;
                    ctx->lastHashMismatchTick = ack->serverTickProcessed;
                    LOGW("Hash mismatch at tick=%u local=%llu server=%llu",
                         ack->serverTickProcessed,
                         (unsigned long long)h,
                         (unsigned long long)ack->serverStateHash);
                }
            }
        }
        return;
    }

    // 2) STATE
    if (auto st = lab::net::DecodeState(data, len)) {
        if (ctx->localPlayerId == 0) {
            ctx->localPlayerId = st->playerId;
            LOGI("Assigned localPlayerId=%u (from STATE)", ctx->localPlayerId);
        }
        if (ctx->lastAuthoritativeTick != 0 && st->tick <= ctx->lastAuthoritativeTick) {
            return;
        }

        // 更新远端输入预测（逐 slot）
        uint8_t localPid = ctx->localPlayerId ? ctx->localPlayerId : 1;
        const size_t count = std::min<size_t>(st->players.size(), ClientCtx::kMaxPlayers);
        for (size_t i = 0; i < count; ++i) {
            uint8_t pid = static_cast<uint8_t>(i + 1);
            if (pid == localPid) continue;
            int8_t lastMoveX = ctx->remoteHasLast[pid - 1] ? ctx->remoteLast[pid - 1].moveX : 0;
            int8_t lastMoveY = ctx->remoteHasLast[pid - 1] ? ctx->remoteLast[pid - 1].moveY : 0;
            const float v = FromMM(st->players[i].v_mm);
            if (st->players[i].onGround && std::fabs(v) < 0.01f) {
                // 地面且速度极小，重置旧意图
                lastMoveX = 0;
                lastMoveY = 0;
            }
            int8_t remoteMoveX = lab::app::PredictMoveXFromState(st->players[i], lastMoveX);
            int8_t remoteMoveY = lab::app::PredictMoveYFromState(st->players[i], lastMoveY);

            InputCmd rc;
            rc.tick = st->tick;
            rc.moveX = remoteMoveX;
            rc.moveY = remoteMoveY;
            rc.buttons = 0;
            // 若远端已在空中，推断跳跃曾发生，避免预测阶段一直贴地
            if (st->players[i].onGround == 0 && st->players[i].vy_mm > 0) {
                rc.buttons |= BIN_JUMP;
            }

            ctx->remoteHist[pid - 1].Put(rc);
            ctx->remoteHasLast[pid - 1] = 1;
            ctx->remoteLast[pid - 1] = rc;
        }

        ApplyAuthoritativeState(*ctx, *st);
        return;
    }

    // unknown -> ignore
}

static void SendHello(ClientCtx& ctx) {
    lab::net::InputPacket p;
    p.playerId = (ctx.localPlayerId != 0) ? ctx.localPlayerId : 1;
    p.newestTick = ctx.tick;
    p.clientAckServerTick = ctx.lastServerTick;
    p.count = 0;
    auto bytes = lab::net::EncodeInput(p);
    ctx.sock.SendTo(ctx.server, bytes);
}

static void PollInput(ClientCtx& ctx) {
    ctx.input.moveX = 0;
    ctx.input.moveY = 0;
    ctx.input.attack = false;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            ctx.running = false;
        }
    }

    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    if (ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_LEFT]) ctx.input.moveX = -1;
    if (ks[SDL_SCANCODE_D] || ks[SDL_SCANCODE_RIGHT]) ctx.input.moveX = +1;
    if (ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP]) ctx.input.moveY = +1;
    if (ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN]) ctx.input.moveY = -1;
    ctx.input.attack = ks[SDL_SCANCODE_J] || ks[SDL_SCANCODE_K] || ks[SDL_SCANCODE_SPACE];
}

// -------------------- Send：tick 驱动采样输入 + 本地预测 + 发 InputPacket --------------------
static void OnTick(evutil_socket_t, short, void* user) {
    auto* ctx = static_cast<ClientCtx*>(user);

    const double now = Clock::NowSeconds();
    double frame = now - ctx->prev;
    ctx->prev = now;
    if (frame > ClientCtx::maxFrame) frame = ClientCtx::maxFrame;
    ctx->acc += frame;

    PollInput(*ctx);

    if (!ctx->running) {
        if (ctx->base) event_base_loopbreak(ctx->base);
        return;
    }

    if (!ctx->hasStart) {
        // 未收到开局信号，定期发 hello 让 server 分配 slot
        if (now >= ctx->nextHelloSec) {
            SendHello(*ctx);
            ctx->nextHelloSec = now + 0.25;
        }
        lab::app::RenderFrame(ctx->render,
                              ctx->worldPred.Snapshot(),
                              ctx->rollbackCount,
                              ctx->hashMismatchCount);
        return;
    }

    while (ctx->acc >= ClientCtx::dt) {
        // 1) 采样本地输入（我自己的）
        InputCmd localCmd{};
        localCmd.tick = ctx->tick;
        localCmd.moveX = ctx->input.moveX;
        localCmd.moveY = ctx->input.moveY;
        localCmd.buttons = 0;
        if (ctx->input.attack) localCmd.buttons |= BIN_ATK;
        ctx->localHist.Put(localCmd);

        // 2) 远端输入预测（逐 slot）
        std::vector<InputCmd> remoteCmds(ClientCtx::kMaxPlayers, InputBuffer::DefaultForTick(ctx->tick));
        for (uint8_t pid = 1; pid <= ClientCtx::kMaxPlayers; ++pid) {
            if (pid == ctx->localPlayerId) continue;
            remoteCmds[pid - 1] = GetRemoteCmdForTick(*ctx, pid, ctx->tick);
        }

        // 3) 本地预测推进（多玩家）
        auto cmds = BuildCmdVec(ctx->localPlayerId, localCmd, remoteCmds);
        ctx->worldPred.Step(cmds, float(ClientCtx::dt));
        ctx->stateHist.Put(ctx->worldPred.Snapshot());

        // 4) 打包冗余输入（只发送“我自己的输入历史”）
        lab::net::InputPacket p;
        p.playerId = (ctx->localPlayerId != 0) ? ctx->localPlayerId : 1; // server 实际按 addr 分配，这里只是自描述
        p.seq = ctx->inputSeq++;
        p.newestTick = ctx->tick;
        p.clientAckServerTick = ctx->lastServerTick;

        p.cmds.clear();
        p.cmds.reserve(ClientCtx::kRedundancy);
        for (int i = 0; i < ClientCtx::kRedundancy; ++i) {
            Tick t = (ctx->tick >= (Tick)i) ? (ctx->tick - (Tick)i) : 0;
            auto c = ctx->localHist.Get(t).value_or(InputBuffer::DefaultForTick(t));
            p.cmds.push_back(c);
        }
        p.count = (uint8_t)p.cmds.size();

        auto bytes = lab::net::EncodeInput(p);
        ctx->sock.SendTo(ctx->server, bytes);

        ctx->tick++;
        ctx->acc -= ClientCtx::dt;

        if (ctx->tick % 60 == 0) {
            const int32_t lead = int32_t(ctx->tick) - int32_t(ctx->lastServerTick);
            LOGI("STAT tick=%u lead=%d rollbacks=%u last_rb=%u hash_mismatch=%u last_hash=%u",
                 ctx->tick,
                 lead,
                 ctx->rollbackCount,
                 ctx->lastRollbackTick,
                 ctx->hashMismatchCount,
                 ctx->lastHashMismatchTick);
            PrintHud(*ctx);
        }
    }

    lab::app::RenderFrame(ctx->render,
                          ctx->worldPred.Snapshot(),
                          ctx->rollbackCount,
                          ctx->hashMismatchCount);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    std::string serverIp = "127.0.0.1";
    uint16_t serverPort = 40000;

    event_base* base = event_base_new();
    if (!base) {
        LOGE("Client: event_base_new failed");
        return 1;
    }

    ClientCtx ctx;
    ctx.server = lab::net::UdpAddr::FromIPv4(serverIp, serverPort);
    ctx.prev = Clock::NowSeconds();
    ctx.acc = 0.0;
    ctx.base = base;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        LOGE("TTF_Init failed: %s", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    const char* fontPath = "/System/Library/Fonts/Menlo.ttc";
    if (!lab::app::InitRenderer(ctx.render, "Lab Client", fontPath, 14)) {
        LOGW("Renderer init failed (font=%s)", fontPath);
    }

    ctx.remoteHist.reserve(ClientCtx::kMaxPlayers);
    ctx.remoteHasLast.assign(ClientCtx::kMaxPlayers, 0);
    ctx.remoteLast.assign(ClientCtx::kMaxPlayers, InputCmd{});
    for (uint8_t i = 0; i < ClientCtx::kMaxPlayers; ++i) {
        ctx.remoteHist.emplace_back(4096);
    }

    if (!ctx.sock.Open() || !ctx.sock.Bind(0 /*ephemeral*/) || !ctx.sock.SetNonBlocking(true)) {
        LOGE("Client: failed to open/bind UDP");
        event_base_free(base);
        return 1;
    }
    ctx.sock.SetRecvBuf(1 << 20);
    ctx.sock.SetSendBuf(1 << 20);

    LOGI("Client -> Server %s", ctx.server.ToString().c_str());

    if (!ctx.sock.StartEventRead(base, &OnUdp, &ctx)) {
        LOGE("Client: StartEventRead failed");
        event_base_free(base);
        return 1;
    }

    event* evTick = event_new(base, -1, EV_PERSIST, &OnTick, &ctx);
    if (!evTick) {
        LOGE("Client: event_new(tick) failed");
        event_base_free(base);
        return 1;
    }
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 1000; // 1ms
    event_add(evTick, &tv);

    event_base_dispatch(base);

    event_free(evTick);
    event_base_free(base);
    lab::app::ShutdownRenderer(ctx.render);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
