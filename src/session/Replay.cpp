#include <lab/session/Replay.h>
#include <lab/sim/Hasher.h>
#include <stdexcept>

namespace lab::session {
namespace {
constexpr int kReplayFormatVersion = 1;
constexpr int kReplayProtocolVersion = 6;
constexpr int kSimulationVersion = 1;
constexpr size_t kMaxRecordBytes = 100000;
constexpr double kMaxPlaybackElapsed = .25;
} // namespace

ReplayWriter::ReplayWriter(const std::string &path, const WorldSnapshot &initial,
                           const Json &identity)
    : file_(path), last_(initial.tick) {
    if (!file_)
        throw std::runtime_error("cannot create replay: " + path);
    file_ << Json{{"type", "header"},
                  {"format", kReplayFormatVersion},
                  {"protocol", kReplayProtocolVersion},
                  {"simulation", kSimulationVersion},
                  {"step", kStep},
                  {"identity", identity},
                  {"initial", SnapshotJson(initial)}}
                 .dump()
          << '\n';
}

void ReplayWriter::Frame(const WorldSnapshot &snapshot, const std::vector<InputCmd> &inputs) {
    if (finished_ || snapshot.tick != last_ + 1 || inputs.size() != snapshot.players.size())
        throw std::runtime_error("record frame sequence/count");
    Json recordedInputs = Json::array();
    for (const auto &input : inputs) {
        if (input.tick != snapshot.tick)
            throw std::runtime_error("input frame");
        recordedInputs.push_back(InputJson(input));
    }
    // 地图只写入文件头；逐帧保留原始浮点状态，供精确重建与差异诊断。
    file_ << Json{{"type", "frame"},
                  {"inputs", recordedInputs},
                  {"snapshot", SnapshotJson(snapshot, false)},
                  {"hash", Hasher::Hash(snapshot)}}
                 .dump()
          << '\n';
    if (!file_)
        throw std::runtime_error("replay write failure");
    last_ = snapshot.tick;
    ++count_;
}

void ReplayWriter::Finish() {
    if (!finished_) {
        file_ << Json{{"type", "end"}, {"frames", count_}, {"lastTick", last_}}.dump() << '\n';
        file_.flush();
        if (!file_)
            throw std::runtime_error("replay flush failure");
        finished_ = true;
    }
}

ReplayWriter::~ReplayWriter() {
    if (!finished_) {
        try {
            Finish();
        } catch (...) {
        }
    }
}

std::optional<Difference> FirstDifference(const WorldSnapshot &expected,
                                          const WorldSnapshot &actual) {
    const auto expectedJson = SnapshotJson(expected), actualJson = SnapshotJson(actual);
    auto compareField = [&](const Json &expectedValue, const Json &actualValue,
                            const std::string &path) -> std::optional<Difference> {
        if (expectedValue != actualValue)
            return Difference{expected.tick, path, expectedValue, actualValue};
        return {};
    };
    if (auto difference = compareField(expected.tick, actual.tick, "tick"))
        return difference;
    if (auto difference =
            compareField(expected.players.size(), actual.players.size(), "players.size"))
        return difference;
    // 精确比较原始字段，先定位差异再检查哈希；字段顺序固定，不依赖 JSON 字典顺序。
    for (size_t i = 0; i < expected.players.size(); ++i) {
        for (const char *key :
             {"x", "v", "y", "vy", "facing", "hp", "action", "stateTimer", "atkActive",
              "attackConnected", "onGround", "shotCooldown", "aimX", "aimY"}) {
            if (auto difference =
                    compareField(expectedJson["players"][i][key], actualJson["players"][i][key],
                                 "players[" + std::to_string(i) + "]." + key))
                return difference;
        }
    }
    if (auto difference = compareField(expected.projectiles.size(), actual.projectiles.size(),
                                       "projectiles.size"))
        return difference;
    for (size_t i = 0; i < expected.projectiles.size(); ++i) {
        for (const char *key : {"x", "y", "vx", "vy", "alive", "life", "owner"}) {
            if (auto difference = compareField(expectedJson["projectiles"][i][key],
                                               actualJson["projectiles"][i][key],
                                               "projectiles[" + std::to_string(i) + "]." + key))
                return difference;
        }
    }
    for (const char *key : {"mazeSeed", "mazeWidth", "mazeHeight", "maze"})
        if (auto difference = compareField(expectedJson[key], actualJson[key], key))
            return difference;
    return {};
}

ReplayPlayer::ReplayPlayer(const std::string &path) {
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("cannot open replay: " + path);
    std::string line;
    size_t lineNumber = 0;
    bool footerSeen = false;
    try {
        // 先校验版本与完整初始状态，后续动态快照从这里取得地图。
        if (!std::getline(file, line) || line.size() > kMaxRecordBytes)
            throw std::runtime_error("missing header");
        ++lineNumber;
        auto header = Json::parse(line);
        if (header.at("type") != "header" || header.at("format") != kReplayFormatVersion ||
            header.at("protocol") != kReplayProtocolVersion ||
            header.at("simulation") != kSimulationVersion || header.at("step") != kStep)
            throw std::runtime_error("unsupported replay version/step");
        initial_ = ParseSnapshot(header.at("initial"));
        Tick lastRecordedTick = initial_.tick;

        while (std::getline(file, line)) {
            ++lineNumber;
            if (line.size() > kMaxRecordBytes || footerSeen)
                throw std::runtime_error("oversize/trailing record");
            auto record = Json::parse(line);
            if (record.at("type") == "end") {
                if (record.at("frames") != frames_.size() ||
                    record.at("lastTick") != lastRecordedTick)
                    throw std::runtime_error("invalid footer");
                footerSeen = true;
                continue;
            }
            if (record.at("type") != "frame" || !record.at("inputs").is_array() ||
                record.at("inputs").size() != initial_.players.size())
                throw std::runtime_error("invalid frame");
            auto snapshot = ParseSnapshot(record.at("snapshot"), &initial_);
            if (snapshot.tick != ++lastRecordedTick)
                throw std::runtime_error("nonconsecutive tick");
            for (auto &input : record.at("inputs"))
                if (ParseInput(input).tick != lastRecordedTick)
                    throw std::runtime_error("input tick mismatch");
            if (!record.at("hash").is_number_unsigned())
                throw std::runtime_error("invalid hash");
            frames_.push_back(std::move(record));
        }
        // EOF 本身不能证明录制完整：必须有匹配的尾部计数，且不得出现后续记录。
        if (!footerSeen || file.bad())
            throw std::runtime_error("truncated replay (missing footer)");
    } catch (const std::exception &e) {
        throw std::runtime_error("replay line " + std::to_string(lineNumber) + ": " + e.what());
    }
    Restart();
}

void ReplayPlayer::Restart() {
    world_.Restore(initial_);
    position_ = 0;
    difference_.reset();
}

bool ReplayPlayer::Step() {
    if (finished() || difference_)
        return false;
    const auto &record = frames_[position_];
    std::vector<InputCmd> inputs;
    for (const auto &input : record.at("inputs"))
        inputs.push_back(ParseInput(input));
    world_.Step(inputs, static_cast<float>(kStep));
    auto expected = ParseSnapshot(record.at("snapshot"), &initial_), actual = world_.Snapshot();
    difference_ = FirstDifference(expected, actual);
    if (!difference_ && record.at("hash") != Hasher::Hash(actual))
        difference_ = Difference{actual.tick, "hash", record.at("hash"), Hasher::Hash(actual)};
    ++position_;
    return !difference_;
}

void ReplayPlayback::Action(ReplayAction action) {
    switch (action) {
    case ReplayAction::TogglePause:
        playing_ = !playing_;
        // 丢弃暂停前的不足一帧时间，恢复播放时不补推进。
        accumulator_ = 0;
        break;
    case ReplayAction::SingleStep:
        // 单步直接推进一次权威模拟，仅在暂停时生效，不累计播放时间。
        if (!playing_)
            replay_.Step();
        break;
    case ReplayAction::Restart:
        replay_.Restart();
        accumulator_ = 0;
        break;
    case ReplayAction::HalfSpeed:
        speed_ = .5;
        break;
    case ReplayAction::NormalSpeed:
        speed_ = 1;
        break;
    case ReplayAction::DoubleSpeed:
        speed_ = 2;
        break;
    }
}

void ReplayPlayback::Advance(double elapsed) {
    if (playing_ && !replay_.difference()) {
        accumulator_ += std::clamp(elapsed, 0.0, kMaxPlaybackElapsed) * speed_;
        while (accumulator_ >= kStep && !replay_.finished() && !replay_.difference()) {
            replay_.Step();
            accumulator_ -= kStep;
        }
    }
    if (replay_.finished() || replay_.difference())
        playing_ = false;
}

} // namespace lab::session
