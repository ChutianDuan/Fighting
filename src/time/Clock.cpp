#include <lab/time/Clock.h>

#include <chrono>

uint64_t Clock::NowNS() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

double Clock::NowSeconds() {
    using namespace std::chrono;
    auto ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    return double(ns) / 1e9;
}
