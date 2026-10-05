#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>
#include <random>
#include <utility>

#include <lab/sim/InputCmd.h>
#include <lab/sim/StateSnapshot.h>

namespace lab::sim {

class World {
public:
    explicit World(size_t numPlayers = 1);

    // 每帧命令数须等于 NumPlayers()，各命令 tick 须一致；成功推进后快照标记该 tick。
    // dt 由调用方固定为 1/60 秒，在线预测、权威推进和重放共用同一实现。
    void Step(const std::vector<InputCmd>& cmds, float dt);

    WorldSnapshot Snapshot() const;
    // 内部只读状态已在 Step/Restore 后同步；引用不能跨下一次世界修改持有。
    const WorldSnapshot& View() const { return snap_; }
    // 恢复地图、弹道和最近瞄准方向，不能只覆盖玩家的位置与 HP。
    void Restore(const WorldSnapshot& s);
    void SetMazeSeed(uint32_t seed, bool resetPlayers = false);

    size_t NumPlayers() const { return snap_.players.size(); }

private:
    WorldSnapshot snap_;
    struct Projectile {
        float x=0, y=0, vx=0, vy=0;
        uint8_t life=0;
        uint8_t owner=0;
    };
    std::vector<Projectile> projectiles_;
    std::vector<float> lastDirX_;
    std::vector<float> lastDirY_;
    std::vector<uint8_t> maze_; // 0 通路、1 墙体
    uint32_t mazeW_ = 15;
    uint32_t mazeH_ = 15;
    uint32_t mazeSeed_ = 12345;
    std::mt19937 rng_{12345};

    void EnsureMaze();
    void GenerateMaze();
    bool IsWall(float x, float y) const;
    bool IsWallCell(int cx, int cy) const;
    bool BoxHitsWall(float x, float y, float radius) const;
    std::pair<float, float> CellCenter(int cx, int cy) const;
    void SnapshotMaze(WorldSnapshot& out) const;
    bool SpawnProjectile(size_t owner, const InputCmd& cmd);
    void StepProjectiles(float dt);
    void SyncProjectiles(WorldSnapshot& out) const;
    void PlacePlayers();
};

} // namespace lab::sim
