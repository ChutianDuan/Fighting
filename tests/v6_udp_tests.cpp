#include <chrono>
#include <iostream>
#include <lab/session/ClientSession.h>
#include <lab/session/ServerRuntime.h>
#include <lab/sim/Hasher.h>
#include <memory>
#include <thread>
using namespace lab::session;
void Check(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
struct Bot {
    lab::net::UdpSocket socket;
    ClientSession session;
    explicit Bot(uint32_t room) : session(room) {
        Check(socket.Open() && socket.Bind(0, "127.0.0.1") && socket.SetNonBlocking(true),
              "bot UDP init");
    }
};
int main() {
    try {
        ServerRuntime runtime({64, 20240625, 42});
        Check(runtime.Open(0, "127.0.0.1"), "server UDP init");
        auto addr = lab::net::UdpAddr::FromIPv4("127.0.0.1", runtime.port());
        std::vector<std::unique_ptr<Bot>> bots;
        for (int n = 0; n < 4; ++n)
            bots.push_back(std::make_unique<Bot>(n / 2 + 1));
        double now = 0;
        bool drop = false;
        auto step = [&]() {
            for (size_t i = 0; i < 4; ++i) {
                auto &b = *bots[i];
                lab::net::UdpAddr from;
                Bytes bytes;
                while (b.socket.RecvFrom(from, bytes))
                    if (!(drop && i == 1))
                        b.session.HandleDatagram(bytes, from.Key() == addr.Key(), now);
                InputCmd input;
                input.moveX = 1;
                b.session.Update(input, now);
                for (auto &bytes : b.session.DrainOutgoing())
                    if (!(drop && i == 1))
                        Check(b.socket.SendTo(addr, bytes), "send");
            }
            runtime.Poll(now);
            now += kStep;
        };
        for (int n = 0; n < 120; ++n)
            step();
        for (int n = 0; n < 4; ++n)
            Check(bots[n]->session.state() == SyncState::Playing &&
                      bots[n]->session.identity().room == static_cast<uint32_t>(n / 2 + 1),
                  "room handshake");
        auto original = bots[1]->session.identity(), other = bots[2]->session.identity();
        drop = true;
        for (int n = 0; n < 180; ++n)
            step();
        drop = false;
        for (int n = 0; n < 100; ++n)
            step();
        Check(bots[1]->session.identity().session == original.session &&
                  bots[1]->session.identity().match == original.match &&
                  bots[1]->session.identity().generation > original.generation,
              "UDP reconnect original match");
        Check(bots[2]->session.identity() == other, "room isolation reconnect");
        Message old;
        old.kind = Kind::Input;
        old.id = original;
        old.inputs = {{runtime.server.Snapshot(1)->tick + 1, 0, 0, 0}};
        auto rejected = runtime.server.stats().rejected;
        bots[1]->socket.SendTo(addr, Encode(old));
        runtime.Poll(now);
        for (int retry = 0; retry < 200 && runtime.server.stats().rejected == rejected; ++retry) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            runtime.Poll(now);
        }
        Check(runtime.server.stats().rejected >= rejected + 1,
              "old connection generation rejected");
        drop = true;
        for (int n = 0; n < 670; ++n)
            step();
        Check(bots[2]->session.identity() == other && runtime.server.Snapshot(2)->tick > 1000,
              "expired room isolated");
        Check(runtime.server.Snapshot(1)->tick == 0, "old match ended after grace");
        for (auto &b : bots)
            Check(b->session.stats().invalidWire == 0, "UDP snapshot hash/codec failure");
        std::cout << "v6 real UDP rooms, reconnect and generation passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
