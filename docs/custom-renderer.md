# Experimental custom field renderer

The default is the original PPU renderer at **4:3**. Enable
**Widescreen (Experimental)** in Mods, then choose **Fit to window**,
**16:9**, or **21:9** to opt in. Disabling the mod restores the original
renderer, with no custom composition or actor observation in normal play.
Fit reveals additional horizontal map area as the window grows. It keeps
the native 224-line height and 7:6 pixel aspect;
it does not zoom out vertically to fit an entire level. The logical width is
256–1024 pixels, approximately 4:3 through 16:3. Narrow windows and fixed
ratios use letterboxing. Standard battle backgrounds expose any authored
horizontal borders. Menus, the title and unrecognized scene contracts stay
centered at native size.

The package remains opt-in. Its existing package/feature IDs are retained
so saved enable/disable selections continue to work. The former split-HUD
option has been removed; UI and battle combatants retain native placement.

## Try the prepared Windows build

From this game worktree:

```powershell
./tools/run_custom_renderer.ps1
```

The helper refreshes a copy of the executable, assets, and mod catalog under
`build-custom/playtest`. New settings start with widescreen disabled; enable
it in Mods to use the custom renderer. Existing mod choices and saves are
preserved, including choices saved by earlier playtest builds. Earlier
helpers enabled widescreen automatically; disable it once in Mods if you
want stock rendering in that existing playtest. It uses the verified
`smrpg.sfc` in this worktree when available; the launcher can also choose a
ROM. To skip the launcher and use the saved mod selection:

```powershell
./tools/run_custom_renderer.ps1 -DirectRomPath ./smrpg.sfc
```

`-CheckOnly` prints the paths; `-PrepareOnly` stages the playtest without
launching. `-RuntimeBin` selects the MinGW runtime DLL directory. The helper
writes `stdout.log` and `stderr.log` inside the playtest directory.

This work was developed on paired branches `codex/smrpg-custom-renderer`
and `codex/smrpg-custom-renderer-engine`, starting from game `b722c0b` and
engine `b04d8fa`. Release v0.0.4 pins launcher `e0ae4bc` and engine `fda93d2`.
The engine adds the read-only SA-1 instruction observer needed for
frame-correct actor snapshots. A recursive release checkout includes the
required dependencies; the commands below use the paired development checkout.

```powershell
$env:PATH = 'C:/msys64/mingw64/bin;' + $env:PATH
& C:/msys64/mingw64/bin/cmake.exe -S . -B build-custom -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/ninja.exe `
  -DSNESRECOMP_ROOT=../_wt-smrpg-custom-renderer-engine `
  -DSNESRECOMP_STATIC_RUNTIME=OFF `
  -DSDL3_DIR=../_tools/SDL3-3.4.12/x86_64-w64-mingw32/lib/cmake/SDL3
& C:/msys64/mingw64/bin/cmake.exe --build build-custom -j 8
& C:/msys64/mingw64/bin/ctest.exe --test-dir build-custom --output-on-failure
```

As with the normal build, generate `src/gen` from the verified ROM first.
The prepared worktree already contains generated files and the build.

## How it works

The guest PPU always renders 256×224. The old shadow-map widening and HUD
anchors are removed. The host copies the native raster plus each scanline's
registers, VRAM, palette and OAM. Window changes and paused redraws consume
that immutable frame without running guest logic again.

The compositor reads decompressed BG1/BG2/BG3 metatiles from WRAM, handles the
32/64/128-column assignment layouts, and matches them against the current
native tilemaps to recover each layer's camera. It follows connected room
geometry without the old ten-tile extension limit. BG3 assignments use
one-byte metatile IDs and 2bpp graphics; the main-screen layer supplies
Bowser's Keep's red-cloud sky. Repeating map axes wrap using the game's
bounds; the BG3 subscreen effect plane supplies water and similar color-math
effects. Native HDMA apertures, brightness and palette changes are applied
per line. Low-confidence map matches fall back to the native image.

Standard battles use a separate finite BG1 tilemap, uploaded to VRAM
`$4000`. The renderer samples existing background pixels outside the native
256-pixel view without wrapping or extending blank map space. The castle
arena contains only 272 pixels of artwork, with eight hidden pixels on each
side of the native camera. Its remaining margins stay black. Other layouts
are expanded only when they match the supported battle register contract.
HUD, menus and combatants remain in the untouched native image. The title's
BG1/BG2 maps are 256 pixels wide and 512 tall; there is no extra horizontal
title artwork to expose.

At retail US `$C0:AAF3`, a read-only SA-1 observer copies each actor's pose
before the native culler. At `$C0:6E5F`, it associates those poses with the
completed OAM submission. The renderer selects the submission matching the
displayed OAM, then reads sparse/grid sprite molds and graphics from the
actor's ROM binding. This makes ordinary actors available beyond the
native OAM/CHR upload region. All these copies are discarded on reset or
state load. No ROM instruction, guest RAM, OAM limit or camera clamp is
patched, and the observer does not advance guest clocks.

## Current limits

- This is a playable field-rendering experiment, not full-game visual
  certification. Some special sprite transforms, shadows and effects are
  incomplete in the added area. Their appearance can differ as they cross
  the native seam; ordinary actor reconstruction does not remove every
  special-case culler in the game.
- Actors that guest logic has not instantiated or activated are not
  invented. Actor depth ordering outside the native image uses the
  projected ground coordinate; unusual overlaps still need validation.
- Disconnected room filtering can omit detached scenery. Effect planes,
  animated tilemap substitutions, map boundaries and camera wraps need
  further playthrough coverage. Unsupported scenes intentionally retain
  the native image.
- The native 240-pixel open field interior is copied exactly. Full custom
  recomposition of that interior is available for diagnostics, not enabled
  for normal play. The fully opened eight-pixel side masks are extended;
  iris transitions retain their native bounds.
- Renderer captures are local, uncompressed implementation snapshots of
  about 15 MiB each, not portable save states or distributable assets.

## Validation and capture tools

On 2026-09-19, the renderer's synthetic tests passed for aspect fitting,
assignment strides, negative-coordinate wrapping, connected scenery beyond
the former reach limit, actor/OAM snapshot pairing, snapshot immutability,
apertures, native fallback, reset and malformed capture rejection. The
engine's SA-1 test passed for observer boundaries and unchanged memory and
timing, plus disabled/reset behavior. The SDL3 desktop playtest launched
with the enabled mod and exited normally after 120 frames.
Live resize checks produced the expected 542×224 framebuffer for a
1584×561 client area and 256×224 for a 584×861 portrait client area.
A narrow-to-wide resize while paused also produced a valid 542×224 frame.

A full 18,000-frame attract run supplied 30 snapshots, replayed and visually
inspected at width 684. Forest, river, pipe, stump, Yoshi village, indoor,
snow and transition scenes were sampled; four scenes were also replayed at
widths 342, 456 and 1024. Artifacts are under `build-custom/qa-final`.

Two 6,000-frame runs, widths 256 and 684, saved at frame 3100 and loaded at
3300. Their per-frame hashes of WRAM, BWRAM, IRAM, VRAM, CGRAM and OAM, and
their main CPU clocks and SA-1 instruction counts, matched exactly after
drawing. The trace SHA-256 is
`b3282a66814d3c4467c746f579baf6af10e3b5825142f740f6ce2ccf96b69f69`.
Audio sample/activity counters also matched. The headless harness still
exits 8 for audio underruns: one on the normal attract boot and 198 in both
save/load runs. These checks demonstrate renderer parity, not a passing
audio qualification gate; the existing audio issue is tracked separately.
Remaining sprite/effect coverage is tracked in `beads-4c5.5`.

The castle/title/battle follow-up (`beads-4c5.6`) additionally checks BG3
byte-index assignments, 2bpp sky composition, finite battle borders and
zero-entry padding even when CHR tile 0 contains opaque graphics. Replaying
the same 30 attract captures plus the castle exterior and first castle
battle preserved every native interior pixel. Existing field/title outputs
were unchanged; supported battle captures gained only their authored
borders. The castle battle gained exactly eight pixels per side. Artifacts
are under `build-custom/qa-scene-expansion`.

Two 900-frame runs from the castle entrance through the first battle, at
widths 256 and 684, passed the headless harness with identical guest-state
traces and audio counters, and zero audio underruns. The trace SHA-256 is
`2b3f82d5c3e55fc36c215e851ab5f5a99e9cc52d1f924967400f6cc7cfb0afb1`.
Both logged the same 115 existing APU frame-boundary sync timeouts after
state restore; renderer parity does not resolve that audio timing issue.

The mod-default follow-up (`beads-4c5.7`) passed four isolated 120-frame
desktop runs: fresh defaults, saved widescreen at 21:9, disabled again, and
disabled with diagnostic capture. Stock runs produced identical 256x224
images; opting in produced 448x224. Opposite legacy environment overrides
did not bypass the mod selection. Preparing the helper again preserved the
saved selection and an unrelated save. Evidence is retained under
`build-custom/qa-opt-in-y721azql`; Release build and renderer CTest passed.

Useful environment variables:

| Variable | Purpose |
| --- | --- |
| `SNESRECOMP_WIDESCREEN` | Legacy desktop override; ignored. Use the Mods toggle. |
| `SNESRECOMP_WIDESCREEN_EXTRA=214` | Headless width 684; use 0 for native. |
| `SNESRECOMP_STATE_TRACE_FILE=state.txt` | Per-frame guest-memory hashes and clocks. |
| `SMRPG_RENDER_CAPTURE=frame.srpg` | Save one immutable renderer frame. |
| `SMRPG_RENDER_CAPTURE_FRAME=3600` | Host frame number for that capture. |
| `SMRPG_RENDER_CAPTURE_DIR=captures` | Capture into an existing directory. |
| `SMRPG_RENDER_CAPTURE_INTERVAL=600` | Period for directory captures. |
| `SMRPG_RENDER_DIAGNOSTIC_FULL=1` | Recompose the field interior for visual comparison. |

Replay a capture without executing the guest:

```powershell
./build-custom/smrpg_render_capture.exe smrpg.sfc frame.srpg preview.ppm 684
```

The tool prints camera, map-match, field/battle-line and offscreen-actor statistics.
The ROM, generated code, captures, saves and screenshots remain ignored.

The architecture follows the sibling F-Zero, Mega Man X and Super Metroid
custom renderers. Format investigation also used
[LazyShell's sprite readers](https://github.com/CaptainSwag101/LazyShell/tree/master/LAZYSHELL/Sprites)
and the [SMRPG disassembly](https://github.com/Yoshifanatic1/Super-Mario-RPG-Disassembly)
at `57cb707669d71bb55817a0f88d28b3018c8bec57`, with instruction boundaries
checked against the verified retail US ROM. No disassembled game routines
are redistributed in this renderer.
