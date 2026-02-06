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

static BITMAPINFO gBmi = {};
static void* gPixels = nullptr;          // raw pointer returned by CreateDIBSection
static uint32_t* pixelMem = nullptr;     // typed view of gPixels (uint32 per pixel)

static uint8_t* gHeat = nullptr;         // simulation buffer: 1 byte per pixel
static uint32_t gPalette[256] = {};      // palette[heat] -> 0x00RRGGBB

// RNG (useful later for "sparklies" etc. - not required for basic fire)
static std::mt19937 rng{ std::random_device{}() };
static std::uniform_int_distribution<int> dist(0, 255);
static std::uniform_real_distribution<float> angleDist(0.0f, 6.283185f);  // 0 to 2*PI
static std::uniform_real_distribution<float> speedVariance(0.85f, 1.15f); // ±15% speed variance
static std::uniform_real_distribution<float> chance(0.0f, 1.0f);          // for percentage rolls

// Speed as fraction of screen width per frame (resolution-independent)
static const float PARTICLE_SPEED_FACTOR = 0.004f;  // 0.4% of screen width per frame
static float userSpeedMultiplier = 1.0f;            // for future UI control

// Particle system
static std::vector<Particle> particles;
static const int INITIAL_PARTICLE_RESERVE = 2000;

// Mouse coordinates stored in WINDOW client space (not buffer space)
static int gMouseX = 1;
static int gMouseY = 1;
static bool mouseDown = false;

// Control panel dialog
static HWND gControlPanel = nullptr;

// Fire tuning
static const int BORDER_MARGIN = 1;   // skip 1-pixel border to avoid bounds issues
static const int BURNFADE = 3;   // how fast heat decays (bigger = faster fade)
static int particleSize = 2;          // deposit size in pixels (for future UI control)

// SIMD toggle - set to true to use SSE2 diffusion, false for scalar
// TODO: Add this into the dialog box.
static bool useSIMD = false;

// Function pointer type for diffusion implementations
using DiffusionFunc = void(*)();
static void DiffuseScalar();
static void DiffuseSSE2();
static DiffusionFunc diffuseHeat = DiffuseScalar;

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

// ------------------------------------------------------------
// Helper: build a simple "fire" palette.
// heat 0   -> black
// heat mid -> green
// heat 255 -> white
// ------------------------------------------------------------
static void BuildFirePalette()
{
    for (int i = 0; i < 256; i++)
    {
        // Piecewise: black -> green -> yellow -> white
        int r = 0, g = 0, b = 0;

        if (i < 128)
        {
            // 0..127: black -> green
            r = 0;
            g = i * 2;   // 0..254
            b = 0;
        }
        else
        {
            // 128..255: green -> yellow -> white-ish
            g = 255;
            r = (i - 128) * 2;         // 0..254
            if (r > 255) r = 255;

            // Add a little blue near the top end to approach white
            b = (i - 200) * 4;         // starts turning on around 200
            if (b < 0) b = 0;
            if (b > 255) b = 255;
        }

        gPalette[i] = (uint32_t)((r << 16) | (g << 8) | (b));
    }
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
    static HBITMAP sKeepAlive = nullptr;
    sKeepAlive = dib;

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

    // Build our heat->color palette.
    BuildFirePalette();

    // Reserve space for particles (avoids reallocation during normal use)
    particles.reserve(INITIAL_PARTICLE_RESERVE);

    // Initialize FPS timer
    QueryPerformanceFrequency(&fpsFrequency);
    QueryPerformanceCounter(&fpsLastTime);

    // Set diffusion method based on toggle and CPU capability
    if (useSIMD && HasSSE2())
        diffuseHeat = DiffuseSSE2;
    else
        diffuseHeat = DiffuseScalar;
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
    p.heat = 255;
    p.color = 0x00FFFFFF;  // white for now
    p.active = true;

    particles.push_back(p);
}

// ------------------------------------------------------------
// EmitFirework: spawn many particles in all directions from a point
// 90% get normal speed (with some variance), 10% get 40% speed (stragglers)
// ------------------------------------------------------------
static void EmitFirework(float x, float y, int count)
{
    // Clear existing particles
    particles.clear();

    // Calculate base speed from screen width (resolution-independent)
    float baseSpeed = PARTICLE_SPEED_FACTOR * gW * userSpeedMultiplier;

    for (int i = 0; i < count; i++)
    {
        float speed;
        if (chance(rng) < 0.10f)
        {
            // 10% are slow stragglers (with variance)
            speed = baseSpeed * 0.4f * speedVariance(rng);
        }
        else
        {
            // 90% get normal speed with ±15% variance
            speed = baseSpeed * speedVariance(rng);
        }
        EmitParticle(x, y, speed);
    }
}

// ------------------------------------------------------------
// DepositHeatLine: draw a line of heat from (x0,y0) to (x1,y1)
// ------------------------------------------------------------
static void DepositHeatLine(float x0, float y0, float x1, float y1, uint8_t heat)
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
                    gHeat[hy * gW + hx] = heat;
                }
            }
        }
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

        // Store previous position
        float prevX = p.x;
        float prevY = p.y;

        // Move particle
        p.x += p.dx;
        p.y += p.dy;

        // Bounce off walls
        if (p.x < 0)
        {
            p.x = -p.x;
            p.dx = -p.dx;
        }
        else if (p.x >= gW)
        {
            p.x = 2 * gW - p.x - 1;
            p.dx = -p.dx;
        }

        if (p.y < 0)
        {
            p.y = -p.y;
            p.dy = -p.dy;
        }
        else if (p.y >= gH)
        {
            p.y = 2 * gH - p.y - 1;
            p.dy = -p.dy;
        }

        // Deposit heat along the line from previous to current position
        DepositHeatLine(prevX, prevY, p.x, p.y, p.heat);
    }
}

// ------------------------------------------------------------
// DiffuseScalar: original heat diffusion (one pixel at a time)
// ------------------------------------------------------------
static void DiffuseScalar()
{
    for (int y = BORDER_MARGIN; y < gH - BORDER_MARGIN - 1; y++)
    {
        uint8_t* line = gHeat + y * gW;
        for (int x = BORDER_MARGIN; x < gW - BORDER_MARGIN; x++)
        {
            int pixel =
                (line[x] +                 // self
                    line[x - 1] +             // left
                    line[x + 1] +             // right
                    line[x + gW])             // below (same x, next row)
                >> 2;                      // divide by 4

            pixel -= BURNFADE;             // fade out

            // Clamp to 0..255
            line[x] = (pixel < 0) ? 0 : (uint8_t)pixel;
        }
    }
}

// ------------------------------------------------------------
// DiffuseSSE2: SIMD heat diffusion (16 pixels at a time)
// Same algorithm as scalar: avg(self, left, right, below) - fade
// Processes 16 pixels per iteration using 128-bit SSE2 registers.
// Because adding 4 bytes can overflow uint8, we unpack to 16-bit,
// do the math, then pack back to 8-bit.
// ------------------------------------------------------------
static void DiffuseSSE2()
{
    __m128i zero = _mm_setzero_si128();
    __m128i fade = _mm_set1_epi16((short)BURNFADE);

    for (int y = BORDER_MARGIN; y < gH - BORDER_MARGIN - 1; y++)
    {
        uint8_t* line = gHeat + y * gW;
        int x = BORDER_MARGIN;

        // Process 16 pixels at a time
        // We need x-1 and x+1, so we stop 16 pixels before the right border
        int xEnd = gW - BORDER_MARGIN - 16;

        //Note the 16 here, we're going through 16 bytes of memory at a time instead of one
        for (; x <= xEnd; x += 16)
        {
            // Load 16 bytes for each neighbor
            /*
                Lots to unpack (pun not intended) here:
                __m128i is a 128-bit integer register variable, we're going to load our ints from the line array into this
                _mm_loadu_si128 function, loads what we cast as a __m128i into the variable
                &line[x] - "address of line[x]" - a uint8_t* pointer
                Note that the & means "address of" - we're not getting the value.
            */

            __m128i self  = _mm_loadu_si128((__m128i*)&line[x]);
            __m128i left  = _mm_loadu_si128((__m128i*)&line[x - 1]);
            __m128i right = _mm_loadu_si128((__m128i*)&line[x + 1]);
            __m128i below = _mm_loadu_si128((__m128i*)&line[x + gW]);

            /*
                We're going to "space out" the 8-bit values into 16-bit values, going for "low and high"
                parts of the register.  Low and high being the first 8 8-bit values, and high being the last.
                We're doing this so we can add above 255.  Which is done with _mm_add_epi16 below.
            */

            // Unpack low 8 bytes to 16-bit (prevents overflow during addition)
            __m128i self_lo  = _mm_unpacklo_epi8(self, zero);
            __m128i left_lo  = _mm_unpacklo_epi8(left, zero);
            __m128i right_lo = _mm_unpacklo_epi8(right, zero);
            __m128i below_lo = _mm_unpacklo_epi8(below, zero);

            // Unpack high 8 bytes to 16-bit
            __m128i self_hi  = _mm_unpackhi_epi8(self, zero);
            __m128i left_hi  = _mm_unpackhi_epi8(left, zero);
            __m128i right_hi = _mm_unpackhi_epi8(right, zero);
            __m128i below_hi = _mm_unpackhi_epi8(below, zero);

            // Sum the 4 neighbors (16-bit, no overflow possible)
            __m128i sum_lo = _mm_add_epi16(self_lo, left_lo);
            sum_lo = _mm_add_epi16(sum_lo, right_lo);
            sum_lo = _mm_add_epi16(sum_lo, below_lo);

            __m128i sum_hi = _mm_add_epi16(self_hi, left_hi);
            sum_hi = _mm_add_epi16(sum_hi, right_hi);
            sum_hi = _mm_add_epi16(sum_hi, below_hi);

            /*
                Just like in the normal version, we use bit-shifting to divide by four.
                This technically loses some precision but doesn't affect the output in a
                way that matters to the naked eye.  Who will notice a single pixel off by
                a degree at 100+ fps?
            */

            // Divide by 4 (shift right by 2)
            sum_lo = _mm_srli_epi16(sum_lo, 2);
            sum_hi = _mm_srli_epi16(sum_hi, 2);

            // We subtract here.  "Saturation" means we stop at the maximum or minimum.
            // So we'll safely stay at 0 if we get that low.

            // Subtract BURNFADE with unsigned saturation (clamps to 0 automatically)
            sum_lo = _mm_subs_epu16(sum_lo, fade);
            sum_hi = _mm_subs_epu16(sum_hi, fade);

            // We re-pack, and same here we have saturation - so if somehow we had a number
            // too high (not really possible with this math, but still) it'll clamp at 255.

            // Pack 16-bit back to 8-bit with unsigned saturation
            __m128i result = _mm_packus_epi16(sum_lo, sum_hi);

            // Store 16 result pixels
            _mm_storeu_si128((__m128i*)&line[x], result);
        }

        //That's the end of the SSE2 faster code!  It's really neat that it can be done this way.

        // This handles the "leftover" pixels.  Anything that doesn't divide by 16 goes here.
        // Handle remaining pixels with scalar code
        for (; x < gW - BORDER_MARGIN; x++)
        {
            int pixel = (line[x] + line[x - 1] + line[x + 1] + line[x + gW]) >> 2;
            pixel -= BURNFADE;
            line[x] = (pixel < 0) ? 0 : (uint8_t)pixel;
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
    // 1b) Update particles - move them and deposit heat
    // ----------------------------
    UpdateParticles();

    // ----------------------------
    // 2) Update heat using active diffusion method (scalar or SIMD)
    // ----------------------------
    diffuseHeat();

    // ----------------------------
    // 3) Clear border pixels (not processed by diffusion, would accumulate heat)
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
    // 4) Map heat -> RGB pixels using palette
    // ----------------------------
    // Each frame we "paint" the heat field into the visible pixel buffer.
    for (int i = 0; i < gW * gH; i++)
    {
        pixelMem[i] = gPalette[gHeat[i]];
    }

    // ----------------------------
    // 5) Update FPS counter
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

        // Populate palette dropdown
        HWND hCombo = GetDlgItem(hDlg, IDC_COMBO_PALETTE);
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Green");
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Red");
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Blue");
        SendMessage(hCombo, CB_SETCURSEL, 0, 0);  // select first item

        // Check bounce by default
        CheckDlgButton(hDlg, IDC_CHECK_BOUNCE, BST_CHECKED);

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

        if (controlId == IDC_COMBO_PALETTE && notifyCode == CBN_SELCHANGE)
        {
            // Palette changed - rebuild palette
            // TODO: hook up palette switching
        }

        if (controlId == IDC_CHECK_BOUNCE && notifyCode == BN_CLICKED)
        {
            // Bounce toggled
            // TODO: hook up bounce toggle
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

        // Simple animation driver: request another paint.
        // (Later you can move this to a timer or a PeekMessage loop.)
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
        // If left button is held, update the injection point as you drag.
        if (wParam & MK_LBUTTON)
        {
            gMouseX = GET_X_LPARAM(lParam);
            gMouseY = GET_Y_LPARAM(lParam);
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
        // Convert window coords to buffer coords
        RECT rc;
        GetClientRect(hwnd, &rc);
        int winW = rc.right - rc.left;
        int winH = rc.bottom - rc.top;

        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        float bx = (float)mx * gW / (winW ? winW : 1);
        float by = (float)my * gH / (winH ? winH : 1);

        EmitFirework(bx, by, 2000);
        return 0;
    }

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

    HWND hwnd = CreateWindowEx(
        0,
        cls,
        L"Particle Toy: Remake - Have fun!",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1600, 1200,
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
