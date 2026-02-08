#pragma once
#include <cstdint>

struct Particle {
    float x, y;      // position
    float dx, dy;    // velocity (delta per frame)
    uint8_t heat;    // how hot (affects color when deposited)
    uint32_t color;  // RGB color (0x00RRGGBB), for future multi-color support
    bool active;     // is this particle alive?
    int leaderIdx;   // -1 = no leader, 0+ = index into leader array (future use)
};
