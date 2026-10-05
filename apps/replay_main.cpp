#include <iostream>
#include <lab/session/Replay.h>
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("usage: lab_replay <file.jsonl>");
        lab::session::ReplayPlayer replay(argv[1]);
        while (!replay.finished() && replay.Step()) {
        }
        if (replay.difference()) {
            const auto &d = *replay.difference();
            std::cerr << "tick " << d.tick << " " << d.field << " expected=" << d.expected
                      << " actual=" << d.actual << '\n';
            return 1;
        }
        std::cout << "verified " << replay.frame_count() << " frames\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
