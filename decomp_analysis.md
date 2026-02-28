# Particle Toy (ptoy.exe) — Ghidra Decompilation Analysis

## Program Overview

- **Internal window title:** "Burning Particles"
- **Binary type:** 32-bit Win32, MSVC compiled, statically linked CRT
- **Display:** 8-bit paletted DIBSection (GDI), with DirectDraw for surface blitting
- **No DirectX rendering** — all pixel work is GDI/software

---

## Key Architecture

### Display System

The program uses an **8-bit paletted DIBSection** created with `CreateDIBSection(..., DIB_PAL_COLORS, ...)`.
- Pixel values (0–255) ARE the palette indices directly — no separate palette lookup loop
- The palette is managed via `CreatePalette` / `RealizePalette` / `SetDIBColorTable`
- Both an `HPALETTE` and a DIB color table are maintained in sync

### Rendering Pipeline (per frame)
1. Update particle positions, deposit heat into pixel buffer
2. Seed bottom row with new heat (random walk or Perlin noise)
3. Run fire diffusion on the 8-bit buffer
4. Sparkle effect (brighten random dark pixels per row)
5. Blit to screen via DirectDraw surface Blt OR GDI StretchBlt/BitBlt

### Frame Timing
**No vsync. No frame limiter.**

Main loop:
```c
while (no_message) {
    FUN_00403330();  // simulation + render
    Sleep(1);        // yield ~15ms (no timeBeginPeriod, WINMM not imported)
}
```
Effective ceiling: ~66fps on modern hardware. On 1998 hardware, CPU-bound.
DirectDraw is used only for the surface blit — NOT for WaitForVerticalBlank.

---

## Fire Diffusion (FUN_00403330)

### Formula
```c
result = (left + above + right + self - 6) >> 2;
if (result < 1) result = 0;
write to pixel_above;
```

**BURNFADE = 6, subtracted BEFORE the >>2 divide.**
Clamp is `< 1` (not `< 0`) — both 0 and negative clamp to 0.

Loop direction: top-to-bottom, writing to row[y-1] (above) from row[y] (current).
Heat propagates upward. In-place Gauss-Seidel update (new values used immediately).

### Loop Optimization: 32-bit Zero Skip
```c
if (*(int *)pbVar18 == 0) {
    // 4 pixels are all zero
    if (flag_array[col] != 0) {
        // row above had heat last frame: still process
    }
    flag_array[col] = 0;
} else {
    // pixels have heat: always process
    flag_array[col] = 1;
}
```
Reads 4 bytes at once to skip all-zero regions. Tracks per-column state to handle
heat that has risen above a zero row. This is the optimization mentioned in the
original blog post ("32-bit integer reads to skip chunks of the buffer that are all 0").

### Loop Unrolling
4 pixels processed per loop iteration (manually unrolled), each calculated independently.

---

## Bottom Row Seeding

Two modes controlled by `DAT_00410068`:

**Mode 0 — Random Walk** (default):
```c
seed = rand() & 0xff;  // random start
for each x:
    if pixel[x] != 0: seed = pixel[x];  // follow existing heat
    seed += rand() % 65 - 32;           // add -32 to +32
    clamp(seed, 0, 255);
    pixel[x] = seed;
```

**Mode 1 — Perlin Noise** (`FUN_00404550`):
```c
FUN_00404550(0x4c32f0, col * _DAT_00410048, _DAT_004100d8, 2, 0, 0);
pixel[col] = (uint8)clamp((int)(result * 255) + 64, 0, 255);
```

| Parameter | Value | Notes |
|-----------|-------|-------|
| x | `col * 0.03` | Raw pixel column × hardcoded scale; 1 period per ~33px |
| y | `_DAT_004100d8` | Scrolling Y phase, advances each frame |
| numOctaves | `2` | 3 octave passes |
| periodX | `0` | Infinite (no tiling) |
| periodY | `0` | Infinite (no tiling) |
| result offset | `+64` (`'@'`) | Shifts noise output up from baseline |

At 320px wide: ~10 periods of flame tongues across the bottom row.
`_DAT_00410048` (= 0.03) is a hardcoded data constant, not exposed in the dialog.

`_DAT_00410048` serves double duty — it is both the X scale factor and the Y scroll
increment per frame:
```c
_DAT_004100d8 += _DAT_00410048;   // advance Y phase each frame
x = col * _DAT_00410048;          // scale column to noise space
```
At 0.03/frame, the Y phase advances one full noise period (~1.0 unit) every ~33 frames.
The spatial period is also ~33px (1/0.03). So the pattern scrolls exactly one "blob
width" per 33 frames — scroll speed is intrinsically tied to visual frequency.

---

## Sparkle Effect

```c
// Per row, pick one random pixel
if (32 < pixel_value && pixel_value < 128):
    new_value = pixel_value * 2;  // double it, cap at 255
    write to: above, below, left, right  // 4 neighbors get the brightened value
```

Original thresholds: **32–127** (vs. remake's 40 threshold, fixed +80 boost).
Original effect: doubles the value and spreads to 4 neighbors.

---

## Particle System

### Particle Structure
Each particle is **9 doubles = 72 bytes**.

Fields (relative to internal `pdVar16` pointer within struct):
- `pdVar16[-2]`, `pdVar16[-1]`: position x, y (doubles)
- `pdVar16[0]`, `pdVar16[1]`: velocity dx, dy (doubles)
- `pdVar16[4]`, `pdVar16[5]`: target/attraction position (doubles)
- `pdVar16[6]` bit 0: gravity active flag
- `pdVar16[6]` bit 1: attraction/steering active flag
- `*(int*)((int)pdVar16 + 0x34)`: palette index / heat value (byte)

### Particle Initialization
```c
x = rand() % (width - 10) + 5;   // inset 5px from edges
y = rand() % (height - 6) + 3;
dx = dy = 0;                       // STATIONARY at spawn
heat = (rand() & 0x7f) + 0x80;   // range 128–255
```
**Particles spawn with zero velocity.** All motion added by mode handlers.

### Particle Rendering
Bresenham line drawing from previous to current position.
**3 pixels wide**: writes center pixel + pixel above + pixel below per step.
Uses `DAT_004c5560[]` lookup table for neighbor shade values (slightly different palette index).

### Bounce
```c
// x < 5 (left wall):
x = 5.0;
dx = |dx| * 0.95;  // 95% speed retained
dy += rand_kick * 0.5 * 0.0002;

// x >= width - 5 (right wall): mirror
// Same pattern for y walls (top: y < 3, bottom: y >= height - 3)
```
Speed constant on bounce: **0.95** (5% energy loss).
Random perpendicular kick factor: **0.5 × 0.0002**.

**No drag or friction.** Particles maintain full velocity between bounces — there is no
per-frame speed decay. Any perceived slowdown after an explosion is purely from gravity
curving trajectories, not from deceleration code.

### Steering / Attraction
Uses `fpatan` (atan2) to find angle to target, then blends velocity toward target by **5% per frame** (STEER_RATE = 0.05):
```c
new_angle = current_angle + angle_diff * 0.05;
```

### Leaders

Leaders are **not a separate type** — they are specific particles in the array designated
by index. Particles whose index is a multiple of 64 are leaders; all others are followers.

```c
// Per-particle, each frame:
uVar10 = 0x3F & particle_index;

if (uVar10 == 0) {
    // This particle IS a leader
    // Target = mouse cursor position
    target.x = mouse_x;
    target.y = mouse_y;
} else {
    // This particle is a follower
    // Target = its group leader's position
    leader_index = particle_index & ~0x3F;  // round down to nearest multiple of 64
    target.x = particles[leader_index].x;
    target.y = particles[leader_index].y;
}
```

`DAT_00410070` (dialog checkbox 0x3ed) controls group size:
- **Unchecked** (`== 0`): mask = `0x7FFF` → groups of 32768 (effectively one global leader)
- **Checked** (`!= 0`): mask = `0x3F` → groups of 64 particles per leader

The target assignment only runs when left mouse is held (`DAT_004100a4`) or auto-mode
fires an attractor event (`DAT_00410088`). When neither is active, particles just drift
under gravity with no steering target.

**Fallback angle** (`_DAT_004100c0`): incremented by 0.01 radians per frame, wraps at ±π.
Used as the steering reference direction when a particle's velocity is near zero,
preventing stationary particles from getting stuck with no direction to rotate from.

### Explosion (DAT_00410080)

Triggered by right-click (`WM_RBUTTONDOWN`) or auto-mode (case 2).

**Step 1 — Base speed** (computed once per explosion, random):
```c
speed = ABS((rand() % 10000 - 5000) * 8.0 * 0.0002) + 4.0;  // range [4.0, 12.0]
```

**Step 2 — Origin:**
- Right-click: cursor position, clamped to [5, width-6] × [3, height-4]
- Auto-mode: random position on screen
- `DAT_004100b0/b4` cleared to 0 after reading

**Step 3 — All particles teleported and scattered:**
```c
for each particle:
    position = origin
    perParticleSpeed = ABS((rand() % 10000 - 5000) * speed * 0.0002)  // [0, speed]
    angle = (rand() % 10000 - 5000) * π * 0.0002                      // [-π, +π]
    dx = cos(angle) * perParticleSpeed
    dy = sin(angle) * perParticleSpeed
```

Every particle is teleported to the origin and given a uniformly random direction and a
random speed in [0, base_speed]. Base speed varies per explosion ([4, 12]), so explosions
range from weak to violent. **No burst/decay — velocities are set once and not touched
again.** The only subsequent speed reduction is the 0.95 wall bounce multiplier.

### Gravity

Applied per-particle each frame if bit 0 of particle flags is set:
```c
if (flags & 1) {
    dx += gravity_x;  // _DAT_004c5670/_DAT_004c5674
    dy += gravity_y;  // _DAT_004c2588/_DAT_004c258c
}
```

Gravity direction cycles every 40 seconds (see Auto Mode section):
- 0–40s:  Y = +0.1 (downward), X = 0
- 40–80s: X = -0.1 (leftward)
- 80–120s: Y = -0.1 (upward), X = 0
- 120–160s: X = +0.1 (rightward)

Gravity enable is controlled by `DAT_00410074` (dialog checkbox 0x3eb).
The flags field is **recomputed every frame** from `DAT_00410074`, so the dialog's
live particle-walk update and the per-frame flag assignment are both consistent.

Particle flags field (`*(uint*)(pdVar16 + 2)`):
- Bit 0: gravity active
- Bit 1: steering/attraction active

---

## Color Palettes

Three built-in schemes, selected by `DAT_00410044`:

| Scheme | Index 0–127 | Index 128–255 | Effect |
|--------|-------------|----------------|--------|
| 0 (Fire) | R=i×2, G=i, B=0 | R=255, G=i, B=0 | Black→orange→yellow |
| 1 (Ice)  | R=0, G=i, B=i×2 | R=0, G=i, B=255 | Black→blue→cyan |
| 2 (Purple) | R=i, G=0, B=i | R=i, G=i-128, B=i | Black→purple→magenta |

Palette stored at `DAT_004c25e8` (512 bytes low half) + `DAT_004c27e8` (512 bytes high half).
Applied via `FUN_004019b0` → `FUN_004023d0` (CreatePalette + SetDIBColorTable).

---

## Auto Mode System

### Random Events (DAT_00410098 — auto-mode checkbox)

Each frame, three conditions are checked:
```c
if ((time_advanced_this_second) && (DAT_00410098 != 0) && (rand() % 20 == 0))
```
- `time_advanced_this_second`: only true once per wall-clock second
- `DAT_00410098`: auto-mode enabled (checkbox 0x3f3 in dialog)
- `rand() % 20 == 0`: 5% chance

Effective rate: **~5% chance per second** = one random event every ~20 seconds on average.

When triggered, picks one of 5 events at random:
```c
switch(rand() % 5) {
case 0: DAT_00410090 = 1;  // gravity direction change
case 1: DAT_00410094 = 1;  // freeze all particles
case 2: DAT_00410080 = 1;  // explosion at random position
case 3: DAT_0041009c = 1;  // comet (all particles, same direction)
case 4: DAT_004100a0 = 1;  // emit to center (all particles → screen center)
}
```
These are **identical flags** to the keyboard shortcuts — auto-mode literally fires
random "keypresses" on a timer.

### Time-Based Gravity Cycling

```c
switch((time() - start_time) / 40 % 4)
```
Every **40 seconds**, independently of the random events, cycles through 4 gravity states:
- Case 0: gravity Y = +0.1 (downward), gravity X = 0
- Case 1: gravity X = -0.1 (leftward)
- Case 2: gravity Y = -0.1 (upward), gravity X = 0
- Case 3: gravity X = +0.1 (rightward)

`_DAT_004c5670/_DAT_004c5674` = gravity X component (double)
`_DAT_004c2588/_DAT_004c258c` = gravity Y component (double)

These two systems (random events + gravity cycling) run simultaneously and independently.

---

## Resolution System

Width and height come from a lookup table:
```c
width  = DAT_0040f2c0[resolution_index * 8];
height = DAT_0040f2c4[resolution_index * 8];
```
`DAT_00410064` = resolution selector index (set from UI).

---

## Object Layout (Rendering Manager)

The main rendering object (`DAT_004c1820`) contains:

| Offset | Content |
|--------|---------|
| `+0x40` | HWND |
| `+0x44` | DIBSection object (~0x850 bytes) |
| `+0x89c` | `IDirectDrawSurface*` (destination/primary) |
| `+0x8a0` | `IDirectDrawSurface*` (source/offscreen) |
| `+0x8a4` | Another DD interface (palette) |
| `+0x8a8` | Palette RGB staging array (1024 bytes) |
| `+0xcd8` | Lock/pause guard counter |
| `+0xcf4` | `DDSURFACEDESC` (size = 0x6c = 108 bytes) |

### DIBSection Sub-Object (at offset +0x44)

| Offset | Content |
|--------|---------|
| `+0x00` | Stride/pitch (bytes per row, 4-byte aligned) |
| `+0x04` | Bit depth (= 8) |
| `+0x08` | HWND |
| `+0x14` | `BITMAPINFOHEADER` (40 bytes) |
| `+0x3c` | `RGBQUAD[256]` color table (1024 bytes) |
| `+0x43c` | `HBITMAP` |
| `+0x440` | Saved `HGDIOBJ` |
| `+0x444` | Pixel data pointer (returned by CreateDIBSection) |
| `+0x448` | `LOGPALETTE` header |
| `+0x44c` | `PALETTEENTRY[256]` (1024 bytes) |
| `+0x84c` | `HPALETTE` |

---

## FUN_00401210 — Window + DirectDraw Initialization

**WNDCLASSEXA embedded in object at `+0x10`** (48 bytes):
- style = 3 (`CS_HREDRAW | CS_VREDRAW`)
- lpfnWndProc = `FUN_00402bb0`
- hCursor = `IDC_ARROW`
- lpszClassName = `"Burning Particles"`

Window created with style `0xCF0000` = `WS_OVERLAPPEDWINDOW`.
**Centered on screen** using `GetSystemMetrics` + `AdjustWindowRectEx` + `MoveWindow`.

DirectDraw setup:
```c
DirectDrawCreate(NULL, &this->0x898, NULL);       // IDirectDraw* at +0x898
IDirectDraw::SetCooperativeLevel(hwnd, DDSCL_NORMAL=8);  // windowed mode
```
`DDSCL_NORMAL` = windowed cooperative level. DirectDraw never goes exclusive/fullscreen
at the API level — fullscreen is just a window resize.

Updated object layout (outer rendering manager):
| Offset | Content |
|--------|---------|
| `+0x10`–`+0x3f` | `WNDCLASSEXA` (48 bytes, embedded) |
| `+0x40` | HWND |
| `+0x44` | DIBSection sub-object (~0x850 bytes) |
| `+0x898` | `IDirectDraw*` |
| `+0x89c` | `IDirectDrawSurface*` (destination/primary) |
| `+0x8a0` | `IDirectDrawSurface*` (source/offscreen) |
| `+0x8a4` | DD palette interface |
| `+0x8a8` | Palette RGB staging (1024 bytes) |
| `+0xcc4` | Bytes per pixel (cached from lock) |
| `+0xcc8` | Width (cached) |
| `+0xccc` | Height (cached) |
| `+0xcd0` | Pitch (cached) |
| `+0xcd4` | Pixel data pointer (cached) |
| `+0xcd8` | Lock counter / pause guard |
| `+0xcdc` | Window X position (centered) |
| `+0xce0` | Window Y position (centered) |
| `+0xce4` | Canvas width |
| `+0xce8` | Canvas height |
| `+0xcec` | Window width (with chrome) |
| `+0xcf0` | Window height (with chrome) |
| `+0xcf4` | `DDSURFACEDESC` (108 bytes) |

---

## Lookup Tables (initialized in WinMain / FUN_00402950)

Two 256-byte lookup tables built at startup:

**`DAT_004c31f0[]`** — "dim by 4" table:
```c
DAT_004c31f0[i] = max(i - 4, i & 0xC0)  // each entry is i minus 4, floored at nearest-64
```

**`DAT_004c5560[]`** — neighbor shade table (used in particle line drawing):
Similar formula using FPU value, produces a slightly darker palette index for each
input index. Used to draw the pixel above and below each particle position in a
slightly dimmer color for a 3D trail effect.

---

## Key Function Map

| Address | Name | Description |
|---------|------|-------------|
| `FUN_004021e0` | `DIBSection_Init` | Creates DIBSection, sets up BITMAPINFOHEADER |
| `FUN_004023d0` | `DIBSection_SetPalette` | Fills LOGPALETTE + RGBQUAD, calls CreatePalette + SetDIBColorTable |
| `FUN_004024c0` | `DIBSection_RealizePalette` | Selects + realizes GDI palette into window DC |
| `FUN_00402580` | `RenderSurface_BitBlt` | 1:1 blit to window |
| `FUN_004025e0` | `RenderSurface_StretchBlt` | Scaled blit to window |
| `FUN_00401a60` | `RenderSurface_Present` | Main present: DirectDraw Blt or GDI blit |
| `FUN_00402850` | `BuildPalette` | Builds one of 3 color schemes into palette staging area |
| `FUN_004019b0` | `ApplyPalette` | Copies palette staging → DIBSection object, triggers CreatePalette |
| `FUN_00403330` | `SimulateAndRender` | Main per-frame: particles, diffusion, seeding, blit |
| `FUN_00402950` | `WinMain` | Entry point, main message loop |
| `FUN_00404550` | `PerlinNoise1D` | 1D Perlin noise for smooth flame base |
| `FUN_00404e90` | `SeedRNG` | Seeds the random number generator with time() |
| `FUN_00401210` | `InitWindow` | Creates WNDCLASSEX, window, centers on screen, initializes DirectDraw |
| `FUN_00402bb0` | `WndProc` | Window message handler (mouse, keyboard, paint) |
| `FUN_00402e80` | `ControlPanelDlgProc` | Dialog procedure for the settings panel |
| `FUN_00401c60` | `RenderSurface_Lock` | Locks pixel buffer, returns dimensions + pointer |
| `FUN_00402760` | `Timer_Start` | QueryPerformanceCounter timestamp |
| `FUN_00402770` | `Timer_ElapsedUs` | Elapsed microseconds since last Timer_Start |

---

## WndProc (FUN_00402bb0) — Full Control Map

### Mouse Input
| Message | Value | Action |
|---------|-------|--------|
| `WM_MOUSEMOVE` | 0x200 | Store cursor X/Y → `DAT_004100b8` (X), `DAT_004100bc` (Y) |
| `WM_LBUTTONDOWN` | 0x201 | `DAT_004100a4 = 1` — activates particle attraction toward cursor |
| `WM_LBUTTONUP` | 0x202 | `DAT_004100a4 = 0` |
| `WM_RBUTTONDOWN` | 0x204 | `DAT_00410080 = 1` (explosion flag) + store cursor X/Y → `DAT_004100b0/b4` |

**Right-click does NOT change BURNFADE.** The visual impression of a hotter burn is
purely from particles depositing concentrated heat at high speed. BURNFADE = 6 always.

### Keyboard Input (WM_KEYDOWN = 0x100)
| Key | VK Code | Effect |
|-----|---------|--------|
| Space | 0x20 | `DAT_00410094 = 1` — freeze: zero all particle velocities |
| Enter | 0x0d | `DAT_0041009c = 1` — comet: all particles same position + direction |
| Backspace | 0x08 | `DAT_004100a0 = 1` — emit: move all particles to screen center, stopped |
| Escape | 0x1b | `DestroyWindow` — quit |
| F12 | 0x7b | Toggle fullscreen (`DAT_00410078`), send dialog update, set reinit flag |

### Window Messages
| Message | Value | Action |
|---------|-------|--------|
| `WM_CREATE` | 0x01 | Return 0 (no-op) |
| `WM_DESTROY` | 0x02 | `PostQuitMessage(0)` |
| `WM_CLOSE` | 0x10 | `DestroyWindow` |
| `WM_PAINT` | 0x0f | `BeginPaint` → blit only (`FUN_00401a60`) → `EndPaint`. No simulation. |
| `WM_ACTIVATE` | 0x06 | `DAT_004100a8 = wParam >> 16` — pause when minimized |
| `WM_ACTIVATEAPP` | 0x1c | `DAT_004c25e0 = wParam` — gates main loop rendering |

### Global Flag Map
| Address | Meaning | Set by |
|---------|---------|--------|
| `DAT_004100a4` | Left mouse held | WM_LBUTTONDOWN/UP |
| `DAT_004100b8/bc` | Current mouse X, Y | WM_MOUSEMOVE |
| `DAT_004100b0/b4` | Explosion origin X, Y | WM_RBUTTONDOWN |
| `DAT_00410080` | Explosion trigger | WM_RBUTTONDOWN / auto-timer case 2 |
| `DAT_00410094` | Freeze particles | Space key |
| `DAT_0041009c` | Comet mode | Enter key |
| `DAT_004100a0` | Emit-to-center mode | Backspace key |
| `DAT_00410090` | Gravity change | Auto-timer case 0 |
| `DAT_00410078` | Fullscreen flag | F12 key |
| `DAT_0041008c` | Reinit needed | F12 key |
| `DAT_004100a8` | Paused/minimized | WM_ACTIVATE |
| `DAT_004c25e0` | App active | WM_ACTIVATEAPP |
| `DAT_00410068` | Bottom row mode (0=random, 1=Perlin) | UI dialog |
| `DAT_00410064` | Resolution index | UI dialog |
| `DAT_00410044` | Color scheme (0/1/2) | UI dialog |

---

## Developer Identity

**Longbow Digital Arts** — small Canadian studio, late 1990s.
- Email (from dialog button): `longbow@sympatico.ca` (sympatico.ca = Canadian ISP)
- Website (from dialog button): `http://www.longbowdigitalarts.co...`

---

## Control Panel Dialog (FUN_00402e80 / DAT_00402e80)

### Controls

| Control ID | Type | Variable | Meaning |
|------------|------|----------|---------|
| `0x3e9` | Checkbox | `DAT_00410078` | Fullscreen toggle (triggers reinit) |
| `0x3ea` | Combobox | `DAT_00410044` | Color scheme — calls `BuildPalette` on change |
| `0x3eb` | Checkbox | `DAT_00410074` | Gravity enable — live-updates ALL particle flags |
| `0x3ec` | Checkbox | `DAT_0041006c` | Unknown (likely individual vs. shared attraction) |
| `0x3ed` | Checkbox | `DAT_00410070` | Unknown (possibly sparkles or burn direction) |
| `0x3ee` | Checkbox | `DAT_00410068` | Bottom row mode: 0=random walk, 1=Perlin noise |
| `0x3ef` | Edit | `DAT_00410040` | Particle count (max capped at 10000) |
| `0x3f3` | Checkbox | `DAT_00410098` | Auto-mode cycling enable |
| `1000` | Combobox | `DAT_00410064` | Resolution selector |
| `0x3f0` | Button | — | `ShellExecute` mailto:longbow@sympatico.ca |
| `0x3f1` | Button | — | `ShellExecute` website URL |

### Color Scheme Names (embedded strings, 100 bytes apart)
- Index 0: `"Fiery Orange"` — matches palette 0 (black→orange→yellow) ✓
- Index 1, 2: next strings at +100, +200 bytes in binary (blue and purple names)

### Resolution Options (strings 100 bytes apart)
First entry: `"320 x 240 (too fast)"` — developer's own label. At 320×240 with no
frame limiter the simulation runs too fast to be enjoyable. Reveals hardware assumptions
from 1998 development.

### Gravity Toggle — Live Particle Update
When checkbox 0x3eb changes, immediately walks all particles (72-byte stride) and
sets or clears bit 0 of each particle's flags field:
```c
for each particle (+= 72 bytes):
    gravity ON:  particle->flags |= 1;
    gravity OFF: particle->flags &= ~1;
```
Confirms 72-byte particle stride from particle loop analysis.

### Perlin Noise Object (FUN_00404550)
Classic multi-octave Ken Perlin noise:
- Object at `0x004c32f0`: permutation table at `+0x2050`, gradient table at `+0x50`
- Octaves: 0–6 (returns 0.5 if out of range)
- Smoothstep: `(3 - 2t) * t * t`
- Period params: 0 = infinite; non-zero = tiled noise
- Used for bottom row seeding in mode 1 with numOctaves=2, x=`col*0.03`, periodX=0, periodY=0
- Amplitude table (`DAT_0040f4d8`): `1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625` (confirmed from binary)
- Output scale table (`DAT_0040f510`): `1/partial_sum` per octave count (confirmed from binary)

---

## FUN_00401c60 — RenderSurface_Lock

Locks the pixel buffer for CPU access. On first call per frame:
1. `GdiFlush()` — ensures GDI is finished before CPU touches the buffer
2. Queries DIBSection for pixel pointer, width, height, pitch, bytes-per-pixel
3. Caches values in object at offsets `+0xcc4` through `+0xcd4`
4. Increments lock counter at `+0xcd8`

Returns 5 values into `DAT_004c5540`:
| Address | Content |
|---------|---------|
| `DAT_004c5540` | Bytes per pixel (= 1) |
| `DAT_004c5544` | Width |
| `DAT_004c5548` | Height |
| `DAT_004c554c` | Pitch (stride) |
| `DAT_004c5550` | Pixel buffer base pointer |

DirectDraw path uses `IDirectDrawSurface::Lock` (vtable offset 0x64) instead.

---

## Answers to Priority Questions

| Question | Answer |
|----------|--------|
| BURNFADE value | **6** |
| BURNFADE position | **Before the >>2 divide**: `(sum - 6) >> 2` |
| Frame limiting | **None** — `Sleep(1)` only, no vsync |
| Diffusion operates on | **Single bytes** (8-bit palette indices) |
| AltColor present | **Not confirmed** — no per-particle heat decay seen |
| Particle speed at emission | **Zero** — particles spawn stationary |
| Bottom row algorithms | **Two**: random walk (±32) and Perlin noise |
| Color schemes | **Three**: fire (orange/yellow), ice (blue/cyan), purple/magenta |
