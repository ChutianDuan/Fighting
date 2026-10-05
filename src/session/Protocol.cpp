#include <cmath>
#include <lab/session/Protocol.h>
#include <lab/sim/Hasher.h>
#include <stdexcept>

namespace lab::session {
namespace {
// v6 数据报：LAB0 魔数、2 字节版本、2 字节类型、4 字节大端 JSON 长度。
constexpr uint8_t kProtocolVersion = 6;
constexpr size_t kHeaderSize = 12;
constexpr size_t kBodyLengthOffset = 8;
constexpr size_t kBodyLengthBytes = 4;
constexpr uint16_t kFirstMessageKind = static_cast<uint16_t>(Kind::Hello);
constexpr uint16_t kLastMessageKind = static_cast<uint16_t>(Kind::End);

// 编解码共享边界，超限直接拒绝，不能截断后继续校验哈希。
constexpr size_t kMaxReasonLength = 128;
constexpr size_t kMaxInputsPerMessage = 8;
constexpr size_t kMaxRecordsPerMessage = 2;
constexpr int kMaxJsonDepth = 16;
constexpr uint64_t kInputButtonMask = 15;
constexpr size_t kOnlinePlayerCount = 2;
constexpr size_t kMaxSnapshotPlayers = 8;
constexpr size_t kMaxProjectiles = 255;
constexpr uint32_t kMaxMazeExtent = 64;
constexpr uint32_t kMaxMazeCells = 4096;
constexpr float kMaxFloatMagnitude = 10000.0f;
constexpr float kMillimetersPerMeter = 1000.0f;
constexpr uint64_t kMaxProjectileLifeTicks = 90;

uint64_t ReadUnsigned(const Json &json, const char *key, uint64_t maximum = UINT64_MAX) {
    const auto &value = json.at(key);
    if (!value.is_number_unsigned() && !value.is_number_integer())
        throw std::runtime_error("integer required");
    if (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0)
        throw std::runtime_error("negative integer");
    auto number = value.get<uint64_t>();
    if (number > maximum)
        throw std::runtime_error("integer range");
    return number;
}

int ReadBoundedInteger(const Json &json, const char *key, int minimum, int maximum) {
    const auto &value = json.at(key);
    if (!value.is_number_integer())
        throw std::runtime_error("integer required");
    auto number = value.get<int64_t>();
    if (number < minimum || number > maximum)
        throw std::runtime_error("integer range");
    return static_cast<int>(number);
}

float ReadFiniteFloat(const Json &json, const char *key) {
    const auto &value = json.at(key);
    if (!value.is_number())
        throw std::runtime_error("number required");
    auto number = value.get<float>();
    if (!std::isfinite(number) || std::fabs(number) > kMaxFloatMagnitude)
        throw std::runtime_error("non-finite/range");
    return number;
}
} // namespace

Json IdentityJson(const Identity &identity) {
    return {{"instance", identity.instance}, {"room", identity.room},
            {"match", identity.match},       {"session", identity.session},
            {"token", identity.token},       {"generation", identity.generation},
            {"player", identity.player}};
}

Identity ParseIdentity(const Json &json) {
    Identity identity;
    identity.instance = ReadUnsigned(json, "instance");
    identity.session = ReadUnsigned(json, "session");
    identity.token = ReadUnsigned(json, "token");
    identity.room = ReadUnsigned(json, "room", UINT32_MAX);
    identity.match = ReadUnsigned(json, "match", UINT32_MAX);
    identity.generation = ReadUnsigned(json, "generation", UINT32_MAX);
    identity.player = ReadUnsigned(json, "player", kOnlinePlayerCount);
    if (!identity.room)
        throw std::runtime_error("room zero");
    return identity;
}

Json InputJson(const InputCmd &input) {
    return {{"tick", input.tick},
            {"buttons", input.buttons},
            {"moveX", input.moveX},
            {"moveY", input.moveY}};
}

InputCmd ParseInput(const Json &json) {
    InputCmd input;
    input.tick = ReadUnsigned(json, "tick", UINT32_MAX);
    input.buttons = ReadUnsigned(json, "buttons", kInputButtonMask);
    input.moveX = ReadBoundedInteger(json, "moveX", -1, 1);
    input.moveY = ReadBoundedInteger(json, "moveY", -1, 1);
    return input;
}

Json SnapshotJson(const WorldSnapshot &snapshot, bool includeMap, bool quantize) {
    auto wireValue = [&](float value) {
        if (!std::isfinite(value) || std::fabs(value) > kMaxFloatMagnitude)
            throw std::runtime_error("non-finite/range");
        // 保持 float 乘法和毫米舍入口径，与 Hasher 一致；回放不做量化。
        return quantize ? static_cast<float>(std::lround(value * kMillimetersPerMeter)) /
                              kMillimetersPerMeter
                        : value;
    };
    Json json = {{"tick", snapshot.tick},           {"mazeSeed", snapshot.mazeSeed},
                 {"mazeWidth", snapshot.mazeWidth}, {"mazeHeight", snapshot.mazeHeight},
                 {"players", Json::array()},        {"projectiles", Json::array()}};
    if (includeMap)
        json["maze"] = snapshot.maze;
    json["players"].get_ref<Json::array_t &>().reserve(snapshot.players.size());
    json["projectiles"].get_ref<Json::array_t &>().reserve(snapshot.projectiles.size());
    for (const auto &player : snapshot.players)
        json["players"].push_back({{"x", wireValue(player.x)},
                                   {"v", wireValue(player.v)},
                                   {"y", wireValue(player.y)},
                                   {"vy", wireValue(player.vy)},
                                   {"facing", player.facing},
                                   {"hp", player.hp},
                                   {"action", static_cast<uint8_t>(player.action)},
                                   {"stateTimer", player.stateTimer},
                                   {"atkActive", player.atkActive},
                                   {"attackConnected", player.attackConnected},
                                   {"onGround", player.onGround},
                                   {"shotCooldown", player.shotCooldown},
                                   {"aimX", player.aimX},
                                   {"aimY", player.aimY}});
    for (const auto &projectile : snapshot.projectiles)
        json["projectiles"].push_back({{"x", wireValue(projectile.x)},
                                       {"y", wireValue(projectile.y)},
                                       {"vx", wireValue(projectile.vx)},
                                       {"vy", wireValue(projectile.vy)},
                                       {"alive", projectile.alive},
                                       {"life", projectile.life},
                                       {"owner", projectile.owner}});
    return json;
}

WorldSnapshot ParseSnapshot(const Json &json, const WorldSnapshot *mapSnapshot) {
    WorldSnapshot snapshot;
    snapshot.tick = ReadUnsigned(json, "tick", UINT32_MAX);
    snapshot.mazeSeed = ReadUnsigned(json, "mazeSeed", UINT32_MAX);
    snapshot.mazeWidth = ReadUnsigned(json, "mazeWidth", kMaxMazeExtent);
    snapshot.mazeHeight = ReadUnsigned(json, "mazeHeight", kMaxMazeExtent);
    if (!snapshot.mazeWidth || !snapshot.mazeHeight ||
        snapshot.mazeWidth * snapshot.mazeHeight > kMaxMazeCells)
        throw std::runtime_error("map dimensions");
    // 完整报文携带地图；回放动态记录仅在地图身份一致时复用初始快照的地图。
    if (json.contains("maze")) {
        const auto &mazeCells = json.at("maze");
        if (!mazeCells.is_array() || mazeCells.size() != snapshot.mazeWidth * snapshot.mazeHeight)
            throw std::runtime_error("map length");
        snapshot.maze.reserve(mazeCells.size());
        for (const auto &value : mazeCells) {
            if (!value.is_number_integer() || (value != 0 && value != 1))
                throw std::runtime_error("map cell");
            snapshot.maze.push_back(value.get<uint8_t>());
        }
    } else if (mapSnapshot && mapSnapshot->mazeWidth == snapshot.mazeWidth &&
               mapSnapshot->mazeHeight == snapshot.mazeHeight &&
               mapSnapshot->mazeSeed == snapshot.mazeSeed)
        snapshot.maze = mapSnapshot->maze;
    else
        throw std::runtime_error("missing map");
    const auto &players = json.at("players");
    const auto &projectiles = json.at("projectiles");
    if (!players.is_array() || players.empty() || players.size() > kMaxSnapshotPlayers ||
        !projectiles.is_array() || projectiles.size() > kMaxProjectiles)
        throw std::runtime_error("entity count");
    snapshot.players.reserve(players.size());
    snapshot.projectiles.reserve(projectiles.size());
    for (const auto &playerJson : players) {
        PlayerState player;
        player.x = ReadFiniteFloat(playerJson, "x");
        player.v = ReadFiniteFloat(playerJson, "v");
        player.y = ReadFiniteFloat(playerJson, "y");
        player.vy = ReadFiniteFloat(playerJson, "vy");
        if (std::fabs(player.x) > snapshot.mazeWidth / 2.0f ||
            std::fabs(player.y) > snapshot.mazeHeight / 2.0f)
            throw std::runtime_error("position outside map");
        player.facing = ReadUnsigned(playerJson, "facing", 3);
        player.hp = ReadBoundedInteger(playerJson, "hp", 0, 100);
        player.action = static_cast<Action>(ReadUnsigned(playerJson, "action", 2));
        player.stateTimer = ReadUnsigned(playerJson, "stateTimer", UINT8_MAX);
        player.atkActive = ReadUnsigned(playerJson, "atkActive", 1);
        player.attackConnected = ReadUnsigned(playerJson, "attackConnected", 1);
        player.onGround = ReadUnsigned(playerJson, "onGround", 1);
        player.shotCooldown = ReadUnsigned(playerJson, "shotCooldown", UINT8_MAX);
        player.aimX = ReadBoundedInteger(playerJson, "aimX", -1, 1);
        player.aimY = ReadBoundedInteger(playerJson, "aimY", -1, 1);
        if (!player.aimX && !player.aimY)
            throw std::runtime_error("zero aim");
        snapshot.players.push_back(player);
    }
    for (const auto &projectileJson : projectiles) {
        ProjectileState projectile;
        projectile.x = ReadFiniteFloat(projectileJson, "x");
        projectile.y = ReadFiniteFloat(projectileJson, "y");
        projectile.vx = ReadFiniteFloat(projectileJson, "vx");
        projectile.vy = ReadFiniteFloat(projectileJson, "vy");
        projectile.alive = ReadUnsigned(projectileJson, "alive", 1);
        projectile.life = ReadUnsigned(projectileJson, "life", kMaxProjectileLifeTicks);
        projectile.owner = ReadUnsigned(projectileJson, "owner", snapshot.players.size());
        if (!projectile.owner || !projectile.alive || !projectile.life)
            throw std::runtime_error("inactive projectile");
        snapshot.projectiles.push_back(projectile);
    }
    return snapshot;
}

Bytes Encode(const Message &message) {
    auto messageKind = static_cast<uint16_t>(message.kind);
    if (messageKind < kFirstMessageKind || messageKind > kLastMessageKind ||
        message.reason.size() > kMaxReasonLength || !message.records.is_array() ||
        message.records.size() > kMaxRecordsPerMessage)
        throw std::runtime_error("message type/record limit");
    if (message.inputs.size() > kMaxInputsPerMessage)
        throw std::runtime_error("too many inputs");
    Json json = {{"id", IdentityJson(message.id)}, {"request", message.request},
                 {"sequence", message.sequence},   {"nextTick", message.nextTick},
                 {"running", message.running},     {"record", message.record},
                 {"recordAck", message.recordAck}, {"records", message.records},
                 {"hash", message.hash},           {"reason", message.reason},
                 {"inputs", Json::array()}};
    json["inputs"].get_ref<Json::array_t &>().reserve(message.inputs.size());
    for (const auto &input : message.inputs) {
        auto encodedInput = InputJson(input);
        ParseInput(encodedInput);
        json["inputs"].push_back(std::move(encodedInput));
    }
    if (message.snapshot) {
        // 以接收端将看到的毫米状态校验哈希，避免仅验证未量化的本地对象。
        json["snapshot"] = SnapshotJson(*message.snapshot, true, true);
        auto checkedSnapshot = ParseSnapshot(json["snapshot"]);
        if (checkedSnapshot.players.size() != kOnlinePlayerCount ||
            message.nextTick != checkedSnapshot.tick + 1 ||
            Hasher::Hash(checkedSnapshot) != message.hash)
            throw std::runtime_error("snapshot hash/count");
    }
    if (message.rawInitial) {
        // 初始录制状态只随 Welcome 下发，保留浮点初值供离线重建权威世界。
        if (!message.record || message.kind != Kind::Welcome || !message.snapshot)
            throw std::runtime_error("raw initial context");
        json["rawInitial"] = SnapshotJson(*message.rawInitial);
        auto rawSnapshot = ParseSnapshot(json["rawInitial"]);
        if (rawSnapshot.players.size() != kOnlinePlayerCount ||
            rawSnapshot.tick != message.snapshot->tick || Hasher::Hash(rawSnapshot) != message.hash)
            throw std::runtime_error("raw initial hash/count");
    }
    // 逐帧录制必须绑定当前 State；每条实际应用输入与其原始快照属于同一帧。
    for (const auto &record : message.records) {
        if (!message.record || message.kind != Kind::State || !message.snapshot ||
            !record.at("inputs").is_array() || record.at("inputs").size() != kOnlinePlayerCount)
            throw std::runtime_error("record context/count");
        auto rawSnapshot = ParseSnapshot(record.at("snapshot"), &*message.snapshot);
        if (rawSnapshot.players.size() != kOnlinePlayerCount ||
            Hasher::Hash(rawSnapshot) != ReadUnsigned(record, "hash") ||
            Newer(rawSnapshot.tick, message.snapshot->tick))
            throw std::runtime_error("record hash/tick");
        for (const auto &input : record.at("inputs"))
            if (ParseInput(input).tick != rawSnapshot.tick)
                throw std::runtime_error("record input tick");
    }
    ParseIdentity(json["id"]);
    auto body = json.dump();
    if (body.size() + kHeaderSize > kMaxDatagram)
        throw std::runtime_error("datagram too large");
    Bytes bytes(body.size() + kHeaderSize);
    const uint8_t header[]{
        0x4c, 0x41, 0x42, 0x30, 0, kProtocolVersion, 0, static_cast<uint8_t>(message.kind)};
    std::copy(std::begin(header), std::end(header), bytes.begin());
    uint32_t bodyLength = body.size();
    for (size_t index = 0; index < kBodyLengthBytes; ++index)
        bytes[kBodyLengthOffset + index] =
            static_cast<uint8_t>(bodyLength >> ((kBodyLengthBytes - 1 - index) * 8));
    std::copy(body.begin(), body.end(), bytes.begin() + kHeaderSize);
    return bytes;
}

std::optional<Message> Decode(std::span<const uint8_t> bytes) {
    try {
        // 先验证报头和实际长度，再解析 JSON；所有返回路径都不提交部分报文。
        if (bytes.size() < kHeaderSize || bytes.size() > kMaxDatagram || bytes[0] != 0x4c ||
            bytes[1] != 0x41 || bytes[2] != 0x42 || bytes[3] != 0x30 || bytes[4] != 0 ||
            bytes[5] != kProtocolVersion || bytes[6] != 0 || bytes[7] < kFirstMessageKind ||
            bytes[7] > kLastMessageKind)
            return {};
        uint32_t bodyLength = 0;
        for (size_t index = kBodyLengthOffset; index < kHeaderSize; ++index)
            bodyLength = (bodyLength << 8) | bytes[index];
        if (bodyLength != bytes.size() - kHeaderSize)
            return {};
        auto json = Json::parse(bytes.begin() + kHeaderSize, bytes.end(),
                                [](int depth, Json::parse_event_t, const Json &) {
                                    if (depth > kMaxJsonDepth)
                                        throw std::runtime_error("JSON nesting limit");
                                    return true;
                                });
        Message message;
        message.kind = static_cast<Kind>(bytes[7]);
        message.id = ParseIdentity(json.at("id"));
        message.request = ReadUnsigned(json, "request", UINT32_MAX);
        message.sequence = ReadUnsigned(json, "sequence", UINT32_MAX);
        message.nextTick = ReadUnsigned(json, "nextTick", UINT32_MAX);
        if (!json.at("running").is_boolean())
            return {};
        message.running = json.at("running").get<bool>();
        if (!json.at("record").is_boolean() || !json.at("records").is_array() ||
            json.at("records").size() > kMaxRecordsPerMessage)
            return {};
        message.record = json.at("record").get<bool>();
        message.recordAck = ReadUnsigned(json, "recordAck", UINT32_MAX);
        message.records = json.at("records");
        message.hash = ReadUnsigned(json, "hash");
        message.reason = json.at("reason").get<std::string>();
        if (message.reason.size() > kMaxReasonLength || !json.at("inputs").is_array() ||
            json.at("inputs").size() > kMaxInputsPerMessage)
            return {};
        message.inputs.reserve(json.at("inputs").size());
        for (const auto &inputJson : json.at("inputs"))
            message.inputs.push_back(ParseInput(inputJson));
        if (json.contains("snapshot")) {
            message.snapshot = ParseSnapshot(json.at("snapshot"));
            if (message.snapshot->players.size() != kOnlinePlayerCount ||
                Hasher::Hash(*message.snapshot) != message.hash ||
                message.nextTick != message.snapshot->tick + 1)
                return {};
        }
        if (json.contains("rawInitial")) {
            if (message.kind != Kind::Welcome || !message.record || !message.snapshot)
                return {};
            message.rawInitial = ParseSnapshot(json.at("rawInitial"));
            if (message.rawInitial->players.size() != kOnlinePlayerCount ||
                message.rawInitial->tick != message.snapshot->tick ||
                Hasher::Hash(*message.rawInitial) != message.hash)
                return {};
        }
        for (const auto &record : message.records) {
            if (!message.record || message.kind != Kind::State || !message.snapshot ||
                !record.at("inputs").is_array() || record.at("inputs").size() != kOnlinePlayerCount)
                return {};
            auto rawSnapshot = ParseSnapshot(record.at("snapshot"), &*message.snapshot);
            if (rawSnapshot.players.size() != kOnlinePlayerCount ||
                Newer(rawSnapshot.tick, message.snapshot->tick) ||
                Hasher::Hash(rawSnapshot) != ReadUnsigned(record, "hash"))
                return {};
            for (const auto &input : record.at("inputs"))
                if (ParseInput(input).tick != rawSnapshot.tick)
                    return {};
        }
        if ((message.kind == Kind::Welcome || message.kind == Kind::State ||
             message.kind == Kind::Sync) &&
            !message.snapshot)
            return {};
        if (message.kind == Kind::Input && message.inputs.empty())
            return {};
        return message;
    } catch (const std::exception &) {
        return {};
    }
}
} // namespace lab::session
