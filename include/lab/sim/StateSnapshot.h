#pragma once

#include <cstdint>
#include <vector>

#include <lab/sim/InputCmd.h>

enum class Action : uint8_t {
    Idle = 0,
    Attack = 1,
    Hitstun = 2,
};

struct PlayerState {
    float x = 0.0f;
    float v = 0.0f;
    float y = 0.0f;
    float vy = 0.0f;
    uint8_t facing = 0;   // 0 -> left, 1 -> right
    int16_t hp = 100;
    Action action = Action::Idle;
    uint8_t stateTimer = 0;     // frames remaining in current action (attack/hitstun/recovery)
    uint8_t atkActive = 0;      // frames remaining in active hitbox
    uint8_t attackConnected = 0; // 0/1: whether current attack已命中
    uint8_t onGround = 1;       // 1 grounded, 0 airborne
    uint8_t shotCooldown = 0;   // frames until the next projectile can be fired
    int8_t aimX = 1;            // last synced aim direction for rollback restore
    int8_t aimY = 0;
};

struct ProjectileState {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    uint8_t alive = 0;
    uint8_t life = 0;   // remaining life ticks
    uint8_t owner = 0;  // player slot (1-based)
};

struct WorldSnapshot {
    Tick tick = 0;
    std::vector<PlayerState> players;
    std::vector<ProjectileState> projectiles;
    // 简易迷宫：grid[r][c]=1 表示墙
    uint32_t mazeSeed = 0;
    uint32_t mazeWidth = 0;
    uint32_t mazeHeight = 0;
    std::vector<uint8_t> maze;
};
