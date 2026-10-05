#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include <lab/net/Packets.h>
#include <lab/net/UdpSocket.h>
#include <lab/sim/InputBuffer.h>
#include <lab/sim/World.h>

namespace lab::server {

struct OutboundDatagram {
  lab::net::UdpAddr to{};
  std::vector<uint8_t> bytes;
};

// 在线权威世界与比赛生命周期；不持有 socket，生产入口和无界面测试共用。
class AuthoritativeServer {
public:
  static constexpr uint8_t kMaxPlayers = 2;
  static constexpr uint8_t kRequiredPlayers = 2;
  static constexpr Tick kStartDelayTicks = 30; // 首帧编号偏移，当前不实现墙钟倒计时
  static constexpr Tick kStateEvery = 2;
  static constexpr double kClientTimeoutSec = 10.0;

  explicit AuthoritativeServer(uint32_t mazeSeed = 20240625u,
                               bool enableLogs = true);

  // nowSec 使用单调时钟，供限流/超时判断；返回需要由调用方发送的数据报。
  std::vector<OutboundDatagram> HandleDatagram(
      const lab::net::UdpAddr& from,
      const uint8_t* data,
      size_t len,
      double nowSec);
  // 按 tick_ 取输入、推进一帧、生成 ACK/State，再将 tick_ 增至下一帧。
  std::vector<OutboundDatagram> AdvanceOneTick();
  std::vector<OutboundDatagram> ExpireClients(double nowSec);

  bool started() const { return started_; }
  Tick tick() const { return tick_; } // 下一帧待模拟的编号，不是最近快照的编号
  uint32_t match_id() const { return matchId_; }
  size_t online_count() const;
  WorldSnapshot snapshot() const { return world_.Snapshot(); }

private:
  struct ClientConn {
    lab::net::UdpAddr addr{};
    InputBuffer inputBuf{4096};
    Tick lastInputTick = 0; // 最大已收到输入帧，可能尚未应用于权威世界
    Tick lastAppliedTick = 0; // 最近实际从缓冲取到的输入帧，用于限制缺帧保持时间
    InputCmd lastApplied{};
    bool hasLastApplied = false;
    uint8_t playerId = 0;
    uint64_t sessionId = 0;
    double lastHeardSec = 0.0;
    double rateWindowSec = 0.0;
    uint16_t packetsInWindow = 0;
    bool hasInputSeq = false;
    uint32_t lastInputSeq = 0;
    uint32_t inputPacketsReceived = 0;
    uint32_t inputPacketsLost = 0;
  };

  static constexpr Tick kHoldInputTicks = 6;
  // 相对下一帧 tick_ 的接收窗口；接受过去输入不代表权威世界会重放过去帧。
  static constexpr int32_t kPastInputWindow = 8;
  static constexpr int32_t kMaxInputLead = 12;
  static constexpr uint8_t kMaxCommandsPerPacket = 8;
  static constexpr uint16_t kMaxPacketsPerSecond = 240;

  std::unordered_map<uint64_t, ClientConn> clients_;
  uint64_t playerSession_[kMaxPlayers + 1]{};
  lab::sim::World world_{kMaxPlayers};
  Tick tick_ = 0;
  uint32_t matchId_ = 0;
  uint32_t mazeSeed_ = 0;
  bool started_ = false;
  bool enableLogs_ = true;
  std::mt19937_64 sessionRng_;

  ClientConn* FindBySession(uint64_t sessionId);
  ClientConn* GetPlayer(uint8_t playerId);
  uint8_t AssignSlot(ClientConn& client);
  uint64_t NewSessionId();
  bool AllowPacket(ClientConn& client, double nowSec);
  bool ValidateGameplayInput(const ClientConn& client,
                             const lab::net::InputPacket& input) const;
  InputCmd GetCmdForTick(ClientConn& client, Tick tick);
  void ObserveInputStats(ClientConn& client, const lab::net::InputPacket& input);
  std::vector<OutboundDatagram> MaybeStartMatch();
  std::vector<OutboundDatagram> StartPackets() const;
  std::vector<OutboundDatagram> ResetMatch();
  lab::net::StartPacket MakeStart(const ClientConn& client) const;
};

} // namespace lab::server
