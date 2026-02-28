# Particle Toy: Remake - TODO List

---

## Release 1.0

- [ ] **Particle count wired up** - Wire `IDC_EDIT_PARTICLES` so changing the value respawns particles at the new count.
- [X] **Fullscreen toggle** - F12 hotkey + `IDC_CHECK_FULLSCREEN` checkbox. Save pre-fullscreen style/rect, strip title bar, `SetWindowPos` to cover the monitor. Restore on toggle. Buffer adapts automatically via `WM_SIZE`.
- [X] **Original speed mode** - Replace the bounce toggle with an "Original Speed" checkbox. Puts a `Sleep(1)` in the render loop to match the original's frame pacing. The `Sleep(1)` is already in the code (commented out) — need to decide exact placement with John before wiring it up.
- [ ] **Original speed default** - Set to default on.
- [ ] **Resolution display** - Show current buffer dimensions in the control panel (e.g. `"1920 x 1080"`) using a static text label, updated on each resize. Same pattern as the live FPS counter.
- [X] **Closing the controls window closes the app** - Currently the control panel hides on close (`WM_CLOSE` returns `SW_HIDE`). For 1.0, closing it should post `WM_CLOSE` to the main window instead, so the two windows feel like one application.
- [ ] **Release build / GitHub Action** - Create a GitHub Action that produces a signed/zipped binary on every push to `master`.
- [ ] **Help System** - Create a way to show descriptions for each toggle.  Ideas include Window's help system, or temporarily replacing the controls text.
- [ ] **Github Actions** - Create a github actions pipeline to build the releases.
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

- [ ] **Split source file** - `MakeFireTest.cpp` is getting long. Split into logical units (fire sim, particles, UI/dialog, render) as we approach 1.0 feature-complete.
