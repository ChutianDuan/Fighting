#pragma once
#include <lab/sim/InputCmd.h>
#include <vector>
#include <optional>

// 按 tick 索引的有界输入历史；同一环槽被新帧覆盖后，旧帧不可再读取。
class InputBuffer {
public:
  explicit InputBuffer(size_t capacityTicks);

  // 写入某 tick 的输入（本地采样/网络收到都走这里）
  void Put(const InputCmd& cmd);

  // 未写入或已覆盖均返回 nullopt，由调用方选择保持旧输入或使用空输入。
  std::optional<InputCmd> Get(Tick tick) const;

  // 给当前 tick 生成默认输入（缺失时使用）
  static InputCmd DefaultForTick(Tick tick);

  size_t Capacity() const { return cap_; }

private:
  struct Slot {
    bool valid = false;
    InputCmd cmd{};
  };

  size_t cap_;
  std::vector<Slot> ring_;
};
