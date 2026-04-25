#pragma once
#include <lab/sim/InputCmd.h>
#include <vector>
#include <optional>

class InputBuffer {
public:
  explicit InputBuffer(size_t capacityTicks);

  // 写入某 tick 的输入（本地采样/网络收到都走这里）
  void Put(const InputCmd& cmd);

  // 获取某 tick 的输入；如果没有，返回 nullopt
  std::optional<InputCmd> Get(Tick tick) const;

  // 给当前 tick 生成默认输入（缺失时使用）
  static InputCmd DefaultForTick(Tick tick);

private:
  struct Slot {
    bool valid = false;
    InputCmd cmd{};
  };

  size_t cap_;
  std::vector<Slot> ring_;
};