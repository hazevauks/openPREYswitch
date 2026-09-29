# Nintendo Switch Port (Work in Progress)

Homebrew port of OpenPrey to the Nintendo Switch using devkitPro (devkitA64 + libnx).
Players supply their own retail Prey (2006) `.pk4` files; nothing from the game is
distributed.

Working handbook: open issues, rules, build/test loop, pitfalls and measurements
in [switch-port-handbook.md](switch-port-handbook.md).

## Status

| Piece | State |
|---|---|
| Meson cross target (`horizon`) | Configures |
| `idlib` | Compiles |
| Game code (`src/game` + `src/Prey`, static library) | Compiles |
| Engine sources (framework, renderer, sound, ui, ...) | Compile |
| Platform layer `src/sys/switch/` | Files, time, threads with core placement, controller/touch/gyro input, system keyboard, EGL video, clock profiles, crash reports. Networking is loopback only. |
| GL loading (GLEW + generated GL 1.1 through `eglGetProcAddress`) | Done |
| `OpenPrey.nro` | Playable on hardware: new game, save/load and exit work. Handheld, profile 3: ~30 fps in light scenes, ~9-10 fps in the heaviest (the bar full of NPCs). See Performance. |
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
meson setup builddir-switch-mesa26 --cross-file tools/switch/meson/switch-cross.ini \
    -Dbuildtype=release -Dswitch_mesa_sdk=<mesa-switch>/mesa-install/opt/devkitpro/portlibs/switch
ninja -C builddir-switch-mesa26
```

Status: not recommended yet (slower and with rendering glitches on hardware; see
Performance).

This produces `OpenPrey-mesa-sdk.nro` (title "OpenPrey (mesa-sdk)"), which can sit
next to `OpenPrey.nro`. `-Dswitch_variant=<name>` picks another name. Runtime
switches for that Mesa (environment, set before `eglInitialize`):
`MESA_SWITCH_GL_DRIVER=zink|nvc0`, `MESA_SWITCH_GLTHREAD=0|1`.

The same option takes devkitPro's Mesa 20.1 rebuilt from source
([tools/switch/mesa20](../tools/switch/mesa20/README.md)). That is the base
for driver changes, because the pacman package is binary only.

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

## Performance

Measured on hardware in handheld mode. Where a number moved, the latest one is
given; the reasoning behind each setting is in the source comment next to it.

### Settings

| cvar | Switch default | What it does |
|---|---|---|
| `r_switchPerfProfile` | 3 | Clock profile, official handheld configurations: 0 = system default (GPU 307.2 MHz), 1 = GPU 384, 2 = GPU 460.8, 3 = GPU 460.8 + EMC 1600 + CPU 1224 MHz, 4 = as 3 with CPU 1785 MHz (experimental). Restored on exit. |
| `r_switchGLThread` | 1 | Mesa 26 build only: runs the GL driver on its own thread (core 2). Applies on `vid_restart`. |
| `r_fpsLock` | 30 | Holds frames 33.3 ms apart (0 = off). |
| `r_dynamicResolution` | 1 | 3D resolution follows the frame time (`r_dynamicResolutionFPS` 30, `r_dynamicResolutionMin` 50, `r_renderScale` max). |
| `image_compressTextures` | 1 | DXT5 diffuse/default, DXT1 specular (2 also normal maps). |
| `r_ssao`, `r_bloom` | 0 | Full-screen passes the GPU cannot afford (`OPENPREY_POSTFX_DEFAULT`). |
| `r_cacheProgramParms` | 1 | Skips ARB env parameter updates that do not change the value. |
| `fs_cacheMissingDirs` | 1 | Skips lookups in search-path directories known to be missing. |
| `fs_caseSensitiveOS` | 0 | The SD card is case insensitive. |

### Clocks

- **Profiles 3 and 4** set configuration 0x92220007 and raise the CPU to
  1224 MHz (3) or 1785 MHz (4) through clkrst (pcv before 8.0.0), the service
  sys-clk uses.
  - The rate is set when the profile is applied and again after every loading
    boost, which puts the CPU back to the configuration's rate.
  - One second later it is set once more, and the rate read back is logged
    (`CPU clock check`). On hardware the log said "set" while Status Monitor
    kept showing ~1015 MHz, so something else (sys-clk, the system) was
    overriding it.
  - It is not re-checked periodically. A once-a-second check fought sys-clk:
    the rate flipped between 1020 and 1224 MHz.
- **Map loads and `common->Init`** use the system FastLoad boost mode: CPU 1785 MHz,
  GPU at its minimum.
- **Applying a changed profile in game:** the new configuration is set, then the
  CPU boost mode is cycled so the system re-applies it.

### Threads

libnx starts every thread on core 0. The link wraps `pthread_create`
(`-Wl,--wrap=pthread_create`), so every thread, library ones included, goes to
its own core:

- core 0: the engine (game, render front end and back end);
- core 1: the async tick;
- core 2: everything else (OpenAL Soft mixer, the file system thread, ...).

Before this change, core 0 was at 90-97% while cores 1-2 stayed under 10%.

### Frame rate lock

`Switch_PaceFrame` sleeps between main loop frames, never inside one. Two
earlier designs failed on hardware:

- **Swap interval 2** (EGL reports a huge maximum) froze the loading screen.
- **Sleeping before each swap** made map loads crawl. The loading screen is
  redrawn after nearly every file read (`idSessionLocal::PacifierUpdate`), so
  each read waited up to 33 ms and a load took minutes at ~15% CPU.

Dynamic resolution subtracts the swap and pacing wait (`GLimp_LastFrameWaitMsec`),
so it judges the time a frame worked, and aims 10% under the frame budget.

### Where the time goes

`com_logPerf 1` logs one line per second:

- game, render front end and back end, and swap wait;
- draws, parameters skipped and vertex buffers created.

`com_logHitches` (default 100 ms) breaks each slow frame into the same parts,
plus its file reads.

`r_perfGpuSync 1` is a diagnostic that waits for the GPU before each swap. It
reports the wait, which splits the back end into CPU and GPU time.

Findings, at GPU 460.8 MHz and EMC 1600 MHz:

- **Game logic takes ~2-4 ms and the front end ~2-5 ms.** In the bar full of
  NPCs (~2000 draws), game logic rises to 10-20 ms and the front end to ~10 ms.
- **The render back end is 75-85% of the frame.**
- **Some scenes are fill-rate bound.** Standing in the bathroom, half the
  resolution doubled the frame rate. Status Monitor showed GPU 99% and ~15 GB/s
  of RAM traffic there.
- **Other scenes are bound by per-draw CPU/driver cost.** In the bar, the back
  end stayed at 75-90 ms at both 100% and 50% 3D scale, about 40 µs per draw.
  Dynamic resolution checks every drop and undoes it after a second when frames
  did not get at least 5% faster, then waits 10 s before trying again.
- **CPU and GPU time, measured with `r_perfGpuSync 1`** (bar area, ~1500 draws,
  91% scale):
  - the back end did ~25 ms of CPU work;
  - the GPU still needed ~19-28 ms after that.

  Without the sync, the back end took ~27 ms, so the GPU work overlaps the CPU
  work. Both sides are near the 33 ms budget at GPU 460.8 MHz.
- **Raising the CPU clock helped little.** With sys-clk at ~2.4 GHz, the frame
  rate barely moved. Status Monitor showed cores 0 and 3 busy and cores 1-2
  under 10%. Core 3 belongs to the system, which includes the GPU driver
  service that handles Mesa's submissions.
- **Ruled out:**
  - ARB env parameter uploads: the cache skipped ~3-5k updates per frame with no
    measurable change (22.8 vs 22.9 ms).
  - Vertex buffer re-creation: modest, ~15-20 buffers and ~200 KB per frame.
  - `r_useIndexBuffers 1`: no change in the bar (back end 43-47 ms either way).
    It created 30-60 more buffers per frame for animated models, which cancelled
    any saving on indices.
  - Swap interval 0 vs 1: no change.
- **Research (see sources below)** points to the driver. nouveau, which the
  Switch Mesa ports use, lacks several Maxwell features:
  - Zcull (hierarchical depth/stencil rejection), which matters for Doom 3
    style stencil shadow volumes;
  - compressed render targets, which save bandwidth;
  - the tiled cache;
  - several shader compiler optimizations.

  Its OpenGL path also has much higher CPU overhead than a low-level API. deko3d
  has all of these. The official Doom 3 port (Panic Button, from the BFG
  Edition) runs on Nintendo's NVN.

Sources:

- [deko3d README](https://github.com/devkitPro/deko3d/blob/master/README.md)
- [dhewm3 Switch port](https://github.com/fgsfdsfgs/dhewm3), with its
  [GameBrew notes](https://www.gamebrew.org/wiki/Dhewm3_Switch): 20-30 fps with
  shadows off, less with shadows on
- [Doom 3 (2019 version)](https://doomwiki.org/wiki/Doom_3_(2019_version))

### Mesa 26 experiment

[danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch) 26.2.2 NVC0 was
tested on hardware. The GL probe passes (GL 4.3 compatibility, ARB programs, S3TC).
In game it was slower than devkitPro Mesa 20.1 (17-18 fps vs 21-22 at the same
spot, full resolution) and showed rendering glitches. The build stays on
devkitPro Mesa, and `-Dswitch_mesa_sdk` is kept for re-testing.

That port submits through its own Horizon backend (syncpoints, no
libdrm_nouveau), and its GLthread is on by default
(`src/egl/drivers/switch/egl_switch.c`). GLthread records GL calls on the
calling thread and runs the driver on another one. The test above ran before
the `pthread_create` wrapper existed, so that driver thread shared core 0 with
the engine and could only add overhead.

Retest on 2026-09-29, after thread placement, at ~2000 draws:
- about as fast as Mesa 20.1 (~100 ms frames);
- the back end took 74-79 ms with GLthread and 76-86 ms without.

The glitches remain:
- vertical black stripes over lit walls, posters and the mirror;
- much darker areas;
- in the mirror scene, a black band at the top and part of the image shifted
  to the right.

The handbook (section 0) lists the cvars to bisect them. The port's source is at
[StevensND/mesa-switch](https://github.com/StevensND/mesa-switch), with forks by
danfromtico and NaGaa95.

## Load time

Loading game/roadhouse went from 118 s to ~50 s:

- `fs_caseSensitiveOS` 0: with 1, every failed open also listed the directory.
- No extra `stat()` per open.
- `fs_cacheMissingDirs`: 13246 lookups skipped per load.
- `image_compressTextures`: the generated/ `.bimage` cache was uncompressed
  RGBA8 (`DeriveOpts`: "no need to compress"), 462 MB per map. With
  compression it is 216 MB.

`fs_profileLoads` (on by default on Switch) prints `fsLoadStats` after each load.
It currently reports:

- 1206 images in ~28 s;
- 216 MB of SD reads in ~13 s;
- 1469 opens in 4.4 s.

The image cache regenerates once when `image_compressTextures` changes (it is
keyed on the format), which takes minutes on the Switch CPU.

## Next steps

1. **Mesa 26 glitches:** bisect them with the handbook's list, or update the
   port. Its speed now matches Mesa 20.1.
2. **Measure CPU scaling** with profile 3 vs 4 (1224 vs 1785 MHz), with sys-clk
   not overriding this title.
3. **Render back end on its own core** (SMP), so game + front end and back end
   overlap instead of adding up. The vertex cache already has a CPU-memory mode
   for that. GLthread would cover part of this.
4. **Native renderer (deko3d)**, only if the above cannot reach 30 fps. It is the
   path to Zcull, compressed render targets and low CPU overhead, and a large
   project.
5. **Loading:** fewer, larger reads for the image cache.
6. **Audio:** check which OpenAL Soft backend the devkitPro build uses.
7. **Multiplayer:** real sockets in `switch_net.cpp`.
