# SuperMarioRPGSNESRecomp

Static recompilation of *Super Mario RPG: Legend of the Seven Stars* (SNES)
into native C, using the [snesrecomp](https://github.com/mstan/snesrecomp)
framework. This repo is the per-game side: the runtime, the recompiled C
output, the per-bank `.cfg`, and the build glue.

## What "static recompilation" means here

The 65816 CPU code from the ROM is statically translated to C where the
analysis can prove it safe, and the statically compiled bodies are exact,
proven materializations on top of an authoritative 65816 interpreter
(LakeSnes-derived, MIT) that is the correctness floor — anything the static
pass cannot prove keeps running through the interpreter, loudly. Super Mario
RPG is early in that promotion process: the current build carries only 2
qualified exact AOT variants, so almost everything still runs through the
interpreter. That's expected for a bring-up-stage title, not a defect — see
[Qualification](#qualification) for how coverage is measured and grown.

Super Mario RPG also carries the **SA-1 coprocessor** (a second 65816 running
alongside the main CPU, with its own IRAM/BWRAM and a hardware
multiply/divide unit) — the cartridge's field/battle logic runs largely on
it. snesrecomp's SA-1 core is emulated (LLE), not statically recompiled, same
as the rest of the non-CPU hardware: PPU rendering, the APU/SPC700 audio
coprocessor, DMA/HDMA channels, and hardware register I/O run through
snesrecomp's own runner implementations (`snesrecomp/runner/`).

The ROM is **never** redistributed — you supply your own legally-dumped copy.

## Current status: preview quality, first release (v0.0.1)

This is the first public release of this recompilation. It boots, runs the
attract loop, and holds up under an unattended headless soak (see
[Qualification](#qualification)) — but **the video output has not yet been
verified by a human playthrough.** Everything above the automated attract
loop and headless soak metrics is unverified. Treat it as a preview, and see
[RELEASE_NOTES_v0.0.1.md](RELEASE_NOTES_v0.0.1.md) for the full list of
what's new and what's not yet confirmed.

## Required ROM

Provide a legally obtained, headerless ROM as `smrpg.sfc` (a 512-byte SMC
copier header, if present, is auto-stripped before hashing, so headered or
unheadered both work). It must match:

- Size: 4,194,304 bytes
- SHA-256: `740646f3535bfb365ca44e70d46ab433467b142bd84010393070bd0b141af853`
- SHA-1: `a4f7539054c359fe3f360b0e6b72e394439fe9df`
- MD5: `d0b68d68d9efc0558242f5476d1c5b81`
- Internal header: map mode `$23`, cartridge type `$35` (SA-1 + RAM + battery)

## Quick start (pre-built release)

1. Download the latest release archive from [Releases](../../releases) and
   extract it.
2. Place your legally-obtained, verified `smrpg.sfc` next to
   `SuperMarioRPGSNESRecomp.exe` (Windows) or the `.AppImage` (Linux), or let
   the launcher's file picker point at it on first run.
3. Run the executable / AppImage.

The resolved ROM path is cached to `rom.cfg` next to the executable so
subsequent launches skip the picker.

## Controls

The desktop host's default keyboard bindings are currently **fixed in code**
for this release:

| SNES button | Default key |
|-------------|-------------|
| D-Pad       | Arrow keys |
| B           | Z |
| A           | X |
| Y           | A |
| X           | S |
| L           | Q |
| R           | W |
| Start       | Enter |
| Select      | Right Shift |

A single connected Xbox / PlayStation / Switch Pro controller is
auto-detected via SDL_GameController. Plug it in before launching, or
hot-plug after.

**Known limitation:** the pre-game launcher's Controls page can edit and
save `keybinds.ini`, but the desktop host does not read that file back in
this release — the bindings above apply regardless of what's saved there.
Unlike the other SNES recomps in this family, Super Mario RPG also has no
`config.ini`: it's a from-scratch scaffold whose presentation defaults live
in the launcher's own settings, not a game-owned config file.

System shortcuts:

| Action                    | Default |
|---------------------------|---------|
| Save state, slots 1-12    | Shift+F1..F12 |
| Load state, slots 1-12    | F1..F12 |
| Toggle pause              | P |
| Toggle fullscreen         | Alt+Enter |
| Quit                      | Escape |

## Reporting crashes

The game continuously records its own boot/run diagnostics. If it crashes
(or exits with an error), it writes these files next to the executable —
attaching them to a GitHub issue usually lets the crash be diagnosed without
a repro:

- `crash_report_<timestamp>.json` and `crash_minidump_<timestamp>.dmp` —
  written at the moment of a crash; never overwritten by later runs.
- `last_run_report.json` — written at the end of **every** run (crash or
  clean exit), so grab it right after the bad run if there is no
  `crash_report_*` file.

None of these contain personal data beyond your Windows version, hardware
model, and the folder path the game runs from.

## Building from source

`snesrecomp/` and `recomp-ui/` are pinned git submodules — the shared
framework and the shared, console-agnostic launcher UI:

```bash
git clone --recurse-submodules git@github.com:mstan/SuperMarioRPGRecomp.git
cd SuperMarioRPGRecomp
# or, if already cloned without --recurse-submodules:
git submodule update --init --recursive
```

Generated game C is not redistributed. Stage a legally obtained, verified
US ROM as `smrpg.sfc` at the repo root (`.gitignore` excludes it), then
regenerate before the first build:

```bash
bash tools/regen.sh
```

### Windows (MSYS2 / MinGW, CMake)

Install [MSYS2](https://www.msys2.org/) with the mingw64 toolchain (`cmake`,
`ninja`), the SDL3 development package, Git, and Python 3.9+. Run
`tools/regen.sh` from Git Bash, then:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/SDL3/x86_64-w64-mingw32 \
  "-DSNESRECOMP_BUILD_VERSION:STRING=0.0.1"
cmake --build build-release --target SuperMarioRPGSNESRecomp
```

(Quote `SNESRECOMP_BUILD_VERSION` exactly like that — PowerShell rewrites an
unquoted `-DSNESRECOMP_BUILD_VERSION=0.0.1` into `0`.) SDL3 is the default;
SDL2 remains a supported fallback via `-DSNESRECOMP_SDL_BACKEND=SDL2`. To
package a zip the way releases are built, see `tools/make_release.ps1`
(builds and stamps the version separately; the script only packages).

There is no Visual Studio solution for this game — CMake/Ninja/MinGW is the
only supported Windows toolchain.

### Linux (CMake, AppImage)

Install `cmake`, a C toolchain, `libsdl3-dev` (or `libsdl2-dev` with
`-DSNESRECOMP_SDL_BACKEND=SDL2`), and `libgl1-mesa-dev`. The packaging
script configures, builds, and wraps the result into a self-contained
x86_64 AppImage in one step:

```bash
bash tools/build-linux.sh --regen --version 0.0.1
```

State (`rom.cfg`, `keybinds.ini`, `saves/`) lives next to the `.AppImage`,
never inside the read-only squashfs payload; `tools/test_appimage_layout.sh`
enforces this on every build. There is no macOS build for this game.

## Regenerating the recompiled C (contributors)

1. Stage a legally-obtained, verified US ROM as `smrpg.sfc` at the repo
   root.
2. Run `bash tools/regen.sh`. It verifies the ROM's SHA-256, builds the fast
   native analyzer, and drives the recompiler
   (`snesrecomp/tools/v2_emit.py`) over `recomp/bank*.cfg`, writing
   `src/gen/*.c` and regenerating `recomp/funcs.h`. Never hand-edit
   `recomp/funcs.h` — it's overwritten on every regen.
3. Rebuild as above.

`tools/regen.sh` also feeds `recomp/tier2_coverage.json` (if present) to the
analyzer as `--profile-manifest`, so interpreter entries that were cleanly
observed by a real run get promoted to optional AOT roots on the next
regeneration — see [Qualification](#qualification).

## Adaptive widescreen

Adaptive view is on by default. It requests a 398x224 (16:9) framebuffer and
recognizes SMRPG's validated Mode-1 field contract without changing any
guest PPU register. The host matches the complete 32x28 live viewport
against the game's decompressed 128x128 field maps, then streams only
authored tiles inside the assignment's horizontal mask and the
viewport-connected component. The result is genuine, camera-responsive field
geometry: a camera at one room edge may keep a black margin on that side
while using the available width on the other. Packed maps with only the
game's unlocked default bounds use connectivity filtering, so adjacent
authored geometry can extend without exposing a neighboring room across an
intervening void. Authentic 4:3 remains available from the recomp-ui
launcher's Display view-mode control.

Menus and the battle arena remain native-width. With widescreen HUD enabled
(the default), the captured battle HUD contract anchors Mario's BG2 HP panel
and OAM slots 0-7 (portrait and command diamond) to the adaptive edge while
leaving actors in authentic world coordinates. Set
`SNESRECOMP_WIDESCREEN_HUD=0` for a centered-HUD parity comparison, or
`SNESRECOMP_WIDESCREEN=0` to disable adaptive view entirely.

Unlike the other titles in this family, Super Mario RPG does not ship a
separate opt-in "true widescreen" mod package — adaptive view is the only
wide presentation mode, and it is on by default rather than opt-in.

## Trace builds / debug server

Trace builds (`-DSNESRECOMP_ENABLE_TRACE=ON`) listen on TCP port 4381 by
default (override with `SNESRECOMP_DEBUG_PORT`). Use `get_ppu_state`,
`ppu_lines`, `screenshot`, `read_ram`, controller injection, and
`tier2_dump` for visible-state and coverage validation. `loadstate` and
`savestate` accept slots 0 through 11. Production builds compile the debug
server out entirely (see `SNESRECOMP_TRACE` in
`snesrecomp/runner/src/debug_server.h`).

The headless host (`SuperMarioRPGSNESRecompHeadless`) is useful for
unattended validation without any of the above:

```bash
bash tools/regen.sh
cmake -S . -B build-dev -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-dev --target SuperMarioRPGSNESRecompHeadless
./build-dev/SuperMarioRPGSNESRecompHeadless smrpg.sfc 36000
```

It verifies the ROM before boot, then reports independent logic, video,
audio, and SA-1 activity metrics and returns nonzero on a runtime error or
failed attract-soak thresholds. Useful environment variables:

| Variable | Effect |
|----------|--------|
| `SNESRECOMP_FRAME_DUMP=frame.ppm` | Write the final frame as a PPM. |
| `SNESRECOMP_WAV=attract.wav` | Capture rendered audio as a WAV. |
| `SNESRECOMP_INPUT_SCRIPT` | Comma-separated `FIRST[-LAST]:MASK` spans for deterministic input (e.g. `900:0x8` taps Start). |
| `SNESRECOMP_WIDESCREEN_EXTRA=71` | Force a 398-pixel adaptive framebuffer in headless captures. |
| `SNESRECOMP_WIDESCREEN_HUD=0` | Compare the unanchored (centered) HUD policy. |
| `SNESRECOMP_TIER2_MANIFEST` | Override the coverage-manifest output path (default `tier2_coverage.json`). |

## Qualification

The two-target qualified-AOT build completed an unattended 18,000-frame run
of the canonical US ROM (about five guest minutes, a complete return through
the attract loop):

- 945,940,422 SA-1 instructions
- 17,862 frames with a changed logic-state hash
- 15,854 active-video frames and 11,065 video changes
- 17,372 active-audio frames and a peak sample magnitude of 18,500
- no runtime failure, scheduler cap, IRQ storm, or logic/video/audio
  activity failure

After separating observed coverage from qualified promotion, the
regenerated allowlist build repeated a 3,000-frame gate with 2,941 logic
changes, 2,669 active-video frames, 1,860 video changes, and 2,420
active-audio frames.

TCP validation also exercised state slot 12 end to end: save a known WRAM
position, inject controller input until it changes, then load and confirm
the original bytes are restored. On the Bowser's Keep field, TCP captures
show a 71-pixel adaptive budget, live asymmetric room space (`left=9`,
`right=71` at the saved camera), zero shadow misses, and a seamless
continuation across the native/right-margin tile boundary. A
widescreen-off capture reports a zero budget and an exact 256x224
framebuffer. A default-mask attract field also reports `left=71`,
`right=71` with field-window expansion active; its 398x224 capture shows a
coherent authored scene across the full viewport rather than a centered
4:3 image.

At frame 599, the native picture best-matches the Snes9x reference at frame
605 with 0.73 mean absolute RGB error. The corresponding audio envelopes
have 0.989 correlation after accounting for the same approximately 100 ms
startup phase offset; RMS levels agree within 0.6%.

**All of the above is automated attract-loop / headless-soak measurement —
none of it is a human playthrough.** See [Current status](#current-status-preview-quality-first-release-v001).

The current generation contains 2 qualified exact AOT variants (recorded in
`recomp/tier2_coverage.json`'s `qualified_aot_targets`), with everything
else — including entries that were tried and failed a semantic soak
(`unsafe_aot_targets`) — retaining the interpreter fallback. Merge separate
attract and gameplay captures with
`python tools/merge_tier2_coverage.py recomp/tier2_coverage.json <inputs...>`;
the merged profile is checked in at `recomp/tier2_coverage.json` and
`tools/regen.sh` passes it to the analyzer automatically.

## Public disassembly metadata

`tools/ingest_smrpg_disassembly.py` builds an address-only symbol/reference
catalog from
[Yoshifanatic1/Super-Mario-RPG-Disassembly](https://github.com/Yoshifanatic1/Super-Mario-RPG-Disassembly).
It requires reviewed commit `57cb707669d71bb55817a0f88d28b3018c8bec57` and
verifies the canonical ROM MD5 before ingesting anything. The catalog is
analysis metadata only: public labels are never assumed to be function
entries or AOT-safe boundaries, and no assembly bodies from the source
project are copied into this repo.

```bash
python3 tools/ingest_smrpg_disassembly.py \
  --source-root /path/to/Super-Mario-RPG-Disassembly \
  --rom smrpg.sfc --output build/public-disassembly.json
```

## Repo layout

| Path | Purpose |
|------|---------|
| `src/` | Runtime C (CPU/SA-1 state glue, frame scheduling, widescreen field-map logic, the SDL and headless hosts). |
| `src/gen/` | Recompiler output (gitignored; regenerated from ROM via `tools/regen.sh`). |
| `recomp/bank*.cfg` | Per-bank function declarations + hardware hints the framework cannot derive from the ROM alone. |
| `recomp/funcs.h` | Auto-regenerated by `tools/regen.sh`; never hand-edit. |
| `recomp/tier2_coverage.json` | Checked-in coverage/qualification manifest (see [Qualification](#qualification)). |
| `snesrecomp/` | Pinned submodule containing the [snesrecomp framework](https://github.com/mstan/snesrecomp). |
| `recomp-ui/` | Pinned submodule containing the shared, console-agnostic launcher UI. |
| `tools/` | Regen, packaging (`make_release.ps1`, `build-linux.sh`), and disassembly-ingest scripts. |

## License

Not yet declared. Code in this repo is original.

The *Super Mario RPG* ROM and any data extracted from it are **not** in
this repo and are not licensed for redistribution.
