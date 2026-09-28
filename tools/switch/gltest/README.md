# OpenPrey Switch GL probe

Phase 1 risk test for the Nintendo Switch port. It checks whether the devkitPro
Mesa (nouveau) driver provides what the OpenPrey renderer needs before the engine
port starts:

- an OpenGL compatibility-profile context with an 8-bit stencil buffer
- immediate mode / fixed function (`glBegin`, `glMatrixMode`, ...)
- the real ARB programs from `basepy/glprogs/*.vfp`, loaded the same way as
  `R_LoadARBProgram` (from `!!ARBvp`/`!!ARBfp` to the first `END`)
- the legacy GLSL post-processing shaders (`blur`, `openprey_bloom`)
- DXT1 texture upload, VBOs, and every extension the renderer checks

## Build

From the devkitPro MSYS2 shell:

```sh
cd tools/switch/gltest
make
```

The Makefile copies the shader files from `basepy/glprogs/` into `romfs/` on each
build, so the probe always tests the current shaders.

## Run

1. Copy `openprey_gltest.nro` to `sdmc:/switch/` on the SD card.
2. Start the Homebrew Menu by holding **R** while launching a game (title
   override), not from the Album.
3. Run **OpenPrey GL Test**. The screen flashes blue for about 1.5 seconds, then
   the report appears. Press **+** to exit.

The same report is saved to `sdmc:/openprey_gltest.txt`.

`[FAIL]` lines are hard requirements of the current renderer. `[WARN]` lines are
optional paths with fallbacks.
