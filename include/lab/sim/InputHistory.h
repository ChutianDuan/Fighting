#pragma once

#include <cstddef>
#include <optional>

#include <lab/sim/InputBuffer.h>
#include <lab/sim/InputCmd.h>

namespace lab::sim {

// Thin wrapper for semantic clarity; uses the shared InputBuffer implementation.
class InputHistory {
public:
  explicit InputHistory(size_t capacityTicks) : buf_(capacityTicks) {}

  void Put(const InputCmd& cmd) { buf_.Put(cmd); }
  std::optional<InputCmd> Get(Tick tick) const { return buf_.Get(tick); }

private:
  InputBuffer buf_;
};

} // namespace lab::sim
