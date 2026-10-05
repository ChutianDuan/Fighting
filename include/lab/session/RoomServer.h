#pragma once
#include <array>
#include <deque>
#include <functional>
#include <lab/session/Protocol.h>
#include <lab/sim/InputBuffer.h>
#include <lab/sim/World.h>
#include <map>
#include <random>

namespace lab::session {
struct Datagram {
    uint64_t endpoint = 0;
    Bytes bytes;
};

struct ServerStats {
    uint64_t rejected = 0;
    uint64_t duplicate = 0;
    uint64_t late = 0;
    uint64_t received = 0;
    uint64_t bytes = 0;
    uint64_t applied = 0;
};

struct ServerConfig {
    size_t maxRooms = 64;
    uint32_t mazeSeed = 20240625;
    uint64_t identitySeed = 0;
    std::function<uint64_t()> identitySource = {}; // 实验可替换来源；默认使用随机种子生成器
};

// 不依赖 socket 或真实时钟：输入数据报及注入时间，输出发往各端点的回复。
class RoomServer {
  public:
    explicit RoomServer(ServerConfig config = {});

    // 网络处理与生命周期：推进一帧前也会清理过期席位。
    std::vector<Datagram> HandleDatagram(uint64_t from, std::span<const uint8_t> bytes, double now);
    std::vector<Datagram> AdvanceOneTick(double now);
    std::vector<Datagram> Expire(double now);

    // 只读查询供传输层、实验器和诊断使用。
    size_t room_count() const { return rooms_.size(); }
    uint64_t instance() const { return instance_; }
    const ServerStats &stats() const { return stats_; }
    std::optional<WorldSnapshot> Snapshot(uint32_t room) const;
    std::vector<uint64_t> Endpoints() const;

    // 记录的是实际采用的输入（包括保持/归零），不是收到的冗余输入。
    using Observer = std::function<void(uint32_t, uint32_t, const WorldSnapshot &,
                                        const std::vector<InputCmd> &)>;
    Observer onStart, onFrame;

  private:
    struct Peer {
        // 身份、回包端点与有效活动时间随席位保留，支持凭证重连。
        Identity id;
        uint64_t endpoint = 0;
        double lastHeard = 0;
        double rateStart = 0;
        uint32_t request = 0;
        uint32_t sequence = 0;
        uint32_t helloSequence = 0;
        uint32_t rateCount = 0;
        uint32_t appliedTick = 0;
        bool hasSequence = false;
        bool hasApplied = false;
        bool recording = false;
        bool established = false;

        // 累计录制确认，以及缺帧保持所需的最后实际输入。
        uint32_t recordAck = 0;
        InputCmd applied;
        InputBuffer inputs{4096};
    };

    struct Room {
        // 房间各自持有席位、权威世界和录制缓冲，生命周期互不干扰。
        std::array<std::optional<Peer>, 2> peers;
        std::deque<Json> records;
        std::vector<InputCmd> commands{2};
        sim::World world{2};
        uint32_t match = 0;
        bool running = false;
        double emptySince = -1;
    };

    ServerConfig config_;
    std::mt19937_64 random_;
    uint64_t instance_;
    uint32_t nextMatch_ = 0;
    std::map<uint32_t, Room> rooms_;
    ServerStats stats_;

    uint64_t RandomId();
    Message BuildFullReply(const Room &room, const Peer &peer, Kind kind,
                           uint32_t request = 0) const;
    void TryStartRoom(uint32_t roomNumber, Room &room, std::vector<Datagram> &outgoing);
};
} // namespace lab::session
