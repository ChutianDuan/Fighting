#pragma once
#include <bit>
#include <lab/sim/StateSnapshot.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>

namespace lab::session {
using Json = nlohmann::json;
using Bytes = std::vector<uint8_t>;

// 在线模拟固定为 60 Hz；校正只允许有限重放，超过预算后请求完整状态。
constexpr double kStep = 1.0 / 60.0;
constexpr size_t kMaxDatagram = 16384;
constexpr uint32_t kReplayBudget = 120;

// 半个 uint32 范围内比较，帧号与请求/输入序号都允许回绕。
inline int32_t Distance(uint32_t a, uint32_t b) { return std::bit_cast<int32_t>(a - b); }
inline bool Newer(uint32_t a, uint32_t b) { return Distance(a, b) > 0; }

enum class Kind : uint16_t { Hello = 100, Welcome, Input, State, Resync, Sync, Reject, End };

struct Identity {
    uint64_t instance = 0;
    uint64_t session = 0;
    uint64_t token = 0; // 重连凭证；允许地址变化，不能用地址代替会话身份。
    uint32_t room = 1;
    uint32_t match = 0;
    uint32_t generation = 0; // 重连后递增，使旧连接的玩法包失效。
    uint32_t player = 0;
    bool operator==(const Identity &) const = default;
};

struct Message {
    Kind kind = Kind::Hello;
    Identity id;
    uint32_t request = 0; // 控制回复关联请求，重复请求保持幂等。
    uint32_t sequence = 0;
    uint32_t nextTick = 1; // 快照表示已完成帧，下一输入帧为 snapshot.tick + 1。
    bool running = false;
    bool record = false;
    uint32_t recordAck = 0;
    Json records = Json::array(); // 录制使用服务端实际采用的输入及原始浮点状态。
    std::optional<WorldSnapshot> rawInitial;
    std::vector<InputCmd> inputs; // 玩法包可冗余携带多个本地输入帧。
    std::optional<WorldSnapshot> snapshot;
    uint64_t hash = 0;
    std::string reason;
};

// 网络状态量化到毫米；回放保留原始浮点值，地图可从文件头的完整快照复用。
Json SnapshotJson(const WorldSnapshot &snapshot, bool includeMap = true, bool quantize = false);
WorldSnapshot ParseSnapshot(const Json &json, const WorldSnapshot *mapSnapshot = nullptr);
Json InputJson(const InputCmd &input);
InputCmd ParseInput(const Json &json);
Json IdentityJson(const Identity &id);
Identity ParseIdentity(const Json &json);

// 编码非法状态会抛错；解码完整校验后才返回报文，失败时不暴露部分状态。
Bytes Encode(const Message &message);
std::optional<Message> Decode(std::span<const uint8_t> bytes);
} // namespace lab::session
