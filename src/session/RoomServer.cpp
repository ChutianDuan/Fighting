#include <algorithm>
#include <lab/session/RoomServer.h>
#include <lab/sim/Hasher.h>
#include <stdexcept>

namespace lab::session {
namespace {
constexpr size_t kPlayersPerRoom = 2;
constexpr size_t kInputHistoryCapacity = 4096;
constexpr size_t kRecordsPerReply = 2;
constexpr size_t kRecordedFrameCapacity = 240;
constexpr uint32_t kHeldInputFrames = 6;
constexpr int32_t kAcceptedPastFrames = 8;
constexpr int32_t kAcceptedFutureFrames = 32;
constexpr uint32_t kInputPacketsPerSecond = 240;
constexpr double kReconnectGraceSeconds = 10;
constexpr double kEmptyRoomLifetimeSeconds = 30;
} // namespace

RoomServer::RoomServer(ServerConfig config)
    : config_(config), random_(config.identitySeed ? config.identitySeed : std::random_device{}()),
      instance_(RandomId()) {
    if (!config.maxRooms || config.maxRooms > 65536)
        throw std::runtime_error("invalid room limit");
}

uint64_t RoomServer::RandomId() {
    for (int attempt = 0; attempt < 64; ++attempt) {
        uint64_t id = config_.identitySource ? config_.identitySource() : random_();
        if (id)
            return id;
    }
    throw std::runtime_error("identity source produced only zero");
}

Message RoomServer::BuildFullReply(const Room &room, const Peer &peer, Kind kind,
                                   uint32_t request) const {
    Message reply;
    reply.kind = kind;
    reply.id = peer.id;
    reply.id.match = room.match;
    reply.request = request;
    reply.sequence = kind == Kind::Welcome ? peer.helloSequence : peer.sequence;
    reply.record = peer.recording;
    if (peer.recording && kind == Kind::Welcome)
        reply.rawInitial = room.world.Snapshot();
    if (peer.recording && kind == Kind::State) {
        // 每次携带少量尚未确认的权威记录，后续由客户端累计确认。
        for (const auto &frame : room.records) {
            if (Newer(frame.at("snapshot").at("tick").get<uint32_t>(), peer.recordAck))
                reply.records.push_back(frame);
            if (reply.records.size() == kRecordsPerReply)
                break;
        }
    }
    reply.running = room.running;
    reply.snapshot = room.world.Snapshot();
    reply.nextTick = reply.snapshot->tick + 1;
    reply.hash = Hasher::Hash(*reply.snapshot);
    return reply;
}

void RoomServer::TryStartRoom(uint32_t roomNumber, Room &room, std::vector<Datagram> &outgoing) {
    if (room.running || !room.peers[0] || !room.peers[1])
        return;
    room.match = ++nextMatch_;
    if (!room.match)
        room.match = ++nextMatch_;
    room.records.clear();
    room.world = sim::World(2);
    room.world.SetMazeSeed(config_.mazeSeed + roomNumber, true);
    room.running = true;
    // 新局从完成帧 0 开始；旧局输入和录制确认不延续到新局。
    for (auto &slot : room.peers) {
        slot->id.match = room.match;
        slot->recordAck = 0;
        slot->inputs = InputBuffer(kInputHistoryCapacity);
        slot->hasApplied = false;
        outgoing.push_back(
            {slot->endpoint, Encode(BuildFullReply(room, *slot, Kind::Welcome, slot->request))});
    }
    if (onStart)
        onStart(roomNumber, room.match, room.world.Snapshot(), {});
}

std::vector<Datagram> RoomServer::HandleDatagram(uint64_t from, std::span<const uint8_t> bytes,
                                                 double now) {
    std::vector<Datagram> outgoing;
    auto message = Decode(bytes);
    if (!message) {
        ++stats_.rejected;
        return outgoing;
    }
    auto reject = [&](const std::string &reason) {
        ++stats_.rejected;
        Message reply;
        reply.kind = Kind::Reject;
        reply.id = message->id;
        reply.id.instance = instance_;
        reply.request = message->request;
        reply.reason = reason;
        outgoing.push_back({from, Encode(reply)});
    };

    // 握手单独处理：路由到房间，查找凭证或首次握手重试，再提交连接信息。
    if (message->kind == Kind::Hello) {
        if (!message->request || !message->inputs.empty() || message->snapshot) {
            reject("invalid hello");
            return outgoing;
        }
        if (message->id.instance && message->id.instance != instance_) {
            reject("server restarted");
            return outgoing;
        }
        auto roomIt = rooms_.find(message->id.room);
        if (roomIt == rooms_.end()) {
            if (message->id.session) {
                reject("resume expired");
                return outgoing;
            }
            if (rooms_.size() >= config_.maxRooms) {
                reject("room limit");
                return outgoing;
            }
            roomIt = rooms_.try_emplace(message->id.room).first;
            roomIt->second.world.SetMazeSeed(config_.mazeSeed + message->id.room, true);
        }
        auto &room = roomIt->second;
        Peer *peer = nullptr;
        for (auto &slot : room.peers)
            if (slot && ((message->id.session && slot->id.session == message->id.session) ||
                         (!message->id.session && !slot->established && slot->endpoint == from &&
                          slot->request == message->request)))
                peer = &*slot;
        if (message->id.session) {
            if (!peer || peer->id.token != message->id.token ||
                now - peer->lastHeard > kReconnectGraceSeconds) {
                reject("resume expired");
                return outgoing;
            }
            if (message->request != peer->request && !Newer(message->request, peer->request)) {
                ++stats_.late;
                return outgoing;
            }
            if (message->request == peer->request && peer->endpoint != from) {
                reject("old request address");
                return outgoing;
            }
            // 新请求开启连接代次；相同请求重复回复，不重复更换代次。
            if (message->request != peer->request) {
                ++peer->id.generation;
                if (!peer->id.generation)
                    ++peer->id.generation;
                peer->hasSequence = false;
                peer->sequence = 0;
                peer->inputs = InputBuffer(kInputHistoryCapacity);
            }
        }
        if (!peer && !message->id.session)
            for (const auto &slot : room.peers)
                if (slot && slot->endpoint == from) {
                    reject("resume required");
                    return outgoing;
                }
        if (!peer) {
            for (size_t slotIndex = 0; slotIndex < kPlayersPerRoom; ++slotIndex)
                if (!room.peers[slotIndex]) {
                    room.peers[slotIndex].emplace();
                    peer = &*room.peers[slotIndex];
                    peer->id = {instance_,
                                RandomId(),
                                RandomId(),
                                message->id.room,
                                room.match,
                                1,
                                static_cast<uint32_t>(slotIndex + 1)};
                    break;
                }
            if (!peer) {
                reject("room full");
                return outgoing;
            }
        }
        // 身份及控制请求全部检查后，才能提交回包地址和活动时间。
        if (peer->request != message->request || !peer->recording)
            peer->recordAck = room.world.View().tick;
        if (message->id.session)
            peer->established = true;
        peer->recording = message->record;
        peer->endpoint = from;
        peer->lastHeard = now;
        peer->request = message->request;
        peer->helloSequence = message->sequence;
        room.emptySince = -1;
        outgoing.push_back(
            {from, Encode(BuildFullReply(room, *peer, Kind::Welcome, message->request))});
        TryStartRoom(message->id.room, room, outgoing);
        return outgoing;
    }

    // 普通报文先按房间/槽位路由，再核对整组身份、来源地址及重连宽限期。
    auto roomIt = rooms_.find(message->id.room);
    if (roomIt == rooms_.end()) {
        ++stats_.rejected;
        return outgoing;
    }
    auto &room = roomIt->second;
    Peer *peer = nullptr;
    if (message->id.player >= 1 && message->id.player <= kPlayersPerRoom &&
        room.peers[message->id.player - 1])
        peer = &*room.peers[message->id.player - 1];
    if (!peer || !(peer->id == message->id) || peer->endpoint != from ||
        now - peer->lastHeard > kReconnectGraceSeconds) {
        ++stats_.rejected;
        return outgoing;
    }
    if (message->kind == Kind::Resync) {
        if (!message->request || message->snapshot || !message->inputs.empty()) {
            ++stats_.rejected;
            return outgoing;
        }
        peer->lastHeard = now;
        peer->established = true;
        auto reply = BuildFullReply(room, *peer, Kind::Sync, message->request);
        reply.sequence = message->sequence;
        outgoing.push_back({from, Encode(reply)});
        return outgoing;
    }
    if (message->kind != Kind::Input || !room.running || message->snapshot) {
        ++stats_.rejected;
        return outgoing;
    }

    // 输入按“最新帧、前一帧……”冗余携带；整包帧号检查完成后才写入历史。
    const auto nextTick = room.world.View().tick + 1;
    for (size_t inputIndex = 0; inputIndex < message->inputs.size(); ++inputIndex) {
        const auto &command = message->inputs[inputIndex];
        if (Distance(command.tick, nextTick) < -kAcceptedPastFrames ||
            Distance(command.tick, nextTick) > kAcceptedFutureFrames ||
            (inputIndex &&
             command.tick != message->inputs[0].tick - static_cast<uint32_t>(inputIndex))) {
            ++stats_.rejected;
            return outgoing;
        }
    }
    if (now - peer->rateStart >= 1) {
        peer->rateStart = now;
        peer->rateCount = 0;
    }
    if (peer->rateCount >= kInputPacketsPerSecond) {
        ++stats_.rejected;
        return outgoing;
    }
    ++peer->rateCount;
    peer->lastHeard = now;
    if (peer->hasSequence && !Newer(message->sequence, peer->sequence)) {
        if (message->sequence == peer->sequence)
            ++stats_.duplicate;
        else
            ++stats_.late;
        return outgoing;
    }
    peer->established = true;
    peer->hasSequence = true;
    peer->sequence = message->sequence;
    if (peer->recording && Newer(message->recordAck, peer->recordAck) &&
        !Newer(message->recordAck, room.world.View().tick))
        peer->recordAck = message->recordAck;
    for (const auto &command : message->inputs)
        if (Distance(command.tick, nextTick) >= 0)
            peer->inputs.Put(command);
    ++stats_.received;
    stats_.bytes += bytes.size();
    return outgoing;
}

std::vector<Datagram> RoomServer::Expire(double now) {
    std::vector<Datagram> outgoing;
    for (auto roomIt = rooms_.begin(); roomIt != rooms_.end();) {
        auto &room = roomIt->second;
        bool peerExpired = false;
        for (auto &slot : room.peers)
            if (slot && now - slot->lastHeard > kReconnectGraceSeconds) {
                slot.reset();
                peerExpired = true;
            }
        // 任一席位宽限期结束即结束该局；另一席位保留，用于等待下一局。
        if (peerExpired && room.running) {
            room.running = false;
            room.match = 0;
            room.world = sim::World(2);
            room.world.SetMazeSeed(config_.mazeSeed + roomIt->first, true);
            for (auto &slot : room.peers)
                if (slot) {
                    Message reply;
                    reply.kind = Kind::End;
                    reply.id = slot->id;
                    reply.reason = "peer grace expired";
                    outgoing.push_back({slot->endpoint, Encode(reply)});
                    slot->id.match = 0;
                }
        }
        // 无席位的房间独立计时回收，不影响其他房间的比赛和输入历史。
        if (!room.peers[0] && !room.peers[1]) {
            if (room.emptySince < 0)
                room.emptySince = now;
            if (now - room.emptySince >= kEmptyRoomLifetimeSeconds) {
                roomIt = rooms_.erase(roomIt);
                continue;
            }
        }
        ++roomIt;
    }
    return outgoing;
}

std::vector<Datagram> RoomServer::AdvanceOneTick(double now) {
    auto outgoing = Expire(now);
    for (auto &[roomNumber, room] : rooms_) {
        if (!room.running)
            continue;
        auto nextTick = room.world.View().tick + 1;
        auto &commands = room.commands;
        for (size_t slotIndex = 0; slotIndex < kPlayersPerRoom; ++slotIndex) {
            auto &peer = *room.peers[slotIndex];
            auto command = peer.inputs.Get(nextTick);
            if (command) {
                peer.applied = *command;
                peer.appliedTick = nextTick;
                peer.hasApplied = true;
            } else if (peer.hasApplied && nextTick - peer.appliedTick <= kHeldInputFrames)
                command = peer.applied;
            // 缺帧最多保持 6 帧，随后归零；录制保存的正是这里实际采用的输入。
            commands[slotIndex] = command.value_or(InputCmd{});
            commands[slotIndex].tick = nextTick;
        }
        room.world.Step(commands, static_cast<float>(kStep));
        stats_.applied += 2;
        auto snapshot = room.world.Snapshot();
        if (onFrame)
            onFrame(roomNumber, room.match, snapshot, commands);
        if (room.peers[0]->recording || room.peers[1]->recording) {
            Json inputs = Json::array();
            for (const auto &command : commands)
                inputs.push_back(InputJson(command));
            room.records.push_back({{"inputs", inputs},
                                    {"snapshot", SnapshotJson(snapshot, false)},
                                    {"hash", Hasher::Hash(snapshot)}});
            while (room.records.size() > kRecordedFrameCapacity)
                room.records.pop_front();
        }
        for (auto &slot : room.peers)
            if (slot->recording || nextTick % 2 == 0)
                outgoing.push_back(
                    {slot->endpoint, Encode(BuildFullReply(room, *slot, Kind::State))});
    }
    return outgoing;
}

std::optional<WorldSnapshot> RoomServer::Snapshot(uint32_t room) const {
    auto roomIt = rooms_.find(room);
    if (roomIt == rooms_.end())
        return {};
    return roomIt->second.world.Snapshot();
}

std::vector<uint64_t> RoomServer::Endpoints() const {
    std::vector<uint64_t> result;
    result.reserve(rooms_.size() * 2);
    for (const auto &[number, room] : rooms_) {
        (void)number;
        for (const auto &peer : room.peers)
            if (peer)
                result.push_back(peer->endpoint);
    }
    return result;
}

} // namespace lab::session
