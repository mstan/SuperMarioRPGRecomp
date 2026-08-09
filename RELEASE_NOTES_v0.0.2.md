Patch release for a full production playthrough and coverage/QA pass. This is
still **preview quality**: automated qualification is strong, but the game has
not yet been verified end to end by a human playthrough.

## What's new

- **Loss-resistant dispatch-miss capture.** Production builds now record the
  complete recompiler-actionable miss key `(site, target, M/X, kind)`: direct
  dispatch/bank misses and call/jump gaps discovered while interpreting.
- **Append-only crash recovery.** Each distinct miss is appended to a JSONL
  journal and flushed before its missing target executes. A crash or forced
  termination can therefore leave useful coverage even when the final report
  was never written.
- **No cross-run overwrite.** With the default settings, every launch creates
  a unique timestamp-and-PID `.json` manifest and matching `.jsonl` journal.
  The in-memory set grows as needed instead of silently stopping at the former
  4,096-tuple ceiling.
- **Merge tooling and guardrails.** `tools/merge_tier2_coverage.py` accepts
  clean-exit manifests and recovery journals, preserves pending outcomes, and
  rejects torn journal lines. The new behavior has dedicated automated tests.
- **Current shared dependencies.** The game pins the validated capture runtime
  (`snesrecomp` `4fa1b57`), current launcher (`recomp-ui` `ca27c83`), and its
  current networking dependency (`recomp-net` `6d848d6`).

## Playthrough artifact checklist

1. Use this unmodified Production release and leave
   `SNESRECOMP_TIER2_MANIFEST` and `SNESRECOMP_TIER2_JOURNAL` unset.
2. Keep every `tier2_super_mario_rpg_<UTC>_p<PID>.json` and matching `.jsonl`
   file produced beside the executable/AppImage. Do not rename files into one
   another or discard earlier sessions.
3. For a normally ended session, submit its final `.json`. If the session
   crashed or was killed before that file appeared, submit its same-stem
   `.jsonl` instead. Do not submit both files from the same session for merging
   because the final manifest already contains the journal's evidence.
4. Keep crash reports and `last_run_report.json` separately as ordinary QA
   evidence; `g_dispatch_log` is diagnostic-only and is not a coverage input.

A complete collection should report `overflowed_tuples: 0` and
`journal_write_failures: 0`. If either counter is nonzero, retain all files and
report it with the run diagnostics.

## Validation

- Framework C harness: all tests passed, including 4,354 distinct synthetic
  tuples (beyond the old fixed cap), pre-execution journaling, unique output
  paths, and abnormal-exit recovery behavior.
- SMRPG merger tests: all passed, including union/deduplication and rejection
  of a torn JSONL record.
- Native Windows Production desktop and headless builds succeeded. Three
  normal runs produced exact manifest/journal sets with zero capture failures;
  a forced termination preserved all 104 flushed records without a final
  manifest.
- A deterministic one-count audio underrun in the 3,600-frame headless gate is
  unchanged from v0.0.1's framework pin and is tracked separately; the
  599-frame smoke gate passes.

## Known limitations

- The video output and full game flow still need the human playthrough this
  release is intended to support.
- Static recompilation coverage remains small (2 qualified exact AOT variants);
  other code continues through the correctness-floor interpreter.
- Gamepad bindings remain fixed in code. The launcher and `keybinds.ini` edit
  keyboard bindings only.
- The game has no repo-owned `config.ini`; launcher settings and `rom.cfg` live
  beside the executable.

## Install

Extract the Windows zip or make the Linux AppImage executable, place your own
verified Super Mario RPG (USA) ROM beside it, and run it. The ROM is not
included; expected hashes are documented in the README.
