#include <cmath>
#include <iostream>
#include <lab/session/ClientSession.h>
#include <lab/session/RoomServer.h>
#include <lab/sim/Hasher.h>
#include <stdexcept>
using namespace lab::session;
void Check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Rig {
    RoomServer server{{64, 20240625, 42}};
    ClientSession a{1}, b{1};
    double now = 0;
    void Deliver(const std::vector<Datagram> &out) {
        for (const auto &d : out)
            (d.endpoint == 1 ? a : b).HandleDatagram(d.bytes, true, now);
    }
    void Flush(ClientSession &c, uint64_t endpoint) {
        for (auto &bytes : c.DrainOutgoing())
            Deliver(server.HandleDatagram(endpoint, bytes, now));
    }
    void Step(bool online = true) {
        a.Update({}, now);
        b.Update({}, now);
        if (online) {
            Flush(a, 1);
            Flush(b, 2);
        } else {
            a.DrainOutgoing();
            b.DrainOutgoing();
        }
        auto out = server.AdvanceOneTick(now);
        if (online)
            Deliver(out);
        now += kStep;
    }
    Rig() {
        a.Update({}, 0);
        Flush(a, 1);
        b.Update({}, 0);
        Flush(b, 2);
    }
};
int main() {
    try {
        Check(Newer(0, UINT32_MAX), "frame wrap");
        Check(Distance(UINT32_MAX, 0) == -1, "wrap distance");
        ServerConfig injected;
        uint64_t randomValue = 100;
        injected.identitySource = [&]() { return ++randomValue; };
        RoomServer deterministic(injected);
        Check(deterministic.instance() == 101, "identity RNG injection");
        Rig r;
        Check(r.a.state() == SyncState::Playing && r.a.next_tick() == 1,
              "init completed tick zero");
        for (int i = 0; i < 100; ++i)
            r.Step();
        auto before = r.a.Snapshot();
        Message state;
        state.kind = Kind::State;
        state.id = r.a.identity();
        state.snapshot = r.server.Snapshot(1);
        state.nextTick = state.snapshot->tick + 1;
        state.hash = Hasher::Hash(*state.snapshot);
        auto bytes = Encode(state);
        bytes.back() = '!';
        r.a.HandleDatagram(bytes, true, r.now);
        Check(Hasher::Hash(before) == Hasher::Hash(r.a.Snapshot()),
              "bad length/JSON modifies state");
        bytes = Encode(state);
        r.a.HandleDatagram(bytes, false, r.now);
        Check(Hasher::Hash(before) == Hasher::Hash(r.a.Snapshot()), "source modifies state");
        state.id.generation++;
        r.a.HandleDatagram(Encode(state), true, r.now);
        Check(Hasher::Hash(before) == Hasher::Hash(r.a.Snapshot()), "identity modifies state");
        state.id = r.a.identity();
        state.snapshot->tick += 20;
        state.nextTick = state.snapshot->tick + 1;
        state.hash = Hasher::Hash(*state.snapshot);
        r.a.HandleDatagram(Encode(state), true, r.now);
        Check(r.a.next_tick() == state.nextTick, "future snapshot must fast forward");
        auto same = r.a.Snapshot();
        r.a.HandleDatagram(Encode(state), true, r.now);
        Check(Hasher::Hash(same) == Hasher::Hash(r.a.Snapshot()), "duplicate state");
        Message anonymous;
        anonymous.kind = Kind::Hello;
        anonymous.request = 1;
        auto hijack = r.server.HandleDatagram(1, Encode(anonymous), r.now);
        Check(!hijack.empty() && Decode(hijack.front().bytes)->kind == Kind::Reject,
              "established seat restored without credential");
        auto oldSession = r.b.identity().session;
        auto oldMatch = r.b.identity().match;
        auto oldGeneration = r.b.identity().generation;
        for (int i = 0; i < 180; ++i)
            r.Step(false);
        for (int i = 0; i < 100; ++i)
            r.Step();
        Check(r.b.identity().session == oldSession && r.b.identity().match == oldMatch &&
                  r.b.identity().generation > oldGeneration,
              "three second reconnect");
        Check(r.a.stats().maxAdvance <= 4 && r.b.stats().maxAdvance <= 4, "catchup bound");
        auto simHash = Hasher::Hash(r.b.Snapshot());
        r.b.SetSmoothing(false);
        r.b.Display(r.now);
        r.b.SetSmoothing(true);
        r.b.Display(r.now);
        Check(Hasher::Hash(r.b.Snapshot()) == simHash, "display alters simulation");
        ClientSession small{2, 1}, peer{2};
        RoomServer s{{64, 20240625, 123}};
        auto dispatch = [&](const std::vector<Datagram> &out) {
            for (const auto &d : out)
                (d.endpoint == 3 ? small : peer).HandleDatagram(d.bytes, true, 0);
        };
        small.Update({}, 0);
        for (auto &b : small.DrainOutgoing())
            dispatch(s.HandleDatagram(3, b, 0));
        peer.Update({}, 0);
        for (auto &b : peer.DrainOutgoing())
            dispatch(s.HandleDatagram(4, b, 0));
        small.Update({}, .1);
        auto m = state;
        m.id = small.identity();
        m.snapshot = s.Snapshot(2);
        m.snapshot->tick = 1;
        m.nextTick = 2;
        m.hash = Hasher::Hash(*m.snapshot);
        small.HandleDatagram(Encode(m), true, .1);
        Check(small.state() == SyncState::Resynchronizing, "missing history must resync");
        auto prior = s.Snapshot(2);
        Message input;
        input.kind = Kind::Input;
        input.id = small.identity();
        input.id.generation++;
        input.inputs = {{2, 0, 0, 0}};
        s.HandleDatagram(3, Encode(input), 9);
        Check(Hasher::Hash(*prior) == Hasher::Hash(*s.Snapshot(2)), "bad input mutates world");
        s.Expire(11);
        Check(s.Snapshot(2)->tick == 0, "expired match terminates");
        s.Expire(42);
        Check(s.room_count() == 0, "empty room reclaim");
        RoomServer restarted{{64, 20240625, 43}};
        r.a.Update({}, r.now + 2);
        for (auto &b : r.a.DrainOutgoing()) {
            for (auto &d : restarted.HandleDatagram(1, b, r.now + 2))
                r.a.HandleDatagram(d.bytes, true, r.now + 2);
        }
        r.a.Update({}, r.now + 2.3);
        for (auto &b : r.a.DrainOutgoing())
            for (auto &d : restarted.HandleDatagram(1, b, r.now + 2.3))
                r.a.HandleDatagram(d.bytes, true, r.now + 2.3);
        Check(r.a.identity().instance == restarted.instance(), "restart must create new identity");
        // 快照坏哈希、截断和旧版本均不能进入提交阶段。
        auto valid = Encode(state);
        auto json = Json::parse(valid.begin() + 12, valid.end());
        json["hash"] = 0;
        auto body = json.dump();
        Bytes bad(valid.begin(), valid.begin() + 8);
        uint32_t length = body.size();
        for (int shift = 24; shift >= 0; shift -= 8)
            bad.push_back(static_cast<uint8_t>(length >> shift));
        bad.insert(bad.end(), body.begin(), body.end());
        Check(!Decode(bad), "bad hash accepted");
        valid.pop_back();
        Check(!Decode(valid), "truncated packet accepted");
        valid = Encode(state);
        valid[5] = 5;
        Check(!Decode(valid), "v5 accepted on v6 route");
        Message rawMessage;
        rawMessage.kind = Kind::Welcome;
        rawMessage.record = true;
        rawMessage.snapshot = lab::sim::World(2).Snapshot();
        rawMessage.rawInitial = rawMessage.snapshot;
        rawMessage.hash = Hasher::Hash(*rawMessage.snapshot);
        rawMessage.nextTick = 1;
        rawMessage.rawInitial->players[0].hp = 200;
        bool encodingRejected = false;
        try {
            Encode(rawMessage);
        } catch (...) {
            encodingRejected = true;
        }
        Check(encodingRejected, "raw record encoder accepts invalid value");
        // 恢复与重放跨越 UINT32_MAX，不丢帧，也不重复已完成帧。
        ClientSession wrapped;
        Message welcome;
        welcome.kind = Kind::Welcome;
        welcome.request = 1;
        welcome.id = {1, 2, 3, 1, 4, 1, 1};
        welcome.running = true;
        welcome.snapshot = lab::sim::World(2).Snapshot();
        welcome.snapshot->tick = UINT32_MAX - 2;
        welcome.nextTick = UINT32_MAX - 1;
        welcome.hash = Hasher::Hash(*welcome.snapshot);
        wrapped.HandleDatagram(Encode(welcome), true, 0);
        wrapped.Update({}, 0);
        wrapped.Update({}, kStep);
        Check(wrapped.next_tick() == 1 && wrapped.Snapshot().tick == 0, "prediction tick wrap");
        welcome.kind = Kind::State;
        welcome.snapshot->tick = UINT32_MAX - 1;
        welcome.nextTick = UINT32_MAX;
        welcome.hash = Hasher::Hash(*welcome.snapshot);
        wrapped.HandleDatagram(Encode(welcome), true, 0);
        Check(wrapped.stats().lastReplay == 2 && wrapped.Snapshot().tick == 0,
              "replay wrap interval");
        // 显示偏移只存在于副本，百毫秒内归零。
        ClientSession smooth;
        Message start = welcome;
        start.kind = Kind::Welcome;
        start.snapshot = lab::sim::World(2).Snapshot();
        start.nextTick = 1;
        start.hash = Hasher::Hash(*start.snapshot);
        smooth.HandleDatagram(Encode(start), true, 0);
        smooth.Update({}, 0);
        start.kind = Kind::State;
        start.snapshot->tick = 1;
        start.snapshot->players[0].x += .1f;
        start.nextTick = 2;
        start.hash = Hasher::Hash(*start.snapshot);
        smooth.HandleDatagram(Encode(start), true, 0);
        auto simulation = smooth.Snapshot();
        auto immediate = smooth.Display(0), half = smooth.Display(.05),
             settled = smooth.Display(.1);
        Check(std::fabs(immediate.players[0].x - simulation.players[0].x) > .05f,
              "correction offset absent");
        Check(std::fabs(half.players[0].x - simulation.players[0].x) < .06f &&
                  std::fabs(settled.players[0].x - simulation.players[0].x) < .0001f,
              "offset decay deadline");
        // 超大 RTT 不能扩大追帧至超过恢复预算。
        ClientSession budget;
        Message base = welcome;
        base.kind = Kind::Welcome;
        base.snapshot = lab::sim::World(2).Snapshot();
        base.nextTick = 1;
        base.hash = Hasher::Hash(*base.snapshot);
        budget.HandleDatagram(Encode(base), true, 0);
        budget.Update({}, 0);
        base.kind = Kind::State;
        base.sequence = 1;
        base.snapshot->tick = 1;
        base.nextTick = 2;
        base.hash = Hasher::Hash(*base.snapshot);
        budget.HandleDatagram(Encode(base), true, 5);
        budget.Update({}, 5);
        Check(budget.state() == SyncState::Resynchronizing, "replay scheduling budget");
        // 重复 Hello 不增加连接代次；凭证恢复允许地址改变。
        Message hello;
        hello.kind = Kind::Hello;
        hello.id = r.b.identity();
        hello.request = 100;
        auto replies = r.server.HandleDatagram(9, Encode(hello), r.now);
        Check(!replies.empty(), "address resume reply");
        auto resumed = Decode(replies.front().bytes);
        auto generation = resumed->id.generation;
        auto duplicate = r.server.HandleDatagram(9, Encode(hello), r.now);
        Check(Decode(duplicate.front().bytes)->id.generation == generation,
              "control duplicate changed generation");
        // 靠墙的重叠玩家只能向可用空间移动；不能用推箱绕过墙体约束。
        lab::sim::World wall(2);
        auto snapshot = wall.Snapshot();
        snapshot.mazeWidth = snapshot.mazeHeight = 7;
        snapshot.maze.assign(49, 0);
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 7; ++x)
                if (x <= 1 || x == 6 || y == 0 || y == 6)
                    snapshot.maze[y * 7 + x] = 1;
        snapshot.players[0].x = -1.14f;
        snapshot.players[1].x = -.84f;
        for (auto &p : snapshot.players) {
            p.y = 0;
            p.v = p.vy = 0;
        }
        wall.Restore(snapshot);
        wall.Step({{1, 0, 0, 0}, {1, 0, 0, 0}}, static_cast<float>(kStep));
        for (const auto &p : wall.Snapshot().players)
            Check(p.x - .35f >= -1.5f, "pushbox enters wall");
        // 历史共享静态地图，完整导出保持值语义；地图变化不能污染旧槽。
        lab::sim::StateHistory maps(4);
        auto originalMap = wall.Snapshot();
        originalMap.tick = 0;
        maps.Put(originalMap);
        auto nextMap = originalMap;
        nextMap.tick = 1;
        maps.Put(nextMap);
        Check(&maps.GetView(0)->map == &maps.GetView(1)->map, "map not shared");
        auto exported = *maps.Get(0);
        exported.maze[0] ^= 1;
        Check(maps.Get(0)->maze == originalMap.maze, "export aliases map");
        nextMap.tick = 2;
        nextMap.maze[8] ^= 1;
        maps.Put(nextMap);
        Check(maps.Get(0)->maze == originalMap.maze &&
                  &maps.GetView(1)->map != &maps.GetView(2)->map,
              "old map changed");
        nextMap.tick = 4;
        maps.Put(nextMap);
        Check(!maps.GetView(0), "view misses overwrite check");
        Check(Hasher::Hash(wall.View()) == Hasher::Hash(wall.Snapshot()),
              "cached world export changed hash");
        std::cout << "v6 session reliability passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
