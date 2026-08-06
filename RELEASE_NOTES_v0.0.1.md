First public release of the Super Mario RPG SNES recompilation. **Preview
quality** — see Known limitations below before reporting a bug against the
video output specifically.

## What's new

- **First release.** Boots the US ROM, runs the attract loop, and holds up
  under an unattended headless soak (see the README's
  [Qualification](README.md#qualification) section). Super Mario RPG is an
  **SA-1 coprocessor** title: you supply your own legally obtained ROM
  (`smrpg.sfc`); expected hashes are in the README.
- **Family-wide audio fix.** The shared engine's SNES audio consumer used to
  retire a hardcoded 534 native samples per callback regardless of how many
  the host actually asked for — a holdover from upstream LakeSnes, where one
  call meant one video frame. Any host callback size other than exactly 534
  put the output ring permanently out of balance. Measured on the sibling
  Super Mario Kart port: 33 callbacks/s x 534 = ~17.8k natives/s consumed
  against ~32.04k/s produced, pinning the ring and dropping **43% of every
  generated audio sample** (458,001 samples over a 33 s capture, in 64,935
  separate drop runs) — audible as static rather than obviously wrong pitch,
  which is why it went unattributed for a long time. Engine commit
  `1c271f9` replaced the fixed-534-sample consume with an exact-consume path
  plus a ring-occupancy servo; the immediate follow-up, `ad7860b`, restored
  correct native-32040Hz-to-device-rate conversion for non-native output
  rates (which the exact-consume rewrite had briefly dropped) and fixed
  measured servo flutter and a fade-in that could stall mid-ramp. Super Mario
  RPG picks up both commits in this release.
- **SDL3 desktop host**, with real crash-report/minidump capture
  (`crash_report_*.json` / `crash_minidump_*.dmp` / `last_run_report.json`
  next to the executable on every run) and a per-frame diagnostic trace
  gated behind `SNESRECOMP_TRACE` (compiled out of this release's Production
  build).
- **Adaptive widescreen (16:9, 398x224) is on by default.** It reconstructs
  genuine, camera-responsive field geometry from SMRPG's decompressed field
  maps rather than stretching the image — see the README's
  [Adaptive widescreen](README.md#adaptive-widescreen) section. Authentic
  4:3 remains selectable from the launcher's Display view-mode control.

## Known limitations

- **The video output has not been verified by a human playthrough.**
  Everything in this release is qualified by automated attract-loop replay
  and headless soak metrics (logic/video/audio activity hashes, frame
  counts, an audio/video cross-check against a Snes9x reference) — not by
  anyone actually playing the game start to finish. Treat visual and
  behavioral correctness as unconfirmed until that happens.
- **No `config.ini`.** Unlike the other titles in this family, Super Mario
  RPG has no repo-owned config module — it's a from-scratch scaffold whose
  presentation defaults live in the launcher's own settings rather than a
  game-side config file. The one state file this release genuinely owns is
  `rom.cfg` (the cached ROM path).
- **Gamepad bindings are fixed in code.** The pre-game launcher's Controls
  page and `keybinds.ini` cover keyboard bindings only (matching the rest of
  this family); a connected controller always uses the fixed SDL_GameController
  mapping documented in the README.
- Static recompilation coverage is small at this stage (2 qualified AOT
  variants); nearly everything still executes through the correctness-floor
  interpreter. This is expected for a bring-up-stage title and is tracked
  incrementally, not a defect to report.

## Install

**Windows:** download the release zip, extract it into a folder, place your
verified `smrpg.sfc` beside `SuperMarioRPGSNESRecomp.exe` (or let the file
picker find it), and run the executable.

**Linux:** download the `.AppImage`, `chmod +x` it, place your verified
`smrpg.sfc` beside it, and run it. Settings and saves are stored next to the
AppImage, never inside it.

A verified Super Mario RPG (USA) ROM is required and is not included.
