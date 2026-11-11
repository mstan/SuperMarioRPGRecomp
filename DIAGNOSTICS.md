# Crash diagnostics

Extract the whole release into a writable directory. Keep your original saves.
Diagnostics are optional: enable **Crash diagnostics and replay** in Mods for
ordinary play, or use the diagnostic launcher to include startup failures.

On Windows, run **Start diagnostics.cmd**. On Linux Mint with Wine, open a
terminal in the extracted directory and run:

```sh
sh "Start diagnostics.sh"
```

Choose your own verified USA ROM and play normally. The launcher records Wine
and OS output before the game starts; startup logs and crash reports work even
if gameplay never begins. A missing runtime DLL can only be reported by this
outer launcher because the game has not reached its entry point yet.

After the failure, close the game. Run **Collect diagnostics.cmd** on Windows,
or `sh "Collect diagnostics.sh"` on Linux. Attach the resulting
`smrpg-diagnostics-*.zip`, along with the encounter and character. The archive
contains game memory, native saves and settings, never the ROM file. Nothing
is uploaded automatically. Collection reads the originals without changing them.

Replay segments and their `.sav`, `.mods.txt`, `.mods.toml` and `.config.ini`
sidecars belong together. The bounded background writer keeps up to ten minutes
and 120 MiB of machine checkpoints per session, flushing about every 100 ms.
A slow disk stops optional capture while gameplay continues. A crash may leave
one incomplete final frame; the completed prefix remains useful.

For support/development, replay uses the recording's exact release, platform,
ROM and mod selection. Set `SMRPG_REPLAY_INPUT_FILE` to the `.replay` path and
launch the game with the explicit ROM path and `--no-launcher`. Each completed
frame compares WRAM, CPU/SA-1 timing, SPC/BWRAM/IRAM and VRAM. A mismatch or an
incomplete final frame is reported explicitly. For a prepared reproduction,
`SMRPG_RESUME_STATE` can load an execution snapshot from the same build before
capture starts; the supplied snapshot file is kept. Old save states are not a
compatibility requirement; native saves remain separate.

This release adds reproducible evidence collection. Successful Windows checks
do not establish that a Wine-specific level-up crash is fixed.
