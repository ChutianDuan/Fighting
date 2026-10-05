#pragma once

#include <cstdint>

// 离散逻辑帧编号；输入帧 t 经过 Step 后生成标记为 t 的快照。
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
    int8_t moveX = 0; // 在线输入限定为 -1、0、1，表示意图而非直接指定位置
    int8_t moveY = 0;
};
