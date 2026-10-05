#include <algorithm>
#include <chrono>
#include <cmath>
#include <lab/session/ClientSession.h>
#include <stdexcept>

namespace lab::session {
namespace {
constexpr double kReplyTimeoutSeconds = 1.0;
constexpr double kControlRetrySeconds = 0.25;
constexpr double kSmoothingDurationSeconds = 0.1;
constexpr double kRemoteDisplayDelaySeconds = 0.1;
constexpr float kRemoteVelocityThreshold = 0.1f;
constexpr double kCorrectionCountThresholdMeters = 0.0005;
constexpr float kSnapDistanceMeters = 2.0f;
constexpr size_t kRecentSampleCount = 64;
constexpr uint32_t kInputRedundancy = 4;
constexpr uint32_t kMaxAdvancePerUpdate = 4;
constexpr uint32_t kAllowedInputButtons = 15u;
constexpr int kMinimumLead = 2;
constexpr int kMaximumLead = 10;
constexpr double kTicksPerSecond = 60.0;
} // namespace

const char *StateName(SyncState state) {
    switch (state) {
    case SyncState::Connecting:
        return "connecting";
    case SyncState::Waiting:
        return "waiting";
    case SyncState::Playing:
        return "playing";
    case SyncState::Resynchronizing:
        return "resync";
    case SyncState::Rejected:
        return "rejected";
    }
    return "unknown";
}

ClientSession::ClientSession(uint32_t room, size_t historyCapacity)
    : inputs_(historyCapacity), history_(historyCapacity) {
    if (!room)
        throw std::runtime_error("room zero");
    id_.room = room;
}

void ClientSession::QueueOutgoing(Message message) {
    auto bytes = Encode(message);
    ++stats_.packetsSent;
    stats_.bytesSent += bytes.size();
    outgoing_.push_back(std::move(bytes));
}

std::vector<Bytes> ClientSession::DrainOutgoing() {
    auto outgoing = std::move(outgoing_);
    outgoing_.clear();
    return outgoing;
}

void ClientSession::ResetConnectionIdentity(double now) {
    const auto room = id_.room;
    id_ = {};
    id_.room = room;
    ++request_;
    if (!request_)
        ++request_;
    state_ = SyncState::Connecting;
    hasAuth_ = false;
    lastControl_ = -1;
    lastValid_ = now;
    sent_.fill(SentTime{});
    samples_.clear();
}

void ClientSession::RequestResync(double now) {
    if (state_ != SyncState::Resynchronizing) {
        ++stats_.resyncs;
        syncRequest_ = ++request_;
        if (!syncRequest_)
            syncRequest_ = ++request_;
    }
    state_ = SyncState::Resynchronizing;
    lastControl_ = now;
    Message request;
    request.kind = Kind::Resync;
    request.sequence = ++controlSequence_;
    controls_[request.sequence % controls_.size()] = {now, request.sequence, true};
    request.id = id_;
    request.request = syncRequest_;
    QueueOutgoing(request);
}

const std::vector<InputCmd> &ClientSession::BuildPredictedCommands(InputCmd localInput,
                                                                   const WorldSnapshot &snapshot) {
    auto &commands = commands_;
    std::fill(commands.begin(), commands.end(), InputCmd{});
    for (auto &command : commands)
        command.tick = localInput.tick;
    const size_t localIndex = id_.player - 1, remoteIndex = 1 - localIndex;
    commands[localIndex] = localInput;
    const auto &remotePlayer = snapshot.players[remoteIndex];
    // 本地历史是实际按键；远端只按权威速度预测移动，不预测射击、命中或 HP。
    if (remotePlayer.action != Action::Hitstun) {
        commands[remoteIndex].moveX = std::fabs(remotePlayer.v) > kRemoteVelocityThreshold
                                          ? (remotePlayer.v > 0 ? 1 : -1)
                                          : 0;
        commands[remoteIndex].moveY = std::fabs(remotePlayer.vy) > kRemoteVelocityThreshold
                                          ? (remotePlayer.vy > 0 ? 1 : -1)
                                          : 0;
    }
    return commands;
}

void ClientSession::RestoreAndReplay(const WorldSnapshot &snapshot, double now, bool resetHistory,
                                     bool snapDisplay) {
    float previousDisplayX = 0, previousDisplayY = 0;
    if (!snapDisplay && id_.player && world_.View().players.size() == 2) {
        const auto &localPlayer = world_.View().players[id_.player - 1];
        float decay = smoothing_
                          ? static_cast<float>(std::clamp(
                                1 - (now - offsetTime_) / kSmoothingDurationSeconds, 0.0, 1.0))
                          : 0;
        previousDisplayX = localPlayer.x + offsetX_ * decay;
        previousDisplayY = localPlayer.y + offsetY_ * decay;
    }
    // 恢复模拟状态后才更新显示偏移；显示偏移不会写回 World。
    world_.Restore(snapshot);
    if (resetHistory) {
        nextTick_ = snapshot.tick + 1;
        inputs_ = InputBuffer(inputs_.Capacity());
        history_ = sim::StateHistory(history_.Capacity());
    }
    history_.Put(world_.View());
    if (!resetHistory) {
        // 输入帧等于完成帧加一；只重放 auth.tick+1 .. localNextTick-1。
        for (Tick tick = snapshot.tick + 1; tick != nextTick_; ++tick) {
            auto localInput = inputs_.Get(tick);
            world_.Step(BuildPredictedCommands(*localInput, world_.View()),
                        static_cast<float>(kStep));
            history_.Put(world_.View());
        }
    }
    const auto &current = world_.View();
    if (id_.player && current.players.size() == 2) {
        const auto localIndex = id_.player - 1;
        const float correctionX = previousDisplayX - current.players[localIndex].x,
                    correctionY = previousDisplayY - current.players[localIndex].y;
        if (snapDisplay || std::hypot(correctionX, correctionY) > kSnapDistanceMeters) {
            offsetX_ = offsetY_ = 0;
        } else {
            offsetX_ = correctionX;
            offsetY_ = correctionY;
        }
        offsetTime_ = now;
    }
}

void ClientSession::HandleDatagram(std::span<const uint8_t> bytes, bool fromServer, double now) {
    auto message = fromServer ? Decode(bytes) : std::nullopt;
    if (!message) {
        ++stats_.rejected;
        ++stats_.invalidWire;
        return;
    }
    if (message->id.room != id_.room) {
        ++stats_.rejected;
        return;
    }
    if (message->kind == Kind::Reject) {
        if (message->request != request_) {
            ++stats_.late;
            return;
        }
        reason_ = message->reason;
        if (message->reason == "server restarted" || message->reason == "resume expired")
            ResetConnectionIdentity(now);
        else {
            state_ = SyncState::Rejected;
            lastControl_ = now;
        }
        return;
    }
    if (message->kind == Kind::End) {
        if (message->id == id_) {
            reason_ = message->reason;
            // 对方席位到期终止比赛，保留自身凭证，避免再次占用第二个槽位。
            state_ = SyncState::Connecting;
            ++request_;
            if (!request_)
                ++request_;
            lastControl_ = -1;
            lastValid_ = now;
            hasAuth_ = false;
            sent_.fill(SentTime{});
            samples_.clear();
            ++stats_.reconnects;
        } else
            ++stats_.rejected;
        return;
    }
    // Welcome 初始化会话；State/Sync 校验身份与完整重放区间后校正。
    const bool accepted = message->kind == Kind::Welcome
                              ? AcceptWelcome(*message, now)
                              : AcceptAuthoritativeSnapshot(*message, now);
    if (!accepted)
        return;
    ++stats_.packetsReceived;
    stats_.bytesReceived += bytes.size();
    auto &sendTime = message->kind == Kind::State ? sent_[message->sequence % sent_.size()]
                                                  : controls_[message->sequence % controls_.size()];
    if (sendTime.valid && sendTime.sequence == message->sequence) {
        ObserveRtt(now - sendTime.time);
        sendTime.valid = false;
    }
}

bool ClientSession::AcceptWelcome(const Message &message, double now) {
    if (message.request != request_ || !message.id.instance || !message.id.session ||
        !message.id.token || !message.id.player || !message.id.generation ||
        (id_.session && (message.id.session != id_.session || message.id.token != id_.token ||
                         message.id.instance != id_.instance))) {
        ++stats_.rejected;
        return false;
    }
    if (state_ == SyncState::Playing) {
        if (message.id == id_)
            ++stats_.duplicate;
        else
            ++stats_.late;
        return false;
    }
    reason_.clear();
    id_ = message.id;
    sent_.fill(SentTime{});
    if (onRecord && message.rawInitial && message.running) {
        recordAck_ = message.rawInitial->tick;
        onRecord(*message.rawInitial, {}, true);
    }
    RestoreAndReplay(*message.snapshot, now, true, true);
    authTick_ = message.snapshot->tick;
    authTime_ = now;
    hasAuth_ = true;
    state_ = message.running ? SyncState::Playing : SyncState::Waiting;
    samples_.clear();
    samples_.emplace_back(now, *message.snapshot);
    lastValid_ = now;
    return true;
}

bool ClientSession::AcceptAuthoritativeSnapshot(const Message &message, double now) {
    if (!(message.id == id_) || (message.kind != Kind::State && message.kind != Kind::Sync)) {
        ++stats_.rejected;
        return false;
    }
    const auto &map = world_.View();
    if (message.snapshot->mazeSeed != map.mazeSeed ||
        message.snapshot->mazeWidth != map.mazeWidth ||
        message.snapshot->mazeHeight != map.mazeHeight || message.snapshot->maze != map.maze) {
        ++stats_.rejected;
        return false;
    }
    if (message.kind == Kind::Sync &&
        (state_ != SyncState::Resynchronizing || message.request != syncRequest_)) {
        ++stats_.late;
        return false;
    }
    if (message.kind == Kind::State && state_ != SyncState::Playing) {
        ++stats_.late;
        return false;
    }
    const auto &snapshot = *message.snapshot;
    // 录制帧按自身确认进度补齐，重复状态包也可能携带尚未落盘的帧。
    if (onRecord)
        for (const auto &record : message.records) {
            auto recordedSnapshot = ParseSnapshot(record.at("snapshot"), &snapshot);
            if (recordedSnapshot.tick == recordAck_ + 1) {
                std::vector<InputCmd> inputs;
                for (const auto &input : record.at("inputs"))
                    inputs.push_back(ParseInput(input));
                onRecord(recordedSnapshot, inputs, false);
                recordAck_ = recordedSnapshot.tick;
            }
        }
    if (hasAuth_ && !Newer(snapshot.tick, authTick_) && message.kind == Kind::State) {
        if (snapshot.tick == authTick_)
            ++stats_.duplicate;
        else
            ++stats_.late;
        return false;
    }
    const bool isFutureSnapshot = Distance(snapshot.tick, nextTick_ - 1) > 0;
    const uint32_t replayFrames = isFutureSnapshot ? 0 : nextTick_ - (snapshot.tick + 1);
    const bool resetHistory = isFutureSnapshot || message.kind == Kind::Sync;
    if (!resetHistory) {
        if (replayFrames > kReplayBudget) {
            RequestResync(now);
            return false;
        }
        for (Tick tick = snapshot.tick + 1; tick != nextTick_; ++tick)
            if (!inputs_.Get(tick)) {
                RequestResync(now);
                return false;
            }
    }
    // 重放区间完整才提交权威状态；缺失历史不再静默填空输入。
    auto predictedState = history_.GetView(snapshot.tick);
    if (predictedState) {
        const auto localIndex = id_.player - 1;
        const double positionError = std::hypot(
            predictedState->dynamic.players[localIndex].x - snapshot.players[localIndex].x,
            predictedState->dynamic.players[localIndex].y - snapshot.players[localIndex].y);
        stats_.positionErrors.push_back(positionError);
        if (positionError > kCorrectionCountThresholdMeters)
            ++stats_.corrections;
    }
    const auto replayBegin = std::chrono::steady_clock::now();
    RestoreAndReplay(snapshot, now, resetHistory, message.kind == Kind::Sync);
    stats_.replayMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - replayBegin)
            .count();
    stats_.lastReplay = resetHistory ? 0 : replayFrames;
    stats_.replayFrames += stats_.lastReplay;
    authTick_ = snapshot.tick;
    authTime_ = now;
    hasAuth_ = true;
    lastValid_ = now;
    state_ = message.running ? SyncState::Playing : SyncState::Waiting;
    samples_.emplace_back(now, snapshot);
    while (samples_.size() > kRecentSampleCount)
        samples_.pop_front();
    return true;
}

void ClientSession::ObserveRtt(double sample) {
    auto rtt = std::clamp(sample, 0.0, 5.0);
    stats_.rttMs = rtt * 1000;
    rtts_.push_back(rtt);
    if (rtts_.size() > kRecentSampleCount)
        rtts_.pop_front();
    auto sorted = std::vector<double>(rtts_.begin(), rtts_.end());
    std::sort(sorted.begin(), sorted.end());
    double p50 = sorted[(sorted.size() - 1) / 2],
           p95 = sorted[static_cast<size_t>((sorted.size() - 1) * .95)];
    targetLead_ =
        std::clamp(static_cast<int>(std::ceil((rtt / 2 + (p95 - p50) / 2) / kStep)) + kMinimumLead,
                   kMinimumLead, kMaximumLead);
}

void ClientSession::Update(InputCmd input, double now) {
    if ((input.buttons & ~kAllowedInputButtons) || input.moveX < -1 || input.moveX > 1 ||
        input.moveY < -1 || input.moveY > 1)
        throw std::invalid_argument("local input range");
    if (lastValid_ < 0)
        lastValid_ = now;
    if ((state_ == SyncState::Playing || state_ == SyncState::Resynchronizing ||
         state_ == SyncState::Waiting) &&
        now - lastValid_ >= kReplyTimeoutSeconds) {
        state_ = SyncState::Connecting;
        ++request_;
        if (!request_)
            ++request_;
        lastControl_ = -1;
        lastValid_ = now;
        ++stats_.reconnects;
    }
    // 连接/重同步期间只重试控制包，保持预测暂停。
    if (state_ != SyncState::Playing) {
        if (lastControl_ < 0 || now - lastControl_ >= kControlRetrySeconds) {
            if (state_ == SyncState::Resynchronizing)
                RequestResync(now);
            else {
                Message message;
                message.kind = Kind::Hello;
                message.sequence = ++controlSequence_;
                controls_[message.sequence % controls_.size()] = {now, message.sequence, true};

                message.record = static_cast<bool>(onRecord);
                message.id = id_;
                message.request = request_;
                QueueOutgoing(message);
                lastControl_ = now;
            }
        }
        return;
    }
    // 注入时间只决定推进次数；World::Step 始终使用固定步长，单次最多补四帧。
    const double serverProgress =
        authTick_ + (now - authTime_ + stats_.rttMs / 2000) * kTicksPerSecond;
    const int32_t currentLead = Distance(nextTick_, authTick_);
    const int desiredLead = static_cast<int>(std::floor(serverProgress - authTick_)) + targetLead_;
    if (desiredLead - currentLead > static_cast<int>(kReplayBudget)) {
        RequestResync(now);
        return;
    }
    uint32_t advanced = 0;
    while (Distance(nextTick_, authTick_) <= desiredLead && advanced < kMaxAdvancePerUpdate) {
        input.tick = nextTick_;
        inputs_.Put(input);
        world_.Step(BuildPredictedCommands(input, world_.View()), static_cast<float>(kStep));
        history_.Put(world_.View());
        Message message;
        message.kind = Kind::Input;
        message.recordAck = recordAck_;
        message.id = id_;
        message.sequence = ++sequence_;
        message.inputs.reserve(kInputRedundancy);
        // 新输入携带最多 4 帧冗余，服务端按帧去重；已完成的输入允许迟到但不应用。
        for (uint32_t offset = 0; offset < kInputRedundancy; ++offset) {
            auto redundantInput = inputs_.Get(nextTick_ - offset);
            if (!redundantInput)
                break;
            message.inputs.push_back(*redundantInput);
        }
        sent_[message.sequence % sent_.size()] = {now, message.sequence, true};
        QueueOutgoing(message);
        ++nextTick_;
        ++advanced;
    }

    stats_.maxAdvance = std::max(stats_.maxAdvance, advanced);
}

WorldSnapshot ClientSession::Display(double now) const {
    // 只改显示副本：离散字段取较早样本，远端位置才做插值。
    auto display = world_.Snapshot();
    if (!smoothing_ || !id_.player || display.players.size() != 2)
        return display;
    const auto localIndex = id_.player - 1, remoteIndex = 1 - localIndex;
    float alpha = static_cast<float>(
        std::clamp(1 - (now - offsetTime_) / kSmoothingDurationSeconds, 0.0, 1.0));
    display.players[localIndex].x += offsetX_ * alpha;
    display.players[localIndex].y += offsetY_ * alpha;
    if (!samples_.empty()) {
        const double target = now - kRemoteDisplayDelaySeconds;
        const auto *earlier = &samples_.back();
        const auto *later = earlier;
        // 时间窗口无法夹住目标时，保持最近状态，不外推位置。
        if (samples_.size() >= 2 && target >= samples_.front().first &&
            target <= samples_.back().first) {
            earlier = &samples_.front();
            later = earlier;
            for (const auto &sample : samples_) {
                if (sample.first <= target)
                    earlier = &sample;
                if (sample.first >= target) {
                    later = &sample;
                    break;
                }
                later = &sample;
            }
        }
        const auto &previousPlayer = earlier->second.players[remoteIndex];
        const auto &nextPlayer = later->second.players[remoteIndex];
        const double blend =
            later->first > earlier->first
                ? std::clamp((target - earlier->first) / (later->first - earlier->first), 0.0, 1.0)
                : 0;
        display.players[remoteIndex] = previousPlayer;
        display.players[remoteIndex].x =
            static_cast<float>(previousPlayer.x + (nextPlayer.x - previousPlayer.x) * blend);
        display.players[remoteIndex].y =
            static_cast<float>(previousPlayer.y + (nextPlayer.y - previousPlayer.y) * blend);
    }
    return display;
}
} // namespace lab::session
