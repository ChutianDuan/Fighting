#include <csignal>
#include <event2/event.h>
#include <filesystem>
#include <iostream>
#include <lab/session/Replay.h>
#include <lab/session/ServerRuntime.h>
#include <lab/time/Clock.h>
#include <memory>
using namespace lab::session;
namespace {
void TimerTick(evutil_socket_t, short, void *user) {
    static_cast<ServerRuntime *>(user)->Poll(Clock::NowSeconds());
}
void Stop(evutil_socket_t, short, void *user) {
    event_base_loopbreak(static_cast<event_base *>(user));
}
} // namespace
int main(int argc, char **argv) {
    try {
        uint16_t port = 40000;
        ServerConfig config;
        std::string directory;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (i + 1 >= argc)
                throw std::runtime_error("missing option value");
            std::string value = argv[++i];
            if (arg == "--port") {
                auto p = std::stoul(value);
                if (!p || p > 65535)
                    throw std::runtime_error("invalid port");
                port = p;
            } else if (arg == "--max-rooms")
                config.maxRooms = std::stoul(value);
            else if (arg == "--record-dir")
                directory = value;
            else
                throw std::runtime_error("unknown option: " + arg);
        }
        // base 先创建最后释放；定时/信号事件先释放，socket 读事件随后停止。
        std::unique_ptr<event_base, decltype(&event_base_free)> base(event_base_new(),
                                                                     event_base_free);
        if (!base)
            throw std::runtime_error("event_base_new failed");
        ServerRuntime app(config);
        if (!app.Open(port) || !app.Attach(base.get()))
            throw std::runtime_error("UDP initialization failed");
        std::map<uint32_t, std::unique_ptr<ReplayWriter>> recordings;
        if (!directory.empty()) {
            std::filesystem::create_directories(directory);
            app.server.onStart = [&](uint32_t room, uint32_t match, const WorldSnapshot &s,
                                     const std::vector<InputCmd> &) {
                auto path = std::filesystem::path(directory) /
                            ("instance" + std::to_string(app.server.instance()) + "_room" +
                             std::to_string(room) + "_match" + std::to_string(match) + ".jsonl");
                recordings[room] = std::make_unique<ReplayWriter>(
                    path.string(), s,
                    Json{{"instance", app.server.instance()}, {"room", room}, {"match", match}});
            };
            app.server.onFrame = [&](uint32_t room, uint32_t, const WorldSnapshot &s,
                                     const std::vector<InputCmd> &inputs) {
                recordings.at(room)->Frame(s, inputs);
            };
        }
        using Event = std::unique_ptr<event, decltype(&event_free)>;
        Event timer(event_new(base.get(), -1, EV_PERSIST, TimerTick, &app), event_free);
        Event interrupt(evsignal_new(base.get(), SIGINT, Stop, base.get()), event_free);
        Event terminate(evsignal_new(base.get(), SIGTERM, Stop, base.get()), event_free);
        timeval interval{0, 1000};
        if (!timer || !interrupt || !terminate || event_add(timer.get(), &interval) != 0 ||
            event_add(interrupt.get(), nullptr) != 0 || event_add(terminate.get(), nullptr) != 0)
            throw std::runtime_error("event initialization failed");
        std::cout << "v6 server UDP port " << port << ", room limit " << config.maxRooms << '\n';
        event_base_dispatch(base.get());
        for (auto &[room, writer] : recordings) {
            (void)room;
            writer->Finish();
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
