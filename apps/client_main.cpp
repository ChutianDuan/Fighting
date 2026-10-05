#include <arpa/inet.h>
#include <iostream>
#include <lab/app/ClientRender.h>
#include <lab/net/UdpSocket.h>
#include <lab/session/ClientSession.h>
#include <lab/session/Replay.h>
#include <lab/sim/Hasher.h>
#include <lab/time/Clock.h>
#include <memory>

using namespace lab::session;
namespace {
struct SdlContext {
    lab::app::RenderCtx render;
    ~SdlContext() {
        lab::app::ShutdownRenderer(render);
        if (TTF_WasInit())
            TTF_Quit();
        SDL_Quit();
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        std::string host = "127.0.0.1", font = "/System/Library/Fonts/Menlo.ttc", replayFile,
                    recordFile;
        uint16_t port = 40000;
        uint32_t room = 1;
        bool smooth = true;
        int frameLimit = 0;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() {
                if (++i >= argc)
                    throw std::runtime_error("missing option value");
                return std::string(argv[i]);
            };
            if (arg == "--server") {
                host = value();
                auto pos = host.find(':');
                if (pos != std::string::npos) {
                    auto n = std::stoul(host.substr(pos + 1));
                    if (!n || n > 65535)
                        throw std::runtime_error("invalid port");
                    port = n;
                    host.resize(pos);
                }
            } else if (arg == "--room") {
                auto n = std::stoull(value());
                if (!n || n > UINT32_MAX)
                    throw std::runtime_error("invalid room");
                room = n;
            } else if (arg == "--font")
                font = value();
            else if (arg == "--replay")
                replayFile = value();
            else if (arg == "--record")
                recordFile = value();
            else if (arg == "--no-smoothing")
                smooth = false;
            else if (arg == "--frames")
                frameLimit = std::stoi(value());
            else
                throw std::runtime_error("unknown option: " + arg);
        }
        in_addr parsed{};
        if (inet_pton(AF_INET, host.c_str(), &parsed) != 1)
            throw std::runtime_error("--server requires IPv4[:port]");
        SdlContext sdl;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0 || TTF_Init() != 0 ||
            !lab::app::InitRenderer(sdl.render, "Authority demo v6", font, 16))
            throw std::runtime_error(std::string("SDL/font initialization: ") + SDL_GetError() +
                                     " " + TTF_GetError());
        ClientSession session(room);
        session.SetSmoothing(smooth);
        std::unique_ptr<ReplayWriter> recording;
        unsigned segment = 0;
        if (!recordFile.empty())
            session.onRecord = [&](const WorldSnapshot &state, const std::vector<InputCmd> &inputs,
                                   bool initial) {
                if (initial) {
                    if (recording)
                        recording->Finish();
                    auto path =
                        segment ? recordFile + ".segment" + std::to_string(segment) : recordFile;
                    ++segment;
                    recording = std::make_unique<ReplayWriter>(path, state,
                                                               IdentityJson(session.identity()));
                } else if (recording)
                    recording->Frame(state, inputs);
            };
        std::unique_ptr<ReplayPlayer> replay;
        std::unique_ptr<ReplayPlayback> playback;
        lab::net::UdpSocket socket;
        auto server = lab::net::UdpAddr::FromIPv4(host, port);
        if (!replayFile.empty()) {
            replay = std::make_unique<ReplayPlayer>(replayFile);
            playback = std::make_unique<ReplayPlayback>(*replay);
        } else if (!socket.Open() || !socket.Bind(0) || !socket.SetNonBlocking(true))
            throw std::runtime_error("client UDP initialization failed");
        bool quit = false;
        double previous = Clock::NowSeconds();
        int frames = 0;
        while (!quit && (!frameLimit || frames < frameLimit)) {
            double now = Clock::NowSeconds(), elapsed = std::clamp(now - previous, 0.0, .25);
            previous = now;
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT)
                    quit = true;
                if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                    auto key = event.key.keysym.sym;
                    if (key == SDLK_ESCAPE)
                        quit = true;
                    if (replay) {
                        if (key == SDLK_SPACE)
                            playback->Action(ReplayAction::TogglePause);
                        if (key == SDLK_RIGHT)
                            playback->Action(ReplayAction::SingleStep);
                        if (key == SDLK_1)
                            playback->Action(ReplayAction::HalfSpeed);
                        if (key == SDLK_2)
                            playback->Action(ReplayAction::NormalSpeed);
                        if (key == SDLK_3)
                            playback->Action(ReplayAction::DoubleSpeed);
                        if (key == SDLK_r)
                            playback->Action(ReplayAction::Restart);
                    } else if (key == SDLK_f) {
                        smooth = !smooth;
                        session.SetSmoothing(smooth);
                    }
                }
            }
            lab::app::NetworkStats hud;
            WorldSnapshot display;
            if (replay) {
                playback->Advance(elapsed);
                display = replay->Snapshot();
                hud.status = playback->playing() ? "replay playing" : "replay paused";
                hud.detail = "hash " + std::to_string(Hasher::Hash(display)) + " speed " +
                             std::to_string(playback->speed());
                if (replay->difference()) {
                    const auto &d = *replay->difference();
                    hud.detail =
                        "DIFF " + d.field + " " + d.expected.dump() + " / " + d.actual.dump();
                }
            } else {
                lab::net::UdpAddr from;
                Bytes bytes;
                for (int n = 0; n < 1024 && socket.RecvFrom(from, bytes); ++n)
                    session.HandleDatagram(bytes, from.Key() == server.Key(), now);
                auto keys = SDL_GetKeyboardState(nullptr);
                InputCmd input;
                input.moveX = (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) -
                              (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]);
                input.moveY = (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) -
                              (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]);
                input.buttons = keys[SDL_SCANCODE_SPACE] ? BIN_ATK : 0;
                session.Update(input, now);
                for (const auto &b : session.DrainOutgoing())
                    socket.SendTo(server, b);
                display = session.Display(now);
                hud.rttMs = session.stats().rttMs;
                hud.inputLeadTicks = Distance(session.next_tick() - 1, session.auth_tick());
                hud.stateDelayTicks = static_cast<int32_t>(session.state_delay(now) / kStep);
                hud.inputPacketsReceived = static_cast<uint32_t>(session.stats().packetsReceived);
                hud.replayCostMs = session.stats().replayMs;
                hud.replayTicks = session.stats().lastReplay;
                hud.targetLead = session.target_lead();
                hud.displayDelayMs = smooth ? 100 : 0;
                hud.status = StateName(session.state());
                hud.detail = "room " + std::to_string(room) + " | F smoothing " +
                             (smooth ? "on" : "off") + " | " + session.reason();
            }
            lab::app::RenderFrame(sdl.render, display, session.stats().corrections,
                                  session.stats().invalidWire, &hud);
            ++frames;
            SDL_Delay(1);
        }
        if (recording)
            recording->Finish();
        return replay && replay->difference() ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
