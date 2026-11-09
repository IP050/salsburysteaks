# Changelog

Fixes and changes per release. Found a problem? Please use the
[bug report form](../../issues/new?template=bug_report.yml) and attach `Metroid Prime 4 Beyond.log`: it holds the
timings and errors needed to trace most issues, and one report per problem keeps each one easy to track.

## 2026.11.13

### Native Vulkan starts again after a crash
On some AMD cards (RX 9070 XT, Ryzen Z1 / Legion Go) Native Vulkan crashes during the opening cutscene, and after
that the game stayed on "Loading" at every start until the `vulkan_native` folder was deleted by hand. After a run
that did not close properly, Native Vulkan now sets its shader cache aside (as `vulkan_native\shader_cache.crashed`)
and builds a new one, so the game opens again by itself. The first start after a crash builds shaders again.

The cutscene crash itself is not fixed yet. To find it:
- The game now keeps its own crash reporter in place when a driver installs another one over it, and the log names
  the file that did.
- A program exit asked for by a driver is now written to the log too.

If the game crashes for you, please attach the log from this version.

## 2026.11.12

### The game stays at console speed
Metroid Prime 4 moves forward one step per frame, so the 120, 144 and 240 FPS choices ran the whole game faster
instead of smoother (2.4 times the speed at 144 on Native Vulkan; OpenGL stayed at 60 regardless). The game now
runs at 60 on every renderer, a saved 120/144/240 is set back to 60 once, and "Best settings for this PC" picks 60.
Unlimited is still there and says that it speeds the game up. Smooth output above 60 for high-refresh screens is
planned as frame generation.

### AMD Radeon crashes: a workaround to try and clearer reports
Some AMD cards (RX 9070 XT, Ryzen Z1 / Legion Go) crash in AMD's driver about 2 seconds after PLAY on OpenGL, or
during the opening cutscene on Native Vulkan. This release does not fix that yet, but:
- `shader_threads=-1` in `Metroid Prime 4 Beyond.ini` makes every shader on the main graphics thread. On OpenGL,
  try it if the game crashes right after starting.
- A crash inside a driver now names the thread and the exact game code that called the driver, and crashes that
  end the game at once now still reach the log. Please attach the log from this version to your report.

### Checksums for every release
Each release now has `SHA256SUMS.txt` to check downloads against (see SECURITY.md). SECURITY.md also explains how
to report a security problem privately, and NOTICE.txt now requires keeping the contributors' credits in copies.

### Updates install themselves
When the launcher finds a new version it now downloads and installs it straight away, then restarts or rebuilds the
game code if the update needs it, with no UPDATE press. PLAY waits while it downloads. Every file is still checked
before anything is replaced, and UNDO still goes back a version (that version is then not installed again by
itself). It can be turned off in the Play screen's Updates section. Players on older versions press UPDATE once more
to get this.

### Progress bar on the splash screen
The splash shown after PLAY now has a progress bar and a label ("Preparing shaders", then "Starting the game"). Most
of the wait is preparing the shaders cached from earlier sessions, and the bar follows that real progress.

## 2026.11.11

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
