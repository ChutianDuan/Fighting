#include <lab/app/InputPrediction.h>

#include <cmath>

namespace lab::app {

int8_t PredictMoveXFromState(const lab::net::PackedPlayerState& ps, int8_t lastMoveX) {
  if (ps.action == 2) return 0; // Hitstun

  constexpr float eps = 0.02f;
  constexpr float stopEps = 0.005f;
  const float vx = float(ps.v_mm) / 1000.0f;
  if (std::fabs(vx) <= stopEps) return 0;
  if (vx > eps) return +1;
  if (vx < -eps) return -1;
  if (lastMoveX != 0) return lastMoveX;
  return 0;
}

int8_t PredictMoveYFromState(const lab::net::PackedPlayerState& ps, int8_t lastMoveY) {
  if (ps.action == 2) return 0; // Hitstun

  constexpr float eps = 0.02f;
  constexpr float stopEps = 0.005f;
  const float vy = float(ps.vy_mm) / 1000.0f;
  if (std::fabs(vy) <= stopEps) return 0;
  if (vy > eps) return +1;
  if (vy < -eps) return -1;
  if (lastMoveY != 0) return lastMoveY;
  return 0;
}

const char* ActionName(Action a) {
  switch (a) {
    case Action::Idle: return "Idle";
    case Action::Attack: return "Atk";
    case Action::Hitstun: return "Hit";
  }
  return "?";
}

} // namespace lab::app
