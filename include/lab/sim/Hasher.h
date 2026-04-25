#pragma once

#include <cstddef>
#include <cstdint>

#include "lab/sim/StateSnapshot.h"

class Hasher {
public:
  // 返回 64-bit hash（用于比较两次运行是否一致）
  static uint64_t Hash(const WorldSnapshot& s);
};
