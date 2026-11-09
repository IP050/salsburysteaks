# Changelog

Fixes and changes per release. Found a problem? Please use the
[bug report form](../../issues/new?template=bug_report.yml) and attach `Metroid Prime 4 Beyond.log`: it holds the
timings and errors needed to trace most issues, and one report per problem keeps each one easy to track.

## Next release

### Scan visor no longer drops to 2 FPS at higher resolutions
With the resolution setting above or below 1 (for example 2 for 4K), the scan visor ran at about 2 FPS whenever it
was open. To find what the reticle is over, the game reads a small patch of depth and stencil around it each frame;
before that read the whole depth buffer was scaled down to the game's size, which took 600-770 ms per read on NVIDIA.
The patch is now read directly from the full-resolution buffer. Measured at resolution=2: 1-12 ms per read and
55-60 FPS while scanning (was 1.5 FPS). Scans still lock on and complete. Resolution 1 was not affected.

### Setup reads more .xci / .nsp files
- **Files and folders with non-English names.** A game file whose name or folder path had characters outside the
  Windows language setting (Japanese, Cyrillic, Arabic and so on) failed with "Cannot open". These paths now work.
- **Full cartridge dumps** that keep the card's key area in front of the header are now recognised.
- **Cartridges that hold the game and an update together** now use the base game, and choosing an update file as
  the game gives a clear message instead of a failed unpack.

## 2026.11.10

- Docked mode is the default (1920x1080 output instead of handheld 1280x720), and the game no longer waits for
  shaders by default. Existing settings get both once.
- The splash screen closes at the first picture on every renderer (it could stay up on Native Vulkan).
