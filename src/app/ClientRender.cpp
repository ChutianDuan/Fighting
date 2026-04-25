#include <lab/app/ClientRender.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <lab/app/InputPrediction.h>

namespace {
SDL_Texture* MakeText(SDL_Renderer* r, TTF_Font* f, const std::string& text, SDL_Color color, int& w, int& h) {
  SDL_Surface* surf = TTF_RenderText_Blended(f, text.c_str(), color);
  if (!surf) return nullptr;
  SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
  w = surf->w; h = surf->h;
  SDL_FreeSurface(surf);
  return tex;
}
} // namespace

namespace lab::app {

bool InitRenderer(RenderCtx& rc, const std::string& title, const std::string& fontPath, int fontSize) {
  rc.window = SDL_CreateWindow(title.c_str(),
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               rc.width, rc.height,
                               SDL_WINDOW_SHOWN);
  if (!rc.window) return false;
  rc.renderer = SDL_CreateRenderer(rc.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!rc.renderer) return false;
  rc.font = TTF_OpenFont(fontPath.c_str(), fontSize);
  return rc.font != nullptr;
}

void ShutdownRenderer(RenderCtx& rc) {
  if (rc.font) TTF_CloseFont(rc.font);
  rc.font = nullptr;
  if (rc.renderer) SDL_DestroyRenderer(rc.renderer);
  rc.renderer = nullptr;
  if (rc.window) SDL_DestroyWindow(rc.window);
  rc.window = nullptr;
}

void RenderFrame(RenderCtx& rc,
                 const WorldSnapshot& snap,
                 uint32_t rollbackCount,
                 uint32_t hashMismatchCount) {
  if (!rc.renderer) return;

  SDL_SetRenderDrawColor(rc.renderer, 20, 20, 30, 255);
  SDL_RenderClear(rc.renderer);

  const float mazeScale = (snap.mazeWidth > 0 && snap.mazeHeight > 0)
      ? std::min(rc.width / float(snap.mazeWidth), rc.height / float(snap.mazeHeight)) * 0.8f
      : 50.0f;
  const float scale = mazeScale;
  const float halfW = float(snap.mazeWidth) * 0.5f;
  const float halfH = float(snap.mazeHeight) * 0.5f;
  const float cellSize = 1.0f;

  auto toScreen = [&](float wx, float wy) {
    const float sx = rc.width * 0.5f + wx * scale;
    const float sy = rc.height * 0.5f - wy * scale;
    return std::pair<int, int>{int(std::round(sx)), int(std::round(sy))};
  };

  // Maze walls
  if (!snap.maze.empty() && snap.mazeWidth > 0 && snap.mazeHeight > 0) {
    SDL_SetRenderDrawColor(rc.renderer, 40, 50, 70, 255);
    for (uint32_t cy = 0; cy < snap.mazeHeight; ++cy) {
      for (uint32_t cx = 0; cx < snap.mazeWidth; ++cx) {
        const size_t idx = size_t(cy) * snap.mazeWidth + size_t(cx);
        if (idx >= snap.maze.size() || snap.maze[idx] == 0) continue;
        const float wx = (float(cx) + 0.5f - halfW) * cellSize;
        const float wy = (float(cy) + 0.5f - halfH) * cellSize;
        auto [sx, sy] = toScreen(wx, wy);
        SDL_Rect r{int(sx - scale * 0.5f), int(sy - scale * 0.5f),
                   int(scale), int(scale)};
        SDL_RenderFillRect(rc.renderer, &r);
      }
    }
  }

  for (size_t i = 0; i < snap.players.size(); ++i) {
    const auto& p = snap.players[i];
    auto [px, py] = toScreen(p.x, p.y);
    const int size = int(scale * 0.7f);
    SDL_Rect r{int(px - size / 2), int(py - size / 2), size, size};
    if (i == 0) SDL_SetRenderDrawColor(rc.renderer, 80, 180, 255, 255);
    else SDL_SetRenderDrawColor(rc.renderer, 255, 120, 120, 255);
    SDL_RenderFillRect(rc.renderer, &r);
  }

  SDL_SetRenderDrawColor(rc.renderer, 255, 220, 120, 255);
  for (const auto& pr : snap.projectiles) {
    if (!pr.alive) continue;
    auto [px, py] = toScreen(pr.x, pr.y);
    SDL_Rect r{int(px - scale * 0.15f), int(py - scale * 0.15f),
               int(scale * 0.3f), int(scale * 0.3f)};
    SDL_RenderFillRect(rc.renderer, &r);
  }

  if (rc.font) {
    auto drawText = [&](int x, int y, const std::string& text) {
      SDL_Color color{220, 220, 220, 255};
      int w=0, h=0;
      SDL_Texture* tex = MakeText(rc.renderer, rc.font, text, color, w, h);
      if (!tex) return;
      SDL_Rect dst{x, y, w, h};
      SDL_RenderCopy(rc.renderer, tex, nullptr, &dst);
      SDL_DestroyTexture(tex);
    };

    drawText(10, 10, "tick: " + std::to_string(snap.tick));
    drawText(10, 30, "rollbacks: " + std::to_string(rollbackCount));
    drawText(10, 50, "hash mismatch: " + std::to_string(hashMismatchCount));
    drawText(10, 70, "maze seed: " + std::to_string(snap.mazeSeed));

    for (size_t i = 0; i < snap.players.size(); ++i) {
      const auto& p = snap.players[i];
      std::string line = "P" + std::to_string(i + 1) +
                         " hp=" + std::to_string(p.hp) +
                         " act=" + ActionName(p.action) +
                         " t=" + std::to_string(p.stateTimer);
      drawText(10, 90 + int(i) * 20, line);
    }
  }

  SDL_RenderPresent(rc.renderer);
}

} // namespace lab::app
