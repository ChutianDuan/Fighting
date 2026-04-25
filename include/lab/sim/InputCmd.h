#pragma once

#include <cstdint>

// 逻辑编号
using Tick = uint32_t;

enum ButtonBits : uint16_t {
    BIN_LEFT  = 1 << 0,
    BIN_RIGHT = 1 << 1,
    BIN_JUMP  = 1 << 2,
    BIN_ATK   = 1 << 3,
};

struct InputCmd {
    Tick tick = 0;
    uint16_t buttons = 0;
    int8_t moveX = 0;
    int8_t moveY = 0;
};
