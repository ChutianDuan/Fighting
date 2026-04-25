#pragma once

#include <cstdint>

namespace lab::app {

struct GameConfig {
  static constexpr uint8_t kMaxPlayers = 2;
  static constexpr uint8_t kRequiredPlayers = 2;
  static constexpr uint32_t kStartDelayTicks = 30;

  static constexpr double kDt = 1.0 / 60.0;
  static constexpr double kMaxFrame = 0.25;
  static constexpr int kInputRedundancy = 4;
};

} // namespace lab::app
