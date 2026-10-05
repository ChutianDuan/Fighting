#pragma once

#include <cstdint>

namespace lab::app {

// 客户端编译期参数；服务端仍有自己的人数、首帧和状态发送间隔常量。
struct GameConfig {
  static constexpr uint8_t kMaxPlayers = 2;
  static constexpr uint8_t kRequiredPlayers = 2;
  static constexpr uint32_t kStartDelayTicks = 30; // 保留的首帧偏移；客户端实际采用 Start.startTick

  static constexpr double kDt = 1.0 / 60.0;
  static constexpr double kMaxFrame = 0.25;
  static constexpr int kInputRedundancy = 4;
};

} // namespace lab::app
