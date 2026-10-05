#pragma once

#include <cstdint>

#include <lab/net/Packets.h>
#include <lab/sim/StateSnapshot.h>

namespace lab::app {

// 从权威速度推断移动意图；低速区沿用上次方向，无法还原真实按键或射击输入。
int8_t PredictMoveXFromState(const lab::net::PackedPlayerState& ps, int8_t lastMoveX);
int8_t PredictMoveYFromState(const lab::net::PackedPlayerState& ps, int8_t lastMoveY);
const char* ActionName(Action a);

} // namespace lab::app
