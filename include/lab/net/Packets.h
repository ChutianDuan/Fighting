#pragma once
#include <cstdint>
#include <vector>
#include <optional>

#include <lab/sim/InputCmd.h> // 复用你的 Tick / InputCmd

namespace lab::net {

constexpr uint32_t kMagic = 0x4C414230u; // 'LAB0'
constexpr uint16_t kVersion = 3;

enum class PacketType : uint16_t {
  Input = 1,
  Ack   = 2,
  State = 3,
  Start = 4,
};

#pragma pack(push, 1)
struct PacketHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
};
#pragma pack(pop)

struct PackedPlayerState {
  int32_t x_mm = 0;
  int32_t v_mm = 0;
  int32_t y_mm = 0;
  int32_t vy_mm = 0;
  int16_t hp = 0;
  uint8_t action = 0;
  uint8_t facing = 0;
  uint8_t stateTimer = 0;
  uint8_t atkActive = 0;
  uint8_t attackConnected = 0;
  uint8_t onGround = 1;
  uint8_t shotCooldown = 0;
  int8_t aimX = 1;
  int8_t aimY = 0;
};

struct PackedProjectile {
  int32_t x_mm = 0;
  int32_t y_mm = 0;
  int32_t vx_mm = 0;
  int32_t vy_mm = 0;
  uint8_t owner = 0;
  uint8_t life = 0;
};

// 发送端：每包带最近 K 个 tick 的输入（冗余）
struct InputPacket {
  uint8_t  playerId = 1;
  uint8_t  count = 0;            // cmd 数量
  uint16_t reserved = 0;
  uint32_t seq = 0;              // 输入序号，用于检测丢包
  Tick newestTick = 0; // 本包最新 tick
  Tick clientAckServerTick = 0; // 客户端确认的 server tick（预留给后续回滚/状态）
  std::vector<InputCmd> cmds;    // cmds[i].tick 必须有效
};

// Server -> Client ACK（最小闭环）
struct AckPacket {
  uint8_t  playerId = 1;
  uint8_t  reserved[3] = {0,0,0};
  Tick serverTickProcessed = 0;   // server 权威推进到的 tick
  Tick serverLastInputTick = 0;   // server 已收到该 client 的最大输入 tick
  uint64_t serverStateHash = 0;             // 可选：debug 用（一致性/分叉定位）
};

struct StatePacket{
  uint8_t playerId = 1;
  uint8_t playerCount = 0; // 有效玩家数量
  uint8_t projectileCount = 0;
  uint8_t reserved = 0;
  Tick tick = 0;
  std::vector<PackedPlayerState> players;
  std::vector<PackedProjectile> projectiles;
  uint64_t stateHash = 0;
  uint32_t mazeSeed = 0;
};

struct StartPacket {
  uint8_t playerId = 1;
  uint8_t totalPlayers = 2;
  uint16_t reserved = 0;
  Tick startTick = 0;
};

} // namespace lab::net
