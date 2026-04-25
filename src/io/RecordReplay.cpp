#include "lab/io/RecordReplay.h"
#include "lab/sim/InputBuffer.h"

void RecordReplay::StartRecord() {
  mode_ = Mode::Record;
  recorded_.clear();
}

void RecordReplay::StartReplay() {
  mode_ = Mode::Replay;
}

void RecordReplay::PushRecorded(const InputCmd& cmd) {
  if (mode_ != Mode::Record) return;
  recorded_.push_back(cmd);
}

InputCmd RecordReplay::GetReplayOrDefault(Tick tick) const {
  if (mode_ != Mode::Replay) {
    return InputBuffer::DefaultForTick(tick);
  }
  if (tick < recorded_.size()) {
    return recorded_[tick]; // 假设从 tick=0 连续录制
  }
  return InputBuffer::DefaultForTick(tick);
}
