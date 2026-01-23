# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Particle Toy: Remake is a Windows-native interactive fire simulation using a heat buffer technique. It's a minimal Win32 application written in C++20 that renders a classic Seumas McNally fire algorithm.

## Build Commands

This is a Visual Studio 2022 project. Build from command line with MSBuild:

```bash
# Build Release x64 (recommended)
msbuild ptoy-remake.vcxproj /p:Configuration=Release /p:Platform=x64

# Build Debug x64
msbuild ptoy-remake.vcxproj /p:Configuration=Debug /p:Platform=x64
```

Or open `ptoy-remake.slnx` in Visual Studio 2022 and build with Ctrl+Shift+B.

Output location: `x64/Release/` or `x64/Debug/`

## Architecture

**Two-layer rendering system:**
1. **Heat buffer** (`gHeat`, 8-bit per pixel) - Stores heat intensity 0-255, used for physics simulation
2. **Display buffer** (`pixelMem`, 32-bit RGBA) - DIBSection for screen output, populated via palette lookup

**Core algorithm in `RenderFire()`:**
1. Mouse input injects heat=255 in 5x5 region
2. Heat diffusion: `new_heat = (self + left + right + below) / 4 - BURNFADE`
3. Palette lookup maps heat values to RGB colors (black → red → yellow → white)

**Key constants:**
- `gW=640, gH=360` - Buffer dimensions (scaled to window on display)
- `BURNFADE=1` - Heat decay rate per frame
- `DONTBURN=1` - Border pixels to skip for bounds safety

## Source Files

- `MakeFireTest.cpp` - Main implementation with fire simulation (active)
- `ptoy-remake.cpp.disabled` - Archived test pattern implementation
- `framework.h` - Windows SDK includes with WIN32_LEAN_AND_MEAN
- `Resource.h` - Auto-generated resource identifiers

## Runtime Behavior

- Left-click and drag to inject heat and create fire effects
- Window displays 640x360 buffer scaled to window size via StretchDIBits
- Continuous WM_PAINT invalidation drives the render loop
