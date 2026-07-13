#include <lab/net/NetCodec.h>
#include <lab/sim/Hasher.h>
#include <lab/sim/InputBuffer.h>
#include <lab/sim/StateHistory.h>
#include <lab/sim/World.h>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

WorldSnapshot OnePlayerSnapshot() {
  WorldSnapshot s{};
  s.tick = 42;
  s.players.resize(1);
  s.players[0].x = 1.23456f;
  s.players[0].y = -2.5f;
  s.players[0].v = 0.25f;
  s.players[0].vy = -0.5f;
  s.players[0].hp = 90;
  s.players[0].facing = 1;
  s.players[0].shotCooldown = 3;
  s.players[0].aimX = 1;
  s.players[0].aimY = -1;
  s.mazeSeed = 7;
  s.mazeWidth = 3;
  s.mazeHeight = 3;
  s.maze = {1, 1, 1, 1, 0, 1, 1, 1, 1};
  return s;
}

} // namespace

int main() {
  {
    InputBuffer buf(0);
    InputCmd cmd{};
    cmd.tick = 5;
    cmd.moveX = 1;
    buf.Put(cmd);
    Require(buf.Get(5).has_value(), "InputBuffer handles zero capacity");
  }

  {
    lab::sim::StateHistory hist(0);
    WorldSnapshot s = OnePlayerSnapshot();
    hist.Put(s);
    Require(hist.Get(s.tick).has_value(), "StateHistory handles zero capacity");
  }

  {
    lab::net::InputPacket p{};
    p.playerId = 2;
    p.sessionId = 0x1122334455667788ULL;
    p.matchId = 9;
    p.count = 99;
    p.seq = 12;
    p.newestTick = 8;
    p.cmds = {InputCmd{7, BIN_ATK, 1, 0}, InputCmd{8, 0, -1, 1}};

    auto bytes = lab::net::EncodeInput(p);
    auto decoded = lab::net::DecodeInput(bytes.data(), bytes.size());
    Require(decoded.has_value(), "InputPacket decodes");
    Require(decoded->count == 2, "InputPacket count follows encoded command vector");
    Require(decoded->sessionId == p.sessionId && decoded->matchId == p.matchId,
            "InputPacket preserves response identity");
    Require(decoded->cmds.size() == 2, "InputPacket preserves commands");
    Require(decoded->cmds[1].moveY == 1, "InputPacket preserves moveY");
  }

  {
    lab::net::AckPacket p{};
    p.playerId = 2;
    p.sessionId = 55;
    p.matchId = 9;
    p.serverTickProcessed = 123;
    p.serverLastInputTick = 120;
    p.serverStateHash = 0x123456789abcdef0ULL;
    p.serverRecvInputSeq = 44;
    p.serverInputPacketsReceived = 40;
    p.serverInputPacketsLost = 3;

    auto bytes = lab::net::EncodeAck(p);
    auto decoded = lab::net::DecodeAck(bytes.data(), bytes.size());
    Require(decoded.has_value(), "AckPacket decodes");
    Require(decoded->sessionId == p.sessionId && decoded->matchId == p.matchId,
            "AckPacket preserves response identity");
    Require(decoded->serverRecvInputSeq == p.serverRecvInputSeq, "AckPacket preserves recv input seq");
    Require(decoded->serverInputPacketsReceived == p.serverInputPacketsReceived,
            "AckPacket preserves received input packet count");
    Require(decoded->serverInputPacketsLost == p.serverInputPacketsLost,
            "AckPacket preserves lost input packet count");
  }

  {
    lab::net::StatePacket p{};
    p.playerId = 1;
    p.sessionId = 55;
    p.matchId = 9;
    p.playerCount = 1;
    p.projectileCount = 1;
    p.tick = 33;
    p.mazeSeed = 1234;
    p.stateHash = 0xabcdefu;
    lab::net::PackedPlayerState player{};
    player.x_mm = 1200;
    player.y_mm = -500;
    player.hp = 80;
    player.shotCooldown = 7;
    player.aimX = -1;
    player.aimY = 1;
    p.players.push_back(player);
    lab::net::PackedProjectile projectile{};
    projectile.x_mm = 100;
    projectile.life = 3;
    projectile.owner = 1;
    p.projectiles.push_back(projectile);

    auto bytes = lab::net::EncodeState(p);
    auto decoded = lab::net::DecodeState(bytes.data(), bytes.size());
    Require(decoded.has_value(), "StatePacket decodes");
    Require(decoded->projectileCount == 1, "StatePacket preserves projectileCount");
    Require(decoded->players[0].shotCooldown == 7, "StatePacket preserves shotCooldown");
    Require(decoded->players[0].aimX == -1, "StatePacket preserves aimX");
    Require(decoded->players[0].aimY == 1, "StatePacket preserves aimY");
    Require(decoded->stateHash == p.stateHash, "StatePacket preserves stateHash");
    Require(decoded->sessionId == p.sessionId && decoded->matchId == p.matchId,
            "StatePacket preserves response identity");
  }

  {
    lab::net::StartPacket start{};
    start.playerId = 2;
    start.totalPlayers = 2;
    start.sessionId = 77;
    start.matchId = 3;
    start.startTick = 30;
    start.mazeSeed = 1234;
    start.mazeWidth = 3;
    start.mazeHeight = 3;
    start.maze = {1, 1, 1, 1, 0, 1, 1, 1, 1};
    auto bytes = lab::net::EncodeStart(start);
    auto decoded = lab::net::DecodeStart(bytes.data(), bytes.size());
    Require(decoded.has_value(), "StartPacket decodes");
    Require(decoded->maze == start.maze, "StartPacket preserves authoritative maze");
    Require(decoded->sessionId == start.sessionId && decoded->matchId == start.matchId,
            "StartPacket preserves response identity");

    bytes.push_back(0);
    Require(!lab::net::DecodeStart(bytes.data(), bytes.size()),
            "packet decoder rejects trailing data");
  }

  {
    lab::net::ResetPacket reset{};
    reset.playerId = 1;
    reset.sessionId = 88;
    reset.matchId = 4;
    const auto bytes = lab::net::EncodeReset(reset);
    const auto decoded = lab::net::DecodeReset(bytes.data(), bytes.size());
    Require(decoded.has_value(), "ResetPacket decodes");
    Require(decoded->sessionId == reset.sessionId && decoded->matchId == reset.matchId,
            "ResetPacket preserves response identity");
  }

  {
    WorldSnapshot server = OnePlayerSnapshot();
    WorldSnapshot decoded = server;
    decoded.players[0].x = 1.235f;
    Require(Hasher::Hash(server) == Hasher::Hash(decoded),
            "Hasher uses network millimeter precision for floats");

    decoded.players[0].shotCooldown = 4;
    Require(Hasher::Hash(server) != Hasher::Hash(decoded),
            "Hasher includes shot cooldown state");
  }

  {
    lab::sim::World world(0);
    Require(world.NumPlayers() == 1, "World clamps zero players to one");
  }

  std::cout << "core_tests OK\n";
  return 0;
}
