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

The cross file turns on link-time optimization (`b_lto`, 4 threads). Every
rebuild then relinks everything, about 10 minutes on the development PC even
for a one-file change. For quick iterations a separate dir with
`-Db_lto=false` works. Profile-guided optimization is covered in "LTO and PGO".

The result is `builddir-switch/OpenPrey.nro`. Useful partial targets:

- `ninja -C builddir-switch libopenprey_game_idlib.a`
- `ninja -C builddir-switch basepy/libgame_arm64.a`

### Homebrew entry and release zip

The homebrew menu lists the NRO as **openPREY** by **hazevauks**, with the
project version and the icon `assets/switch/icon.jpg` (256x256 JPEG).
`meson.build` passes them to `nacptool` and `elf2nro`. The file keeps the name
`OpenPrey.nro`, as in the SD card layout below.

`tools/switch/package_release.py` packs a release from the NRO and the overlay:

```sh
python3 tools/switch/package_release.py [--nro <path>] [--version <x>]
```

It writes `.tmp/release/openPREY-switch-<version>.zip`, laid out for the root of
the SD card:

- `switch/openprey/OpenPrey.nro`;
- the overlay in `switch/openprey/basepr/`: the files `basepy/meson.build`
  installs (keep the script in sync with it);
- a note in `switch/openprey/base/` that tells players where their `.pk4`
  files go.

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

This produces `OpenPrey-mesa-sdk.nro` (title "openPREY (mesa-sdk)"), which can sit
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
└── basepr/               OpenPrey overlay (from the release zip; the repo keeps
                          its sources in basepy/)
```

This folder is also `fs_savepath`: configs, saves and the log are written to
`basepr/`. The log (`basepr/logs/openprey.log`, on by default) is flushed line by
line, so its last line is the last thing the engine printed. At startup the
previous one is renamed to `openprey-previous.log`. Two more files appear on
failure:

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
clicks, **B** goes back, and touching the screen clicks where you touch. Clicking
a text field (for example the savegame name) opens the system keyboard. The typed
text replaces the field's contents; the menu's own button still confirms.

Default scheme (modeled on current console shooters; applied only to unbound keys,
so rebinding is kept): ZR fire, ZL alt fire, B jump, Y reload, X next weapon,
A spirit walk, R grenade, L lighter, L3 sprint and R3 crouch (toggles), D-pad up
zoom (toggle), down center view, right/left next/previous weapon. Schemes are
versioned (`in_switchControlScheme`): on upgrade, keys still holding the previous
default move to the new one.

**-** opens the settings menu (below). Its **Y** opens the console. While the
console is down, **A** opens the system keyboard to type a command, D-pad up/down
walks the history, L/R scroll, and **B** or **-** close it.

### Settings menu

`src/sys/switch/switch_settings.cpp` draws a settings overlay itself, so no
retail GUI file is changed or shipped. **-** opens and closes it:

- during play it also opens Prey's pause menu, so the game waits behind it,
  and closing it returns to the game;
- it does not go through Escape, which would skip a cutscene;
- D-pad up/down selects, left/right or **A** change the value, **B**/**-**/**+**
  close, **Y** opens the console.

Every change applies at once and is saved with the config (archived cvars).

| Item | cvar | Values |
|---|---|---|
| Show FPS | `com_showFPS` | Off / On |
| Frame rate lock | `r_fpsLock` | 30 fps / Off |
| Shadows | `r_shadows` | Off / On |
| Dynamic resolution | `r_dynamicResolution` | Off / On |
| GPU clock profile | `r_switchPerfProfile` | System default, GPU 384 MHz, GPU 460 MHz, GPU 460 + RAM 1600 |
| Gyro aiming | `in_gyro` | Off / Always / While aiming (ZL) |
| Gyro sensitivity | `in_gyroSensitivityX` and `Y` | 0.25 to 6.0 |
| Look speed | `in_yawspeed` and `in_pitchspeed` | 60 to 400 degrees per second at full tilt |
| Invert look | `in_joystickInvertLook` | Off / On (right stick, vertical) |
| Subtitles | `g_subtitles` | Off / On |
| Noclip | `g_noclip` | Off / On (single player; not archived) |

`g_noclip` lives in the game module (`hhPlayer::Think`): it follows the player's
`noclip` flag, so the item stays right after the `noclip` command, a new map or a
savegame, and setting it turns noclip on or off.

**Settings defaults** are versioned like the control scheme
(`com_switchSettings`, `s_settingsDefaults`). An entry applies once to configs
saved before it existed, so later choices stick. Version 1 turns shadows off,
version 2 turns gyro aiming off (`in_gyro 0` is now also the cvar default).

### Gyro aiming

`src/sys/switch/switch_gyro.cpp` adds motion aiming on top of the right stick, fed
through the mouse path so it applies exact angles. Yaw is measured around the
world vertical (from gravity), so it works with the console upright or a Pro
Controller lying flat. Handheld, Pro Controller and dual Joy-Cons (right one aims)
are supported.

| cvar | default | meaning |
|---|---|---|
| `in_gyro` | 0 | 0 off, 1 always, 2 only while ZL (aim) is held |
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
| `r_switchPerfProfile` | 3 | Clock profile, official handheld configurations: 0 = system default (GPU 307.2 MHz), 1 = GPU 384, 2 = GPU 460.8, 3 = GPU 460.8 + EMC 1600. The CPU clock is left to overclocking tools. Restored on exit. |
| `r_vertexPages` | 1 | Vertex cache blocks are carved out of shared 8 MB buffers, so the driver does not allocate per block (see "Vertex buffer churn, vertex pages and shadows"). Read at startup. |
| `r_shadows` | 0 | Stencil shadows, off by default on the Switch (see "Vertex buffer churn, vertex pages and shadows"). Applied once to older configs through `com_switchSettings`. |
| `r_switchGLThread` | 1 | Mesa 26 build only: runs the GL driver on its own thread (core 2). Applies on `vid_restart`. |
| `r_fpsLock` | 30 | Holds frames 33.3 ms apart (0 = off). |
| `r_dynamicResolution` | 1 | 3D resolution follows the frame time (`r_dynamicResolutionFPS` 30, `r_dynamicResolutionMin` 50, `r_renderScale` max). |
| `image_compressTextures` | 1 | DXT5 diffuse/default, DXT1 specular (2 also normal maps). |
| `r_ssao`, `r_bloom` | 0 | Full-screen passes the GPU cannot afford (`OPENPREY_POSTFX_DEFAULT`). |
| `r_cacheProgramParms` | 1 | Skips ARB env parameter updates that do not change the value. |
| `fs_cacheMissingDirs` | 1 | Skips lookups in search-path directories known to be missing. |
| `fs_caseSensitiveOS` | 0 | The SD card is case insensitive. |

### Clocks

- **Profiles 1-3** pick official handheld PerformanceConfigurations, which set
  the GPU and memory clocks. Profile 3 is 0x92220007 (GPU 460.8 MHz, EMC 1600
  MHz).
- **The CPU clock is not touched.** An earlier build raised it through clkrst
  (1224 or 1785 MHz). The log read the rate back as set, but overclocking
  sysmodules (sys-clk, Horizon-OC) keep applying their own CPU rate, and
  Status Monitor kept showing ~1015 MHz.
- **Recommended CPU clock:** set 1224 MHz, or 1785 MHz, in sys-clk or
  Horizon-OC for this title. Title override runs under the host game's title
  ID, so the rule goes on that game.
- **Map loads and `common->Init`** use the system FastLoad boost mode: CPU 1785
  MHz, GPU at its minimum.
- **Applying a changed profile in game:** the new configuration is set, then
  the CPU boost mode is cycled so the system re-applies it.

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
  - Vertex buffer re-creation in the roadhouse: modest, ~15-20 buffers and
    ~200 KB per frame. Busy later maps are another story (see "Vertex buffer
    churn, vertex pages and shadows").
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

### Vertex buffer churn, vertex pages and shadows

In game/feedingtowera (the first map after the roadhouse), `com_logHitches`
showed 200-750 vertex cache buffers created per frame, 1-3 MB, and single
frames with 700-1600 when new areas came into view. `r_shadows 0` made the game
much faster and removed stutters when looking around.

**The crash.** It happened twice in that map, both times during such a burst:

- a corrupted slab pointer in `nouveau_mm_allocate`, reached from
  `glBufferData` (`R_AddModelSurfaces` → `idVertexCache::Alloc` →
  `st_bufferobj_data` → `nouveau_buffer_create`);
- the second time with `r_shadows 0` and `r_reuseVertexStorage 1`, in a
  1594-buffer frame.

So shadows only add to the load. The problem is the number of driver
allocations.

**Where the buffers come from.** Animated models are re-instantiated whenever
their entity changes, which is every frame for anything animating. Each new
surface gets fresh caches through `idVertexCache::Alloc`, and the old ones are
freed:

- its vertexes (`R_CreateAmbientCache`);
- its shadow vertexes, twice the vertex count
  (`R_CreateVertexProgramShadowCache`);
- index caches for its light and shadow triangles.

Static surfaces entering view for the first time also create caches, which
makes the bursts.

**Why it hurts on the Switch.** Before this change, every static block owned a
GL buffer, and every allocation was a `glBufferData`. In Mesa 20.1's nouveau
driver, each one costs:

- a new resource from its suballocator (`nouveau_mm_allocate`);
- a staging copy, which is a second allocation;
- deferred frees through fence callbacks (`nouveau_fence_work`, which also
  flushes the command buffer once a fence has more than 64 of them).

In `libdrm_nouveau` for the Switch, every GPU buffer is `memalign` memory on
the same heap as the game. That machinery ran thousands of times in a busy
frame, and it is where both crashes were.

**The first attempt.** `r_reuseVertexStorage` (build of 2026-09-29) reused
same-size storage. On hardware it reused about 80% in steady frames but none
in bursts, and did not stop the crash. It was removed.

**Change: vertex pages** (`r_vertexPages`, on by default on the Switch).

- Static blocks up to 1 MB are carved out of shared 8 MB buffers ("pages"),
  created with `GL_STREAM_DRAW` so the driver keeps them in CPU-mappable
  memory.
- A block is written with `glMapBufferRange(..., GL_MAP_UNSYNCHRONIZED_BIT)`.
  In nouveau that returns a pointer into the page with no wait, no staging and
  no allocation (`nouveau_buffer_transfer_map`), and the write is a plain
  copy.
- The engine keeps it safe:
  - a freed range goes on a retired list;
  - at the end of each frame, the retired ranges get a fence
    (`glFenceSync`);
  - they are handed out again only when that fence has signaled, so the GPU
    never reads a range that is being rewritten.
- Ranges come in size classes: multiples of 64 bytes up to 256, then four per
  power of two. Each class has its own free list, so there is no search or
  merge, and a block wastes at most a quarter of its range.
- The driver allocates only when a page is added. Larger blocks and the frame
  temp buffers keep their own buffer.
- The frame temp buffers are written the same way, after the fence of the frame
  that last used them. Before, a `glBufferSubData` into a buffer drawn from
  earlier in the frame made the driver stage every write.
- `com_logPerf` / `com_logHitches` show `buffers N (K KB, P paged)`: N blocks
  allocated, P of them from pages. The rest are driver allocations.
- `com_logPerf` also shows `heap N MB`, the heap in use (GPU buffers and
  textures included). A leak would show there.
- `listVertexCache` prints the pages, free ranges and pending fences.

**Not done:** moving per-frame surfaces to the frame temp buffer
(`AllocFrameTemp`). Cached dynamic models (`DM_CACHED`) keep their surfaces
across frames when the entity does not change, so a frame temp cache could be
read after it was overwritten. Pages give the same saving without that risk.

**Shadows themselves.** With the allocations gone, shadows still cost:

- the stencil volume fill, with no Zcull on nouveau;
- shadow caches twice the size of the model's vertexes.

Hardware numbers in the same scene (2026-09-29 log): shadows on, back end
33-50 ms at 15-20 fps; shadows off, back end 17-22 ms at 25-28 fps.

### LTO and PGO

Evaluated on 2026-09-30.

**What they can reach.** Both only optimize code we compile:

- game logic, the render front end, and the engine's share of the back end;
- not Mesa or libdrm_nouveau, which come prebuilt from devkitPro (`-O2`);
- not GPU time.

On hardware the back end dominates heavy scenes (33-50 ms in feedingtowera
with shadows, ~40 µs of driver CPU per draw in the bar). Game plus front end
are 5-20 ms. Typical gains for game engines are a few percent from LTO and
5-15% from PGO on the code they cover.

**LTO is the default** (`b_lto` in the cross file). On hardware (2026-09-30,
shadows off, CPU 1224 MHz through Horizon-OC), the LTO build held 30 fps
through the opening maps, dipping to 25. The plain `-O3` build dropped below
that in the same places. The NRO is 16.3 MB instead of 15.3 MB, because of
more inlining. Two things had to be fixed:

- **The game module.** It is prelinked into one relocatable object whose
  symbols `objcopy` localizes (`tools/switch/make_game_object.py`). Under LTO
  that object held bytecode that `objcopy` cannot localize, and the final link
  failed on 1349 duplicate idlib symbols. With `--lto` (passed when `b_lto` is
  on), the prelink runs the optimizer and emits machine code
  (`-flinker-output=nolto-rel`). The game is then its own LTO unit, as a DLL
  is, and the engine is another.
- **An ODR violation.** Two different `clipTri_t` structs existed, in
  `Interaction.cpp` and `tr_stencilshadow.cpp`. The first is now in an
  anonymous namespace.

A remaining linker note about the size of `__nx_exception_stack` is harmless:
the final symbol is 32 KB, as defined in `switch_main.cpp`.

Costs:

- every rebuild relinks everything, ~10 minutes even for a one-file change;
- inlining makes crash backtraces coarser. `addr2line` still names the
  functions.

**PGO** (`-Dswitch_pgo`). An instrumented build records which code runs and
how often while someone plays. A second build uses that profile to lay out
and inline the hot paths.

1. **Instrumented build:**

   ```sh
   meson setup builddir-switch-pgo --cross-file tools/switch/meson/switch-cross.ini \
       -Dbuildtype=release -Dswitch_pgo=generate -Dswitch_variant=pgo
   ninja -C builddir-switch-pgo
   ```

   It gives `OpenPrey-pgo.nro`, which runs slower than the release build.
2. **Training run:**
   - play a representative session (menus, a map load, combat, walking
     through busy areas);
   - quit from the game's menu. The profiles are written at a clean exit;
     a crash or closing from HOME without quitting writes nothing.
   - one `.gcda` per object lands in `sdmc:/switch/openprey/pgo/`.
3. **Copy the profiles:** copy that folder's files to `.tmp/pgo-data/` in the
   repository.
4. **Optimized build:**

   ```sh
   meson configure builddir-switch-pgo -Dswitch_pgo=use -Dswitch_variant=pgo-use
   ninja -C builddir-switch-pgo
   ```

   `switch_pgo_data` points elsewhere if the profiles are not in
   `.tmp/pgo-data/`.

How the profile files are named and found:

- **Names:** `-fprofile-generate=<dir>` and `-fprofile-prefix-path=<build dir>`
  name each file after its object path relative to the build dir, with `/`
  turned into `#` (for example `OpenPrey-client_arm64.elf.p#src_renderer_VertexCache.cpp.gcda`).
  So the files do not depend on where the build dir is. The prefix must be
  spelled as the compiler's working directory, in Windows form under MSYS2
  (`cygpath -w` in meson.build).
- **Separator:** devkitA64's GCC runs on Windows and joins the directory and
  the name with a backslash. The instrumented build wraps `fopen`
  (`-Wl,--wrap=fopen`, `OPENPREY_PGO_GENERATE` in `switch_main.cpp`) to turn
  it into `/`, and creates the `pgo` folder at startup, because libgcov
  creates no directories here.
- **Threads:** `-fprofile-update=prefer-atomic` keeps counters right when
  several threads run engine code.
- **Optimized build:** `-fprofile-partial-training` keeps code the session did
  not reach optimized normally. `-fprofile-correction` accepts small
  inconsistencies.

The profile has to be redone when the code changes much. The next CPU lever is
the driver: Mesa 20.1 built from source
([tools/switch/mesa20](../tools/switch/mesa20/README.md)) could itself be
built with LTO/PGO.

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

## Memory

The libnx heap is ~3.1 GB (title override or forwarder). Every `malloc`, and
every GPU buffer and texture (`libdrm_nouveau` allocates them with `memalign`),
comes out of it. `openprey.log` reports it around each map load
(`memory before loading:` / `memory after loading:`), with lines for images
(`images: purged ... kept ...`, `N images resident`) and sounds
(`sounds: released ...`, `sounds: N samples resident`). "Out of memory"
errors give the size requested, and `openprey_error.txt` the heap at that
moment.

**Sounds were the largest user, and they were never released** (fixed
2026-09-30):

- **Decoding:** the BFG-style sound code decodes every sample to PCM when it
  loads, music included, with no streaming. All of Prey's audio decodes to
  ~2.7 GB, 2.2 GB of it music. The largest track,
  `music/score/spindle_b1_action`, is 123 MB of PCM.
- **Two copies:** each decoded sample was kept twice, once in the engine and
  once in OpenAL (`alBufferData` copies the data). The engine copy is now
  freed after the upload. Voices only play the OpenAL buffer and use the
  sample counts, which are kept.
- **Never released:** `idSoundSystemLocal::LoadSample` marked every sample
  never-purge; the level-based condition was commented out. Each map's music,
  voices and ambience stayed after the map was left, and a run from the start
  ran out of memory loading the third map.
  - Now only samples referenced before the first map load (menus) stay.
  - `BeginLevelLoad` releases the rest, except the ones still playing (the
    loading music starts before it).
- **Reloading:** a released sample comes back when:
  - `LoadSample` is called for it again;
  - the loading level looks up its sound shader
    (`idDeclManagerLocal::FindType` → `idSoundShader::ReloadPurgedSamples`),
    which covers the sounds a map precaches;
  - it plays (`idSoundEmitterLocal::StartSound`), for anything a level did
    not precache. That costs a decode on the game thread the first time.
- **Other users:**
  - textures, ~350 MB per map at most (a cached load of feedingtowera reads
    364 MB, all files included);
  - models, collision, AAS, scripts;
  - the heap fragmentation large decodes cause.

**Savegame restore left garbage in render entities** (fixed 2026-09-30):

- **Cause:** `renderEntity_t` / `renderLight_t` carry fields from the Raven
  lineage that Prey savegames do not store (`overlayShader`,
  `suppressSurfaceMask`, `weaponDepthHackInViewID`, `globalLight`...).
  `hhGameLocal::Restore` re-creates the static render entities with a plain
  `new`, so those fields held whatever was in that memory.
- **Symptoms after loading a save:**
  - `overlayShader` pointing at a real material drew models a second time with
    a random material: graphical errors and a slower frame;
  - a pointer that was not a material crashed `R_GlobalShaderOverride`.
- **Fix:** `ReadRenderEntity` and `ReadRenderLight` now set those fields as
  spawning does, and the static entities are cleared before reading.

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

**The first visit to a map** builds that map's cache. game/feedingtowera took
130 s the first time, 100 s of it for 1550 images, 816 of which were compressed
and written to `generated/`. File reads were only ~23 s of that. Later visits
read the cache. Compressing on cores 1-2 while core 0 loads would shorten
first visits.

**Cached loads were still slow (2026-09-30 log).** Loading feedingtowera from
a save, with its cache built, took 78.5 s:

- 1503 images took 45 s;
- 364 MB of OS reads took 21.6 s, about 17 MB/s.

newlib gives every `FILE` a 1 KB stdio buffer, so each KB was a separate call
into the system file service. Files the engine opens now get a 64 KB buffer
(`SWITCH_STDIO_BUFFER_BYTES`, set in `OpenOSFile` and in `Unzip.cpp` for
pk4s), which also covers writes to `generated/`. To confirm, compare the
`OS reads: N MB in T s` line on the same load.

**Rebuilt collision files.** game/feedingtowerb printed
`maps/game/feedingtowerb.cm is out of date` and rebuilt its collision model
from the map: 79 s of a 175 s load. The cause was the engine's map CRC, not the
retail files (see "Map data: collision and AAS"); with it fixed, the shipped
`.cm` loads and first visits skip that rebuild.

## Map data: collision and AAS

Prey's `.cm` (collision) and `.aas` (monster navigation) files start with the CRC
of the map's geometry, and the engine only uses them when it matches the CRC it
computes from the `.map`. `idMapEntity::GetGeometryCRC` (`src/idlib/mapfile.cpp`)
came from Quake 4, which also XORs the entity's `model` key into the CRC once per
primitive. Prey's tools used the Doom 3 CRC, so no shipped file ever matched:

- **Collision:** every `.cm` was "out of date". The engine rebuilt the collision
  from the `.map` and wrote the result to `basepr/maps/game/<map>.cm`, which is
  searched before the pk4s, so later loads used the rebuild without a message.
  The rebuild is not what Prey's tools made. In game/feedingtowerc it had ~7900
  fewer polygons (39792 against 47652) and giant unclipped polygons that
  stretched the world bounds to 524288 units ("map bounds are (524288.0, ...)";
  the real world is ~7200 x 6900 units). `g_debugPlayerPhysics` showed Tommy
  standing at z 254.6, under the shipped clip brushes (266-272) and floor (268)
  of that room, then falling out of the map; in the tunnel he walked through
  walls. With bounds that large, the clip sector grid (`_HH_CLIP_FASTSECTORS`,
  64 x 64 cells over the world) also put the whole map in a few cells, so every
  trace tested every clip model of the map.
- **AAS:** every `.aas` was rejected with a developer-only message (`DPrintf`,
  `DWarning`), so monsters had no navigation data on any map. That also hid a
  parser bug: `ParseAreas` read the two feature fields of OpenQ4's AAS 1.08,
  which Prey's 1.07 files do not have (`( flags contents firstFace numFaces
  cluster clusterAreaNum )`), and would have derailed on the first area. It now
  takes either layout; a script that follows the same steps parses all 75 retail
  `.aas` files to the end, every reachability pointing at a valid area.

The fix computes the Doom 3 CRC, with `char` read as signed like on x86, where
the shipped CRCs were made (recomputing it in Python from the retail `.map`
files gives exactly the stored 2519709809 for feedingtowerc and 3253044049 for
lotaa; the Quake 4 CRC gives 2519710351 and 3253043229). A rebuild left by an
older build still hides the pk4 copy and now fails the CRC check, so
`LoadCollisionModelFile` removes it (`RemoveFile` only touches the writable game
directory) and loads the shipped file. The log shows
`removing the rebuilt maps/game/<map>.cm to load the shipped one` once per map.

On the first load after the fix, look for:

- `map bounds are` close to the real size of the map;
- `collision data:` numbers that match the shipped `.cm` (feedingtowerc:
  445 models, 55790 vertices, 47652 polygons, 3953 brushes, ~17 k nodes);
- `[Load AAS]` and `loading maps/game/<map>.aas48` followed by `done.` for
  each AAS file the map has (a map lists more AAS types than it ships; the
  missing ones end without `done.`).

The deathwalk level (`deathwalk1-3`, appended to most maps in retail Prey) is
still not loaded: `shouldappendlevel` is never set and `AppendMap` is a stub.

## Next steps

1. **PGO:** a training run with `OpenPrey-pgo.nro`, then the optimized build
   (see "LTO and PGO"). Compare it with the LTO build at the same spots.
2. **Front end in heavy scenes:** late feedingtowerb ran at 6-7 fps with a
   60-120 ms front end. Read the `hitch front end:` lines from that spot, then
   optimize what dominates: subviews, CPU skinning (NEON SIMD), or
   interactions. Vertex pages were confirmed on hardware on 2026-09-30: no
   crash, every block paged, heap stable.
3. **Loading:** compress first-visit textures on cores 1-2.
4. **Render back end on its own core** (SMP), so game + front end and back end
   overlap instead of adding up. The vertex cache already has a CPU-memory mode
   for that. GLthread would cover part of this.
5. **Native renderer (deko3d)**, only if the above cannot reach 30 fps. It is the
   path to Zcull, compressed render targets and low CPU overhead, and a large
   project.
6. **Audio:** check which OpenAL Soft backend the devkitPro build uses.
7. **Multiplayer:** real sockets in `switch_net.cpp`.
