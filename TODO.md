# Particle Toy: Remake - TODO List

## Bugs / Behavior Fixes
- [ ] **Speed fix for steering from frozen state** - When particles are frozen (space) and then clicked, they get explosion-level speed instead of a gentler pull. Need a lower initial speed for the steering kick vs firework emit.
- [ ] **Tweak sparkle effect** - Sparkles not quite right yet. Revisit threshold, boost values, and possibly frequency.

## Features to Implement
- [ ] **Gravity system** - Rotating gravity direction (original cycles every ~50 seconds through down/left/up/right). Needs a toggle.  Let user control gravity direction as well.
- [ ] **Follow-the-leader** - A randomly moving leader on screen that particles follow via steering. Particle struct already has `leaderIdx` stub.
- [ ] **Multiple leaders** - 1-5 leaders on screen, particles randomly assigned to follow one. Extension of follow-the-leader.
- [ ] **Bottom fire** - Original has a smoother bottom fire, probably due to "Pixel walking" - make a similar adjustment.
- [ ] **Perlin noise bottom fire** - Original uses Perlin noise for smooth, coherent flame pillars along the bottom. Our current pure-random seeding is more chaotic. Needs a toggle to switch between styles.
- [ ] **Perlin noise bottom fire** - Toggle for the fire types mentioned above?
- [ ] **Sunburst explosion mode** - All particles get the same speed (creates a clean expanding ring). 50/50 coin flip in original. Fun optional mode.
- [ ] **AltColor tuning** - Per-frame particle heat fade + bounce brightening. Implemented but currently disabled because it flattens the color. Needs tuning to look right with our palette system.
- [ ] **Original Speed Calc** - This is a signficant add on but allow the user to have the original base speed calculation.  Pixels per frame, not resolution indpendent.

## UI / Control Panel Wiring
- [ ] **SIMD toggle** (line 61) - Wire `useSIMD` to a checkbox in the dialog.
- [ ] **Sparkle toggle** (line 65) - Wire `useSparkles` to a checkbox in the dialog.
- [ ] **Frame limiter toggle** (line 92) - Wire `useFrameLimiter` to a checkbox in the dialog.
- [ ] **Color scheme selector** (line 128) - Wire `currentPalette` to the existing combo box (`IDC_COMBO_PALETTE`). Update palette presets in dropdown.
- [ ] **Nitro toggle** (line 128) - Wire `useNitro` to a checkbox in the dialog.
- [ ] **AltColor toggle** (line 339) - Wire `useAltColor` to a checkbox in the dialog.
- [ ] **Palette combo box** (line 757) - Hook up `CBN_SELCHANGE` to rebuild palette with selected scheme.
- [ ] **Bounce toggle** (line 763) - Hook up `IDC_CHECK_BOUNCE` to control wall bounce behavior.
- [ ] **Gravity toggle** - Add checkbox for gravity enable/disable (once gravity is implemented).
- [ ] **Leader toggle** - Add controls for leader mode (once leaders are implemented).
- [ ] **Particle count** - Wire `IDC_EDIT_PARTICLES` to control how many particles are emitted.
- [ ] **Speed multiplier** - Expose `userSpeedMultiplier` as a slider or input.

## Code refactor
- [ ] Code will eventually need to be split up, it's becoming cluttered.

 
## Polish / Future Ideas
- [ ] **UI layout pass** - Redesign control panel to accommodate all toggles without being overwhelming. Consider grouping by category (fire, particles, display).
- [ ] **Performance on older machines** - Profile and test on lower-end hardware. SIMD toggle exists for this.
- [ ] **Delta-time movement** - Decouple simulation speed from frame rate. Currently frame-rate dependent. Non-trivial change.
- [ ] **Color cycling** - Original smoothly interpolates between color schemes over time. Cool screensaver-like effect.

## Release 
- [ ] **Release system / Git Actions ** - Create a git action to create a binary every time we release to master.