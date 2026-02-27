// ptoy-remake.cpp - minimal Win32 software backbuffer (GDI DIBSection)
// Now with a classic "heat buffer" fire sim + palette rendering.
// Left-click injects heat (a "dot of fire") into the heat buffer.

#include <windows.h>
#include <cstdint>
#include <cmath>
#include <random>
#include <vector>
#include <windowsx.h>
#include <intrin.h>
#include <immintrin.h>

#include "particle.h"
#include "Resource.h"

// ------------------------------------------------------------
// Backbuffer (what we DISPLAY): 32-bit pixels (0x00RRGGBB)
// Heat buffer (what we SIMULATE): 8-bit intensity (0..255)
// ------------------------------------------------------------

static int gW = 1600;
static int gH = 1200;

static const int MAX_PARTICLES = 100000;

// Resolution presets: { width, height, dropdown label }
// Comments match the original ptoy notes where applicable.
struct ResPreset { int w, h; const wchar_t* label; };
static const ResPreset RES_PRESETS[] = {
    {  320,  240, L"320 x 240  (Too Fast!)"             },
    {  640,  480, L"640 x 480  (Yummy)"                 },
    {  800,  600, L"800 x 600  (Getting There)"         },
    { 1024,  768, L"1024 x 768  (CPU for Dinner)"       },
    { 1152,  864, L"1152 x 864  (Classic Desktop)"      },
    { 1280, 1024, L"1280 x 1024  (Whoah There)"         },
    { 1600, 1200, L"1600 x 1200  (Go For It All!)"      },
    { 1920, 1080, L"1920 x 1080  (HD Baby)"             },
    { 2048, 1536, L"2048 x 1536  (Biiiig 4:3)"          },
    { 2560, 1440, L"2560 x 1440  (John's Desktop)"      },
    { 3440, 1440, L"3440 x 1440  (Ultrawide)"           },
    { 3840, 2160, L"3840 x 2160  (4K!!!)"               },
    { 3840, 2160, L"4000 x 4000  (Cover all monitors)"  },
};
static const int NUM_RES_PRESETS = _countof(RES_PRESETS);
static int currentResPreset = 6;  // Default: 1600 x 1200

static BITMAPINFO gBmi = {};
static void* gPixels = nullptr;          // raw pointer returned by CreateDIBSection
static uint32_t* pixelMem = nullptr;     // typed view of gPixels (uint32 per pixel)
static HBITMAP   sDibSection = nullptr;  // DIBSection handle kept alive; file-scope so ChangeResolution can free it

static uint8_t* gHeat = nullptr;         // simulation buffer: 1 byte per pixel
static uint32_t gPalette[256] = {};      // palette[heat] -> 0x00RRGGBB

// RNG (useful later for "sparklies" etc. - not required for basic fire)
static std::mt19937 rng{ std::random_device{}() };
static std::uniform_int_distribution<int> dist(0, 255);
static std::uniform_real_distribution<float> angleDist(0.0f, 6.283185f);  // 0 to 2*PI
static std::uniform_real_distribution<float> speedVariance(0.85f, 1.15f); // ±15% speed variance
static std::uniform_real_distribution<float> chance(0.0f, 1.0f);          // for percentage rolls

// Speed system
// TODO: Add original speed toggle and speed multiplier slider to the dialog box.
static const float ORIGINAL_BASE_SPEED = 8.0f;         // original's ~8 px/frame at any resolution
static const float DEFAULT_SPEED_FACTOR = 0.01f;        // resolution-independent default
static float PARTICLE_SPEED_FACTOR = 0.01f;             // active speed factor (recalculated)
static float userSpeedMultiplier = 0.5f;                 // user-adjustable slider
static bool useOriginalSpeeds = true;                    // match original's absolute pixel speeds

// Recalculate speed factor based on mode and current resolution
static void UpdateSpeedFactor()
{
    if (useOriginalSpeeds)
        PARTICLE_SPEED_FACTOR = ORIGINAL_BASE_SPEED / (gW * userSpeedMultiplier);
    else
        PARTICLE_SPEED_FACTOR = DEFAULT_SPEED_FACTOR;
}

// Particle system
static std::vector<Particle> particles;
static const int INITIAL_PARTICLE_RESERVE = 2000;

// Mouse coordinates stored in WINDOW client space (not buffer space)
static int gMouseX = 1;
static int gMouseY = 1;
static bool mouseDown = false;
static bool rightMouseDown = false;

// Control panel dialog
static HWND gControlPanel = nullptr;

// Fire tuning
static const int BORDER_MARGIN = 1;   // skip 1-pixel border to avoid bounds issues
static const int BURNFADE = 6;   // how fast heat decays (bigger = faster fade)
static int particleSize = 2;          // deposit size in pixels (for future UI control)

// SIMD toggle - set to true to use SSE2 diffusion, false for scalar
// TODO: Add this into the dialog box.
static bool useSIMD = false;

// Gravity
static bool  useGravity = true;
static float gravityX   = 0.0f;
static float gravityY   = 0.1f;   // downward, matching original's default direction
static int   gGravityState = 0;   // 0=down, 1=left, 2=up, 3=right (for cycling)

// Follow Leader / Multiple Leaders
// When useFollowLeader is off: all particles steer to cursor (current behavior)
// When on + useMultiLeader off: particle 0 is leader (targets cursor), rest target particle 0
// When on + useMultiLeader on:  every 64th particle leads (targets cursor), others follow their group leader
static bool useFollowLeader = true;
static bool useMultiLeader  = true;

// Fallback angle: slowly rotates each frame, gives stationary particles a direction to start from
// Matching original's _DAT_004100c0 that increments 0.01 radians/frame, wraps at ±PI
static float gFallbackAngle = 0.0f;

// Random Events (auto-mode)
// 5% chance per second of a random event: freeze, explosion, comet, emit-to-center, gravity change
static bool   useRandEvents      = true;
static time_t gLastRandEventSec  = 0;   // wall-clock second of last event check

// ------------------------------------------------------------
// 2D value noise matching original's PerlinNoise (FUN_00404550)
//
// X = horizontal position across the bottom row (fixed per pixel)
// Y = time phase, increments each frame — this is the "scroll" dimension
//
// Key differences from standard Perlin gradient noise:
//   - Value noise: gradient table holds scalar values, not direction vectors
//   - Smoothstep applied to Y only; X uses plain linear interpolation
//   - Octave X progression: (coordX + 7) * 2  (shift prevents octave correlation)
//   - Amplitude table and output scale confirmed from original binary
// ------------------------------------------------------------
static uint8_t gPerlinPerm[512];     // permutation table (doubled for wrapping)
static double  gPerlinGrad[256];     // value noise table: random doubles in [0, 1]
static double  gPerlinY = 0.0;       // Y phase, incremented each frame for animation
static bool    usePerlinFire = false; // toggle: Perlin vs random-walk bottom row

// Confirmed from original binary (FUN_00404550 / DAT_0040f4d8 / DAT_0040f510)
static const double kPerlinAmp[7] = {
    1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625
};
static const double kPerlinScale[7] = {
    1.0,
    0.6666666667,   // 1/1.5
    0.5714285714,   // 1/1.75
    0.5333333333,   // 1/1.875
    0.5161290323,   // 1/1.9375
    0.5081300813,   // 1/1.96875
    0.5040322581,   // 1/1.984375
};

static void InitPerlinTable()
{
    uint8_t p[256];
    for (int i = 0; i < 256; i++) p[i] = (uint8_t)i;
    for (int i = 255; i > 0; i--)  // Fisher-Yates shuffle
    {
        int j = rng() % (i + 1);
        uint8_t tmp = p[i]; p[i] = p[j]; p[j] = tmp;
    }
    for (int i = 0; i < 512; i++)
        gPerlinPerm[i] = p[i & 255];

    std::uniform_real_distribution<double> d01(0.0, 1.0);
    for (int i = 0; i < 256; i++)
        gPerlinGrad[i] = d01(rng);
}

// 2D value noise, numOctaves in [0..6]
static double PerlinNoise2D(double x, double y, int numOctaves)
{
    if (numOctaves < 0 || numOctaves >= 7) return 0.5;

    const double* amp = kPerlinAmp;
    double accumulated = 0.0;
    double coordX      = x;
    int    iters       = numOctaves + 1;

    do {
        int xi0 = (int)floor(coordX);
        int xi1 = xi0 + 1;
        double fracX = coordX - floor(coordX);

        int    yi0    = (int)floor(y);
        int    yi1    = yi0 + 1;
        double fracY  = y - floor(y);
        double smoothY = (3.0 - 2.0 * fracY) * fracY * fracY;  // smoothstep on Y only

        // Hash the 4 cell corners through the permutation table
        int hY0 = gPerlinPerm[yi0 & 255];
        int hY1 = gPerlinPerm[yi1 & 255];
        double gBL = gPerlinGrad[gPerlinPerm[(xi0 + hY0) & 255]];
        double gBR = gPerlinGrad[gPerlinPerm[(xi1 + hY0) & 255]];
        double gTL = gPerlinGrad[gPerlinPerm[(xi0 + hY1) & 255]];
        double gTR = gPerlinGrad[gPerlinPerm[(xi1 + hY1) & 255]];

        // Bilinear: linear on X, smoothstep on Y (matching original)
        double top    = gTR * fracX + gTL * (1.0 - fracX);
        double bottom = gBL * (1.0 - fracX) + gBR * fracX;
        accumulated  += (top * smoothY + bottom * (1.0 - smoothY)) * (*amp++);

        // Next octave: shift+double to avoid correlation between octaves
        coordX = (coordX + 7.0) * 2.0;
        iters--;
    } while (iters != 0);

    return kPerlinScale[numOctaves] * accumulated;
}

// Sparkle effect - brightens random dark pixels along each row for shimmer
// TODO: Add this as a toggle in the dialog box.
static bool useSparkles = true;
// Original thresholds: 32 < value < 128. Effect: double the value, spread to 4 neighbors.
static const uint8_t SPARKLE_LOW  = 32;   // only sparkle pixels above this
static const uint8_t SPARKLE_HIGH = 128;  // only sparkle pixels below this

// Function pointer type for diffusion implementations
using DiffusionFunc = void(*)();
static void DiffuseScalar();
static void DiffuseSSE2();
static DiffusionFunc diffuseHeat = DiffuseScalar;
static void EmitParticle(float x, float y, float speed);
static void SpawnParticlesRandom(int count);

// ------------------------------------------------------------
// CPU feature detection
// ------------------------------------------------------------
static bool HasSSE2()
{
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    return (cpuInfo[3] & (1 << 26)) != 0;  // EDX bit 26 = SSE2
}

// FPS tracking
static LARGE_INTEGER fpsFrequency = {};    // ticks per second
static LARGE_INTEGER fpsLastTime = {};     // last time we updated FPS display
static int fpsFrameCount = 0;             // frames since last update

// Frame limiter
// TODO: Add this as a toggle in the dialog box.

//A note about frame rate.  The particle fire screensaver is set to 25fps.
//But particle toy runs smoother than that.  It's either 60fps, or the screen's
//refresh rate.  We won't know with decompiling it... or doing some screen recording

static bool useFrameLimiter = false;
static bool useOriginalSpeed = true;   // Sleep(1) each frame to match original's loop pacing
static int gRefreshRate = 60;                       // detected monitor refresh rate
static double gTargetFrameTime = 1.0 / 60.0;       // seconds per frame
static LARGE_INTEGER gLastFrameTime = {};           // when last frame completed

// ------------------------------------------------------------
// UpdateRefreshRate: detect current monitor's refresh rate.
// Uses MonitorFromWindow to handle multi-monitor setups.
// ------------------------------------------------------------
static void UpdateRefreshRate(HWND hwnd)
{
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEX mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMon, &mi);

    DEVMODE dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettings(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
    {
        gRefreshRate = dm.dmDisplayFrequency;
    }
    else
    {
        gRefreshRate = 60;  // safe fallback
    }
    gTargetFrameTime = 1.0 / (double)gRefreshRate;
}

// Palette configuration
// TODO: Add color scheme selector and nitro toggle to the dialog box.
static bool useNitro = false;  // boost blue at high heat for white-hot effect

// Preset color schemes: { midpoint color (Color 1), bright color (Color 2) }
struct ColorScheme { uint8_t r1, g1, b1, r2, g2, b2; };
static const ColorScheme PALETTE_PRESETS[] = {
    { 255, 128,   0,  255, 255,   0 },  // 0: Fiery Orange (original default)
    {   0, 128, 255,    0, 255, 255 },  // 1: Skyish Teal
    {  64,  64, 128,  192, 192, 255 },  // 2: Velvet Blue
    {   0, 255,   0,  255, 255,  55 },  // 3: Terry's Green
    {  32, 128,  32,  160, 255, 160 },  // 4: Slimy Green
    { 255,  64,  64,  255, 192, 192 },  // 5: Burning Pink
};
static const int NUM_PALETTE_PRESETS = sizeof(PALETTE_PRESETS) / sizeof(PALETTE_PRESETS[0]);
static int currentPalette = 0;

// ------------------------------------------------------------
// BuildPalette: two-color spread palette with optional "nitro" boost.
// 0-127:   black -> color1 (linear ramp)
// 128-254: color1 -> color2 (linear interpolation)
// 255:     forced white
// Nitro:   adds blue channel ramp starting at index 200 for white-hot tips
// ------------------------------------------------------------
static void BuildPalette(uint8_t r1, uint8_t g1, uint8_t b1,
                         uint8_t r2, uint8_t g2, uint8_t b2)
{
    for (int i = 0; i < 256; i++)
    {
        int r, g, b;

        if (i < 128)
        {
            // Black -> Color 1
            float t = (float)i / 127.0f;
            r = (int)(r1 * t);
            g = (int)(g1 * t);
            b = (int)(b1 * t);
        }
        else
        {
            // Color 1 -> Color 2
            float t = (float)(i - 127) / 128.0f;
            float it = 1.0f - t;
            r = (int)(r1 * it + r2 * t);
            g = (int)(g1 * it + g2 * t);
            b = (int)(b1 * it + b2 * t);
        }

        // Nitro: boost blue near the top for white-hot intensity
        if (useNitro && i >= 200)
        {
            int boost = (i - 200) * 4;
            b = b + boost;
            if (b > 255) b = 255;
        }

        gPalette[i] = (uint32_t)((r << 16) | (g << 8) | (b));
    }

    // Force index 255 to pure white
    gPalette[255] = 0x00FFFFFF;
}

// ------------------------------------------------------------
// InitBackbuffer: create the DIBSection (pixel buffer) and allocate heat.
// ------------------------------------------------------------
static void InitBackbuffer(HWND hwnd)
{
    // Describe our 32-bit RGB bitmap (top-down so [0] is top-left).
    gBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    gBmi.bmiHeader.biWidth = gW;
    gBmi.bmiHeader.biHeight = -gH; // negative = top-down
    gBmi.bmiHeader.biPlanes = 1;
    gBmi.bmiHeader.biBitCount = 32;
    gBmi.bmiHeader.biCompression = BI_RGB;

    // Allocate pixel memory (Windows allocates; we get pointer via gPixels).
    HDC hdc = GetDC(hwnd);
    HBITMAP dib = CreateDIBSection(hdc, &gBmi, DIB_RGB_COLORS, &gPixels, nullptr, 0);
    ReleaseDC(hwnd, hdc);

    // Keep the DIB handle alive for the lifetime of the program.
    sDibSection = dib;

    if (!dib || !gPixels)
    {
        MessageBox(hwnd, L"CreateDIBSection failed", L"Error", MB_OK);
        return;
    }

    // Typed view of the same memory: now we can index pixels as uint32_t.
    pixelMem = (uint32_t*)gPixels;

    // Allocate the heat buffer (1 byte per pixel).
    // VirtualAlloc gives page-aligned memory; good for big buffers.
    gHeat = (uint8_t*)VirtualAlloc(nullptr, (size_t)gW * (size_t)gH,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!gHeat)
    {
        MessageBox(hwnd, L"Heat buffer allocation failed", L"Error", MB_OK);
        return;
    }

    // Start cold (all zeros).
    ZeroMemory(gHeat, (size_t)gW * (size_t)gH);

    // Build our heat->color palette from current scheme.
    const ColorScheme& cs = PALETTE_PRESETS[currentPalette];
    BuildPalette(cs.r1, cs.g1, cs.b1, cs.r2, cs.g2, cs.b2);

    // Reserve space for particles (avoids reallocation during normal use)
    particles.reserve(INITIAL_PARTICLE_RESERVE);

    // Initialize FPS timer
    QueryPerformanceFrequency(&fpsFrequency);
    QueryPerformanceCounter(&fpsLastTime);
    QueryPerformanceCounter(&gLastFrameTime);

    // Detect monitor refresh rate for frame limiter
    UpdateRefreshRate(hwnd);

    // Calculate speed factor based on mode and resolution
    UpdateSpeedFactor();

    // Initialize Perlin permutation table for flame base mode
    InitPerlinTable();

    // Set diffusion method based on toggle and CPU capability
    if (useSIMD && HasSSE2())
        diffuseHeat = DiffuseSSE2;
    else
        diffuseHeat = DiffuseScalar;

    // Spawn particles randomly across the screen with zero velocity (matching original)
    SpawnParticlesRandom(INITIAL_PARTICLE_RESERVE);
}

// ------------------------------------------------------------
// SpawnParticlesRandom: populate the particle array with stationary particles
// at random screen positions. Matches original's startup state:
// zero velocity, random position inset from edges, heat 128-255.
// ------------------------------------------------------------
static void SpawnParticlesRandom(int count)
{
    particles.clear();
    for (int i = 0; i < count; i++)
    {
        float x = (float)(5 + (int)(rng() % (gW - 10)));  // inset 5px (matching original)
        float y = (float)(3 + (int)(rng() % (gH - 6)));   // inset 3px
        EmitParticle(x, y, 0.0f);                          // zero velocity — gravity builds speed naturally
    }
}

// ------------------------------------------------------------
// ChangeResolution: tear down and rebuild the buffers at a new size,
// then resize the main window to match (accounting for decorations).
// Pass resizeWindow=false when the window is already the target size
// (e.g., called from WM_SIZE during a maximize).
// ------------------------------------------------------------
static void ChangeResolution(HWND hwnd, int newW, int newH, bool resizeWindow)
{
    int particleCount = (int)particles.size();

    // --- Free old buffers ---
    if (gHeat)      { VirtualFree(gHeat, 0, MEM_RELEASE); gHeat = nullptr; }
    if (sDibSection){ DeleteObject(sDibSection); sDibSection = nullptr; }
    gPixels  = nullptr;
    pixelMem = nullptr;

    // --- Update dimensions ---
    gW = newW;
    gH = newH;

    // --- Rebuild BITMAPINFO for new size ---
    gBmi.bmiHeader.biWidth  =  gW;
    gBmi.bmiHeader.biHeight = -gH;  // negative = top-down

    // --- Recreate DIBSection ---
    HDC hdc = GetDC(hwnd);
    sDibSection = CreateDIBSection(hdc, &gBmi, DIB_RGB_COLORS, &gPixels, nullptr, 0);
    ReleaseDC(hwnd, hdc);
    if (!sDibSection || !gPixels) return;
    pixelMem = (uint32_t*)gPixels;

    // --- Reallocate heat buffer ---
    gHeat = (uint8_t*)VirtualAlloc(nullptr, (size_t)gW * (size_t)gH,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!gHeat) return;
    ZeroMemory(gHeat, (size_t)gW * (size_t)gH);

    // --- Update speed factor for new resolution ---
    UpdateSpeedFactor();

    // --- Resize window to show the new buffer 1:1 ---
    if (resizeWindow)
    {
        // Restore from maximized first so SetWindowPos can set the new size freely
        if (IsZoomed(hwnd))
            ShowWindow(hwnd, SW_RESTORE);

        DWORD style   = (DWORD)GetWindowLong(hwnd, GWL_STYLE);
        DWORD exStyle = (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE);
        RECT rc = { 0, 0, gW, gH };
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);  // FALSE = no menu bar
        SetWindowPos(hwnd, nullptr, 0, 0,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }

    // --- Respawn particles at positions valid for the new buffer ---
    SpawnParticlesRandom(particleCount > 0 ? particleCount : INITIAL_PARTICLE_RESERVE);
}

// ------------------------------------------------------------
// EmitParticle: spawn a particle at (x,y) with a random direction
// ------------------------------------------------------------
static void EmitParticle(float x, float y, float speed)
{
    float angle = angleDist(rng);

    Particle p;
    p.x = x;
    p.y = y;
    //The speed here works as such - cos gives us x part of speed, sin gives us y.  Combined gives us the direction and full speed.
    //Using trig will let us preserve speed properly since cos² + sin² = 1
    p.dx = cosf(angle) * speed;
    p.dy = sinf(angle) * speed;
    p.heat = 128 + (dist(rng) % 128);  // range 128-255, matching original
    p.color = 0x00FFFFFF;  // white for now
    p.active = true;
    p.leaderIdx = -1;     // no leader assigned

    particles.push_back(p);
}

// ------------------------------------------------------------
// EmitFirework: spawn many particles in all directions from a point.
// Each explosion gets a random max speed (50%-100% of base), and each
// particle gets a random speed from 0 to that max. This creates a
// filled disc rather than a ring, and each explosion feels different.
// ------------------------------------------------------------
static void EmitFirework(float x, float y, int count)
{
    // Clear existing particles
    particles.clear();

    // Calculate base speed, then pick a random max for this explosion
    float baseSpeed = PARTICLE_SPEED_FACTOR * gW * userSpeedMultiplier;
    float maxSpeed = baseSpeed * (0.5f + chance(rng));  // 50%-150% of base → [4.0, 12.0] at original speeds

    for (int i = 0; i < count; i++)
    {
        // Each particle gets a random speed from 0 to maxSpeed (filled disc)
        float speed = chance(rng) * maxSpeed;
        EmitParticle(x, y, speed);
    }
}

// ------------------------------------------------------------
// DepositHeatLine: Bresenham integer line draw, matching original ptoy.
// Writes a 3-pixel vertical strip (center + above + below) at each step —
// same as original's "center pixel + pixel above + pixel below" pattern.
// No sqrtf, no float division per step, pure integer arithmetic.
// ------------------------------------------------------------
static void DepositHeatLine(float x0f, float y0f, float x1f, float y1f, uint8_t heat)
{
    int x0 = (int)x0f;
    int y0 = (int)y0f;
    int x1 = (int)x1f;
    int y1 = (int)y1f;

    int dx  =  abs(x1 - x0);
    int dy  =  abs(y1 - y0);
    int sx  = (x0 < x1) ? 1 : -1;
    int sy  = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;)
    {
        // 3-pixel vertical strip at (x0, y0): center, above, below.
        // Unsigned cast turns negative coords into large values, failing the < gW/gH check — safe one-shot bounds test.
        if ((unsigned)x0 < (unsigned)gW)
        {
            if ((unsigned)y0       < (unsigned)gH) gHeat[ y0      * gW + x0] = heat;
            if ((unsigned)(y0 - 1) < (unsigned)gH) gHeat[(y0 - 1) * gW + x0] = heat;
            if ((unsigned)(y0 + 1) < (unsigned)gH) gHeat[(y0 + 1) * gW + x0] = heat;
        }

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// ------------------------------------------------------------
// DepositHeatLineHeavy: original float-based line draw (kept for comparison).
// draw a line of heat from (x0,y0) to (x1,y1)
// ------------------------------------------------------------
static void DepositHeatLineHeavy(float x0, float y0, float x1, float y1, uint8_t heat)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float dist = sqrtf(dx * dx + dy * dy);

    // Number of steps = distance, so we deposit at least once per pixel
    int steps = (int)dist + 1;

    for (int i = 0; i <= steps; i++)
    {
        float t = (steps > 0) ? (float)i / steps : 0.0f;
        int ix = (int)(x0 + dx * t);
        int iy = (int)(y0 + dy * t);

        // Deposit heat in a square around this point
        int half = particleSize / 2;
        for (int oy = -half; oy < particleSize - half; oy++)
        {
            for (int ox = -half; ox < particleSize - half; ox++)
            {
                int hx = ix + ox;
                int hy = iy + oy;
                if ((unsigned)hx < (unsigned)gW && (unsigned)hy < (unsigned)gH)
                {
                    uint8_t& h = gHeat[hy * gW + hx];
                    if (heat > h) h = heat;
                }
            }
        }
    }
}

// AltColor: particles fade per-frame and brighten on wall bounce
// TODO: Add this as a toggle in the dialog box.
static bool useAltColor = false;
static const uint8_t HEAT_FADE = 1;         // heat lost per frame per particle
static const uint8_t HEAT_FLOOR = 128;      // minimum heat (particles never go fully dark)
static const uint8_t BOUNCE_BRIGHTEN = 32;  // heat gained on wall bounce

// Bounce tuning
static const float BOUNCE = 0.95f;           // speed retained on bounce (1.0 = perfect, <1.0 = loses energy)
static const float KICK_STRENGTH = 0.5f;    // max random perpendicular kick on bounce

// Attraction steering: how fast particles turn toward their target (0.0 = no turn, 1.0 = instant)
static const float STEER_RATE = 0.05f;
static const float PI = 3.14159265f;

// ------------------------------------------------------------
// SteerParticles: rotate each particle's velocity toward its target.
// Preserves speed, only changes direction. Orbiting emerges naturally
// because the turn rate is slow enough that particles overshoot.
//
// Target depends on leader mode:
//   useFollowLeader=false : all particles target cursor
//   useFollowLeader=true, useMultiLeader=false : particle 0 leads, rest follow particle 0
//   useFollowLeader=true, useMultiLeader=true  : every 64th particle leads, rest follow their group leader
// ------------------------------------------------------------
static void SteerParticles(float cursorX, float cursorY)
{
    // The leader mask determines group size.
    // 0x7FFF = effectively one global leader (particle 0); 0x3F = groups of 64
    int leaderMask = useMultiLeader ? 0x3F : 0x7FFF;

    for (size_t i = 0; i < particles.size(); i++)
    {
        Particle& p = particles[i];
        if (!p.active) continue;

        // Determine this particle's steering target
        float targetX, targetY;
        if (!useFollowLeader || (i & leaderMask) == 0)
        {
            // Leader (or all-to-cursor mode): target cursor.
            // In follow-leader mode, only chase cursor while right mouse is held;
            // otherwise the leader drifts freely (followers still chain to it).
            if (useFollowLeader && !rightMouseDown)
                continue;
            targetX = cursorX;
            targetY = cursorY;
        }
        else
        {
            // Follower: target its group leader (the nearest particle whose index
            // is a multiple of leaderMask+1)
            size_t leaderIdx = i & ~(size_t)leaderMask;
            if (leaderIdx < particles.size() && particles[leaderIdx].active)
            {
                targetX = particles[leaderIdx].x;
                targetY = particles[leaderIdx].y;
            }
            else
            {
                targetX = cursorX;
                targetY = cursorY;
            }
        }

        // Current speed. If near-zero, use fallback angle (matching original's rotating
        // reference direction) so stationary particles don't get stuck.
        float speed = sqrtf(p.dx * p.dx + p.dy * p.dy);
        float velAngle;
        if (speed < 0.001f)
        {
            velAngle = gFallbackAngle;
            speed = 0.5f + chance(rng) * 0.5f;
        }
        else
        {
            velAngle = atan2f(p.dy, p.dx);
        }

        // Angle from particle to target
        float targetAngle = atan2f(targetY - p.y, targetX - p.x);

        // Angle difference, normalized to -PI..PI
        float angleDiff = targetAngle - velAngle;
        if (angleDiff >  PI) angleDiff -= PI * 2.0f;
        if (angleDiff < -PI) angleDiff += PI * 2.0f;

        // Turn a fraction toward the target, reconstruct velocity
        velAngle += angleDiff * STEER_RATE;
        p.dx = cosf(velAngle) * speed;
        p.dy = sinf(velAngle) * speed;
    }
}

// ------------------------------------------------------------
// UpdateParticles: move particles, deposit heat, deactivate if off-screen
// ------------------------------------------------------------
static void UpdateParticles()
{
    for (size_t i = 0; i < particles.size(); i++)
    {
        Particle& p = particles[i];
        if (!p.active) continue;

        // AltColor: fade heat each frame, floor at HEAT_FLOOR
        if (useAltColor && p.heat > HEAT_FLOOR)
        {
            p.heat = (p.heat - HEAT_FADE < HEAT_FLOOR) ? HEAT_FLOOR : p.heat - HEAT_FADE;
        }

        // Store previous position
        float prevX = p.x;
        float prevY = p.y;

        // Apply gravity before moving
        if (useGravity)
        {
            p.dx += gravityX;
            p.dy += gravityY;
        }

        // Move particle
        p.x += p.dx;
        p.y += p.dy;

        // Bounce off walls with energy loss and random perpendicular kick
        if (p.x < 0)
        {
            p.x = -p.x;
            p.dx = fabsf(p.dx) * BOUNCE;
            p.dy += (chance(rng) * 2.0f - 1.0f) * KICK_STRENGTH;
            if (useAltColor) p.heat = (p.heat + BOUNCE_BRIGHTEN > 254) ? 254 : p.heat + BOUNCE_BRIGHTEN;
        }
        else if (p.x >= gW)
        {
            p.x = 2.0f * gW - p.x - 1;
            p.dx = -fabsf(p.dx) * BOUNCE;
            p.dy += (chance(rng) * 2.0f - 1.0f) * KICK_STRENGTH;
            if (useAltColor) p.heat = (p.heat + BOUNCE_BRIGHTEN > 254) ? 254 : p.heat + BOUNCE_BRIGHTEN;
        }

        if (p.y < 0)
        {
            p.y = -p.y;
            p.dy = fabsf(p.dy) * BOUNCE;
            p.dx += (chance(rng) * 2.0f - 1.0f) * KICK_STRENGTH;
            if (useAltColor) p.heat = (p.heat + BOUNCE_BRIGHTEN > 254) ? 254 : p.heat + BOUNCE_BRIGHTEN;
        }
        else if (p.y >= gH)
        {
            p.y = 2.0f * gH - p.y - 1;
            p.dy = -fabsf(p.dy) * BOUNCE;
            p.dx += (chance(rng) * 2.0f - 1.0f) * KICK_STRENGTH;
            if (useAltColor) p.heat = (p.heat + BOUNCE_BRIGHTEN > 254) ? 254 : p.heat + BOUNCE_BRIGHTEN;
        }

        // Deposit heat along the line from previous to current position
        DepositHeatLine(prevX, prevY, p.x, p.y, p.heat);
    }
}

// ------------------------------------------------------------
// DiffuseScalar: heat diffusion (one pixel at a time)
// Heat rises by reading from above and writing result upward.
// Loop runs top-to-bottom so we never read rows we've already written.
// ------------------------------------------------------------
static void DiffuseScalar()
{
    // Start one row down from top border, go to bottom
    for (int y = BORDER_MARGIN + 1; y < gH - BORDER_MARGIN; y++)
    {
        uint8_t* row = gHeat + y * gW;
        uint8_t* rowAbove = row - gW;

        for (int x = BORDER_MARGIN; x < gW - BORDER_MARGIN; x++)
        {
            // Sum the 4 neighbors: above, left, self, right
            int sum = rowAbove[x] + row[x - 1] + row[x] + row[x + 1];

            // Subtract fade BEFORE dividing (slower fade, more persistent fire)
            int result = (sum - BURNFADE) >> 2;

            // Clamp at 1 (matching original: values < 1 collapse to 0)
            rowAbove[x] = (result < 1) ? 0 : (uint8_t)result;
        }
    }
}

// ------------------------------------------------------------
// DiffuseSSE2: SIMD heat diffusion (16 pixels at a time)
// Same algorithm as scalar: avg(above, left, self, right)
// then minus fade and divide by 4.
// Processes 16 pixels per iteration using 128-bit SSE2 registers.
// Because adding 4 bytes can overflow uint8, we unpack to 16-bit,
// do the math, then pack back to 8-bit.
// ------------------------------------------------------------
static void DiffuseSSE2()
{
    __m128i zero = _mm_setzero_si128();
    __m128i fade = _mm_set1_epi16((short)BURNFADE);

    // Start one row down from top border, go to bottom
    for (int y = BORDER_MARGIN + 1; y < gH - BORDER_MARGIN; y++)
    {
        uint8_t* row = gHeat + y * gW;
        uint8_t* rowAbove = row - gW;
        int x = BORDER_MARGIN;

        // Process 16 pixels at a time
        // We need x-1 and x+1, so stop 16 pixels before right border
        int xEnd = gW - BORDER_MARGIN - 16;

        //Note the 16 here, we're going through 16 bytes of memory at a time instead of one
        for (; x <= xEnd; x += 16)
        {
            // Load 16 bytes for each neighbor
            /*
                Lots to unpack (pun not intended) here:
                __m128i is a 128-bit integer register variable, we're going to load our ints from the row array into it
                _mm_loadu_si128 function, loads what we cast as a __m128i into the variable
                &row[x] - "address of row[x]" - a uint8_t* pointer
                Note that the & means "address of" - we're not getting the value.
            */
            __m128i above = _mm_loadu_si128((__m128i*)&rowAbove[x]);
            __m128i left  = _mm_loadu_si128((__m128i*)&row[x - 1]);
            __m128i self  = _mm_loadu_si128((__m128i*)&row[x]);
            __m128i right = _mm_loadu_si128((__m128i*)&row[x + 1]);

            /*
                We're going to "space out" the 8-bit values into 16-bit values, going for "low and high"
                parts of the register.  Low and high being the first 8 8-bit values, and high being the last.
                We're doing this so we can add above 255.  Which is done with _mm_add_epi16 below.
            */

            // Unpack low 8 bytes to 16-bit (prevents overflow during addition)
            __m128i above_lo = _mm_unpacklo_epi8(above, zero);
            __m128i left_lo  = _mm_unpacklo_epi8(left, zero);
            __m128i self_lo  = _mm_unpacklo_epi8(self, zero);
            __m128i right_lo = _mm_unpacklo_epi8(right, zero);

            // Unpack high 8 bytes to 16-bit
            __m128i above_hi = _mm_unpackhi_epi8(above, zero);
            __m128i left_hi  = _mm_unpackhi_epi8(left, zero);
            __m128i self_hi  = _mm_unpackhi_epi8(self, zero);
            __m128i right_hi = _mm_unpackhi_epi8(right, zero);

            // Sum the 4 neighbors (16-bit addition, we can't overflow)
            __m128i sum_lo = _mm_add_epi16(above_lo, left_lo);
            sum_lo = _mm_add_epi16(sum_lo, self_lo);
            sum_lo = _mm_add_epi16(sum_lo, right_lo);

            __m128i sum_hi = _mm_add_epi16(above_hi, left_hi);
            sum_hi = _mm_add_epi16(sum_hi, self_hi);
            sum_hi = _mm_add_epi16(sum_hi, right_hi);

            // We subtract here.  "Saturation" means we stop at the maximum or minimum.
            // So we'll safely stay at 0 if we get that low.

            // Using saturating subtract so we clamp at 0 automatically
            sum_lo = _mm_subs_epu16(sum_lo, fade);
            sum_hi = _mm_subs_epu16(sum_hi, fade);

            /*
                Just like in the normal version, we use bit-shifting to divide by four.
                This technically loses some precision but doesn't affect the output in a
                way that matters to the naked eye.  Who will notice a single pixel off by
                a degree at 100+ fps?
            */
            sum_lo = _mm_srli_epi16(sum_lo, 2);
            sum_hi = _mm_srli_epi16(sum_hi, 2);


            // We re-pack, and same here we have saturation - so if somehow we had a number
            // too high (not really possible with this math, but still) it'll clamp at 255.
            // (Or 0)
            __m128i result = _mm_packus_epi16(sum_lo, sum_hi);

            // Store result to the row ABOVE so the heat rises
            _mm_storeu_si128((__m128i*)&rowAbove[x], result);
        }

        // Handle leftover pixels that don't fit in a 16-byte chunk
        for (; x < gW - BORDER_MARGIN; x++)
        {
            int sum = rowAbove[x] + row[x - 1] + row[x] + row[x + 1];
            int result = (sum - BURNFADE) >> 2;
            rowAbove[x] = (result < 1) ? 0 : (uint8_t)result;
        }
    }
}

// ------------------------------------------------------------
// RenderFire: 1) inject heat (mouse) 2) update heat 3) map heat->pixels
// IMPORTANT: This updates gHeat in place (like Seumas' sample).
// ------------------------------------------------------------
static void RenderFire(HWND hwnd)
{
    if (!pixelMem || !gHeat) return;

    // ----------------------------
    // Convert window mouse coords -> buffer coords (bx,by)
    // ----------------------------
    RECT rc;
    GetClientRect(hwnd, &rc);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;

    int bx = gMouseX * gW / (winW ? winW : 1);
    int by = gMouseY * gH / (winH ? winH : 1);

    // Clamp bx/by safely (so we never write out of bounds).
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    if (bx >= gW) bx = gW - 1;
    if (by >= gH) by = gH - 1;

    // ----------------------------
    // 1) Inject heat at the mouse position (a small "dot of fire")
    // ----------------------------
    // We write heat=255 into a small 5x5 square centered at (bx,by).
    // This is your "click makes fire" source.
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
        {
            int x = bx + dx;
            int y = by + dy;
            if (mouseDown && (unsigned)x < (unsigned)gW && (unsigned)y < (unsigned)gH)
            {
                gHeat[y * gW + x] = 255;
            }
        }

    // ----------------------------
    // 1b) Advance fallback angle (rotates 0.01 rad/frame, wraps at ±PI)
    // Used by SteerParticles to give stationary particles a starting direction
    // ----------------------------
    gFallbackAngle += 0.01f;
    if (gFallbackAngle > PI) gFallbackAngle -= PI * 2.0f;

    // ----------------------------
    // 1c) Random events (auto-mode): 5% chance per second of a random event
    // ----------------------------
    if (useRandEvents && !particles.empty())
    {
        time_t nowSec = time(nullptr);
        if (nowSec != gLastRandEventSec)
        {
            gLastRandEventSec = nowSec;
            if ((int)(rng() % 20) == 0)  // 5% chance
            {
                switch (rng() % 5)
                {
                case 0: // Cycle gravity direction
                    gGravityState = (gGravityState + 1) % 4;
                    switch (gGravityState)
                    {
                    case 0: gravityX =  0.0f; gravityY =  0.1f; break;  // down
                    case 1: gravityX = -0.1f; gravityY =  0.0f; break;  // left
                    case 2: gravityX =  0.0f; gravityY = -0.1f; break;  // up
                    case 3: gravityX =  0.1f; gravityY =  0.0f; break;  // right
                    }
                    break;

                case 1: // Freeze all particles
                    for (auto& p : particles) { p.dx = 0; p.dy = 0; }
                    break;

                case 2: // Explosion at random position
                    EmitFirework((float)(rng() % gW), (float)(rng() % gH),
                                 (int)particles.size());
                    break;

                case 3: // Comet: all particles to same random position + direction
                {
                    float cx    = (float)(rng() % gW);
                    float cy    = (float)(rng() % gH);
                    float angle = angleDist(rng);
                    float spd   = PARTICLE_SPEED_FACTOR * gW * userSpeedMultiplier;
                    for (auto& p : particles)
                    {
                        p.x = cx; p.y = cy;
                        p.dx = cosf(angle) * spd;
                        p.dy = sinf(angle) * spd;
                    }
                    break;
                }

                case 4: // Emit to center: all particles converge on screen center
                    for (auto& p : particles)
                    {
                        p.x  = gW * 0.5f;
                        p.y  = gH * 0.5f;
                        p.dx = 0;
                        p.dy = 0;
                    }
                    break;
                }
            }
        }
    }

    // ----------------------------
    // 1d) Steer particles
    // Follow-leader mode: always call so followers chain to leaders every frame.
    //   Leaders only target cursor when right mouse is held.
    // Normal mode: only steer when left mouse is held (original behavior).
    // ----------------------------
    if (!particles.empty() && (useFollowLeader || mouseDown))
    {
        SteerParticles((float)bx, (float)by);
    }

    // ----------------------------
    // 1c) Update particles - move them and deposit heat
    // ----------------------------
    UpdateParticles();

    // ----------------------------
    // 2) Update heat using active diffusion method (scalar or SIMD)
    // ----------------------------
    diffuseHeat();

    // ----------------------------
    // 3) Sparkles: brighten one random dark pixel per row for shimmer effect
    // ----------------------------
    if (useSparkles)
    {
        for (int y = BORDER_MARGIN; y < gH - BORDER_MARGIN; y++)
        {
            int x = BORDER_MARGIN + rng() % (gW - 2 * BORDER_MARGIN);
            uint8_t h = gHeat[y * gW + x];
            if (h > SPARKLE_LOW && h < SPARKLE_HIGH)
            {
                uint8_t bright = (h * 2 > 255) ? 255 : (uint8_t)(h * 2);
                // Write brightened value to the 4 neighbors (cross pattern)
                if (y > BORDER_MARGIN)              gHeat[(y-1) * gW + x] = bright;  // above
                if (y < gH - BORDER_MARGIN - 1)    gHeat[(y+1) * gW + x] = bright;  // below
                if (x > BORDER_MARGIN)              gHeat[y * gW + x - 1] = bright;  // left
                if (x < gW - BORDER_MARGIN - 1)    gHeat[y * gW + x + 1] = bright;  // right
            }
        }
    }

    // ----------------------------
    // 4) Clear border pixels (not processed by diffusion, would accumulate heat)
    // Why we're doing it this way: If we write our loops and particle handlers to
    // handle the border, We'll have to write border-checking logic everywhere.  
    // Setting the borders to 0 heat every frame is actually less calculations
    // ----------------------------
    for (int x = 0; x < gW; x++)
    {
        gHeat[x] = 0;                       // top row
        gHeat[(gH - 1) * gW + x] = 0;      // bottom row
    }
    for (int y = 0; y < gH; y++)
    {
        gHeat[y * gW] = 0;                  // left column
        gHeat[y * gW + (gW - 1)] = 0;       // right column
    }

    // ----------------------------
    // 5) Seed bottom row: Perlin noise or correlated random walk
    // ----------------------------
    uint8_t* bottomRow = gHeat + (gH - 2) * gW;  // second-to-last row (last row is border)
    if (usePerlinFire)
    {
        // Y increments each frame — this is the time/animation dimension.
        // X = col * 0.03 matching original: ~33px per noise period, resolution-independent blobs.
        gPerlinY += 0.05;

        for (int x = BORDER_MARGIN; x < gW - BORDER_MARGIN; x++)
        {
            double noise = PerlinNoise2D(x * 0.03, gPerlinY, 2);
            // noise is in [0, 1]; map to [64, 255] matching original's +64 offset
            int val = (int)(noise * 191.0 + 64.0);
            if (val < 0)   val = 0;
            if (val > 255) val = 255;
            bottomRow[x] = (uint8_t)val;
        }
    }
    else
    {
        // Correlated random walk: each pixel drifts ±32 from its left neighbor
        // Produces smooth, wave-like flame base matching original mode 0
        int seed = dist(rng);
        for (int x = BORDER_MARGIN; x < gW - BORDER_MARGIN; x++)
        {
            if (bottomRow[x] != 0)
                seed = bottomRow[x];
            seed += (int)(dist(rng) % 65) - 32;
            if (seed < 0) seed = 0;
            if (seed > 255) seed = 255;
            bottomRow[x] = (uint8_t)seed;
        }
    }

    // ----------------------------
    // 6) Map heat -> RGB pixels using palette
    // ----------------------------
    // Each frame we "paint" the heat field into the visible pixel buffer.
    for (int i = 0; i < gW * gH; i++)
    {
        pixelMem[i] = gPalette[gHeat[i]];
    }

    // ----------------------------
    // 7) Update FPS counter
    // ----------------------------
    fpsFrameCount++;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - fpsLastTime.QuadPart) / fpsFrequency.QuadPart;
    if (elapsed >= 1.0)
    {
        if (gControlPanel)
        {
            SetDlgItemInt(gControlPanel, IDC_FPSLIVE, fpsFrameCount, FALSE);
        }
        fpsFrameCount = 0;
        fpsLastTime = now;
    }
    // Original speed mode: Sleep(1) each frame to match the original ptoy's loop pacing.
    // The original called Sleep(1) unconditionally in its main loop, which on Windows
    // yields ~15ms (one timer tick), capping it to roughly 60fps on period hardware.
    if (useOriginalSpeed) Sleep(1);
}

// ------------------------------------------------------------
// Control panel dialog procedure
// ------------------------------------------------------------
INT_PTR CALLBACK ControlPanelProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        // Set default particle count
        SetDlgItemInt(hDlg, IDC_EDIT_PARTICLES, 2000, FALSE);

        // Populate resolution dropdown.
        // Use CB_INSERTSTRING (not CB_ADDSTRING) to preserve order despite CBS_SORT on the control.
        HWND hResCombo = GetDlgItem(hDlg, IDC_COMBO_RESOLUTION);
        for (int i = 0; i < NUM_RES_PRESETS; i++)
            SendMessage(hResCombo, CB_INSERTSTRING, (WPARAM)i, (LPARAM)RES_PRESETS[i].label);
        SendMessage(hResCombo, CB_SETCURSEL, (WPARAM)currentResPreset, 0);

        // Populate palette dropdown (names match PALETTE_PRESETS order)
        HWND hCombo = GetDlgItem(hDlg, IDC_COMBO_PALETTE);
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Fiery Orange");
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Skyish Teal");
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Velvet Blue");
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Terry's Green");
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Slimy Green");
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Burning Pink");
        SendMessage(hCombo, CB_SETCURSEL, currentPalette, 0);

        // Original speed on by default
        CheckDlgButton(hDlg, IDC_CHECK_ORIGSPEED, BST_CHECKED);

        // Check SIMD by default (matches useSIMD initial value)
        //CheckDlgButton(hDlg, IDC_CHECK_SIMD, BST_CHECKED);

        // Check these on by default (matching their initial values above)
        CheckDlgButton(hDlg, IDC_CHECK_GRAVITY,  BST_CHECKED);
        CheckDlgButton(hDlg, IDC_FOLLOWLEADER,   BST_CHECKED);
        CheckDlgButton(hDlg, IDC_MULTILEADER,    BST_CHECKED);
        CheckDlgButton(hDlg, IDC_CHECK_RANDEVENT, BST_CHECKED);

        // Set controls reference text
        SetDlgItemText(hDlg, IDC_CONTROLSTEXT,
            L"Space: Freeze\r\n"
            L"Enter: Comet\r\n"
            L"Backspace: Emit\r\n"
            L"Left Mouse: Follow Pointer\r\n"
            L"Right Mouse: Explosion");

        return TRUE;
    }

    case WM_COMMAND:
    {
        int controlId = LOWORD(wParam);
        int notifyCode = HIWORD(wParam);

        if (controlId == IDC_COMBO_RESOLUTION && notifyCode == CBN_SELCHANGE)
        {
            int sel = (int)SendDlgItemMessage(hDlg, IDC_COMBO_RESOLUTION, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < NUM_RES_PRESETS)
            {
                currentResPreset = sel;
                ChangeResolution(GetParent(hDlg),
                                 RES_PRESETS[sel].w, RES_PRESETS[sel].h,
                                 true);  // resize window to match
            }
        }

        if (controlId == IDC_COMBO_PALETTE && notifyCode == CBN_SELCHANGE)
        {
            int sel = (int)SendDlgItemMessage(hDlg, IDC_COMBO_PALETTE, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < NUM_PALETTE_PRESETS)
            {
                currentPalette = sel;
                const ColorScheme& cs = PALETTE_PRESETS[currentPalette];
                BuildPalette(cs.r1, cs.g1, cs.b1, cs.r2, cs.g2, cs.b2);
            }
        }

        if (controlId == IDC_CHECK_ORIGSPEED && notifyCode == BN_CLICKED)
        {
            useOriginalSpeed = (IsDlgButtonChecked(hDlg, IDC_CHECK_ORIGSPEED) == BST_CHECKED);
        }

        if (controlId == IDC_CHECK_SIMD && notifyCode == BN_CLICKED)
        {
            useSIMD = (IsDlgButtonChecked(hDlg, IDC_CHECK_SIMD) == BST_CHECKED);
            if (useSIMD && HasSSE2())
                diffuseHeat = DiffuseSSE2;
            else
                diffuseHeat = DiffuseScalar;
        }

        if (controlId == IDC_PERLIN_FIRE && notifyCode == BN_CLICKED)
        {
            usePerlinFire = (IsDlgButtonChecked(hDlg, IDC_PERLIN_FIRE) == BST_CHECKED);
        }

        if (controlId == IDC_CHECK_GRAVITY && notifyCode == BN_CLICKED)
        {
            useGravity = (IsDlgButtonChecked(hDlg, IDC_CHECK_GRAVITY) == BST_CHECKED);
        }

        if (controlId == IDC_FOLLOWLEADER && notifyCode == BN_CLICKED)
        {
            useFollowLeader = (IsDlgButtonChecked(hDlg, IDC_FOLLOWLEADER) == BST_CHECKED);
        }

        if (controlId == IDC_MULTILEADER && notifyCode == BN_CLICKED)
        {
            useMultiLeader = (IsDlgButtonChecked(hDlg, IDC_MULTILEADER) == BST_CHECKED);
        }

        if (controlId == IDC_CHECK_RANDEVENT && notifyCode == BN_CLICKED)
        {
            useRandEvents = (IsDlgButtonChecked(hDlg, IDC_CHECK_RANDEVENT) == BST_CHECKED);
            gLastRandEventSec = time(nullptr);  // reset timer so first event isn't immediate
        }

        if (controlId == IDC_EDIT_PARTICLES && notifyCode == EN_CHANGE)
        {
            BOOL ok;
            int count = (int)GetDlgItemInt(hDlg, IDC_EDIT_PARTICLES, &ok, FALSE);
            if (ok && count > 0)
            {
                if (count > MAX_PARTICLES) count = MAX_PARTICLES;
                int current = (int)particles.size();
                if (count > current)
                {
                    // Grow: append new particles at random positions with zero velocity
                    for (int i = current; i < count; i++)
                    {
                        float x = (float)(5 + (int)(rng() % (gW - 10)));
                        float y = (float)(3 + (int)(rng() % (gH - 6)));
                        EmitParticle(x, y, 0.0f);
                    }
                }
                else if (count < current)
                {
                    // Shrink: drop from the end of the array
                    particles.resize(count);
                }
            }
        }

        return TRUE;
    }

    case WM_CLOSE:
        // Hide instead of destroy - user can reopen later
        ShowWindow(hDlg, SW_HIDE);
        return TRUE;
    }

    return FALSE;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        InitBackbuffer(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1; // prevent flicker

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        if (gPixels)
        {
            // Run the simulation + render into pixelMem
            RenderFire(hwnd);

            // Blit (scaled) to the window
            RECT rc;
            GetClientRect(hwnd, &rc);

            StretchDIBits(
                hdc,
                0, 0, rc.right - rc.left, rc.bottom - rc.top,
                0, 0, gW, gH,
                gPixels,
                &gBmi,
                DIB_RGB_COLORS,
                SRCCOPY
            );
        }

        EndPaint(hwnd, &ps);

        // Frame limiter: spin-wait until target frame time has elapsed.
        // Sleep is too coarse (15ms granularity), so we spin on QPC for precision.
        if (useFrameLimiter)
        {
            LARGE_INTEGER now;
            double elapsed;
            do {
                QueryPerformanceCounter(&now);
                elapsed = (double)(now.QuadPart - gLastFrameTime.QuadPart) / fpsFrequency.QuadPart;
            } while (elapsed < gTargetFrameTime);
            gLastFrameTime = now;
        }

        // Request another paint to keep the render loop going
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        mouseDown = true;

        // Store mouse position in *window* coordinates.
        gMouseX = GET_X_LPARAM(lParam);
        gMouseY = GET_Y_LPARAM(lParam);

        // Optional: capture mouse so dragging stays active even if you leave client area.
        SetCapture(hwnd);

        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        // Always track cursor position (needed for particle attraction target)
        gMouseX = GET_X_LPARAM(lParam);
        gMouseY = GET_Y_LPARAM(lParam);
        if (wParam & MK_LBUTTON)
        {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        mouseDown = false;
        ReleaseCapture();
        return 0;

    case WM_RBUTTONDOWN:
    {
        rightMouseDown = true;

        // Convert window coords to buffer coords
        RECT rc;
        GetClientRect(hwnd, &rc);
        int winW = rc.right - rc.left;
        int winH = rc.bottom - rc.top;

        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        float bx = (float)mx * gW / (winW ? winW : 1);
        float by = (float)my * gH / (winH ? winH : 1);

        EmitFirework(bx, by, particles.size());
        return 0;
    }

    case WM_RBUTTONUP:
        rightMouseDown = false;
        return 0;

    case WM_KEYDOWN:
    {
        if (wParam == VK_SPACE)
        {
            // Freeze all particles
            for (size_t i = 0; i < particles.size(); i++)
            {
                particles[i].dx = 0;
                particles[i].dy = 0;
            }
        }
        else if (wParam == VK_RETURN)
        {
            // Comet: all particles at same random position, same random direction
            float cx = (float)(rng() % gW);
            float cy = (float)(rng() % gH);
            float angle = angleDist(rng);
            float baseSpeed = PARTICLE_SPEED_FACTOR * gW * userSpeedMultiplier;
            float cdx = cosf(angle) * baseSpeed;
            float cdy = sinf(angle) * baseSpeed;
            for (size_t i = 0; i < particles.size(); i++)
            {
                particles[i].x = cx;
                particles[i].y = cy;
                particles[i].dx = cdx;
                particles[i].dy = cdy;
            }
        }
        else if (wParam == VK_BACK)
        {
            // Send all particles to center of buffer, stopped
            float cx = gW * 0.5f;
            float cy = gH * 0.5f;
            for (size_t i = 0; i < particles.size(); i++)
            {
                particles[i].x = cx;
                particles[i].y = cy;
                particles[i].dx = 0;
                particles[i].dy = 0;
            }
        }
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            // Keep buffer 1:1 with the client area on any resize.
            // lParam is already the client size (excludes title bar and borders).
            // The (newW != gW || newH != gH) guard prevents re-entrancy when
            // ChangeResolution itself calls SetWindowPos (dropdown path).
            int newW = LOWORD(lParam);
            int newH = HIWORD(lParam);
            if (newW > 0 && newH > 0 && (newW != gW || newH != gH))
                ChangeResolution(hwnd, newW, newH, false);
        }
        return 0;

    case WM_DISPLAYCHANGE:
        // Monitor settings changed (resolution, refresh rate)
        UpdateRefreshRate(hwnd);
        return 0;

    case WM_MOVE:
        // Window moved - may be on a different monitor now
        UpdateRefreshRate(hwnd);
        return 0;

    case WM_CANCELMODE:
    case WM_KILLFOCUS:
        ReleaseCapture();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    const wchar_t* cls = L"PToyRemakeClass";

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClass(&wc);

    // Calculate window size needed for desired client area (1600x1200)
    // Without this, the title bar and borders eat into our client space
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rc = { 0, 0, gW, gH };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int windowWidth = rc.right - rc.left;
    int windowHeight = rc.bottom - rc.top;

    HWND hwnd = CreateWindowEx(
        0,
        cls,
        L"Particle Toy: Remake - Have fun!",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, windowWidth, windowHeight,
        nullptr, nullptr, hInstance, nullptr
    );

    ShowWindow(hwnd, nCmdShow);

    // Create modeless control panel dialog
    gControlPanel = CreateDialog(hInstance, MAKEINTRESOURCE(IDD_CONTROLPANEL), hwnd, ControlPanelProc);
    if (gControlPanel)
    {
        ShowWindow(gControlPanel, SW_SHOW);
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        // Let the dialog process its own messages (tab, keyboard, etc.)
        if (gControlPanel && IsDialogMessage(gControlPanel, &msg))
            continue;

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
