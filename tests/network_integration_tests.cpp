#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <lab/net/NetCodec.h>
#include <lab/net/UdpSocket.h>
#include <lab/server/AuthoritativeServer.h>
#include <lab/sim/Hasher.h>

namespace {

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "network integration FAIL: " << message << "\n";
  std::exit(1);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

float FromMm(int32_t value) {
  return static_cast<float>(value) / 1000.0f;
}

Action ToAction(uint8_t value) {
  if (value == 1) return Action::Attack;
  if (value == 2) return Action::Hitstun;
  return Action::Idle;
}

struct BotClient {
  lab::net::UdpSocket socket;
  lab::net::UdpAddr server{};
  uint8_t playerId = 0;
  uint64_t sessionId = 0;
  uint32_t matchId = 0;
  Tick tick = 0;
  uint32_t sequence = 0;
  bool started = false;
  bool reset = false;
  uint32_t states = 0;
  WorldSnapshot authoritativeMaze{};
};

struct TestRig {
  lab::net::UdpSocket socket;
  lab::server::AuthoritativeServer server;
  double nowSec = 1.0;
};

template <typename Callback>
void Drain(lab::net::UdpSocket& socket, Callback&& callback) {
  for (;;) {
    lab::net::UdpAddr from{};
    std::vector<uint8_t> bytes;
    if (!socket.RecvFrom(from, bytes)) break;
    callback(from, bytes);
  }
}

void SendOutputs(TestRig& rig,
                 const std::vector<lab::server::OutboundDatagram>& output,
                 uint8_t dropStartForPlayer = 0) {
  for (const auto& datagram : output) {
    if (auto start = lab::net::DecodeStart(datagram.bytes.data(), datagram.bytes.size())) {
      if (start->playerId == dropStartForPlayer) continue;
    }
    Require(rig.socket.SendTo(datagram.to, datagram.bytes), "server send failed");
  }
}

void DrainServer(TestRig& rig, uint8_t dropStartForPlayer = 0) {
  Drain(rig.socket, [&](const lab::net::UdpAddr& from, const std::vector<uint8_t>& bytes) {
    SendOutputs(rig,
                rig.server.HandleDatagram(from, bytes.data(), bytes.size(), rig.nowSec),
                dropStartForPlayer);
  });
}

void PumpServer(TestRig& rig, uint8_t dropStartForPlayer = 0, int attempts = 8) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    DrainServer(rig, dropStartForPlayer);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WorldSnapshot DecodeSnapshot(const BotClient& client,
                             const lab::net::StatePacket& state) {
  WorldSnapshot snapshot = client.authoritativeMaze;
  snapshot.tick = state.tick;
  snapshot.players.resize(state.players.size());
  for (size_t i = 0; i < state.players.size(); ++i) {
    const auto& source = state.players[i];
    auto& player = snapshot.players[i];
    player.x = FromMm(source.x_mm);
    player.v = FromMm(source.v_mm);
    player.y = FromMm(source.y_mm);
    player.vy = FromMm(source.vy_mm);
    player.hp = source.hp;
    player.action = ToAction(source.action);
    player.facing = source.facing;
    player.stateTimer = source.stateTimer;
    player.atkActive = source.atkActive;
    player.attackConnected = source.attackConnected;
    player.onGround = source.onGround;
    player.shotCooldown = source.shotCooldown;
    player.aimX = source.aimX;
    player.aimY = source.aimY;
  }
  snapshot.projectiles.clear();
  for (const auto& source : state.projectiles) {
    ProjectileState projectile{};
    projectile.x = FromMm(source.x_mm);
    projectile.y = FromMm(source.y_mm);
    projectile.vx = FromMm(source.vx_mm);
    projectile.vy = FromMm(source.vy_mm);
    projectile.owner = source.owner;
    projectile.life = source.life;
    projectile.alive = source.life > 0;
    snapshot.projectiles.push_back(projectile);
  }
  return snapshot;
}

void DrainClient(BotClient& client) {
  Drain(client.socket, [&](const lab::net::UdpAddr& from, const std::vector<uint8_t>& bytes) {
    if (from.Key() != client.server.Key()) return;
    if (auto start = lab::net::DecodeStart(bytes.data(), bytes.size())) {
      if (client.started && client.sessionId == start->sessionId &&
          client.matchId == start->matchId) {
        return;
      }
      Require(start->sessionId != 0 && start->matchId != 0, "invalid Start identity");
      client.playerId = start->playerId;
      client.sessionId = start->sessionId;
      client.matchId = start->matchId;
      client.tick = start->startTick;
      client.started = true;
      client.reset = false;
      client.authoritativeMaze = {};
      client.authoritativeMaze.mazeSeed = start->mazeSeed;
      client.authoritativeMaze.mazeWidth = start->mazeWidth;
      client.authoritativeMaze.mazeHeight = start->mazeHeight;
      client.authoritativeMaze.maze = start->maze;
      return;
    }
    if (auto reset = lab::net::DecodeReset(bytes.data(), bytes.size())) {
      if (client.started && reset->playerId == client.playerId &&
          reset->sessionId == client.sessionId && reset->matchId == client.matchId) {
        client.started = false;
        client.reset = true;
      }
      return;
    }
    if (auto ack = lab::net::DecodeAck(bytes.data(), bytes.size())) {
      Require(ack->playerId == client.playerId && ack->sessionId == client.sessionId &&
                  ack->matchId == client.matchId,
              "Ack identity mismatch");
      return;
    }
    if (auto state = lab::net::DecodeState(bytes.data(), bytes.size())) {
      Require(state->playerId == client.playerId && state->sessionId == client.sessionId &&
                  state->matchId == client.matchId,
              "State identity mismatch");
      Require(Hasher::Hash(DecodeSnapshot(client, *state)) == state->stateHash,
              "State hash mismatch with authoritative maze");
      ++client.states;
    }
  });
}

void PumpClients(std::array<BotClient, 2>& clients, int attempts = 8) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    DrainClient(clients[0]);
    DrainClient(clients[1]);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void SendHello(BotClient& client) {
  lab::net::InputPacket hello{};
  hello.playerId = client.playerId == 0 ? 1 : client.playerId;
  hello.sessionId = client.sessionId;
  const auto bytes = lab::net::EncodeInput(hello);
  Require(client.socket.SendTo(client.server, bytes), "client hello send failed");
}

void SendInput(BotClient& client, int8_t moveX = 0, Tick lead = 0) {
  lab::net::InputPacket packet{};
  packet.playerId = client.playerId;
  packet.sessionId = client.sessionId;
  packet.matchId = client.matchId;
  packet.seq = client.sequence++;
  packet.newestTick = client.tick + lead;
  InputCmd command{};
  command.tick = packet.newestTick;
  command.moveX = moveX;
  if (client.tick % 37 == 0) command.buttons = BIN_ATK;
  packet.cmds.push_back(command);
  const auto bytes = lab::net::EncodeInput(packet);
  Require(client.socket.SendTo(client.server, bytes), "client input send failed");
  if (lead == 0) ++client.tick;
}

void OpenClient(BotClient& client, uint16_t serverPort) {
  Require(client.socket.Open(), "client socket open failed");
  Require(client.socket.Bind(0, "127.0.0.1"), "client bind failed");
  Require(client.socket.SetNonBlocking(true), "client nonblocking failed");
  client.server = lab::net::UdpAddr::FromIPv4("127.0.0.1", serverPort);
}

} // namespace

int main() {
  TestRig rig;
  Require(rig.socket.Open(), "server socket open failed");
  Require(rig.socket.Bind(0, "127.0.0.1"), "server bind failed");
  Require(rig.socket.SetNonBlocking(true), "server nonblocking failed");
  const uint16_t serverPort = rig.socket.LocalPort();

  std::array<BotClient, 2> clients{};
  for (auto& client : clients) OpenClient(client, serverPort);

  SendHello(clients[0]);
  PumpServer(rig);
  Require(rig.server.online_count() == 1, "server did not assign the first client");
  SendHello(clients[1]);
  PumpServer(rig, 1);
  PumpClients(clients);
  Require(!clients[0].started && clients[1].started,
          "test must drop the first client's initial Start");

  SendHello(clients[0]);
  PumpServer(rig);
  PumpClients(clients);
  Require(clients[0].started, "repeated hello must recover a lost Start");
  Require(clients[0].matchId == clients[1].matchId, "clients started different matches");
  Require(clients[0].authoritativeMaze.maze == clients[1].authoritativeMaze.maze,
          "Start must carry one authoritative maze");

  const WorldSnapshot beforeInvalid = rig.server.snapshot();
  SendInput(clients[0], 127, 100);
  PumpServer(rig);
  SendOutputs(rig, rig.server.AdvanceOneTick());
  const WorldSnapshot afterInvalid = rig.server.snapshot();
  Require(std::fabs(afterInvalid.players[0].x - beforeInvalid.players[0].x) < 0.01f,
          "invalid future/high-range input changed the world");
  ++clients[0].tick;
  ++clients[1].tick;

  for (int i = 0; i < 180; ++i) {
    SendInput(clients[0], i % 40 < 20 ? 1 : -1);
    SendInput(clients[1], i % 50 < 25 ? -1 : 1);
    PumpServer(rig, 0, 1);
    SendOutputs(rig, rig.server.AdvanceOneTick());
    PumpClients(clients, 1);
    rig.nowSec += 1.0 / 60.0;
  }
  Require(clients[0].states > 0 && clients[1].states > 0,
          "clients did not receive authoritative State packets");

  const uint32_t firstMatch = clients[0].matchId;
  PumpServer(rig);
  rig.nowSec += 20.0;
  SendHello(clients[0]);
  PumpServer(rig);
  const auto droppedReset = rig.server.ExpireClients(rig.nowSec);
  Require(!droppedReset.empty(), "server did not produce Reset after peer timeout");
  Require(clients[0].started, "test must initially drop Reset");
  SendInput(clients[0]);
  PumpServer(rig);
  PumpClients(clients);
  Require(clients[0].reset && !rig.server.started(),
          "remaining client did not receive Reset after peer timeout");

  clients[1].playerId = 0;
  clients[1].sessionId = 0;
  clients[1].matchId = 0;
  clients[1].tick = 0;
  clients[1].sequence = 0;
  clients[1].started = false;
  clients[1].reset = false;
  clients[1].states = 0;
  clients[1].authoritativeMaze = {};
  SendHello(clients[1]);
  PumpServer(rig);
  SendHello(clients[0]);
  PumpServer(rig);
  PumpClients(clients);
  Require(clients[0].started && clients[1].started,
          "server did not start a replacement match");
  Require(clients[0].matchId > firstMatch && clients[1].matchId == clients[0].matchId,
          "replacement match identity did not advance");

  std::cout << "network_integration_tests OK\n";
  return 0;
}
