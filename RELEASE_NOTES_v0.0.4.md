Stock 4:3 rendering remains the default. This release replaces the previous
widescreen implementation with an opt-in custom renderer, visibly labeled
**Widescreen (Experimental)** in Mods.

## What's new

- **Adaptive custom rendering.** Enable Widescreen (Experimental) and choose
  Fit to window, 16:9, or 21:9 to reveal additional horizontal field scenery
  and actors. The guest PPU and game logic retain their native viewport.
- **Original renderer when disabled.** Turning the mod off restores the
  original PPU renderer at 4:3. The playtest launcher no longer enables
  widescreen automatically, and old environment overrides cannot bypass the
  Mods selection.
- **Authored backgrounds beyond the camera.** Bowser's Keep's red-cloud sky
  now fills the expanded view. Standard battle backdrops expose only existing
  artwork: the first castle arena has eight extra SNES pixels per side. The
  title has no additional horizontal artwork and stays native.
- **Safer packaged defaults.** Release archives include the mod catalog
  without developer mod selections. Linux updates preserve saved mod choices.

## Validation

- Windows and Linux release builds and renderer tests passed.
- Both packaged builds passed fresh stock, enabled widescreen, and disabled
  again launch checks. Stock images matched before and after the toggle.
- The packaged launcher visibly labels the widescreen feature Experimental
  and leaves it unchecked by default.
- Linux AppImage layout checks passed for writable state placement and
  preservation of saved mod choices. Castle exterior and battle capture
  replays matched the Windows renderer output exactly.
- Earlier renderer validation covered 32 captured scenes and matched
  native/widescreen guest-state traces through the first castle battle.

## Known limitations

The widescreen renderer is experimental. Some actor transforms, shadows,
effects, unusual overlaps and map boundaries remain incomplete in the added
area. Unsupported scenes keep their native view. The full game has not been
verified end to end, and existing audio timing issues remain under investigation.

## Install and upgrade

Extract the complete Windows ZIP, or make the Linux AppImage executable.
Supply your own verified Super Mario RPG (USA) ROM; no ROM is included.
Use Mods to enable Widescreen (Experimental), or leave it disabled for 4:3.
Existing mod choices are preserved when updating. If an earlier build had
widescreen enabled, disable it once in Mods to return to the original renderer.
