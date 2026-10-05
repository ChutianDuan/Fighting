#pragma once

#include <cmath>

#include <lab/sim/StateSnapshot.h>

namespace lab::sim {

// Resolve basic pushbox overlap by nudging players apart.
inline void ResolvePushbox(PlayerState& a, PlayerState& b, float radius = 0.5f) {
    const float minDistance = radius * 2.0f;
    if (minDistance <= 0.0f) return;

    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float distSq = dx * dx + dy * dy;
    if (distSq <= 1e-6f) {
        const float jitter = 0.01f;
        a.x += jitter;
        b.x -= jitter;
        return;
    }

    const float minDistSq = minDistance * minDistance;
    if (distSq >= minDistSq) return;

    const float dist = std::sqrt(distSq);
    const float overlap = minDistance - dist;
    const float nx = dx / dist;
    const float ny = dy / dist;
    const float shift = overlap * 0.5f;

    a.x += nx * shift;
    a.y += ny * shift;
    b.x -= nx * shift;
    b.y -= ny * shift;
}

} // namespace lab::sim
