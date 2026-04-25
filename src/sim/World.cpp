#include <lab/sim/World.h>
#include <lab/sim/Rules.h>
#include <lab/util/log.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace lab::sim {

namespace {

constexpr float kCellSize = 1.0f;
constexpr float kPlayerRadius = 0.35f;
constexpr float kProjectileRadius = 0.15f;
constexpr float kSpeed = 4.5f;
constexpr float kFriction = 8.0f;
constexpr float kStopVel = 0.02f;
constexpr float kBulletSpeed = 12.0f;
constexpr uint8_t kBulletLifeTicks = 90;
constexpr uint8_t kShootCooldown = 10;
constexpr uint8_t kHitstunFrames = 8;
constexpr int16_t kBulletDamage = 10;

bool CmdsAligned(const std::vector<InputCmd>& cmds) {
    if (cmds.empty()) return false;
    const Tick t = cmds[0].tick;
    for (size_t i = 1; i < cmds.size(); ++i) {
        if (cmds[i].tick != t) return false;
    }
    return true;
}

int FacingFromDir(float dx, float dy) {
    const float ax = std::fabs(dx);
    const float ay = std::fabs(dy);
    if (ax < 1e-4f && ay < 1e-4f) return 1; // default right
    if (ax >= ay) return dx >= 0.0f ? 1 : 0; // 0 left, 1 right
    return dy >= 0.0f ? 2 : 3;               // 2 up, 3 down
}

std::pair<float, float> DirFromFacing(uint8_t facing) {
    switch (facing) {
        case 0: return {-1.0f, 0.0f};
        case 2: return {0.0f, 1.0f};
        case 3: return {0.0f, -1.0f};
        case 1:
        default:
            return {1.0f, 0.0f};
    }
}

} // namespace

World::World(size_t numPlayers) {
    const size_t playerCount = std::max<size_t>(1, numPlayers);
    snap_.tick = 0;
    snap_.players.resize(playerCount);
    snap_.projectiles.clear();

    projectiles_.clear();
    lastDirX_.assign(playerCount, 1.0f);
    lastDirY_.assign(playerCount, 0.0f);

    for (auto& p : snap_.players) {
        p.x = 0.0f;
        p.v = 0.0f;
        p.y = 0.0f;
        p.vy = 0.0f;
        p.facing = 1;
        p.hp = 100;
        p.action = Action::Idle;
        p.stateTimer = 0;
        p.atkActive = 0;
        p.attackConnected = 0;
        p.onGround = 1;
        p.shotCooldown = 0;
        p.aimX = 1;
        p.aimY = 0;
    }

    rng_.seed(mazeSeed_);
    GenerateMaze();
    PlacePlayers();
    SnapshotMaze(snap_);
    SyncProjectiles(snap_);
}

void World::EnsureMaze() {
    if (!maze_.empty()) return;
    rng_.seed(mazeSeed_);
    GenerateMaze();
    SnapshotMaze(snap_);
}

void World::SetMazeSeed(uint32_t seed, bool resetPlayers) {
    mazeSeed_ = seed;
    rng_.seed(mazeSeed_);
    maze_.clear();
    GenerateMaze();
    SnapshotMaze(snap_);
    if (resetPlayers) PlacePlayers();
}

std::pair<float, float> World::CellCenter(int cx, int cy) const {
    const float halfW = float(mazeW_) * 0.5f;
    const float halfH = float(mazeH_) * 0.5f;
    return { (float(cx) + 0.5f - halfW) * kCellSize,
             (float(cy) + 0.5f - halfH) * kCellSize };
}

bool World::IsWallCell(int cx, int cy) const {
    if (cx < 0 || cy < 0) return true;
    if (cx >= static_cast<int>(mazeW_) || cy >= static_cast<int>(mazeH_)) return true;
    const size_t idx = static_cast<size_t>(cy) * mazeW_ + static_cast<size_t>(cx);
    if (idx >= maze_.size()) return true;
    return maze_[idx] != 0;
}

bool World::IsWall(float x, float y) const {
    if (maze_.empty()) return false;
    const float halfW = float(mazeW_) * 0.5f;
    const float halfH = float(mazeH_) * 0.5f;
    const int cx = static_cast<int>(std::floor(x / kCellSize + halfW));
    const int cy = static_cast<int>(std::floor(y / kCellSize + halfH));
    return IsWallCell(cx, cy);
}

bool World::BoxHitsWall(float x, float y, float radius) const {
    if (maze_.empty()) return false;
    const float halfW = float(mazeW_) * 0.5f;
    const float halfH = float(mazeH_) * 0.5f;
    const int minCx = static_cast<int>(std::floor((x - radius) / kCellSize + halfW));
    const int maxCx = static_cast<int>(std::floor((x + radius) / kCellSize + halfW));
    const int minCy = static_cast<int>(std::floor((y - radius) / kCellSize + halfH));
    const int maxCy = static_cast<int>(std::floor((y + radius) / kCellSize + halfH));
    for (int cy = minCy; cy <= maxCy; ++cy) {
        for (int cx = minCx; cx <= maxCx; ++cx) {
            if (IsWallCell(cx, cy)) return true;
        }
    }
    return false;
}

void World::SnapshotMaze(WorldSnapshot& out) const {
    out.mazeSeed = mazeSeed_;
    out.mazeWidth = mazeW_;
    out.mazeHeight = mazeH_;
    out.maze = maze_;
}

void World::GenerateMaze() {
    mazeW_ = std::max<uint32_t>(mazeW_, 7u);
    mazeH_ = std::max<uint32_t>(mazeH_, 7u);
    if (mazeW_ % 2 == 0) mazeW_ += 1;
    if (mazeH_ % 2 == 0) mazeH_ += 1;

    maze_.assign(size_t(mazeW_ * mazeH_), 1);

    auto idx = [&](int cx, int cy) { return size_t(cy) * mazeW_ + size_t(cx); };
    auto carve = [&](auto&& self, int cx, int cy) -> void {
        std::array<std::pair<int, int>, 4> dirs{{{2, 0}, {-2, 0}, {0, 2}, {0, -2}}};
        std::shuffle(dirs.begin(), dirs.end(), rng_);
        for (auto [dx, dy] : dirs) {
            const int nx = cx + dx;
            const int ny = cy + dy;
            if (nx <= 0 || ny <= 0 || nx >= int(mazeW_) - 1 || ny >= int(mazeH_) - 1) continue;
            if (maze_[idx(nx, ny)] == 0) continue;
            maze_[idx(nx, ny)] = 0;
            maze_[idx(cx + dx / 2, cy + dy / 2)] = 0;
            self(self, nx, ny);
        }
    };

    const int startX = 1;
    const int startY = 1;
    maze_[idx(startX, startY)] = 0;
    carve(carve, startX, startY);

    const int goalX = static_cast<int>(mazeW_) - 2;
    const int goalY = static_cast<int>(mazeH_) - 2;
    maze_[idx(goalX, goalY)] = 0; // 确保远端出生点可达
}

void World::PlacePlayers() {
    if (snap_.players.empty()) return;
    if (maze_.empty()) EnsureMaze();

    const int startX = 1;
    const int startY = 1;
    const int goalX = static_cast<int>(mazeW_) - 2;
    const int goalY = static_cast<int>(mazeH_) - 2;

    const auto [sx, sy] = CellCenter(startX, startY);
    const auto [gx, gy] = CellCenter(goalX, goalY);

    for (size_t i = 0; i < snap_.players.size(); ++i) {
        auto& p = snap_.players[i];
        if (i == 0) { p.x = sx; p.y = sy; }
        else if (i == 1) { p.x = gx; p.y = gy; }
        else {
            // 其他玩家沿长对角线均匀放置
            const float t = float(i) / float(std::max<size_t>(1, snap_.players.size() - 1));
            p.x = sx * (1.0f - t) + gx * t;
            p.y = sy * (1.0f - t) + gy * t;
        }
        p.v = p.vy = 0.0f;
        p.facing = FacingFromDir(lastDirX_[i], lastDirY_[i]);
        p.aimX = static_cast<int8_t>(lastDirX_[i]);
        p.aimY = static_cast<int8_t>(lastDirY_[i]);
    }
}

void World::SyncProjectiles(WorldSnapshot& out) const {
    out.projectiles.clear();
    out.projectiles.reserve(projectiles_.size());
    for (const auto& pr : projectiles_) {
        if (pr.life == 0) continue;
        ProjectileState ps{};
        ps.x = pr.x;
        ps.y = pr.y;
        ps.vx = pr.vx;
        ps.vy = pr.vy;
        ps.alive = pr.life > 0 ? 1 : 0;
        ps.life = pr.life;
        ps.owner = pr.owner;
        out.projectiles.push_back(ps);
    }
}

WorldSnapshot World::Snapshot() const {
    WorldSnapshot out = snap_;
    SnapshotMaze(out);
    SyncProjectiles(out);
    return out;
}

void World::Restore(const WorldSnapshot& s) {
    snap_ = s;
    mazeSeed_ = s.mazeSeed;
    mazeW_ = s.mazeWidth ? s.mazeWidth : mazeW_;
    mazeH_ = s.mazeHeight ? s.mazeHeight : mazeH_;
    if (!s.maze.empty()) {
        maze_ = s.maze;
    } else {
        rng_.seed(mazeSeed_);
        GenerateMaze();
    }

    lastDirX_.assign(snap_.players.size(), 1.0f);
    lastDirY_.assign(snap_.players.size(), 0.0f);
    for (size_t i = 0; i < snap_.players.size(); ++i) {
        if (snap_.players[i].aimX != 0 || snap_.players[i].aimY != 0) {
            lastDirX_[i] = static_cast<float>(snap_.players[i].aimX);
            lastDirY_[i] = static_cast<float>(snap_.players[i].aimY);
        } else {
            auto [dx, dy] = DirFromFacing(snap_.players[i].facing);
            lastDirX_[i] = dx;
            lastDirY_[i] = dy;
            snap_.players[i].aimX = static_cast<int8_t>(dx);
            snap_.players[i].aimY = static_cast<int8_t>(dy);
        }
    }

    projectiles_.clear();
    for (const auto& pr : s.projectiles) {
        if (!pr.alive) continue;
        Projectile p{};
        p.x = pr.x;
        p.y = pr.y;
        p.vx = pr.vx;
        p.vy = pr.vy;
        p.life = pr.life;
        p.owner = pr.owner;
        projectiles_.push_back(p);
    }

    SnapshotMaze(snap_);
    SyncProjectiles(snap_);
}

bool World::SpawnProjectile(size_t owner, const InputCmd& cmd) {
    if (owner >= snap_.players.size()) return false;
    const auto& sp = snap_.players[owner];

    float dirX = (cmd.moveX != 0) ? float(cmd.moveX) : lastDirX_[owner];
    float dirY = (cmd.moveY != 0) ? float(cmd.moveY) : lastDirY_[owner];
    if (std::fabs(dirX) < 1e-4f && std::fabs(dirY) < 1e-4f) dirX = 1.0f;

    const float len = std::sqrt(dirX * dirX + dirY * dirY);
    dirX /= (len > 0.0f ? len : 1.0f);
    dirY /= (len > 0.0f ? len : 1.0f);

    Projectile pr{};
    pr.owner = static_cast<uint8_t>(owner + 1);
    pr.life = kBulletLifeTicks;
    pr.vx = dirX * kBulletSpeed;
    pr.vy = dirY * kBulletSpeed;
    const float spawnDist = kPlayerRadius + kProjectileRadius + 0.05f;
    pr.x = sp.x + dirX * spawnDist;
    pr.y = sp.y + dirY * spawnDist;
    if (BoxHitsWall(pr.x, pr.y, kProjectileRadius)) return false;
    projectiles_.push_back(pr);
    return true;
}

void World::StepProjectiles(float dt) {
    for (auto& pr : projectiles_) {
        if (pr.life == 0) continue;
        if (pr.life > 0) pr.life--;

        pr.x += pr.vx * dt;
        pr.y += pr.vy * dt;

        if (BoxHitsWall(pr.x, pr.y, kProjectileRadius)) {
            pr.life = 0;
            continue;
        }

        for (size_t i = 0; i < snap_.players.size(); ++i) {
            if (static_cast<uint8_t>(i + 1) == pr.owner) continue;
            auto& pl = snap_.players[i];
            const float dx = pl.x - pr.x;
            const float dy = pl.y - pr.y;
            const float r = kPlayerRadius + kProjectileRadius;
            if (dx * dx + dy * dy > r * r) continue;

            pl.hp = static_cast<int16_t>(std::max<int>(0, int(pl.hp) - kBulletDamage));
            pl.action = Action::Hitstun;
            pl.stateTimer = std::max<uint8_t>(pl.stateTimer, kHitstunFrames);

            const float len = std::sqrt(dx * dx + dy * dy);
            const float nx = (len > 0.0f) ? dx / len : 0.0f;
            const float ny = (len > 0.0f) ? dy / len : 0.0f;
            pl.v += nx * 1.5f;
            pl.vy += ny * 1.5f;
            pr.life = 0;
            break;
        }
    }

    projectiles_.erase(
        std::remove_if(projectiles_.begin(), projectiles_.end(),
                       [](const Projectile& pr) { return pr.life == 0; }),
        projectiles_.end());
}

void World::Step(const std::vector<InputCmd>& cmds, float dt) {
    if (cmds.empty()) return;
    if (cmds.size() != snap_.players.size()) {
        LOGW("World::Step cmds.size=%zu != numPlayers=%zu", cmds.size(), snap_.players.size());
        return;
    }
    if (!CmdsAligned(cmds)) {
        LOGW("World::Step tick mismatch");
        return;
    }

    EnsureMaze();
    snap_.tick = cmds[0].tick;

    for (size_t i = 0; i < snap_.players.size(); ++i) {
        auto& p = snap_.players[i];
        const auto& cmd = cmds[i];

        if (p.action == Action::Hitstun && p.stateTimer > 0) {
            p.stateTimer--;
            if (p.stateTimer == 0) p.action = Action::Idle;
        }

        const bool canMove = (p.action != Action::Hitstun);
        const float alpha = std::clamp(kFriction * dt, 0.0f, 1.0f);

        const float targetVx = canMove ? float(cmd.moveX) * kSpeed : 0.0f;
        const float targetVy = canMove ? float(cmd.moveY) * kSpeed : 0.0f;
        p.v += (targetVx - p.v) * alpha;
        p.vy += (targetVy - p.vy) * alpha;

        if (std::fabs(p.v) < kStopVel) p.v = 0.0f;
        if (std::fabs(p.vy) < kStopVel) p.vy = 0.0f;

        float nx = p.x + p.v * dt;
        float ny = p.y + p.vy * dt;

        if (!BoxHitsWall(nx, p.y, kPlayerRadius)) p.x = nx;
        else p.v = 0.0f;
        if (!BoxHitsWall(p.x, ny, kPlayerRadius)) p.y = ny;
        else p.vy = 0.0f;

        if (cmd.moveX != 0 || cmd.moveY != 0) {
            lastDirX_[i] = float(cmd.moveX);
            lastDirY_[i] = float(cmd.moveY);
        } else if (std::fabs(p.v) > 1e-3f || std::fabs(p.vy) > 1e-3f) {
            lastDirX_[i] = (std::fabs(p.v) >= std::fabs(p.vy)) ? ((p.v >= 0.0f) ? 1.0f : -1.0f) : 0.0f;
            lastDirY_[i] = (std::fabs(p.vy) > std::fabs(p.v)) ? ((p.vy >= 0.0f) ? 1.0f : -1.0f) : 0.0f;
        }
        p.facing = static_cast<uint8_t>(FacingFromDir(lastDirX_[i], lastDirY_[i]));
        p.aimX = static_cast<int8_t>(lastDirX_[i]);
        p.aimY = static_cast<int8_t>(lastDirY_[i]);
        p.onGround = 1;
        p.atkActive = 0;
        p.attackConnected = 0;
        if (p.shotCooldown > 0) {
            p.shotCooldown--;
        }

        if ((cmd.buttons & BIN_ATK) && p.shotCooldown == 0) {
            if (SpawnProjectile(i, cmd)) {
                p.shotCooldown = kShootCooldown;
            }
        }
    }

    for (size_t i = 0; i < snap_.players.size(); ++i) {
        for (size_t j = i + 1; j < snap_.players.size(); ++j) {
            ResolvePushbox(snap_.players[i], snap_.players[j], kPlayerRadius);
        }
    }

    StepProjectiles(dt);
    SyncProjectiles(snap_);
    SnapshotMaze(snap_);
}

} // namespace lab::sim
