#pragma once
#include <cstdint>

class Clock{
public:
    static uint64_t NowNS();
    static double NowSeconds();
};