#pragma once
#include "lab/sim/InputCmd.h"
#include <vector>

// 仅在内存中保存输入的教学辅助，不提供回放文件或完整对局状态持久化。
class RecordReplay {
public:
  void StartRecord();
  void StartReplay();

  bool IsRecording() const { return mode_ == Mode::Record; }
  bool IsReplaying() const { return mode_ == Mode::Replay; }

  void PushRecorded(const InputCmd& cmd);

  // tick 直接作为数组下标，要求从 0 连续录制；非回放模式或越界返回空输入。
  InputCmd GetReplayOrDefault(Tick tick) const;

  // 录制数据暴露给外部做二次运行比较（可选）
  const std::vector<InputCmd>& Recorded() const { return recorded_; }

private:
  enum class Mode { Idle, Record, Replay };
  Mode mode_ = Mode::Idle;
  std::vector<InputCmd> recorded_;
};
