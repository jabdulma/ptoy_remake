// ptoy-remake.cpp - minimal Win32 software backbuffer (GDI DIBSection)

#include <windows.h>
#include <cstdint>
#include <random>
#include <windowsx.h>



static int gW = 640;
static int gH = 360;

static BITMAPINFO gBmi = {};
static void* gPixels = nullptr;

//RNG more advanced generator than rand();
static std::mt19937 rng{ std::random_device{}() };
static std::uniform_int_distribution<int> dist(0, 255);


//Mouse Globals
static int gMouseX = 1;
static int gMouseY = 1;

uint32_t* pixelMem = nullptr;

static void InitBackbuffer(HWND hwnd)
{
    gBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    gBmi.bmiHeader.biWidth = gW;
    gBmi.bmiHeader.biHeight = -gH; // top-down
    gBmi.bmiHeader.biPlanes = 1;
    gBmi.bmiHeader.biBitCount = 32;
    gBmi.bmiHeader.biCompression = BI_RGB;

    // Allocate pixel memory (Windows will allocate; we just get the pointer)
    HDC hdc = GetDC(hwnd);
    HBITMAP dib = CreateDIBSection(hdc, &gBmi, DIB_RGB_COLORS, &gPixels, nullptr, 0);
    ReleaseDC(hwnd, hdc);

    // We don't actually need to keep the HBITMAP if we present with StretchDIBits,
    // but keeping it alive ensures the memory stays valid.
    static HBITMAP sKeepAlive = nullptr;
    sKeepAlive = dib;

    if (!dib || !gPixels)
    {
        MessageBox(hwnd, L"CreateDIBSection failed", L"Error", MB_OK);
        return;
    }

    // Simple gradient so we know the pipeline works
    if (!gPixels) return;
    pixelMem = (uint32_t*)gPixels;


}

static void RenderTestPatternOriginal()
{


    // Simple gradient so we know the pipeline works
    //uint32_t* p = (uint32_t*)gPixels;

    //Random window color
    for (int y = 0; y < gH; y++)
    {
        for (int x = 0; x < gW; x++)
        {
            //Test pattern
            uint8_t r = (uint8_t)(x * 255 / (gW - 1));
            uint8_t g = (uint8_t)(y * 255 / (gH - 1));
            uint8_t b = 0;

            pixelMem[y * gW + x] = (r << 16) | (g << 8) | (b);
        }
    }
}

static void RenderTestPattern(HWND hwnd)
{



    //Random window color
    for (int y = 0; y < gH; y++)
    {
        for (int x = 0; x < gW; x++)
        {
            //Test pattern
            uint8_t r = (uint8_t)(x * 255 / (gW - 1));
            uint8_t g = (uint8_t)(y * 255 / (gH - 1));
            uint8_t b = 0;

            pixelMem[y * gW + x] = (r << 16) | (g << 8) | (b);
        }
    }

    if ((unsigned)gMouseX >= (unsigned)gW || (unsigned)gMouseY >= (unsigned)gH)
        return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;

    int bx = gMouseX * gW / (winW ? winW : 1);
    int by = gMouseY * gH / (winH ? winH : 1);

    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
        {
            int x = bx + dx;
            int y = by + dy;
            if ((unsigned)x < (unsigned)gW && (unsigned)y < (unsigned)gH)
                pixelMem[y * gW + x] = 0x00FFFFFF;
        }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    //We need this to handle various events captured by the window.
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
            RenderTestPattern(hwnd);

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

        // continuously animate (we'll replace test pattern with sim later)
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        // Extract mouse coords from lParam
        gMouseX = GET_X_LPARAM(lParam);
        gMouseY = GET_Y_LPARAM(lParam);
        InvalidateRect(hwnd, nullptr, FALSE);

        SetCapture(hwnd);           // keep receiving mouse messages while held
        Sleep(1);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_CANCELMODE:
    case WM_KILLFOCUS:
        ReleaseCapture();
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
        L"Particle Toy: Reamke - Have fun!",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        nullptr, nullptr, hInstance, nullptr
    );

    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
