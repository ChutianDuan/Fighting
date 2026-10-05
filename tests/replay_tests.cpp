#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <lab/session/ClientSession.h>
#include <lab/session/Replay.h>
#include <lab/session/RoomServer.h>
#include <sstream>
using namespace lab::session;
void Require(bool b, const char *message) {
    if (!b)
        throw std::runtime_error(message);
}
int main() {
    try {
        auto directory = std::filesystem::temp_directory_path() / "fighting-replay-tests";
        std::filesystem::create_directories(directory);
        auto path = (directory / "match.jsonl").string(),
             clientPath = (directory / "client.jsonl").string();
        RoomServer server{{64, 12345, 234}};
        ClientSession a, b;
        std::unique_ptr<ReplayWriter> writer, client;
        server.onStart = [&](uint32_t, uint32_t, const WorldSnapshot &s,
                             const std::vector<InputCmd> &) {
            writer = std::make_unique<ReplayWriter>(path, s, Json::object());
        };
        server.onFrame = [&](uint32_t, uint32_t, const WorldSnapshot &s,
                             const std::vector<InputCmd> &cs) { writer->Frame(s, cs); };
        a.onRecord = [&](const WorldSnapshot &s, const std::vector<InputCmd> &cs, bool initial) {
            if (initial)
                client = std::make_unique<ReplayWriter>(clientPath, s, Json::object());
            else
                client->Frame(s, cs);
        };
        double now = 0;
        auto deliver = [&](const std::vector<Datagram> &ds) {
            for (const auto &d : ds)
                (d.endpoint == 1 ? a : b).HandleDatagram(d.bytes, true, now);
        };
        for (int tick = 0; tick < 200; ++tick) {
            now = tick * kStep;
            InputCmd in;
            in.moveX = (tick / 20) % 3 - 1;
            in.moveY = (tick / 30) % 3 - 1;
            in.buttons = tick % 10 == 0 ? BIN_ATK : 0;
            a.Update(in, now);
            b.Update(in, now);
            for (auto &bytes : a.DrainOutgoing())
                deliver(server.HandleDatagram(1, bytes, now));
            for (auto &bytes : b.DrainOutgoing())
                deliver(server.HandleDatagram(2, bytes, now));
            deliver(server.AdvanceOneTick(now));
        }
        writer->Finish();
        client->Finish();
        for (const auto &file : {path, clientPath}) {
            ReplayPlayer p(file);
            while (!p.finished())
                Require(p.Step(), "recorded replay divergence");
            p.Restart();
            Require(p.Snapshot().tick == 0, "restart");
            Require(p.Step() && p.Snapshot().tick == 1, "single frame");
        }
        ReplayPlayer controlled(path);
        ReplayPlayback controls(controlled);
        controls.Action(ReplayAction::TogglePause);
        controls.Advance(.25);
        Require(controlled.Snapshot().tick == 0, "pause advanced frames");
        controls.Action(ReplayAction::SingleStep);
        controls.Advance(.25);
        Require(controlled.Snapshot().tick == 1, "single step skipped frame");
        controls.Action(ReplayAction::Restart);
        Require(controlled.Snapshot().tick == 0 && !controls.playing(), "restart pause semantics");
        controls.Action(ReplayAction::HalfSpeed);
        controls.Action(ReplayAction::TogglePause);
        controls.Advance(kStep * 2);
        Require(controlled.Snapshot().tick == 1, "half speed");
        std::ifstream input(path);
        std::string line;
        std::vector<Json> records;
        while (std::getline(input, line))
            records.push_back(Json::parse(line));
        records[5]["snapshot"]["players"][1]["shotCooldown"] = 7;
        auto corrupt = (directory / "corrupt.jsonl").string();
        {
            std::ofstream f(corrupt);
            for (const auto &j : records)
                f << j.dump() << '\n';
        }
        ReplayPlayer p(corrupt);
        while (!p.finished() && p.Step()) {
        }
        Require(p.difference() && p.difference()->field == "players[1].shotCooldown",
                "first differing field");
        auto truncated = (directory / "truncated.jsonl").string();
        {
            std::ofstream f(truncated);
            for (size_t i = 0; i + 1 < records.size(); ++i)
                f << records[i].dump() << '\n';
        }
        bool failed = false;
        try {
            ReplayPlayer bad(truncated);
        } catch (...) {
            failed = true;
        }
        Require(failed, "truncated file accepted");
        if (std::getenv("LAB_KEEP_REPLAY"))
            std::cout << "fixture: " << path << '\n';
        else
            std::filesystem::remove_all(directory);
        std::cout << "replay recording, single step, corruption and truncation passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
