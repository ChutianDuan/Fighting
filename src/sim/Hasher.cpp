#include <lab/sim/Hasher.h>

#include <cmath>
#include <cstdint>

namespace {
constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime  = 1099511628211ULL;

void MixByte(uint64_t& h, uint8_t byte) {
  h ^= uint64_t(byte);
  h *= kFnvPrime;
}

void MixU8(uint64_t& h, uint8_t v) {
  MixByte(h, v);
}

void MixU16(uint64_t& h, uint16_t v) {
  MixByte(h, uint8_t(v & 0xffu));
  MixByte(h, uint8_t((v >> 8) & 0xffu));
}

void MixI16(uint64_t& h, int16_t v) {
  MixU16(h, static_cast<uint16_t>(v));
}

void MixU32(uint64_t& h, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    MixByte(h, uint8_t((v >> (i * 8)) & 0xffu));
  }
}

void MixI32(uint64_t& h, int32_t v) {
  MixU32(h, static_cast<uint32_t>(v));
}

void MixU64(uint64_t& h, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    MixByte(h, uint8_t((v >> (i * 8)) & 0xffu));
  }
}

int32_t QuantizeMm(float v) {
  // 与服务端 State 的位置/速度打包一致，避免网络量化后的 float 产生假分叉。
  return static_cast<int32_t>(std::lround(v * 1000.0f));
}
}

uint64_t Hasher::Hash(const WorldSnapshot& s) {
  // 固定字段顺序逐字节混合，不读取结构体填充或直接混合原始浮点内存。
  // 这是状态一致性诊断值，不提供报文认证，也不保证跨平台浮点推进完全一致。
  uint64_t h = kFnvOffset;

  MixU32(h, s.tick);

  const uint64_t count = s.players.size();
  MixU64(h, count);

  for (const auto& p : s.players) {
    MixI32(h, QuantizeMm(p.x));
    MixI32(h, QuantizeMm(p.v));
    MixI32(h, QuantizeMm(p.y));
    MixI32(h, QuantizeMm(p.vy));
    MixU8(h, p.facing);
    MixI16(h, p.hp);
    const uint8_t action = static_cast<uint8_t>(p.action);
    MixU8(h, action);
    MixU8(h, p.stateTimer);
    MixU8(h, p.atkActive);
    MixU8(h, p.attackConnected);
    MixU8(h, p.onGround);
    MixU8(h, p.shotCooldown);
    MixU8(h, static_cast<uint8_t>(p.aimX));
    MixU8(h, static_cast<uint8_t>(p.aimY));
  }

  const uint64_t projCount = s.projectiles.size();
  MixU64(h, projCount);
  for (const auto& pr : s.projectiles) {
    MixI32(h, QuantizeMm(pr.x));
    MixI32(h, QuantizeMm(pr.y));
    MixI32(h, QuantizeMm(pr.vx));
    MixI32(h, QuantizeMm(pr.vy));
    MixU8(h, pr.alive);
    MixU8(h, pr.life);
    MixU8(h, pr.owner);
  }

  MixU32(h, s.mazeSeed);
  MixU32(h, s.mazeWidth);
  MixU32(h, s.mazeHeight);
  MixU64(h, s.maze.size());
  for (uint8_t cell : s.maze) {
    MixU8(h, cell);
  }

  return h;
}
