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

class AuthoritativeServer {
public:
  static constexpr uint8_t kMaxPlayers = 2;
  static constexpr uint8_t kRequiredPlayers = 2;
  static constexpr Tick kStartDelayTicks = 30;
  static constexpr Tick kStateEvery = 2;
  static constexpr double kClientTimeoutSec = 10.0;

  explicit AuthoritativeServer(uint32_t mazeSeed = 20240625u,
                               bool enableLogs = true);

  std::vector<OutboundDatagram> HandleDatagram(
      const lab::net::UdpAddr& from,
      const uint8_t* data,
      size_t len,
      double nowSec);
  std::vector<OutboundDatagram> AdvanceOneTick();
  std::vector<OutboundDatagram> ExpireClients(double nowSec);

  bool started() const { return started_; }
  Tick tick() const { return tick_; }
  uint32_t match_id() const { return matchId_; }
  size_t online_count() const;
  WorldSnapshot snapshot() const { return world_.Snapshot(); }

private:
  struct ClientConn {
    lab::net::UdpAddr addr{};
    InputBuffer inputBuf{4096};
    Tick lastInputTick = 0;
    Tick lastAppliedTick = 0;
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
