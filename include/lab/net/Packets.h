#pragma once
// 旧教学测试的数据格式；实际客户端/服务端只使用 session/Protocol.h 的 v6。
#include <cstdint>
#include <vector>
#include <optional>

#include <lab/sim/InputCmd.h> // 网络与模拟共用帧号和输入数据结构

namespace lab::net {

constexpr uint32_t kMagic = 0x4C414230u; // 'LAB0'
constexpr uint16_t kVersion = 5;

enum class PacketType : uint16_t {
  Input = 1,
  Ack   = 2,
  State = 3,
  Start = 4,
  Reset = 5,
};

#pragma pack(push, 1)
struct PacketHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
};
#pragma pack(pop)

// 位置以毫米、速度以毫米/秒传输；其余字段同样参与快照恢复与哈希。
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

// 上行只传输入。在线客户端每包带最近 K 帧；空 cmds 用作 hello/Start 重试。
struct InputPacket {
  uint8_t  playerId = 1;
  uint8_t  count = 0;            // 编码器以 cmds.size()（最多 255）决定实际上行数量
  uint16_t reserved = 0;
  uint64_t sessionId = 0;        // 首次 hello 为 0，Start 告知；Reset 后 hello 可保留原会话
  uint32_t matchId = 0;          // 未进入比赛时为 0
  uint32_t seq = 0;              // 输入序号，用于检测丢包
  Tick newestTick = 0; // 本包最新 tick
  Tick clientAckServerTick = 0; // 最近收到 ACK 的服务端帧号；服务端当前未消费此字段
  std::vector<InputCmd> cmds;    // cmds[i].tick 必须有效
};

// 下行 ACK 确认服务端进度并提供统计，不直接携带可恢复的世界状态。
struct AckPacket {
  uint8_t  playerId = 1;
  uint8_t  reserved[3] = {0,0,0};
  uint64_t sessionId = 0;
  uint32_t matchId = 0;
  Tick serverTickProcessed = 0;   // server 权威推进到的 tick
  Tick serverLastInputTick = 0;   // 最大已收到输入帧，可能尚未模拟，不是逐帧应用确认
  uint64_t serverStateHash = 0;             // 与已完成帧对应的权威哈希，用于一致性诊断
  uint32_t serverRecvInputSeq = 0;           // server 已收到该 client 的最新 input packet seq
  uint32_t serverInputPacketsReceived = 0;   // 累计收到的 input packet 数（不含 hello）
  uint32_t serverInputPacketsLost = 0;       // 基于 seq gap 的累计丢包估计
};

// 周期性全量玩家/弹道状态；迷宫网格只在 Start 传输，State 用 seed 关联地图。
struct StatePacket{
  uint8_t playerId = 1;
  uint8_t playerCount = 0; // 有效玩家数量
  uint8_t projectileCount = 0;
  uint8_t reserved = 0;
  uint64_t sessionId = 0;
  uint32_t matchId = 0;
  Tick tick = 0;
  std::vector<PackedPlayerState> players;
  std::vector<PackedProjectile> projectiles;
  uint64_t stateHash = 0;
  uint32_t mazeSeed = 0;
};

// 同局 Start 可重发；sessionId 标识连接，matchId 隔离比赛，playerId 指定槽位。
struct StartPacket {
  uint8_t playerId = 1;
  uint8_t totalPlayers = 2;
  uint16_t reserved = 0;
  uint64_t sessionId = 0;
  uint32_t matchId = 0;
  Tick startTick = 0; // 下一帧输入的编号，不是墙钟开始时间或倒计时长度
  uint32_t mazeSeed = 0;
  uint16_t mazeWidth = 0;
  uint16_t mazeHeight = 0;
  std::vector<uint8_t> maze;
};

struct ResetPacket {
  uint8_t playerId = 1;
  uint8_t reserved[3] = {0,0,0};
  uint64_t sessionId = 0;
  uint32_t matchId = 0;
};

} // namespace lab::net
