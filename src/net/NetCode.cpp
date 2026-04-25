#include <lab/net/NetCodec.h>

#include <arpa/inet.h>
#include <cstring>
#include <algorithm>

namespace lab::net {

static void WriteU32(std::vector<uint8_t>& b, uint32_t v) {
  //
  uint32_t x = htonl(v);
  uint8_t* p = reinterpret_cast<uint8_t*>(&x);
  b.insert(b.end(), p, p + 4);
}
static void WriteU16(std::vector<uint8_t>& b, uint16_t v) {
  uint16_t x = htons(v);
  uint8_t* p = reinterpret_cast<uint8_t*>(&x);
  b.insert(b.end(), p, p + 2);
}
static void WriteU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
static void WriteI8(std::vector<uint8_t>& b, int8_t v) { b.push_back((uint8_t)v); }
static void WriteU64(std::vector<uint8_t>& b, uint64_t v) {
  // 兼容写法：拆成两个 u32
  uint32_t hi = uint32_t(v >> 32);
  uint32_t lo = uint32_t(v & 0xffffffffu);
  WriteU32(b, hi);
  WriteU32(b, lo);
}

static bool ReadU32(const uint8_t*& p, const uint8_t* end, uint32_t& out) {
  if (end - p < 4) return false;
  uint32_t x;
  std::memcpy(&x, p, 4);
  p += 4;
  out = ntohl(x);
  return true;
}
static bool ReadU16(const uint8_t*& p, const uint8_t* end, uint16_t& out) {
  if (end - p < 2) return false;
  uint16_t x;
  std::memcpy(&x, p, 2);
  p += 2;
  out = ntohs(x);
  return true;
}
static bool ReadU8(const uint8_t*& p, const uint8_t* end, uint8_t& out) {
  if (end - p < 1) return false;
  out = *p++;
  return true;
}
static bool ReadI8(const uint8_t*& p, const uint8_t* end, int8_t& out) {
  uint8_t u;
  if (!ReadU8(p, end, u)) return false;
  out = (int8_t)u;
  return true;
}
static bool ReadU64(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
  uint32_t hi, lo;
  if (!ReadU32(p, end, hi)) return false;
  if (!ReadU32(p, end, lo)) return false;
  out = (uint64_t(hi) << 32) | uint64_t(lo);
  return true;
}

static void WriteHeader(std::vector<uint8_t>& b, PacketType t) {
  WriteU32(b, kMagic);
  WriteU16(b, kVersion);
  WriteU16(b, (uint16_t)t);
}

static bool ReadHeader(const uint8_t*& p, const uint8_t* end, PacketType& t) {
  uint32_t magic;
  uint16_t ver, type;
  if (!ReadU32(p, end, magic)) return false;
  if (!ReadU16(p, end, ver)) return false;
  if (!ReadU16(p, end, type)) return false;
  if (magic != kMagic || ver != kVersion) return false;
  t = (PacketType)type;
  return true;
}

std::vector<uint8_t> EncodeInput(const InputPacket& p) {
  std::vector<uint8_t> b;
  b.reserve(64);
  WriteHeader(b, PacketType::Input);

  const uint8_t count = static_cast<uint8_t>(std::min<size_t>(p.cmds.size(), 255));
  WriteU8(b, p.playerId);
  WriteU8(b, count);
  WriteU16(b, 0);
  WriteU32(b, p.seq);
  WriteU32(b, p.newestTick);
  WriteU32(b, p.clientAckServerTick);

  // 每个 cmd：tick(u32) buttons(u16) moveX(i8) moveY(i8)
  for (uint8_t i = 0; i < count; ++i) {
    const auto& c = p.cmds[i];
    WriteU32(b, c.tick);
    WriteU16(b, c.buttons);
    WriteI8(b, c.moveX);
    WriteI8(b, c.moveY);
  }
  return b;
}

std::vector<uint8_t> EncodeAck(const AckPacket& p) {
  std::vector<uint8_t> b;
  b.reserve(64);
  WriteHeader(b, PacketType::Ack);

  WriteU8(b, p.playerId);
  WriteU8(b, 0); WriteU8(b, 0); WriteU8(b, 0);
  WriteU32(b, p.serverTickProcessed);
  WriteU32(b, p.serverLastInputTick);
  WriteU64(b, p.serverStateHash);
  return b;
}

std::optional<InputPacket> DecodeInput(const uint8_t* data, size_t len) {
  const uint8_t* p = data;
  const uint8_t* end = data + len;
  PacketType t;
  if (!ReadHeader(p, end, t) || t != PacketType::Input) return std::nullopt;

  InputPacket out;
  uint16_t rsv;
  if (!ReadU8(p, end, out.playerId)) return std::nullopt;
  if (!ReadU8(p, end, out.count)) return std::nullopt;
  if (!ReadU16(p, end, rsv)) return std::nullopt;
  if (!ReadU32(p, end, out.seq)) return std::nullopt;
  if (!ReadU32(p, end, out.newestTick)) return std::nullopt;
  if (!ReadU32(p, end, out.clientAckServerTick)) return std::nullopt;

  out.cmds.clear();
  out.cmds.reserve(out.count);

  for (uint8_t i = 0; i < out.count; ++i) {
    InputCmd c;
    uint16_t buttons;
    int8_t moveX;
    int8_t moveY;

    uint32_t tick;
    if (!ReadU32(p, end, tick)) return std::nullopt;
    if (!ReadU16(p, end, buttons)) return std::nullopt;
    if (!ReadI8(p, end, moveX)) return std::nullopt;
    if (!ReadI8(p, end, moveY)) return std::nullopt;

    c.tick = tick;
    c.buttons = buttons;
    c.moveX = moveX;
    c.moveY = moveY;
    out.cmds.push_back(c);
  }
  return out;
}

std::optional<AckPacket> DecodeAck(const uint8_t* data, size_t len) {
  const uint8_t* p = data;
  const uint8_t* end = data + len;
  PacketType t;
  if (!ReadHeader(p, end, t) || t != PacketType::Ack) return std::nullopt;

  AckPacket out;
  uint8_t dummy;
  if (!ReadU8(p, end, out.playerId)) return std::nullopt;
  if (!ReadU8(p, end, dummy)) return std::nullopt;
  if (!ReadU8(p, end, dummy)) return std::nullopt;
  if (!ReadU8(p, end, dummy)) return std::nullopt;

  if (!ReadU32(p, end, out.serverTickProcessed)) return std::nullopt;
  if (!ReadU32(p, end, out.serverLastInputTick)) return std::nullopt;
  if (!ReadU64(p, end, out.serverStateHash)) return std::nullopt;
  return out;
}
static void WriteI16(std::vector<uint8_t>& b, int16_t v) {
  uint16_t u = htons(static_cast<uint16_t>(v));
  uint8_t* p = reinterpret_cast<uint8_t*>(&u);
  b.insert(b.end(), p, p + 2);
}
static void WriteI32(std::vector<uint8_t>& b, int32_t v) {
  uint32_t u = htonl(static_cast<uint32_t>(v));
  uint8_t* p = reinterpret_cast<uint8_t*>(&u);
  b.insert(b.end(), p, p + 4);
}
static bool ReadI32(const uint8_t*& p, const uint8_t* end, int32_t& out) {
  if (end - p < 4) return false;
  uint32_t u;
  std::memcpy(&u, p, 4);
  p += 4;
  out = static_cast<int32_t>(ntohl(u));
  return true;
}
static bool ReadI16(const uint8_t*& p, const uint8_t* end, int16_t& out) {
  if (end - p < 2) return false;
  uint16_t u;
  std::memcpy(&u, p, 2);
  p += 2;
  out = static_cast<int16_t>(ntohs(u));
  return true;
}

std::vector<uint8_t> EncodeState(const StatePacket& s) {
  std::vector<uint8_t> b;
  const uint8_t count = static_cast<uint8_t>(std::min<size_t>(s.playerCount, s.players.size()));
  
  // 通过设置和玩家列表中选取最小作为 玩家数量，动态扩容
  const uint8_t projCount = static_cast<uint8_t>(std::min<size_t>(s.projectileCount, s.projectiles.size()));
  b.reserve(32 + size_t(count) * 27 + size_t(projCount) * 18);
  WriteHeader(b, PacketType::State);

  WriteU8(b, s.playerId);
  WriteU8(b, count);
  WriteU8(b, projCount);
  WriteU8(b, 0);
  WriteU32(b, s.tick);
  WriteU32(b, s.mazeSeed);

  for (uint8_t i = 0; i < count; ++i) {
    const auto& ps = s.players[i];
    WriteI32(b, ps.x_mm);
    WriteI32(b, ps.v_mm);
    WriteI32(b, ps.y_mm);
    WriteI32(b, ps.vy_mm);
    WriteI16(b, ps.hp);
    WriteU8(b, ps.action);
    WriteU8(b, ps.facing);
    WriteU8(b, ps.stateTimer);
    WriteU8(b, ps.atkActive);
    WriteU8(b, ps.attackConnected);
    WriteU8(b, ps.onGround);
    WriteU8(b, ps.shotCooldown);
    WriteI8(b, ps.aimX);
    WriteI8(b, ps.aimY);
  }

  for (uint8_t i = 0; i < projCount; ++i) {
    const auto& pr = s.projectiles[i];
    WriteI32(b, pr.x_mm);
    WriteI32(b, pr.y_mm);
    WriteI32(b, pr.vx_mm);
    WriteI32(b, pr.vy_mm);
    WriteU8(b, pr.owner);
    WriteU8(b, pr.life);
  }

  WriteU64(b, s.stateHash);
  return b;
}

std::vector<uint8_t> EncodeStart(const StartPacket& s) {
  std::vector<uint8_t> b;
  b.reserve(32);
  WriteHeader(b, PacketType::Start);
  WriteU8(b, s.playerId);
  WriteU8(b, s.totalPlayers);
  WriteU16(b, 0);
  WriteU32(b, s.startTick);
  return b;
}

std::optional<StatePacket> DecodeState(const uint8_t* data, size_t len) {
  const uint8_t* p = data;
  const uint8_t* end = data + len;

  PacketType t;
  if (!ReadHeader(p, end, t) || t != PacketType::State) return std::nullopt;

  StatePacket s;
  uint8_t projCount;
  uint8_t dummy8;

  if (!ReadU8(p, end, s.playerId)) return std::nullopt;
  if (!ReadU8(p, end, s.playerCount)) return std::nullopt;
  if (!ReadU8(p, end, projCount)) return std::nullopt;
  if (!ReadU8(p, end, dummy8)) return std::nullopt;
  s.projectileCount = projCount;

  if (!ReadU32(p, end, s.tick)) return std::nullopt;
  if (!ReadU32(p, end, s.mazeSeed)) return std::nullopt;

  s.players.clear();
  s.players.reserve(s.playerCount);
  for (uint8_t i = 0; i < s.playerCount; ++i) {
    PackedPlayerState ps{};
    if (!ReadI32(p, end, ps.x_mm)) return std::nullopt;
    if (!ReadI32(p, end, ps.v_mm)) return std::nullopt;
    if (!ReadI32(p, end, ps.y_mm)) return std::nullopt;
    if (!ReadI32(p, end, ps.vy_mm)) return std::nullopt;
    if (!ReadI16(p, end, ps.hp)) return std::nullopt;
    if (!ReadU8(p, end, ps.action)) return std::nullopt;
    if (!ReadU8(p, end, ps.facing)) return std::nullopt;
    if (!ReadU8(p, end, ps.stateTimer)) return std::nullopt;
    if (!ReadU8(p, end, ps.atkActive)) return std::nullopt;
    if (!ReadU8(p, end, ps.attackConnected)) return std::nullopt;
    if (!ReadU8(p, end, ps.onGround)) return std::nullopt;
    if (!ReadU8(p, end, ps.shotCooldown)) return std::nullopt;
    if (!ReadI8(p, end, ps.aimX)) return std::nullopt;
    if (!ReadI8(p, end, ps.aimY)) return std::nullopt;
    s.players.push_back(ps);
  }

  s.projectiles.clear();
  s.projectiles.reserve(projCount);
  for (uint8_t i = 0; i < projCount; ++i) {
    PackedProjectile pr{};
    if (!ReadI32(p, end, pr.x_mm)) return std::nullopt;
    if (!ReadI32(p, end, pr.y_mm)) return std::nullopt;
    if (!ReadI32(p, end, pr.vx_mm)) return std::nullopt;
    if (!ReadI32(p, end, pr.vy_mm)) return std::nullopt;
    if (!ReadU8(p, end, pr.owner)) return std::nullopt;
    if (!ReadU8(p, end, pr.life)) return std::nullopt;
    s.projectiles.push_back(pr);
  }

  if (!ReadU64(p, end, s.stateHash)) return std::nullopt;

  return s;
}

std::optional<StartPacket> DecodeStart(const uint8_t* data, size_t len) {
  const uint8_t* p = data;
  const uint8_t* end = data + len;

  PacketType t;
  if (!ReadHeader(p, end, t) || t != PacketType::Start) return std::nullopt;

  StartPacket s;
  uint16_t dummy16;
  if (!ReadU8(p, end, s.playerId)) return std::nullopt;
  if (!ReadU8(p, end, s.totalPlayers)) return std::nullopt;
  if (!ReadU16(p, end, dummy16)) return std::nullopt;
  if (!ReadU32(p, end, s.startTick)) return std::nullopt;
  return s;
}
} // namespace lab::net
