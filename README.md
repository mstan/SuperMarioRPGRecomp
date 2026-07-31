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
.\build\SuperMarioRPGSNESRecompHeadless.exe .\smrpg.sfc 36000
```

The headless host verifies the ROM before boot. It reports independent logic,
video, audio, and SA-1 activity metrics and returns nonzero on a runtime error
or failed attract-soak thresholds. Set `SNESRECOMP_FRAME_DUMP=frame.ppm` for
the final frame or `SNESRECOMP_WAV=attract.wav` for audio capture.

## Qualification

The canonical US ROM completed an unattended 18,000-frame run (about five
guest minutes and a complete return through the attract loop):

- 946,872,185 SA-1 instructions
- 17,861 frames with a changed logic-state hash
- 15,850 active-video frames and 11,021 video changes
- 17,375 active-audio frames; captured output measured -21.1 dB mean and
  -5.0 dB peak
- no runtime failure, scheduler cap, IRQ storm, or logic/video/audio activity
  failure

At frame 599, the native picture best-matches the Snes9x reference at frame
605 with 0.73 mean absolute RGB error. The corresponding audio envelopes have
0.989 correlation after accounting for the same approximately 100 ms startup
phase offset; RMS levels agree within 0.6%.
