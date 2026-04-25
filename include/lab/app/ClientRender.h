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

bool InitRenderer(RenderCtx& rc, const std::string& title, const std::string& fontPath, int fontSize);
void ShutdownRenderer(RenderCtx& rc);
void RenderFrame(RenderCtx& rc,
                 const WorldSnapshot& snap,
                 uint32_t rollbackCount,
                 uint32_t hashMismatchCount);

} // namespace lab::app
