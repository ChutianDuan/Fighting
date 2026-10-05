#include <filesystem>
#include <fstream>
#include <iostream>
#include <lab/session/ClientSession.h>
#include <lab/session/NetworkSimulator.h>
#include <lab/session/RoomServer.h>
#include <lab/sim/Hasher.h>
using namespace lab::session;
double Percentile(std::vector<double> v, double q) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>((v.size() - 1) * q)];
}
Json Stats(const ClientSession &c) {
    const auto &s = c.stats();
    return {{"corrections", s.corrections},
            {"sameFramePositionErrorP50", Percentile(s.positionErrors, .5)},
            {"sameFramePositionErrorP95", Percentile(s.positionErrors, .95)},
            {"sameFramePositionErrorP99", Percentile(s.positionErrors, .99)},
            {"replayFrames", s.replayFrames},
            {"replayMs", s.replayMs},
            {"resyncs", s.resyncs},
            {"reconnects", s.reconnects},
            {"rejected", s.rejected},
            {"invalidWire", s.invalidWire},
            {"duplicate", s.duplicate},
            {"late", s.late},
            {"packetsSent", s.packetsSent},
            {"bytesSent", s.bytesSent},
            {"packetsReceived", s.packetsReceived},
            {"bytesReceived", s.bytesReceived},
            {"state", StateName(c.state())},
            {"nextTick", c.next_tick()},
            {"hash", Hasher::Hash(c.Snapshot())}};
}
struct Result {
    Json report, trace;
};
Result Run(NetworkConfig config, int ticks, bool trace) {
    RoomServer server{
        {64, 20240625,
         (config.seed ^ 0x9e3779b97f4a7c15ULL) ? (config.seed ^ 0x9e3779b97f4a7c15ULL) : 1}};
    ClientSession a, b;
    NetworkSimulator network(config);
    Json inputs = Json::array();
    double recovered = -1;
    auto outgoing = [&](const std::vector<Datagram> &ds, double now) {
        for (const auto &d : ds)
            network.Send(d.endpoint, false, d.bytes, now);
    };
    for (int t = 0; t < ticks + 60; ++t) {
        const double now = t * kStep;
        if (t == ticks)
            network.Healthy();
        for (const auto &e : network.Deliver(now)) {
            if (e.toServer)
                outgoing(server.HandleDatagram(e.endpoint, e.bytes, now), now);
            else
                (e.endpoint == 1 ? a : b).HandleDatagram(e.bytes, true, now);
        }
        InputCmd input;
        input.moveX = (t / 150) % 3 - 1;
        input.moveY = (t / 230) % 3 - 1;
        input.buttons = t % 40 == 0 ? BIN_ATK : 0;
        auto update = [&](ClientSession &client, uint64_t endpoint, InputCmd command) {
            Tick first = client.next_tick();
            client.Update(command, now);
            Json generated = Json::array();
            for (uint32_t n = 0; n < client.next_tick() - first; ++n) {
                command.tick = first + n;
                generated.push_back(InputJson(command));
            }
            if (!generated.empty())
                inputs.push_back({{"time", now},
                                  {"endpoint", endpoint},
                                  {"identity", IdentityJson(client.identity())},
                                  {"commands", generated}});
        };
        update(a, 1, input);
        input.moveX = -input.moveX;
        update(b, 2, input);
        for (auto &bytes : a.DrainOutgoing())
            network.Send(1, true, bytes, now);
        for (auto &bytes : b.DrainOutgoing())
            network.Send(2, true, bytes, now);
        outgoing(server.AdvanceOneTick(now), now);
        if (t >= ticks && recovered < 0 && a.last_valid() >= ticks * kStep &&
            b.last_valid() >= ticks * kStep && a.state() == SyncState::Playing &&
            b.state() == SyncState::Playing)
            recovered = (t - ticks) * kStep * 1000;
    }
    auto authoritative = server.Snapshot(1);
    bool ok = authoritative && Distance(authoritative->tick, a.auth_tick()) <= 2 &&
              Distance(authoritative->tick, b.auth_tick()) <= 2 && recovered >= 0 &&
              recovered <= 1000 && a.state() == SyncState::Playing &&
              b.state() == SyncState::Playing && a.stats().invalidWire == 0 &&
              b.stats().invalidWire == 0;
    Json report = {{"config", ConfigJson(config)},
                   {"ticks", ticks},
                   {"healthyTailTicks", 60},
                   {"recoveryMs", recovered},
                   {"ok", ok},
                   {"eventDigest", network.digest()},
                   {"serverHash", Hasher::Hash(*server.Snapshot(1))},
                   {"serverRejected", server.stats().rejected},
                   {"clients", {Stats(a), Stats(b)}}};
    Json artifact;
    if (trace || !ok)
        artifact = {{"version", 2},     {"config", ConfigJson(config)},
                    {"ticks", ticks},   {"report", report},
                    {"inputs", inputs}, {"events", network.Trace()}};
    return {report, artifact};
}
int main(int argc, char **argv) {
    try {
        NetworkConfig config;
        bool matrix = false;
        int ticks = 3000;
        std::string output, trace, replay;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() {
                if (++i >= argc)
                    throw std::runtime_error("missing option value");
                return std::string(argv[i]);
            };
            if (arg == "--matrix")
                matrix = true;
            else if (arg == "--ticks")
                ticks = std::stoi(value());
            else if (arg == "--seed")
                config.seed = std::stoull(value());
            else if (arg == "--json")
                output = value();
            else if (arg == "--trace")
                trace = value();
            else if (arg == "--replay-trace")
                replay = value();
            else if (arg == "--rtt")
                config.upstream.latencyMs = config.downstream.latencyMs = std::stod(value()) / 2;
            else if (arg == "--up-delay")
                config.upstream.latencyMs = std::stod(value());
            else if (arg == "--down-delay")
                config.downstream.latencyMs = std::stod(value());
            else if (arg == "--jitter")
                config.upstream.jitterMs = config.downstream.jitterMs = std::stod(value());
            else if (arg == "--loss")
                config.upstream.loss = config.downstream.loss = std::stod(value());
            else if (arg == "--reorder")
                config.upstream.reorder = config.downstream.reorder = std::stod(value());
            else if (arg == "--duplicate")
                config.upstream.duplicate = config.downstream.duplicate = std::stod(value());
            else
                throw std::runtime_error("unknown option: " + arg);
        }
        config = ParseConfig(ConfigJson(config));
        if (ticks < 1 || ticks > 1000000)
            throw std::runtime_error("invalid ticks");
        Json result = Json::array();
        bool ok = true;
        if (!replay.empty()) {
            Json saved;
            std::ifstream in(replay);
            if (!in)
                throw std::runtime_error("cannot open trace");
            in >> saved;
            if (saved.at("version") != 2)
                throw std::runtime_error("unsupported trace version");
            auto r = Run(ParseConfig(saved.at("config")), saved.at("ticks"), true);
            // 逐事件比较原始数据报及虚拟投递时间；真实重放耗时不参与一致性比较。
            if (r.trace["events"] != saved.at("events") || r.trace["inputs"] != saved.at("inputs"))
                throw std::runtime_error("trace event/input divergence");
            auto expected = saved.at("report"), actual = r.report;
            for (auto *report : {&expected, &actual})
                for (auto &client : (*report)["clients"])
                    client.erase("replayMs");
            if (expected != actual)
                throw std::runtime_error("functional statistics divergence");
            result.push_back(r.report);
        } else {
            auto run = [&](NetworkConfig c) {
                auto r = Run(c, ticks, !trace.empty());
                ok = ok && r.report["ok"].get<bool>();
                result.push_back(r.report);
                if (!r.trace.is_null()) {
                    auto suffix = "_rtt" +
                                  std::to_string(c.upstream.latencyMs + c.downstream.latencyMs) +
                                  "_loss" + std::to_string(c.upstream.loss);
                    if (trace.empty())
                        std::filesystem::create_directories("build/network_failures");
                    auto name = trace.empty() ? "build/network_failures/seed" +
                                                    std::to_string(c.seed) + suffix + ".json"
                                              : trace + (matrix ? suffix : "");
                    std::ofstream f(name);
                    if (!f)
                        throw std::runtime_error("cannot write trace");
                    f << r.trace.dump();
                }
            };
            if (matrix) {
                for (double rtt : {50., 100., 150.})
                    for (double loss : {0., .01, .05}) {
                        auto c = config;
                        c.upstream.latencyMs = c.downstream.latencyMs = rtt / 2;
                        c.upstream.loss = c.downstream.loss = loss;
                        run(c);
                    }
            } else
                run(config);
        }
        Json report = {
            {"protocol", 6},
            {"determinism", "virtual events and functional statistics; measured duration excluded"},
            {"results", result}};
        if (!output.empty()) {
            std::ofstream f(output);
            if (!f)
                throw std::runtime_error("cannot write JSON");
            f << report.dump(2) << '\n';
        }
        std::cout << report.dump(2) << '\n';
        return ok ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
