# Particle Toy: Remake - TODO List

---

## Release 1.0

- [X] **Particle count wired up** - Wire `IDC_EDIT_PARTICLES` so changing the value respawns particles at the new count.
- [X] **Fullscreen toggle** - F12 hotkey + `IDC_CHECK_FULLSCREEN` checkbox. Save pre-fullscreen style/rect, strip title bar, `SetWindowPos` to cover the monitor. Restore on toggle. Buffer adapts automatically via `WM_SIZE`.
- [X] **Original speed mode** - Replace the bounce toggle with an "Original Speed" checkbox. Puts a `Sleep(1)` in the render loop to match the original's frame pacing. The `Sleep(1)` is already in the code (commented out) — need to decide exact placement with John before wiring it up.
- [X] **Original speed default** - Set to default on.
- [X] **Resolution display** - Show current buffer dimensions in the control panel (e.g. `"1920 x 1080"`) using a static text label, updated on each resize. Same pattern as the live FPS counter.
- [X] **Closing the controls window closes the app** - Currently the control panel hides on close (`WM_CLOSE` returns `SW_HIDE`). For 1.0, closing it should post `WM_CLOSE` to the main window instead, so the two windows feel like one application.
- [ ] **Release build / GitHub Action** - Create a GitHub Action that produces a signed/zipped binary on every push to `master`.
- [X] **Help System** - Create a way to show descriptions for each toggle.  Ideas include Window's help system, or temporarily replacing the controls text.
- [X] **Github Actions** - Create a github actions pipeline to build the releases.
---

## Release 1.x

- [ ] **Hide/show control panel without closing the sim** - UX idea: a small always-on-top toolbar or a tray icon that lets the user dismiss and recall the control panel independently of the sim window. The sim should keep running with the panel hidden. Alt+C or a corner button on the sim window are other options.
- [ ] **Expanded control panel** - Either a "More..." expand button that grows the dialog, or a larger window overall. Needed to fit speed/size sliders and future controls without crowding.
- [ ] **Speed slider** - Expose `userSpeedMultiplier` as a trackbar.
- [ ] **Size slider** - Control `particleSize` (heat deposit radius) via a trackbar.
- [ ] **Nitro colors** - Wire `useNitro` to a checkbox; rebuilds palette with blue boost at high heat for white-hot tips.
- [ ] **Implement a config system** - Allow users to store defaults in something like a conf or ini file.  Format to be determined.

---

## Bugs / Behavior

- [ ] **Speed kick from frozen state** - Frozen particles (Space) get explosion-level speed on first click instead of a gentle pull. Need a lower initial speed for the steering kick path.
- [X] **Sparkle effect tuning** - Threshold, boost, and frequency not quite right yet. Revisit after other changes settle.
- [X] **AltColor tuning** - Per-frame heat fade + bounce brightening is implemented but disabled (flattens colors). Needs tuning to work with the current palette system.

---

## Ideas / Future

- [ ] **Custom colors** - Two `CHOOSECOLOR` pickers feeding directly into `BuildPalette`. Natural extension of the existing palette system.
- [ ] **Perlin debug window** - Secondary window rendering the Perlin noise output as a grayscale scrolling DIBSection. Useful for tuning and a cool easter egg. Taps directly into `gPerlinY` / `PerlinNoise2D`.
- [ ] **Multi-window particle bouncing** - Single-process, multiple sim windows. Particles use world-space coordinates spanning monitor boundaries; each window renders its viewport. Allows multi-monitor support where particles physically cross between screens.
- [ ] **Color cycling** - Smoothly interpolate between color schemes over time. Screensaver-style effect.
- [ ] **Sunburst explosion mode** - All particles same speed (clean expanding ring). 50/50 mode flip, like the original.

---

## Performance Ideas

- [ ] **Sparkle pre-generated random table** - Replace per-frame `rng() % gW` in the sparkle loop (currently ~20% of frame time) with a pre-generated table of x-coordinates built once at startup and on resolution change. Eliminates the `div` instruction from the hot path. Concern: a 4096-entry table cycles every ~3.4 frames at 1200 rows, which may produce a visible repeating pattern. Worth testing with a larger table or a hybrid approach.
- [ ] **SIMD DiffuseHeat** - The heat diffusion loop touches every pixel every frame (~2M uint8_t ops at 1600x1200). Highly SIMD-friendly: stride-1 memory, simple integer arithmetic, no data dependencies between rows. SSE2 `__m128i` would process 16 pixels at a time for a potential ~16x speedup on that loop.

---

## Code Health

- [ ] **Split source file** - `MakeFireTest.cpp` is getting long. Split into logical units per the plan below.

---

## File Split Plan

**Context:** `MakeFireTest.cpp` is ~1800 lines. The goal is to split it into focused, readable translation units without restructuring the code or introducing architectural churn.

### Architectural decision — shared state

Nearly every module touches the simulation settings flags, `gW`/`gH`, `gHeat`, and the RNG objects. Rather than scattering definitions or wrapping everything in accessor functions, keep all variable *definitions* in `MakeFireTest.cpp` and introduce a `globals.h` with `extern` declarations. Each new `.cpp` includes `globals.h` to read or write shared state.

### Proposed file layout

| File | What it contains | Approx. lines |
|------|-----------------|---------------|
| `globals.h` | `extern` declarations for all shared simulation state; shared struct defs (`ResPreset`, `ColorScheme`, `PendingParticle`, `ControlHint`) | new, ~80 |
| `perlin.cpp/.h` | `InitPerlinTable()`, `PerlinNoise2D()`, all Perlin tables and constants | extracted, ~60 |
| `fire_sim.cpp/.h` | `BuildPalette()`, `DiffuseScalar()`, `DiffuseSSE2()`, `HasSSE2()`, `diffuseHeat` pointer, `PALETTE_PRESETS` | extracted, ~200 |
| `particles.cpp/.h` | `EmitParticle()`, `EmitFirework()`, `SpawnParticles*()`, `DrainPendingBurst()`, `UpdateParticles()`, `SteerParticles()`, `DepositHeatLine[Heavy]()`, speed system, bounce constants | extracted, ~380 |
| `dialog.cpp` | `ControlPanelProc()`, `HintSubclassProc()`, hover hint data (`gControlsDefaultText`, `gControlHints`, `gCurrentHintCtrl`) | extracted, ~250 |
| `MakeFireTest.cpp` | All global variable *definitions*, `InitBackbuffer()`, `ChangeResolution()`, `ToggleFullscreen()`, `UpdateResDisplay()`, `UpdateRefreshRate()`, `RenderFire()`, `WndProc()`, `wWinMain()` | trimmed, ~600 |

`RenderFire()` stays in `MakeFireTest.cpp` — it is the master per-frame coordinator that calls into every other module, and its role is clearest when read alongside `WndProc`.

### Extraction order (least-coupled first)

Do each step, build (`msbuild /p:Configuration=Release /p:Platform=x64`), run a quick visual check, then move on. Don't accumulate steps.

- [ ] **Step 1 — `perlin.cpp/.h`**
  - Move: `gPerlinPerm`, `gPerlinGrad`, `gPerlinY`, `kPerlinAmp`, `kPerlinScale`, `InitPerlinTable()`, `PerlinNoise2D()`
  - One extern needed: `rng` (defined in MakeFireTest.cpp, declared in `globals.h`)
  - Verify: Perlin fire mode still produces smooth animated flame base

- [ ] **Step 2 — `fire_sim.cpp/.h`**
  - Move: `ColorScheme`, `PALETTE_PRESETS`, `NUM_PALETTE_PRESETS`, `BuildPalette()`, `SPARKLE_LOW`/`SPARKLE_HIGH`, `HasSSE2()`, `diffuseHeat` function pointer + `using DiffusionFunc`, `DiffuseScalar()`, `DiffuseSSE2()`
  - Also extract the inline sparkle block from `RenderFire()` into a `SparkleEffect()` function here — keeps the constants and logic together, and puts the pre-generated table (performance TODO) in the right place when that work happens
  - Externs needed: `gHeat`, `gW`, `gH`, `gPalette`, `BURNFADE`, `BORDER_MARGIN`, `useSIMD`, `useNitro`, `useSparkles`, `rng`
  - Verify: fire diffuses, SIMD toggle switches implementations, palette changes work, sparkles still shimmer

- [ ] **Step 3 — `particles.cpp/.h`**
  - Move: `PendingParticle` struct, `gPendingBurst`, `gBurstStartTime`, `gPendingBurstIdx`, `gFallbackAngle`, speed system constants + `UpdateSpeedFactor()`, bounce/steer constants (`BOUNCE`, `KICK_STRENGTH`, `STEER_RATE`), AltColor state (`useAltColor`, `HEAT_FADE`, `HEAT_FLOOR`, `BOUNCE_BRIGHTEN`), `EmitParticle()`, `EmitFirework()`, `SpawnParticlesRandom()`, `EmitStartupBurst()`, `SpawnParticles()`, `DrainPendingBurst()`, `SteerParticles()`, `UpdateParticles()`, `DepositHeatLine()`, `DepositHeatLineHeavy()`
  - Externs needed: `gHeat`, `gW`, `gH`, `particles`, `rng` + distributions, all `use*` / gravity flags, `PARTICLE_SPEED_FACTOR`, `fpsFrequency`, `gControlPanel`, `mouseDown`, `rightMouseDown`
  - Verify: particles move, bounce, deposit heat, startup burst drip-feeds correctly

- [ ] **Step 4 — `dialog.cpp`**
  - Move: `gControlsDefaultText`, `ControlHint` struct, `gControlHints`, `gCurrentHintCtrl`, `HintSubclassData` struct, `HintSubclassProc()`, `ControlPanelProc()`
  - Externs needed: essentially everything in `globals.h` (it reads and writes most settings and calls functions from every other module)
  - The `#pragma comment(lib, "comctl32.lib")` and `#include <commctrl.h>` belong here; remove from `MakeFireTest.cpp`
  - Verify: all checkboxes, dropdowns, hover hints, particle count edit, and close behavior work

- [ ] **Step 5 — Trim `MakeFireTest.cpp`**
  - What remains: all global variable definitions, `InitBackbuffer()`, `ChangeResolution()`, `ToggleFullscreen()`, `UpdateResDisplay()`, `UpdateRefreshRate()`, `RenderFire()`, `WndProc()`, `wWinMain()`
  - Add all new `.cpp` files to `ptoy-remake.vcxproj`
  - Verify: full release build is clean, no duplicate symbol errors

### Gotchas to watch for

- `rng` and the `dist`/`angleDist`/`speedVariance`/`chance` distribution objects are used by Perlin, fire seeding, and particle spawning — define them once in `MakeFireTest.cpp`, declare `extern` in `globals.h`
- `PendingParticle` is currently defined inline in `MakeFireTest.cpp`; move its definition to `particles.h` before extracting
- `ControlPanelProc` calls `ChangeResolution()`, `BuildPalette()`, `SpawnParticles()`, `EmitParticle()`, `ToggleFullscreen()` — those functions must be declared in the headers of the files they move to
- `BURNFADE` and `BORDER_MARGIN` are `const int` — if used in multiple translation units, declare them `inline constexpr` in `globals.h` or give them external linkage explicitly
- Forward declarations in `MakeFireTest.cpp` (lines ~238–244) become redundant once proper headers exist — remove them to avoid confusion
