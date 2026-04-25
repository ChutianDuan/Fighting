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

    // 多人 Step：cmds.size() 必须 == players_.size()
    void Step(const std::vector<InputCmd>& cmds, float dt);

    WorldSnapshot Snapshot() const;
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
    std::vector<uint8_t> maze_; // 0 free, 1 wall
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
