#pragma once
#include <array>
#include <deque>
#include <functional>
#include <lab/session/Protocol.h>
#include <lab/sim/InputBuffer.h>
#include <lab/sim/StateHistory.h>
#include <lab/sim/World.h>
#include <string>
#include <vector>

namespace lab::session {
enum class SyncState { Connecting, Waiting, Playing, Resynchronizing, Rejected };
const char *StateName(SyncState state);
struct ClientStats {
    // 校正、包拒绝和连接恢复计数。
    uint64_t corrections = 0;
    uint64_t rejected = 0, invalidWire = 0, duplicate = 0, late = 0;
    uint64_t resyncs = 0, reconnects = 0;

    // 重放成本与传输量；positionErrors 比较同一已完成帧的本地位置。
    uint64_t replayFrames = 0;
    uint64_t packetsSent = 0, bytesSent = 0, packetsReceived = 0, bytesReceived = 0;
    uint32_t lastReplay = 0, maxAdvance = 0;
    double replayMs = 0, rttMs = 0;
    std::vector<double> positionErrors;
};

// 输入、网络校正与显示状态共用一个核心，时间和传输由外部驱动。
class ClientSession {
  public:
    explicit ClientSession(uint32_t room = 1, size_t historyCapacity = 4096);
    // 时间由调用者注入；fromServer 必须由传输层按地址校验。
    void HandleDatagram(std::span<const uint8_t> bytes, bool fromServer, double now);
    void Update(InputCmd input, double now);
    std::vector<Bytes> DrainOutgoing();

    // Snapshot 是模拟状态；Display 返回仅供渲染的副本。
    WorldSnapshot Display(double now) const;
    WorldSnapshot Snapshot() const { return world_.Snapshot(); }

    // 会话进度与诊断读口；next_tick 是下一输入帧，auth_tick 是权威完成帧。
    const Identity &identity() const { return id_; }
    const ClientStats &stats() const { return stats_; }
    SyncState state() const { return state_; }
    uint32_t next_tick() const { return nextTick_; }
    uint32_t auth_tick() const { return authTick_; }
    double last_valid() const { return lastValid_; }
    int target_lead() const { return targetLead_; }
    const std::string &reason() const { return reason_; }
    double state_delay(double now) const { return std::max(0.0, now - authTime_); }

    void SetSmoothing(bool enabled) { smoothing_ = enabled; }
    void RequestResync(double now);

    // initial=true 开始新录制段；后续输入是服务端实际采用的输入。
    std::function<void(const WorldSnapshot &, const std::vector<InputCmd> &, bool)> onRecord;

  private:
    // 模拟、完整输入历史与权威进度。
    Identity id_;
    SyncState state_ = SyncState::Connecting;
    sim::World world_{2};
    InputBuffer inputs_;
    sim::StateHistory history_;
    uint32_t nextTick_ = 1, authTick_ = 0, sequence_ = 0, request_ = 1, syncRequest_ = 0;
    bool hasAuth_ = false, smoothing_ = true;

    // 控制请求、连接活动时间及 RTT 估计。负时间表示尚未开始。
    uint32_t recordAck_ = 0;
    double lastValid_ = -1, lastControl_ = -1, authTime_ = 0;
    int targetLead_ = 2;
    std::deque<double> rtts_;
    struct SentTime {
        double time = 0;
        uint32_t sequence = 0;
        bool valid = false;
    };
    // 环槽须检查完整序号；控制包与玩法包各自记录发送时间。
    std::array<SentTime, 512> sent_{};
    std::array<SentTime, 64> controls_{};
    uint32_t controlSequence_ = 0;

    // 显示插值样本和本地偏移，不写回模拟世界。
    std::deque<std::pair<double, WorldSnapshot>> samples_;
    float offsetX_ = 0, offsetY_ = 0;
    double offsetTime_ = 0;
    std::string reason_;

    ClientStats stats_;
    std::vector<Bytes> outgoing_;
    std::vector<InputCmd> commands_{2};

    // 接收分发只处理消息种类；以下方法分别负责初始化和权威校正。
    bool AcceptWelcome(const Message &message, double now);
    bool AcceptAuthoritativeSnapshot(const Message &message, double now);
    void RestoreAndReplay(const WorldSnapshot &snapshot, double now, bool resetHistory,
                          bool snapDisplay);
    const std::vector<InputCmd> &BuildPredictedCommands(InputCmd localInput,
                                                        const WorldSnapshot &snapshot);
    void QueueOutgoing(Message message);
    void ResetConnectionIdentity(double now);
    void ObserveRtt(double sample);
};
} // namespace lab::session
