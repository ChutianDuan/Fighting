#pragma once

#include <cstdint>
#include <vector>

#include <lab/sim/InputCmd.h>

enum class Action : uint8_t {
    Idle = 0,
    Attack = 1, // 保留的动作字段；当前射击不进入此状态
    Hitstun = 2,
};

struct PlayerState {
    float x = 0.0f;
    float v = 0.0f;
    float y = 0.0f;
    float vy = 0.0f;
    uint8_t facing = 0;   // 0 左、1 右、2 上、3 下
    int16_t hp = 100;
    Action action = Action::Idle;
    uint8_t stateTimer = 0;     // 当前主要用于硬直剩余帧数
    uint8_t atkActive = 0;      // 保留的近战字段，当前 Step 每帧置零
    uint8_t attackConnected = 0; // 保留的近战命中标记，当前 Step 每帧置零
    uint8_t onGround = 1;       // 保留的落地标记，当前顶视角模拟始终置一
    uint8_t shotCooldown = 0;   // 距离可再次射击的剩余帧数，必须随快照恢复
    int8_t aimX = 1;            // 最近瞄准方向；恢复后决定静止时的射击朝向
    int8_t aimY = 0;
};

struct ProjectileState {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    uint8_t alive = 0;
    uint8_t life = 0;   // 剩余寿命帧数
    uint8_t owner = 0;  // 玩家槽位，从 1 开始
};

// 恢复模拟所需的数据：玩家状态、弹道、地图，以及已完成帧号。
// 在线初始化为完成帧 0，第一条输入为帧 1。
struct WorldSnapshot {
    Tick tick = 0;
    std::vector<PlayerState> players;
    std::vector<ProjectileState> projectiles;
    // 行优先网格 maze[r * mazeWidth + c]，0 通路、1 墙体。
    uint32_t mazeSeed = 0;
    uint32_t mazeWidth = 0;
    uint32_t mazeHeight = 0;
    std::vector<uint8_t> maze;
};
