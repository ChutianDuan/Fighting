#pragma once
#include <vector>
#include <optional>
#include <cstddef>

#include <lab/sim/World.h>     // 里边有 WorldSnapshot
#include <lab/sim/InputCmd.h>

namespace lab::sim {

class StateHistory {
public:
  explicit StateHistory(size_t capacityTicks)
    : cap_(capacityTicks == 0 ? 1 : capacityTicks), ring_(cap_) {}

  void Put(const WorldSnapshot& s) {
    const size_t idx = size_t(s.tick % cap_);
    ring_[idx].valid = true;
    ring_[idx].tick = s.tick;
    ring_[idx].snap = s;
  }

  std::optional<WorldSnapshot> Get(Tick tick) const {
    const size_t idx = size_t(tick % cap_);
    if (!ring_[idx].valid) return std::nullopt;
    if (ring_[idx].tick != tick) return std::nullopt;
    return ring_[idx].snap;
  }

private:
  struct Slot {
    bool valid = false;
    Tick tick = 0;
    WorldSnapshot snap{};
  };

  size_t cap_;
  std::vector<Slot> ring_;
};

} // namespace lab::sim
