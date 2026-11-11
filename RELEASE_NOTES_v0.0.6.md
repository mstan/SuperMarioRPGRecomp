# Super Mario RPG 0.0.6

This preview adds optional replay capture and persistent launch/crash logs.
Enable **Crash diagnostics and replay** in Mods, or use **Start diagnostics**
to capture startup failures too. **Collect diagnostics** creates a local ZIP;
nothing is uploaded automatically. See [DIAGNOSTICS.md](DIAGNOSTICS.md).

Capture retains recent inputs and full SA-1 checkpoints with a bounded disk
worker. Replay checks WRAM, CPU/SA-1 timing, SPC/BWRAM/IRAM and VRAM against
the exact build and ROM. Preserved crash reports survive later launches.
Windows and Wine launch scripts also retain output before the game entry point.

Fresh settings enable adaptive Fit widescreen. Existing saved mod choices and
native saves are preserved. The Windows runtime has passed input replay,
startup and in-game crash drills, completed-prefix replay and collection.
Wine-specific behavior still needs validation on Wine; these Windows checks
do not establish a platform-specific crash fix.
