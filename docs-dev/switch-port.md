# Nintendo Switch Port (Work in Progress)

Homebrew port of OpenPrey to the Nintendo Switch using devkitPro (devkitA64 + libnx).
Players supply their own retail Prey (2006) `.pk4` files; nothing from the game is
distributed.

## Status

| Piece | State |
|---|---|
| Meson cross target (`horizon`) | Configures |
| `idlib` | Compiles |
| Game code (`src/game` + `src/Prey`, static library) | Compiles |
| Engine sources (framework, renderer, sound, ui, ...) | Compile |
| Platform layer `src/sys/switch/` | First version: files, time, threads, controller/touch input, EGL video. Networking is loopback only. |
| GL loading (GLEW + generated GL 1.1 through `eglGetProcAddress`) | Done |
| `OpenPrey.nro` | Boots on hardware, passes the intro cutscene and is playable. 10-15 fps at stock clocks, 30+ with the GPU at 921 MHz (before the post-processing defaults below). |
| OpenGL capability probe (`tools/switch/gltest`) | Passed on hardware: GL 4.3 compatibility profile (Mesa 20.1 nouveau), ARB programs, legacy GLSL, S3TC. Only `GL_EXT_texture_lod` (optional) is missing. |

## Toolchain setup (Windows)

1. Install devkitPro with **Switch Development** selected.
2. From the devkitPro MSYS2 shell, install the libraries and the host build tools:

   ```sh
   pacman -S --needed switch-sdl2 switch-mesa switch-libdrm_nouveau switch-glad \
       switch-openal-soft switch-zlib meson ninja python
   ```

3. Run Meson with `MSYSTEM=MSYS`. The default devkitPro shell starts in MinGW mode,
   and the msys `python`/`meson` packages refuse to run there.

If the devkitPro folder is moved after installation, update the `/opt/devkitpro`
line in `<devkitPro>/msys2/etc/fstab`.

## Building

```sh
meson setup builddir-switch --cross-file tools/switch/meson/switch-cross.ini -Dbuildtype=release
ninja -C builddir-switch -k 0
```

`builddir-switch/` is kept separate from the host `builddir/`.

The result is `builddir-switch/OpenPrey.nro`. Useful partial targets:

- `ninja -C builddir-switch libopenprey_game_idlib.a`
- `ninja -C builddir-switch basepy/libgame_arm64.a`

### Alternative Mesa (experimental)

[danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch) is a Mesa 26 port with its
own Horizon GPU backend: a newer nouveau NVC0 GL driver, optional Zink on NVK, and
no libdrm_nouveau. Build it OpenGL-only with its `build-opengl.sh`, using a copy of
its `switch_cross_file.txt` without the `rust`/`bindgen` lines (`CROSS_FILE=...`),
from the devkitPro MSYS2 shell. It needs `python-mako`, `python-yaml`, `bison`,
`flex` and an up-to-date `libexpat`. It stages into its own `mesa-install/` and does
not touch the devkitPro switch-mesa package.

```sh
# long link lines go through a response file, which MSYS2 does not path-convert
export MESON_RSP_THRESHOLD=2147483647
```sh
meson setup builddir-switch-mesa26 --cross-file tools/switch/meson/switch-cross.ini \n    -Dbuildtype=release -Dswitch_mesa_sdk=<mesa-switch>/mesa-install/opt/devkitpro/portlibs/switch
ninja -C builddir-switch-mesa26
```

Status: not recommended yet (slower and with rendering glitches on hardware; see
Next steps).

This produces `OpenPrey-mesa-sdk.nro` (title "OpenPrey (Mesa SDK)"), which can sit
next to `OpenPrey.nro`. Runtime switches for that Mesa (environment, set before
`eglInitialize`): `MESA_SWITCH_GL_DRIVER=zink|nvc0`, `MESA_SWITCH_GLTHREAD=0|1`.

## Design decisions

- **Game module as a "fake DLL".** Switch homebrew cannot `dlopen`, but the game
  is written for DLL semantics (`GAME_DLL`): it has its own `sys`/`common`/...
  globals and its own `idlib`. `tools/switch/make_game_object.py` links the game
  and game-idlib archives into one relocatable object (`ld -r`, whole archives)
  and localizes every symbol except `GetGameAPI` (`objcopy --keep-global-symbol`).
  The engine links that object, and `Sys_DLL_Load`/`Sys_DLL_GetProcAddress` hand
  out `GetGameAPI`. Behavior matches the desktop DLL builds.
- **No libGL.** devkitPro ships Mesa (nouveau) as EGL + glapi, with no libGL, so
  every GL entry point is resolved through `eglGetProcAddress`:
  - the extension/GL 1.2+ pointers through the bundled GLEW
    (`src/external/glew/glew.c` has a `__SWITCH__` branch, marked
    `OpenPrey Switch`, that uses `eglGetProcAddress` and no GLX);
  - the GL 1.1 functions through `tools/switch/gen_gl11_loader.py`, which generates
    forwarders from the `glew.h` prototypes at build time. They are loaded in
    `GLimp_Init`.

  `ID_GL_HARDLINK` is not defined on Switch.
- **Screen.** EGL on the default window, GL 4.3 compatibility profile, RGBA8/D24/S8,
  fixed at 1280x720. `r_mode`/`r_fullscreen` are ignored.
- **Engine thread.** `main` starts the engine on a 16 MB-stack thread, matching
  the Windows `/STACK` size that the recursive game code expects.
- **Platform defines.** `__SWITCH__` (from the cross file) selects the Switch block
  in `src/sys/sys_public.h`. Shared Linux/Mac paths in `idlib` also cover
  `__SWITCH__`, so behavior such as `idMath::FtoiFast` rounding matches Linux.
- **Own platform folder.** `src/sys/posix/` depends on `sys/mman.h`, POSIX signals,
  and `sys/uio.h`, which newlib/libnx do not provide. The Switch backend lives in
  `src/sys/switch/` and reuses the POSIX pieces that work, instead of adding
  `#ifdef`s to the Linux/macOS code.

## SD card layout

```
sdmc:/switch/openprey/
├── OpenPrey.nro          (builddir-switch/OpenPrey.nro)
├── base/                 retail Prey .pk4 files, copied from your own install
└── basepr/               OpenPrey overlay (.install/basepr from a desktop build;
                          the repo keeps its sources in basepy/)
```

This folder is also `fs_savepath`: configs, saves and the log are written there.
The log (`base/logs/openprey.log`, on by default) is flushed line by line, so its
last line is the last thing the engine printed. Two more files appear on failure:

- `openprey_error.txt`: the message of a fatal error. It is written before the
  engine shuts down, so it survives a crash during shutdown.
- `openprey_crash.txt`: CPU exceptions caught by the libnx exception handler,
  with PC, LR, a frame-pointer backtrace and registers. Resolve the `elf offset`
  values against the matching build:

  ```sh
  aarch64-none-elf-addr2line -f -C -e builddir-switch/OpenPrey-client_arm64.elf <offset>...
  ```

  Keep the `.elf` of every build you test: offsets only match their own build.

## Controls

The in-game mapping follows the Windows SDL3 backend so binds carry over (see the
header of `src/sys/switch/switch_input.cpp`). The left stick moves, the right stick
looks, and **+** opens the menu. In menus, the left stick moves the cursor, **A**
clicks, **B** goes back, and touching the screen clicks where you touch.

Default scheme (modeled on current console shooters; applied only to unbound keys,
so rebinding is kept): ZR fire, ZL alt fire, B jump, Y reload, X next weapon,
A spirit walk, R grenade, L lighter, L3 sprint and R3 crouch (toggles), D-pad up
zoom (toggle), down center view, right/left next/previous weapon. Schemes are
versioned (`in_switchControlScheme`): on upgrade, keys still holding the previous
default move to the new one.

**-** toggles the console. While it is down, **A** opens the system keyboard to type
a command, D-pad up/down walks the history, L/R scroll, and **B** or **-** close it.

### Gyro aiming

`src/sys/switch/switch_gyro.cpp` adds motion aiming on top of the right stick, fed
through the mouse path so it applies exact angles. Yaw is measured around the
world vertical (from gravity), so it works with the console upright or a Pro
Controller lying flat. Handheld, Pro Controller and dual Joy-Cons (right one aims)
are supported.

| cvar | default | meaning |
|---|---|---|
| `in_gyro` | 1 | 0 off, 1 always, 2 only while ZL (aim) is held |
| `in_gyroSensitivityX` / `Y` | 2.0 | camera degrees per degree the controller turns |
| `in_gyroDeadZone` | 1.0 | ignore rotation slower than this (deg/s) |
| `in_gyroInvertX` / `Y` | 0 | flip an axis |
| `in_gyroDebug` | 0 | print raw sensor values once per second |

The yaw direction is confirmed on hardware (handheld); pitch follows the assumed
x-right axis. `in_gyroDebug 1` prints raw values if a controller disagrees.

## Next steps

0. Map load time: `fs_profileLoads` (on by default on Switch) prints file system
   timings after each load (`fsLoadStats`). On Switch, `fs_caseSensitiveOS`
   defaults to 0 (the SD card is case insensitive; with 1 every failed open also
   listed the directory), the extra `stat()` per open is skipped, and
   `fs_cacheMissingDirs` skips lookups in search-path directories known to be
   missing (cleared on every write). Baseline before these changes: 72 s for
   game/roadhouse, 44 s of it loading 1206 images.
   With them: 59 s; 13246 lookups skipped by the cache; 462 MB read from the SD
   card in 23 s, almost all of it generated/ .bimage files. Those were
   uncompressed RGBA8 (`DeriveOpts`: "no need to compress"). On Switch,
   `image_compressTextures` (default 1) stores diffuse/default as DXT5 and
   specular as DXT1 (2 also compresses normal maps). This cuts load size and
   GPU memory bandwidth. The cache regenerates once, because it is keyed on the
   format. `com_logHitches` (ms, default 100) logs slow frames with the file
   work done in them.
0b. Clock profile: Status Monitor showed GPU 99% at 307.2 MHz (handheld default
   PerformanceConfiguration 0x00020003) with game logic ~3 ms and swap wait
   ~0.3 ms per 40-50 ms frame, so the game is GPU bound. `r_switchPerfProfile`
   (default 3) selects an official handheld configuration: 1 = GPU 384 MHz,
   2 = 460.8 MHz, 3 = 460.8 MHz + EMC 1600 MHz (0x92220007). The default
   profile is restored on exit.
   At 460.8 MHz the bottleneck moved to CPU core 0 (89-97%, cores 1-2 under 10%;
   GPU 68-99%). libnx starts every thread on core 0, including the OpenAL Soft
   mixer. The engine link wraps `pthread_create` (`-Wl,--wrap=pthread_create`)
   so every thread starts through a trampoline in `switch_threads.cpp`: the engine
   stays on core 0, the async tick on core 1, and all other threads (library ones
   included) go to core 2. `com_logPerf` render front/back columns now use
   clock-tick timers (`R_PerfTime`). Profile changes apply in game: the check
   compares values and cycles the CPU boost mode so the system re-applies the
   configuration.
   With threads placed, `com_logPerf` showed the render back end at 75-85% of the
   frame (23-38 ms standing, 80-95 ms while turning), with game logic ~3 ms and
   front end ~2-5 ms: Mesa validates state and re-uploads program constants on
   every draw. `r_cacheProgramParms` (default 1) skips ARB env parameter updates
   that do not change the value; `com_logPerf` also reports draws and skipped
   updates per frame. The structural next step is running the back end on its
   own core (the vertex cache already has a CPU-memory mode for that).
1. Performance on the Tegra X1. Hardware test at stock clocks: 22 fps at full
   resolution, 43 fps at half (fill-rate bound; shadows cost ~10%). Dynamic
   resolution is on by default on Switch: `r_dynamicResolution`,
   `r_dynamicResolutionFPS` (30), `r_dynamicResolutionMin` (50), `r_renderScale`
   (max, %). `com_showFPS 1` shows the 3D resolution in use. SSAO and bloom
   default to off on Switch (`OPENPREY_POSTFX_DEFAULT`).
2. Mesa 26 experiment (danfromtico/mesa-switch, 26.2.2 NVC0), tested on hardware:
   the GL probe passes (GL 4.3 compatibility, ARB programs, S3TC), but in game it
   was slower than devkitPro Mesa 20.1 (17-18 fps vs 21-22 at the same spot, full
   resolution) and showed rendering glitches. The build stays on devkitPro Mesa;
   `-Dswitch_mesa_sdk` is kept for re-testing newer versions of that port.
   GLthread would not help here: the bottleneck is GPU fill rate.
3. Audio: check which OpenAL Soft backend the devkitPro build uses.
4. Multiplayer: real sockets in `switch_net.cpp`.
