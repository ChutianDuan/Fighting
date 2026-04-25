#pragma once

#include <cstdint>

#include <lab/net/Packets.h>
#include <lab/sim/StateSnapshot.h>

namespace lab::app {

int8_t PredictMoveXFromState(const lab::net::PackedPlayerState& ps, int8_t lastMoveX);
int8_t PredictMoveYFromState(const lab::net::PackedPlayerState& ps, int8_t lastMoveY);
const char* ActionName(Action a);

} // namespace lab::app
