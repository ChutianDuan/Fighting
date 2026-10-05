#pragma once
#include <fstream>
#include <lab/session/Protocol.h>
#include <lab/sim/World.h>

namespace lab::session {

// JSON Lines 录制：文件头保存完整初态，逐帧记录实际采用的输入，尾部确认完整写入。
class ReplayWriter {
  public:
    ReplayWriter(const std::string &path, const WorldSnapshot &initial, const Json &identity);
    void Frame(const WorldSnapshot &snapshot, const std::vector<InputCmd> &inputs);
    void Finish();
    ~ReplayWriter();

  private:
    std::ofstream file_;
    uint64_t count_ = 0;
    bool finished_ = false;
    Tick last_ = 0;
};

// 期望回放状态与实际模拟状态的首个差异；字段名使用稳定的路径表示。
struct Difference {
    Tick tick = 0;
    std::string field;
    Json expected, actual;
};
std::optional<Difference> FirstDifference(const WorldSnapshot &expected,
                                          const WorldSnapshot &actual);

// 完整校验文件结构后，按记录输入逐帧重建世界；首次分叉后停止推进。
class ReplayPlayer {
  public:
    explicit ReplayPlayer(const std::string &path);

    bool Step();
    void Restart();

    bool finished() const { return position_ == frames_.size(); }
    size_t frame_count() const { return frames_.size(); }
    WorldSnapshot Snapshot() const { return world_.Snapshot(); }
    const std::optional<Difference> &difference() const { return difference_; }

  private:
    WorldSnapshot initial_;
    sim::World world_{2};
    std::vector<Json> frames_;
    size_t position_ = 0;
    std::optional<Difference> difference_;
};

enum class ReplayAction { TogglePause, SingleStep, Restart, HalfSpeed, NormalSpeed, DoubleSpeed };

// SDL 将按键转换成动作；暂停/单步语义可在无窗口测试中验证。
class ReplayPlayback {
  public:
    explicit ReplayPlayback(ReplayPlayer &replay) : replay_(replay) {}

    void Action(ReplayAction action);
    void Advance(double elapsed);

    bool playing() const { return playing_; }
    double speed() const { return speed_; }

  private:
    ReplayPlayer &replay_;
    bool playing_ = true;
    double speed_ = 1, accumulator_ = 0;
};

} // namespace lab::session
