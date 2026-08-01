# Super Mario RPG SNES recompilation

Bring-up project for the US release of *Super Mario RPG: Legend of the Seven
Stars*. The initial unattended attract-loop qualification is passing with
changing game state, valid video, active audio, and no runtime or SA-1 errors.

## Required ROM

Provide a legally obtained, headerless ROM as `smrpg.sfc`. It must match:

- Size: 4,194,304 bytes
- SHA-256: `740646f3535bfb365ca44e70d46ab433467b142bd84010393070bd0b141af853`
- SHA-1: `a4f7539054c359fe3f360b0e6b72e394439fe9df`
- MD5: `d0b68d68d9efc0558242f5476d1c5b81`
- Internal header: map mode `$23`, cartridge type `$35` (SA-1 + RAM + battery)

ROM and generated sources are intentionally ignored by Git.

## Generate and build

The active engine worktree is
`F:/Projects/snesrecomp/_wt_smrpg_sa1_snesrecomp` on branch
`codex/smrpg-sa1`.

```powershell
Copy-Item F:\Projects\snesrecomp\_roms\smrpg-us.sfc .\smrpg.sfc
bash tools/regen.sh
cmake -S . -B build -G Ninja `
  -DSNESRECOMP_ROOT=F:/Projects/snesrecomp/_wt_smrpg_sa1_snesrecomp
cmake --build build --parallel
.\build\SuperMarioRPGSNESRecomp.exe .\smrpg.sfc
.\build\SuperMarioRPGSNESRecompHeadless.exe .\smrpg.sfc 36000
```

The headless host verifies the ROM before boot. It reports independent logic,
video, audio, and SA-1 activity metrics and returns nonzero on a runtime error
or failed attract-soak thresholds. Set `SNESRECOMP_FRAME_DUMP=frame.ppm` for
the final frame or `SNESRECOMP_WAV=attract.wav` for audio capture.
For deterministic interaction tests, `SNESRECOMP_INPUT_SCRIPT` accepts
comma-separated `FIRST[-LAST]:MASK` spans (for example, `900:0x8` taps Start).
`SNESRECOMP_WIDESCREEN_EXTRA=71` makes headless captures use a 398-pixel
adaptive framebuffer; set `SNESRECOMP_WIDESCREEN_HUD=0` to compare the
unanchored HUD policy.

Every clean exit writes `tier2_coverage.json` (override the path with
`SNESRECOMP_TIER2_MANIFEST`). A trace desktop also accepts TCP
`tier2_dump <path>`, so validation does not depend on a clean exit. Merge
separate attract and gameplay captures with
`python tools/merge_tier2_coverage.py recomp/tier2_coverage.json <inputs...>`.
The merged profile is checked in at `recomp/tier2_coverage.json`;
`tools/regen.sh` automatically passes it to the analyzer so observed
interpreter entries are promoted to optional AOT roots. The profile retains
bailout evidence so an entry that proved unsafe as a standalone C boundary is
not selected again. `qualified_aot_targets` separates harvested coverage from
boundaries that passed a semantic soak; `unsafe_aot_targets` records failed
promotion trials. The current generation contains 2 qualified exact AOT
variants and 8 manifest LLE variants, with all other gaps retaining the
interpreter fallback.

The desktop host supports keyboard and game-controller input. Keyboard
bindings are arrows for the D-pad, `Z`/`X` for B/A, `A`/`S` for Y/X,
`Q`/`W` for L/R, Enter for Start, and Right Shift for Select. Press `P` to
pause, `F1` through `F12` to load state slots 1 through 12, or hold Shift with
the same keys to save. Alt+Enter toggles fullscreen and Escape quits. The
window title confirms whether the state operation succeeded.

Adaptive view defaults on. It requests a 398x224 (16:9) framebuffer and
recognizes SMRPG's validated Mode-1 field contract without changing guest PPU
registers. The host matches the complete 32x28 live viewport against the
game's decompressed 128x128 field maps, then streams only authored tiles inside
the assignment's horizontal mask and the viewport-connected component. The
result is genuine, camera-responsive field geometry: a camera at one room edge
may keep a black margin on that side while using the available width on the
other. Packed maps with only the game's unlocked default bounds use
connectivity filtering, so adjacent authored geometry can extend without
exposing a neighboring room across an intervening void. Authentic 4:3 remains
available from the recomp-ui display menu.

Menus and the battle arena remain native-width. With widescreen HUD enabled,
the captured battle HUD contract anchors Mario's BG2 HP panel and OAM slots
0-7 (portrait and command diamond) to the adaptive edge while leaving actors
in authentic world coordinates. Set `SNESRECOMP_WIDESCREEN_HUD=0` for a
centered-HUD parity comparison.

Trace builds listen on TCP port 4381 by default (override with
`SNESRECOMP_DEBUG_PORT`). Use `get_ppu_state`, `ppu_lines`, `screenshot`,
`read_ram`, controller injection, and `tier2_dump` for visible-state and
coverage validation. `loadstate` and `savestate` accept slots 0 through 11.

## Qualification

The two-target qualified-AOT build completed an unattended 18,000-frame run of
the canonical US ROM (about five guest minutes and a complete return through
the attract loop):

- 945,940,422 SA-1 instructions
- 17,862 frames with a changed logic-state hash
- 15,854 active-video frames and 11,065 video changes
- 17,372 active-audio frames and a peak sample magnitude of 18,500
- no runtime failure, scheduler cap, IRQ storm, or logic/video/audio activity
  failure

After separating observed coverage from qualified promotion, the regenerated
allowlist build repeated a 3,000-frame gate with 2,941 logic changes, 2,669
active-video frames, 1,860 video changes, and 2,420 active-audio frames.

TCP validation also exercised state slot 12 end to end: save a known WRAM
position, inject controller input until it changes, then load and confirm the
original bytes are restored. On the Bowser's Keep field, TCP captures show a
71-pixel adaptive budget, live asymmetric room space (`left=9`, `right=71` at
the saved camera), zero shadow misses, and a seamless continuation across the
native/right-margin tile boundary. A widescreen-off capture reports a zero
budget and an exact 256x224 framebuffer. A default-mask attract field also
reports `left=71`, `right=71` with field-window expansion active; its 398x224
capture shows a coherent authored scene across the full viewport rather than
a centered 4:3 image.

At frame 599, the native picture best-matches the Snes9x reference at frame
605 with 0.73 mean absolute RGB error. The corresponding audio envelopes have
0.989 correlation after accounting for the same approximately 100 ms startup
phase offset; RMS levels agree within 0.6%.

## Public disassembly metadata

`tools/ingest_smrpg_disassembly.py` builds an address-only symbol/reference
catalog from
[Yoshifanatic1/Super-Mario-RPG-Disassembly](https://github.com/Yoshifanatic1/Super-Mario-RPG-Disassembly).
It requires reviewed commit
`57cb707669d71bb55817a0f88d28b3018c8bec57` and verifies the canonical ROM MD5
before ingesting anything. The catalog is analysis metadata only: public
labels are never assumed to be function entries or AOT-safe boundaries.

```powershell
py -3 tools/ingest_smrpg_disassembly.py `
  --source-root F:\Projects\snesrecomp\_refs\Super-Mario-RPG-Disassembly `
  --rom .\smrpg.sfc --output .\build\public-disassembly.json
```
