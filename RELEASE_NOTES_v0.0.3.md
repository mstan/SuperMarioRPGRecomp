Patch release for the built-in Mods catalog, opt-in coverage proof bundles,
and the first merged early-route user coverage profile. This is still
**preview quality**: an early-game replay segment has been checked against the
regenerated build, but the full game has not been verified end to end by a
human playthrough.

## What's new

- **Built-in Mods tab support.** Release builds now ship a preloaded SMRPG mod
  catalog so recomp-ui's Mods page shows game-owned entries.
- **Widescreen moved to a mod.** Adaptive widescreen is no longer a Display
  setting. Enable the built-in Widescreen mod from the Mods tab; its HUD option
  selects split-edge or authentic centered HUD placement.
- **Coverage proof capture mod.** Enable `Coverage proof capture` from the Mods
  tab to write a `coverage_bundles/smrpg_<timestamp>_p<PID>/` folder with the
  merge-ready manifest, append-only journal, summary, and saves when present.
- **Merged early-route coverage.** The first user-provided early-game coverage
  bundle was imported into `recomp/tier2_coverage.json`, raising the profile to
  1,939 discoveries and 5 RAM routines.
- **Analyzer scaling fix.** The shared native analyzer now reads large coverage
  root sets from temporary files instead of expanding them onto the process
  command line, avoiding Windows/MSYS `Argument list too long` failures during
  regeneration.
- **SDL3 release path.** SDL3 is the default desktop backend and was used for
  the validated Windows build.

## Validation

- SMRPG coverage merge tests passed.
- `tools/regen.sh` succeeded with the merged coverage profile.
- Windows SDL3 release build succeeded and smoke-booted with 2 Mods-tab
  features loaded.
- A user replayed roughly the same early-game segment against the regenerated
  build and reported it played correctly.

## Known limitations

- Static recompilation coverage remains small: the generated manifest has 2
  AOT-eligible variants and 8 LLE-only variants. Other code continues through
  the correctness-floor interpreter.
- The imported coverage proves the profile is mergeable and rebuildable; it
  does not prove every observed target is trusted native/static code.
- The full game still needs a completed end-to-end human playthrough.
- Gamepad bindings remain fixed in code. The launcher and `keybinds.ini` edit
  keyboard bindings only.

## Install

Extract the Windows zip or make the Linux AppImage executable, place your own
verified Super Mario RPG (USA) ROM beside it, and run it. The ROM is not
included; expected hashes are documented in the README.
