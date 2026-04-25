#pragma once
#include "lab/sim/InputCmd.h"
#include <vector>

class RecordReplay {
public:
  void StartRecord();
  void StartReplay();

  bool IsRecording() const { return mode_ == Mode::Record; }
  bool IsReplaying() const { return mode_ == Mode::Replay; }

  void PushRecorded(const InputCmd& cmd);

  // 回放：按 tick 取输入；如果不足返回默认
  InputCmd GetReplayOrDefault(Tick tick) const;

  // 录制数据暴露给外部做二次运行比较（可选）
  const std::vector<InputCmd>& Recorded() const { return recorded_; }

private:
  enum class Mode { Idle, Record, Replay };
  Mode mode_ = Mode::Idle;
  std::vector<InputCmd> recorded_;
};
