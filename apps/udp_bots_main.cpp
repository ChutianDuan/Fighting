#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <lab/session/ClientSession.h>
#include <lab/session/ServerRuntime.h>
#include <lab/sim/Hasher.h>
#include <lab/time/Clock.h>
#include <memory>
#include <new>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <thread>

// 可执行文件内计数，不影响生产库。基线与优化版本使用相同计数口径。
static std::atomic<uint64_t> allocations{0};
void *operator new(std::size_t n) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void *operator new(std::size_t n, std::align_val_t align) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    void *p = nullptr;
    if (posix_memalign(&p, static_cast<size_t>(align), n ? n : 1) == 0)
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void *operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete[](void *p, std::align_val_t a) noexcept { ::operator delete(p, a); }
using namespace lab::session;
namespace net = lab::net;
namespace {
double Percentile(std::vector<double> v, double q) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>((v.size() - 1) * q)];
}
uint64_t Memory() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return r.ru_maxrss;
}
struct Bot {
    net::UdpSocket socket;
    ClientSession session;
    explicit Bot(uint32_t room) : session(room, 256) {}
};
Json Run(size_t rooms, double warmup, double duration, bool udp) {
    ServerRuntime runtime({64, 20240625, 42});
    RoomServer core({64, 20240625, 42});
    if (udp && !runtime.Open(0, "127.0.0.1"))
        throw std::runtime_error("loopback UDP bind failed");
    auto address = net::UdpAddr::FromIPv4("127.0.0.1", runtime.port());
    std::vector<std::unique_ptr<Bot>> bots;
    for (size_t i = 0; i < rooms * 2; ++i) {
        bots.push_back(std::make_unique<Bot>(i / 2 + 1));
        if (udp && (!bots.back()->socket.Open() || !bots.back()->socket.Bind(0, "127.0.0.1") ||
                    !bots.back()->socket.SetNonBlocking(true)))
            throw std::runtime_error("bot socket init failed");
    }
    auto deliver = [&](const std::vector<Datagram> &ds, double now) {
        for (const auto &d : ds)
            bots.at(d.endpoint - 1)->session.HandleDatagram(d.bytes, true, now);
    };
    std::vector<double> times, rtts;
    uint64_t inputPackets = 0, bytesOut = 0, bytesIn = 0, hashErrors = 0;
    uint64_t startAlloc = 0, beginPackets = 0, beginBytesOut = 0, beginBytesIn = 0,
             beginApplied = 0, beginReceived = 0;
    double start = Clock::NowSeconds(), next = start;
    bool sampling = false;
    uint32_t tick = 0;
    while (Clock::NowSeconds() - start < warmup + duration) {
        double now = Clock::NowSeconds();
        if (now < next) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        if (!sampling && now - start >= warmup) {
            sampling = true;
            startAlloc = allocations.load();
            beginPackets = inputPackets;
            beginBytesOut = bytesOut;
            beginBytesIn = bytesIn;
            beginApplied = (udp ? runtime.server : core).stats().applied;
            beginReceived = (udp ? runtime.server : core).stats().received;
            runtime.tickMs.clear();
            runtime.deadlineMiss = 0;
        }
        auto begin = std::chrono::steady_clock::now();
        InputCmd input;
        input.moveX = (tick / 120) % 3 - 1;
        input.moveY = (tick / 180) % 3 - 1;
        input.buttons = tick % 20 == 0 ? BIN_ATK : 0;
        for (size_t i = 0; i < bots.size(); ++i) {
            auto &bot = *bots[i];
            if (udp) {
                net::UdpAddr from;
                Bytes b;
                while (bot.socket.RecvFrom(from, b)) {
                    bytesIn += b.size();
                    bot.session.HandleDatagram(b, from.Key() == address.Key(), now);
                }
            }
            bot.session.Update(input, now);
            for (const auto &b : bot.session.DrainOutgoing()) {
                ++inputPackets;
                bytesOut += b.size();
                if (udp)
                    bot.socket.SendTo(address, b);
                else {
                    auto ds = core.HandleDatagram(i + 1, b, now);
                    for (const auto &d : ds)
                        bytesIn += d.bytes.size();
                    deliver(ds, now);
                }
            }
            if (sampling)
                rtts.push_back(bot.session.stats().rttMs);
        }
        if (udp)
            runtime.Poll(now);
        else {
            auto ds = core.AdvanceOneTick(now);
            for (const auto &d : ds)
                bytesIn += d.bytes.size();
            deliver(ds, now);
        }
        auto cost =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                .count();
        if (sampling)
            times.push_back(cost);
        ++tick;
        next += kStep;
    }
    uint64_t alloc = allocations.load() - startAlloc;
    auto &server = udp ? runtime.server : core;
    bool ok = true;
    for (size_t i = 0; i < bots.size(); ++i) {
        const auto &c = bots[i]->session;
        ok = ok && c.state() == SyncState::Playing && c.identity().room == i / 2 + 1 &&
             (c.identity().player == 1 || c.identity().player == 2) && c.stats().maxAdvance <= 4;
        hashErrors += c.stats().invalidWire;
        if (c.identity().player == bots[i ^ 1]->session.identity().player)
            ok = false;
        if (bots[i]->session.identity().session ==
            bots[(i + 1) % bots.size()]->session.identity().session)
            ok = false;
    }
    size_t misses =
        std::count_if(times.begin(), times.end(), [](double ms) { return ms > kStep * 1000; });
    auto applied = server.stats().applied - beginApplied;
    return {
        {"mode", udp ? "udp" : "core"},
        {"rooms", rooms},
        {"warmupSeconds", warmup},
        {"sampleSeconds", duration},
        {"recording", false},
        {"ok", ok && hashErrors == 0},
        {"hashErrors", hashErrors},
        {"serverRejected", server.stats().rejected},
        {"tickP50Ms", Percentile(times, .5)},
        {"tickP95Ms", Percentile(times, .95)},
        {"tickP99Ms", Percentile(times, .99)},
        {"serverTickP99Ms", Percentile(runtime.tickMs, .99)},
        {"deadlineMissPct", times.empty() ? 0 : 100.0 * misses / times.size()},
        {"ackRttP50Ms", Percentile(rtts, .5)},
        {"ackRttP99Ms", Percentile(rtts, .99)},
        {"ticksSampled", times.size()},
        {"allocations", alloc},
        {"peakRssBytes", Memory()},
        {"datagramsPerSecond", (inputPackets - beginPackets) / duration},
        {"appliedInputsPerSecond", applied / duration},
        {"receivedInputDatagramsPerSecond", (server.stats().received - beginReceived) / duration},
        {"inputBytes", bytesOut - beginBytesOut},
        {"outputBytes", bytesIn - beginBytesIn},
        {"bandwidthBytesPerSecond",
         (bytesIn + bytesOut - beginBytesIn - beginBytesOut) / duration}};
}
} // namespace
int main(int argc, char **argv) {
    try {
        size_t rooms = 2;
        double warmup = 5, duration = 30;
        int repeat = 3;
        bool udp = true;
        std::string output;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() {
                if (++i >= argc)
                    throw std::runtime_error("missing value");
                return std::string(argv[i]);
            };
            if (arg == "--rooms")
                rooms = std::stoul(value());
            else if (arg == "--warmup")
                warmup = std::stod(value());
            else if (arg == "--sample")
                duration = std::stod(value());
            else if (arg == "--repeat")
                repeat = std::stoi(value());
            else if (arg == "--json")
                output = value();
            else if (arg == "--mode") {
                auto v = value();
                if (v != "core" && v != "udp")
                    throw std::runtime_error("mode must be core or udp");
                udp = v == "udp";
            } else
                throw std::runtime_error("unknown option: " + arg);
        }
        if (!rooms || rooms > 64 || !std::isfinite(warmup) || !std::isfinite(duration) ||
            warmup < 0 || duration <= 0 || repeat < 1 || repeat > 20)
            throw std::runtime_error("invalid benchmark parameters");
        utsname machine{};
        uname(&machine);
        Json report = {{"protocol", 6},
                       {"simulation", 1},
                       {"machine",
                        {{"system", machine.sysname},
                         {"release", machine.release},
                         {"architecture", machine.machine}}},
#ifdef NDEBUG
                       {"build", "Release"},
#else
                       {"build", "Debug"},
#endif
                       {"runs", Json::array()}};
        bool ok = true;
        for (int n = 0; n < repeat; ++n) {
            auto r = Run(rooms, warmup, duration, udp);
            ok = ok && r["ok"].get<bool>();
            report["runs"].push_back(r);
            std::cerr << "run " << n + 1 << "/" << repeat << " rooms=" << rooms
                      << " p99=" << r["tickP99Ms"] << " ms\n";
        }
        if (!output.empty()) {
            std::ofstream f(output);
            if (!f)
                throw std::runtime_error("cannot write report");
            f << report.dump(2) << '\n';
        }
        std::cout << report.dump(2) << '\n';
        return ok ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
