#pragma once

#include <string>

#include <SDL.h>
#include <SDL_ttf.h>

#include <lab/sim/StateSnapshot.h>

namespace lab::app {

struct RenderCtx {
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  TTF_Font* font = nullptr;
  int width = 800;
  int height = 600;
};

struct NetworkStats {
  int targetLead=2;
  double displayDelayMs=100;
  std::string status,detail;
  double rttMs = 0.0;
  double packetLossPct = 0.0;
  int32_t inputLeadTicks = 0;
  int32_t stateDelayTicks = 0;
  double replayCostMs = 0.0;
  uint32_t replayTicks = 0;
  uint32_t inputPacketsReceived = 0;
  uint32_t inputPacketsLost = 0;
};

bool InitRenderer(RenderCtx& rc, const std::string& title, const std::string& fontPath, int fontSize);
void ShutdownRenderer(RenderCtx& rc);
void RenderFrame(RenderCtx& rc,
                 const WorldSnapshot& snap,
                 uint32_t rollbackCount,
                 uint32_t hashMismatchCount,
                 const NetworkStats* netStats = nullptr);

} // namespace lab::app
