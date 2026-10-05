#include <lab/server/AuthoritativeServer.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <lab/net/NetCodec.h>
#include <lab/sim/Hasher.h>
#include <lab/util/log.h>

namespace lab::server {
namespace {

constexpr uint16_t kAllowedButtons = BIN_LEFT | BIN_RIGHT | BIN_JUMP | BIN_ATK;

bool IsLater(Tick value, Tick previous) {
  // 模运算差值支持帧号回绕；前提是两帧距离小于 2^31。
  return static_cast<int32_t>(value - previous) > 0;
}

// 快照转网络状态，位置/速度的 lround(*1000) 必须与 Hasher 量化口径一致。
lab::net::StatePacket MakeState(const WorldSnapshot& snap,
                                uint8_t playerId,
                                uint64_t sessionId,
                                uint32_t matchId,
                                uint64_t hash) {
  lab::net::StatePacket state{};
  state.playerId = playerId;
  state.playerCount = static_cast<uint8_t>(snap.players.size());
  state.projectileCount = static_cast<uint8_t>(
      std::min<size_t>(snap.projectiles.size(), std::numeric_limits<uint8_t>::max()));
  state.sessionId = sessionId;
  state.matchId = matchId;
  state.tick = snap.tick;
  state.mazeSeed = snap.mazeSeed;
  state.stateHash = hash;
  state.players.reserve(state.playerCount);
  for (size_t i = 0; i < state.playerCount; ++i) {
    const auto& source = snap.players[i];
    lab::net::PackedPlayerState player{};
    player.x_mm = static_cast<int32_t>(std::lround(source.x * 1000.0f));
    player.v_mm = static_cast<int32_t>(std::lround(source.v * 1000.0f));
    player.y_mm = static_cast<int32_t>(std::lround(source.y * 1000.0f));
    player.vy_mm = static_cast<int32_t>(std::lround(source.vy * 1000.0f));
    player.hp = source.hp;
    player.action = static_cast<uint8_t>(source.action);
    player.facing = source.facing;
    player.stateTimer = source.stateTimer;
    player.atkActive = source.atkActive;
    player.attackConnected = source.attackConnected;
    player.onGround = source.onGround;
    player.shotCooldown = source.shotCooldown;
    player.aimX = source.aimX;
    player.aimY = source.aimY;
    state.players.push_back(player);
  }
  state.projectiles.reserve(state.projectileCount);
  for (size_t i = 0; i < state.projectileCount; ++i) {
    const auto& source = snap.projectiles[i];
    lab::net::PackedProjectile projectile{};
    projectile.x_mm = static_cast<int32_t>(std::lround(source.x * 1000.0f));
    projectile.y_mm = static_cast<int32_t>(std::lround(source.y * 1000.0f));
    projectile.vx_mm = static_cast<int32_t>(std::lround(source.vx * 1000.0f));
    projectile.vy_mm = static_cast<int32_t>(std::lround(source.vy * 1000.0f));
    projectile.owner = source.owner;
    projectile.life = source.life;
    state.projectiles.push_back(projectile);
  }
  return state;
}

} // namespace

AuthoritativeServer::AuthoritativeServer(uint32_t mazeSeed, bool enableLogs)
    : mazeSeed_(mazeSeed), enableLogs_(enableLogs),
      sessionRng_(std::random_device{}()) {
  world_.SetMazeSeed(mazeSeed_, true);
}

uint64_t AuthoritativeServer::NewSessionId() {
  uint64_t sessionId = 0;
  do {
    sessionId = sessionRng_();
  } while (sessionId == 0 || clients_.contains(sessionId));
  return sessionId;
}

AuthoritativeServer::ClientConn* AuthoritativeServer::FindBySession(uint64_t sessionId) {
  auto it = clients_.find(sessionId);
  return it == clients_.end() ? nullptr : &it->second;
}

AuthoritativeServer::ClientConn* AuthoritativeServer::GetPlayer(uint8_t playerId) {
  if (playerId == 0 || playerId > kMaxPlayers) return nullptr;
  return FindBySession(playerSession_[playerId]);
}

uint8_t AuthoritativeServer::AssignSlot(ClientConn& client) {
  for (uint8_t playerId = 1; playerId <= kMaxPlayers; ++playerId) {
    if (playerSession_[playerId] == 0) {
      playerSession_[playerId] = client.sessionId;
      client.playerId = playerId;
      return playerId;
    }
  }
  return 0;
}

size_t AuthoritativeServer::online_count() const {
  return std::count_if(std::begin(playerSession_), std::end(playerSession_),
                       [](uint64_t sessionId) { return sessionId != 0; });
}

bool AuthoritativeServer::AllowPacket(ClientConn& client, double nowSec) {
  if (nowSec - client.rateWindowSec >= 1.0 || nowSec < client.rateWindowSec) {
    client.rateWindowSec = nowSec;
    client.packetsInWindow = 0;
  }
  if (client.packetsInWindow >= kMaxPacketsPerSecond) return false;
  ++client.packetsInWindow;
  return true;
}

bool AuthoritativeServer::ValidateGameplayInput(
    const ClientConn& client,
    const lab::net::InputPacket& input) const {
  if (!started_ || input.sessionId != client.sessionId ||
      input.playerId != client.playerId || input.matchId != matchId_) {
    return false;
  }
  if (input.cmds.empty() || input.cmds.size() > kMaxCommandsPerPacket ||
      input.count != input.cmds.size()) {
    return false;
  }

  Tick newest = input.cmds.front().tick;
  for (const auto& cmd : input.cmds) {
    const int32_t distance = static_cast<int32_t>(cmd.tick - tick_);
    if (distance < -kPastInputWindow || distance > kMaxInputLead) return false;
    if (cmd.moveX < -1 || cmd.moveX > 1 || cmd.moveY < -1 || cmd.moveY > 1) return false;
    if ((cmd.buttons & ~kAllowedButtons) != 0) return false;
    if (IsLater(cmd.tick, newest)) newest = cmd.tick;
  }
  return newest == input.newestTick;
}

void AuthoritativeServer::ObserveInputStats(
    ClientConn& client,
    const lab::net::InputPacket& input) {
  // 累加通过校验的包数；seq gap 不因迟到包补回而减少，因此只用于估计。
  ++client.inputPacketsReceived;
  if (client.hasInputSeq) {
    const int32_t advance = static_cast<int32_t>(input.seq - client.lastInputSeq);
    if (advance > 1) client.inputPacketsLost += static_cast<uint32_t>(advance - 1);
    if (advance <= 0) return;
  }
  client.lastInputSeq = input.seq;
  client.hasInputSeq = true;
}

InputCmd AuthoritativeServer::GetCmdForTick(ClientConn& client, Tick tick) {
  // 只有取到真实输入才更新 lastAppliedTick；保持旧输入不会延长六帧窗口。
  if (auto input = client.inputBuf.Get(tick)) {
    client.hasLastApplied = true;
    client.lastApplied = *input;
    client.lastAppliedTick = tick;
    return *input;
  }
  if (client.hasLastApplied && tick - client.lastAppliedTick <= kHoldInputTicks) {
    InputCmd held = client.lastApplied;
    held.tick = tick;
    return held;
  }
  return InputBuffer::DefaultForTick(tick);
}

lab::net::StartPacket AuthoritativeServer::MakeStart(const ClientConn& client) const {
  const WorldSnapshot snap = world_.Snapshot();
  lab::net::StartPacket start{};
  start.playerId = client.playerId;
  start.totalPlayers = kMaxPlayers;
  start.sessionId = client.sessionId;
  start.matchId = matchId_;
  start.startTick = tick_; // 重发 Start 时取当前下一帧，客户端已有同局 Start 时忽略
  start.mazeSeed = snap.mazeSeed;
  start.mazeWidth = static_cast<uint16_t>(snap.mazeWidth);
  start.mazeHeight = static_cast<uint16_t>(snap.mazeHeight);
  // 传完整地图，避免各平台标准库 shuffle 差异导致相同 seed 生成不同网格。
  start.maze = snap.maze;
  return start;
}

std::vector<OutboundDatagram> AuthoritativeServer::StartPackets() const {
  std::vector<OutboundDatagram> output;
  for (uint8_t playerId = 1; playerId <= kMaxPlayers; ++playerId) {
    const auto it = clients_.find(playerSession_[playerId]);
    if (it == clients_.end()) continue;
    output.push_back({it->second.addr, lab::net::EncodeStart(MakeStart(it->second))});
  }
  return output;
}

std::vector<OutboundDatagram> AuthoritativeServer::MaybeStartMatch() {
  if (started_ || online_count() < kRequiredPlayers) return {};
  ++matchId_;
  if (matchId_ == 0) ++matchId_;
  tick_ = kStartDelayTicks; // 设置编号后即可推进，不额外等待 30 帧墙钟时间
  started_ = true;
  if (enableLogs_) {
    LOGI("Start match=%u tick=%u players=%zu", matchId_, tick_, online_count());
  }
  return StartPackets();
}

std::vector<OutboundDatagram> AuthoritativeServer::HandleDatagram(
    const lab::net::UdpAddr& from,
    const uint8_t* data,
    size_t len,
    double nowSec) {
  const auto input = lab::net::DecodeInput(data, len);
  if (!input) return {};

  ClientConn* client = input->sessionId == 0 ? nullptr : FindBySession(input->sessionId);
  if (!client) {
    for (auto& [sessionId, candidate] : clients_) {
      (void)sessionId;
      if (candidate.addr.Key() == from.Key()) {
        client = &candidate;
        break;
      }
    }
  }

  if (!client) {
    if (!input->cmds.empty() || input->sessionId != 0) return {};
    ClientConn fresh{};
    fresh.addr = from;
    fresh.lastHeardSec = nowSec;
    fresh.rateWindowSec = nowSec;
    fresh.sessionId = NewSessionId();
    const uint64_t sessionId = fresh.sessionId;
    auto [it, inserted] = clients_.emplace(sessionId, std::move(fresh));
    if (!inserted || AssignSlot(it->second) == 0) {
      clients_.erase(sessionId);
      return {};
    }
    client = &it->second;
    if (enableLogs_) {
      LOGI("Assign %s -> player%u session=%llu", from.ToString().c_str(),
           client->playerId, static_cast<unsigned long long>(client->sessionId));
    }
  }

  if (!AllowPacket(*client, nowSec)) return {};
  client->addr = from;
  client->lastHeardSec = nowSec;

  if (input->cmds.empty()) {
    // hello 同时承担首次入局和丢失 Start 后的重试，不建立通用可靠 UDP 通道。
    if (started_) {
      return {{client->addr, lab::net::EncodeStart(MakeStart(*client))}};
    }
    return MaybeStartMatch();
  }
  if (!started_) {
    // 玩家超时结束比赛后，旧局输入会再次触发 Reset，补偿首次 Reset 丢失。
    lab::net::ResetPacket reset{};
    reset.playerId = client->playerId;
    reset.sessionId = client->sessionId;
    reset.matchId = matchId_;
    return {{client->addr, lab::net::EncodeReset(reset)}};
  }
  if (!ValidateGameplayInput(*client, *input)) return {};

  for (const auto& cmd : input->cmds) {
    client->inputBuf.Put(cmd);
    if (client->lastInputTick == 0 || IsLater(cmd.tick, client->lastInputTick)) {
      client->lastInputTick = cmd.tick;
    }
  }
  ObserveInputStats(*client, *input);
  return {};
}

std::vector<OutboundDatagram> AuthoritativeServer::AdvanceOneTick() {
  if (!started_) return {};
  std::vector<InputCmd> commands;
  commands.reserve(kMaxPlayers);
  for (uint8_t playerId = 1; playerId <= kMaxPlayers; ++playerId) {
    ClientConn* client = GetPlayer(playerId);
    commands.push_back(client ? GetCmdForTick(*client, tick_)
                              : InputBuffer::DefaultForTick(tick_));
  }
  // Step 的快照标记为本次 commands 的 tick，ACK 也确认同一个已完成帧。
  world_.Step(commands, 1.0f / 60.0f);
  const WorldSnapshot snap = world_.Snapshot();
  const uint64_t hash = Hasher::Hash(snap);
  const bool sendState = tick_ % kStateEvery == 0;

  std::vector<OutboundDatagram> output;
  output.reserve(kMaxPlayers * 2);
  for (uint8_t playerId = 1; playerId <= kMaxPlayers; ++playerId) {
    ClientConn* client = GetPlayer(playerId);
    if (!client) continue;
    if (sendState) {
      output.push_back({client->addr,
                        lab::net::EncodeState(MakeState(
                            snap, client->playerId, client->sessionId, matchId_, hash))});
    }
    lab::net::AckPacket ack{};
    ack.playerId = client->playerId;
    ack.sessionId = client->sessionId;
    ack.matchId = matchId_;
    ack.serverTickProcessed = tick_;
    ack.serverLastInputTick = client->lastInputTick;
    ack.serverStateHash = hash;
    ack.serverRecvInputSeq = client->lastInputSeq;
    ack.serverInputPacketsReceived = client->inputPacketsReceived;
    ack.serverInputPacketsLost = client->inputPacketsLost;
    output.push_back({client->addr, lab::net::EncodeAck(ack)});
  }
  ++tick_;
  return output;
}

std::vector<OutboundDatagram> AuthoritativeServer::ResetMatch() {
  // 剩余在线玩家保留 sessionId/槽位；清空本局历史，下一次开局递增 matchId。
  std::vector<OutboundDatagram> output;
  if (started_) {
    for (uint8_t playerId = 1; playerId <= kMaxPlayers; ++playerId) {
      ClientConn* client = GetPlayer(playerId);
      if (!client) continue;
      lab::net::ResetPacket reset{};
      reset.playerId = client->playerId;
      reset.sessionId = client->sessionId;
      reset.matchId = matchId_;
      output.push_back({client->addr, lab::net::EncodeReset(reset)});
    }
  }
  started_ = false;
  tick_ = 0;
  world_ = lab::sim::World(kMaxPlayers);
  world_.SetMazeSeed(mazeSeed_, true);
  for (auto& [sessionId, client] : clients_) {
    (void)sessionId;
    client.inputBuf = InputBuffer(4096);
    client.lastInputTick = 0;
    client.lastAppliedTick = 0;
    client.hasLastApplied = false;
    client.hasInputSeq = false;
    client.lastInputSeq = 0;
    client.inputPacketsReceived = 0;
    client.inputPacketsLost = 0;
  }
  if (enableLogs_) LOGI("Reset match=%u players=%zu", matchId_, online_count());
  return output;
}

std::vector<OutboundDatagram> AuthoritativeServer::ExpireClients(double nowSec) {
  bool removedRunningPlayer = false;
  for (auto it = clients_.begin(); it != clients_.end();) {
    if (nowSec - it->second.lastHeardSec <= kClientTimeoutSec) {
      ++it;
      continue;
    }
    if (enableLogs_) {
      LOGI("Expire player%u session=%llu", it->second.playerId,
           static_cast<unsigned long long>(it->second.sessionId));
    }
    playerSession_[it->second.playerId] = 0;
    removedRunningPlayer = removedRunningPlayer || started_;
    it = clients_.erase(it);
  }
  return removedRunningPlayer ? ResetMatch() : std::vector<OutboundDatagram>{};
}

} // namespace lab::server
