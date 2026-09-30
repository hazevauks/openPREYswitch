# Nintendo Switch port handbook (OpenPrey)

Working guide for continuing the OpenPrey Switch port and for starting similar
ports. It collects what was learned in practice: rules, environment, test
loop, pitfalls and measurements.

Technical reference for the port: [switch-port.md](switch-port.md).

**In a new session with Claude, start with:**

> Read `docs-dev/switch-port-handbook.md` and `docs-dev/switch-port.md` before starting.

---

## 0. Open issues (read first)

Updated after every test round. When an issue is solved, it leaves this
section and the lesson goes to section 5.

**Story progress (2026-09-30):**

- **The roadhouse plays through:** NPC conversations, the bar fight, and the
  abduction with its level change.
- **game/feedingtowera loads and plays**, further in each round, until the
  driver crash below.

### Crash in the driver in game/feedingtowera, and stutters (fix to confirm)

- **Symptoms:**
  - two crashes during play in feedingtowera (2026-09-29 and 2026-09-30), the
    second in another area of the map;
  - with `r_shadows 1`, stutters when looking around;
  - `r_shadows 0` makes the game much faster: back end 17-22 ms instead of
    33-50 ms, 25-28 fps instead of 15-20.
- **Crash reports:** corrupted slab pointers (`0x2000000020056a44`,
  `0x6e400000000265d2`) read in `nouveau_bo_ref` inside Mesa's
  `nouveau_mm_allocate`. The call chain is `R_AddModelSurfaces` →
  `idVertexCache::Alloc` → `glBufferData` → `st_bufferobj_data` →
  `nouveau_buffer_create`.
- **Log:** 200-750 vertex cache buffers created per frame, 1-3 MB. The second
  crash came right after a 1594-buffer frame, with `r_shadows 0` and
  `r_reuseVertexStorage 1`. The roadhouse creates ~20.
- **Cause (analysis):**
  - every vertex cache block had its own GL buffer, so every allocation was a
    `glBufferData`;
  - in nouveau, each costs a suballocation, a staging copy and deferred frees
    through fence callbacks;
  - animated models re-create their vertex, shadow and index caches every
    frame, and new areas add bursts of hundreds more;
  - shadows add to the count, but they are not the root cause.
- **First attempt (build of 2026-09-29):** `r_reuseVertexStorage` reused
  same-size storage. It reused ~80% in steady frames but none in bursts, and
  the crash came back. It was removed.
- **Change (build of 2026-09-30): vertex pages** (`r_vertexPages 1`, the
  default):
  - blocks are carved out of shared 8 MB buffers and written through
    unsynchronized maps, which cost no driver allocation;
  - freed ranges wait for a GPU fence before reuse;
  - the frame temp buffers are written the same way.

  Details are in switch-port.md, "Vertex buffer churn, vertex pages and
  shadows".
- **Test** in feedingtowera, with `com_logPerf 1` and `r_shadows 1`:
  1. play through the area where it crashed;
  2. walk into new areas and look around;
  3. send the log.

  In `buffers N (K KB, P paged)`, P should equal N. Check whether the stutters
  and the crash are gone. `heap N MB` should stay flat over time. For
  comparison, `r_vertexPages 0` plus a restart brings the old path back.

### Long first loads of new maps

- **game/feedingtowera took 130 s the first time.** Of that, 100 s were images:
  816 textures compressed to DXT and written to `generated/`.
- **Later visits read the cache.** Roadhouse takes ~50 s with its cache.
- **Idea:** compress on cores 1-2 while core 0 keeps loading (see
  switch-port.md, Next steps).

### Savegame name keyboard (fix to confirm)

- **Test of 2026-09-29:** Y opened the keyboard, but the typed text was
  appended to the default name instead of replacing it. Guessing the button was
  also awkward.
- **Cause:** `idEditWindow` only takes Backspace as a character event (what
  Windows sends with the key). The Switch code posted it as a key event, which
  the field ignores.
- **Change (build of 2026-09-30):**
  - clicking a text field (A, or a touch) opens the keyboard;
  - Backspace is sent as a character, so the old text is erased;
  - Y no longer does anything special in menus.
- **Test:** in the save menu, click the name field with A, type a name, and
  save.

### LTO build (A/B test pending)

- **Asked by a friend of the user (2026-09-30):** evaluate LTO and PGO.
- **LTO builds and links** as `OpenPrey-lto.nro` (`builddir-switch-lto/`,
  `-Db_lto=true`). It needed an LTO mode in `make_game_object.py` and an ODR
  fix. PGO is feasible but not done; see switch-port.md, "LTO and PGO".
- **Expected gain is small:** it covers only our CPU code (game, front end),
  not Mesa or the GPU.
- **Test:** at the same spot, `com_logPerf 1` on `OpenPrey.nro` and on
  `OpenPrey-lto.nro`; compare fps, game, front and back. Make it the default
  only if the gain is clear and nothing breaks.

### Rendering glitches in the Mesa 26 build (parked)

- The user decided not to test Mesa 26 again. OpenPrey now works with Mesa
  20.1 built from source (section 6).
- **Symptoms:**
  - vertical black stripes over lit surfaces;
  - areas darker than they should be;
  - a black band and a shifted image in the mirror scene.
- **Ruled out:** specular, bump, render scale, GLthread and index buffers.
- **Untested:** `r_shadows 0`.

### Zoom (D-pad up)

- Prey's code only zooms with a weapon in hand (section 3). Check once a
  weapon is available.

---

## 1. Rules that do not change

- **Never touch** `C:\Users\Usuario\Downloads\trabalho\openPREYwindows (NÃO MEXER)`.
- **Push only to the user's fork** (`hazevauks/openPREYswitch`), branch
  `switch-port`. PRs to the original repository (`themuffinator/OpenPrey`)
  only on request.
- **Never commit game files** (`.pk4`, textures, audio). Players use their own
  copy of Prey.
- `src/game` mirrors the OpenPrey-GameLibs repository (rule from `AGENTS.md`).
  Game code changes have to be carried over there too. **Pending:** the
  `long` to `int` fix in `src/game/Pvs.cpp`.
- Temporary files go in `.tmp/`, which git ignores.
- Meson is the official build system. Changed the workflow? Update
  `docs-dev/switch-port.md` in the same commit.
- Everything written (READMEs, docs, commits) is in English; chat with the
  user is in Portuguese.
- Claude never types passwords or credentials. Logging in is the user's job.

## 2. Environment (Windows)

| Item | Where / how |
|---|---|
| devkitPro | `C:\Users\Usuario\Downloads\devkitPro` |
| Build shell | `devkitPro\msys2\usr\bin\bash.exe`, **always with `MSYSTEM=MSYS`** (in MinGW mode the msys `meson`/`python` do not run) |
| `/opt/devkitpro` path | Mapped in `devkitPro\msys2\etc\fstab`. Backup: `fstab.bak-antes-openprey`. If the devkitPro folder moves, fix that line. |
| Python | Windows Python is not installed. Use `devkitPro/msys2/usr/bin/python3.exe`. |
| Cross file | `tools/switch/meson/switch-cross.ini` (`host_machine.system = 'horizon'`, `-D__SWITCH__`) |
| Build dirs | `builddir-switch/` (main), `builddir-switch-mesa26/` (Mesa 26 experiment), `builddir-switch-mesa20/` (Mesa 20.1 built from source), `builddir-switch-lto/` (LTO variant) |

### Building

```bash
MSYSTEM=MSYS /c/Users/Usuario/Downloads/devkitPro/msys2/usr/bin/bash.exe -lc "cd /c/Users/Usuario/Downloads/trabalho/openPREYswitch && export MESON_RSP_THRESHOLD=2147483647 && meson compile -C builddir-switch"
```

- `MESON_RSP_THRESHOLD=2147483647` is **required**. Without it, long link
  lines go into a response file, MSYS2 does not convert its paths, and the
  link fails.
- A full build takes about 15 minutes. Touching shared headers (for example
  `RenderSystem.h`) rebuilds everything; touching only
  `src/sys/switch/*.cpp` is quick.
- Configuring from scratch:
  `meson setup builddir-switch --cross-file tools/switch/meson/switch-cross.ini -Dbuildtype=release`
- Output: `builddir-switch/OpenPrey.nro`. With symbols:
  `builddir-switch/OpenPrey-client_arm64.elf`.

### Releasing each build (always in this order)

1. Build.
2. **Archive the ELF:** copy it to `.tmp/elf-builds/OpenPrey-<commit>.elf`.
   Crashes cannot be read without the ELF of the same build.
3. Copy the NRO to `.tmp/cartao-sd/switch/openprey/OpenPrey.nro`.
4. Commit with a descriptive message and `git push origin switch-port`.
5. Give the tester a short numbered test plan with the exact console commands.

## 3. Testing on the console

- Run through **title override**: hold R while opening an installed game, to
  get full memory. Needs Atmosphère, hbmenu/sphaira and Status Monitor (shows
  CPU, GPU, RAM and fps).
- SD card folder: `sdmc:/switch/openprey/`
  - `OpenPrey.nro`
  - `base/`: the original Prey `.pk4` files
  - `basepr/`: the OpenPrey overlay (includes
    `materials/renderscale_openprey.mtr`, needed for dynamic resolution)
- **Files to send after each test:**
  - `base/logs/openprey.log`: written line by line, so the last line is the
    last event before a hang.
  - `openprey_error.txt`: fatal error message.
  - `openprey_crash.txt`: CPU exception (PC, LR, backtrace).
  - Atmosphère reports (`atmosphere/crash_reports/`), when the system shows an
    error screen.
- **The engine writes into `basepr/`** (the first search path):
  - `OpenPreyConfig.cfg`: settings and binds. Deleting it restores the
    defaults.
  - `generated/images/*.bimage`: cache of the DXT-compressed textures.
    Deleting it makes the next load of each map recompress every image on the
    CPU, which takes several minutes and looks like a hang. The log shows
    `Writing generated/images/...` lines. Only delete it when
    `image_compressTextures` changes.
  - Saves and logs.
- Reading a crash:
  `aarch64-none-elf-addr2line -f -C -e .tmp/elf-builds/OpenPrey-<commit>.elf <offsets>`
- **Interacting** (pressing buttons, using screens) is the attack button (ZR),
  as in the original Prey: get close and aim at the target.
  - Zoom (D-pad up) only works with a weapon (`hhPlayer`, game code).
  - **Prey has no button for talking to NPCs.** The bar conversations
    (`script/map_roadhouse.script`) start on their own:
    - when walking through invisible triggers (`trigger_once`, for example
      `Conversation2` in the hallway);
    - when standing within 312 units of the bar counter
      (`WaitPlayerNearBar`).

    The remarks from the drunks and from Jen are touch triggers too.
  - **Do not use `noclip` in scripted areas.** In noclip the player does not
    touch triggers (`if ( !noclip ... ) TouchTriggers()` in `Player.cpp`), so
    the scene does not progress. Load an earlier save and walk normally.
- **Game console:** the **−** button opens and closes it; **A** opens the
  system keyboard.

## 4. Switch code map

| File | Role |
|---|---|
| `src/sys/switch/switch_main.cpp` | `main`, engine thread (16 MB stack), paths, events, crash handler, clocks (`r_switchPerfProfile` GPU/memory profiles, loading boost), `com_logPerf`/`com_logHitches`, clean exit |
| `switch_glimp.cpp` | EGL/Mesa, GL 4.3 compat context at 1280x720, swap, frame rate lock (`r_fpsLock`, `Switch_PaceFrame`) |
| `switch_input.cpp` | Controls (game/menu/console), versioned default binds (`in_switchControlScheme`), system keyboard, touch |
| `switch_gyro.cpp` | Gyro aiming (`in_gyro*`) |
| `switch_threads.cpp` | Threads and locks; `__wrap_pthread_create` places each thread on a core |
| `switch_net.cpp` | Loopback-only networking (stub) |
| `tools/switch/make_game_object.py` | "Fake DLL": links the game into one object (`ld -r`) exporting only `GetGameAPI` |
| `tools/switch/gen_gl11_loader.py` | Generates the GL 1.1 function pointers through `eglGetProcAddress` |
| `tools/switch/gltest/` | GL capability probe for the hardware |
| `tools/switch/mesa20/` | Rebuilds devkitPro's Mesa 20.1 from source |
| `src/renderer/RenderSystem.cpp` | Render scale and dynamic resolution, performance timers |
| `src/renderer/tr_backend.cpp` | Parameter cache, performance counters, `r_perfGpuSync` |
| `src/renderer/Image_load.cpp` | DXT texture compression (`image_compressTextures`) |
| `src/framework/FileSystem.cpp` | Directory cache, load statistics, stopping the download thread |

## 5. Pitfalls found so far (and the fix)

1. **`long` is 64 bits on the Switch (LP64).** The 2006 code assumes 32 bits.
   It caused crashes in collision (sign macros), `RSqrt`, `rvRandom`,
   `Pvs.cpp`, and a truncated `threadHandle` (now `intptr_t`). For any strange
   crash, look for `long` used as if it were 32 bits.
2. **Texture unit overflow:** `MAX_MULTITEXTURE_UNITS` went up to 32. The
   driver reports more units than the array held, and the overflow overwrote
   memory.
3. **Fatal error without a message:** the engine crashed while shutting down
   and the message was lost. It is now written to a file **before** shutdown.
4. **Crash on exit:**
   - Exiting from inside the engine thread: it now uses `pthread_exit` and
     returns from `main`.
   - A thread left alive (the background download thread) made the system
     complain on close: it is now stopped. Every thread created must end
     before `main` returns.
   - System services (gyro, clock profile) must be closed and restored on
     exit.
5. **Crash when loading a save:** null material in `R_GlobalShaderOverride`. A
   bug in the original engine, fixed with a null check.
6. **Build:**
   - The `BIT` macro exists in both idlib and libnx: `#undef BIT` before
     `<switch.h>`.
   - `_LITTLE_ENDIAN` clashes with the network headers.
   - idlib bans `snprintf`: use `idStr::snPrintf`.
   - newlib hides `fileno()` and `setenv()` in strict C++ mode: declare them
     with `extern "C"`.
   - COMDAT groups in `ld -r` need `--force-group-allocation`.
7. **Editing files:**
   - Many engine files use CRLF line endings, and perl/sed substitutions fail
     silently on them. Prefer the Edit tool.
   - When generating C code from a script (heredoc + Python), the `\n` inside
     strings can turn into a real line break and break the `printf`. Check the
     result with `grep`.
8. **Clock profile not applying right away:** the check now compares values
   and cycles the boost mode so the system re-applies the configuration.
9. **Gyro X axis inverted:** fixed in code. Anyone who had
   `in_gyroInvertX 1` saved must set it back to 0.
10. **A frame rate lock that looked like a hang during loads.** While loading,
    the game redraws the loading screen after nearly every file read
    (`PacifierUpdate`). A 33 ms wait at the swap turned into minutes of
    loading at ~15% CPU. Swap interval 2 froze the screen. The lock now waits
    **between** main loop frames (`Switch_PaceFrame`), never inside a frame.
    General rule: never sleep inside the renderer.
11. **Not every control problem belongs to the port.**
    - Interacting with the attack button and the zoom that needs a weapon are
      game rules.
    - NPC conversations are script triggers, which `noclip` disables.

    Before "fixing" a control, read the map script (inside the pk4 files) and
    check whether the Windows build behaves the same.
12. **Leave the CPU clock to overclocking tools.** The game raised the CPU
    through clkrst, and the rate read back as set. But sys-clk and Horizon-OC
    keep applying their own CPU rate, so Status Monitor never showed the
    change. Re-applying every second only made the rate flip. The profiles now
    set only official GPU/memory configurations, and the recommended CPU rate
    (1224 or 1785 MHz) goes in the overclocking tool, per title.
13. **Building a 2020 Mesa with today's devkitPro:** Python 3.12 removed
    `distutils`, and the current newlib declares `timespec_get()` without
    implementing it. `tools/switch/mesa20` patches both.

14. **64-bit script VM: vector parameters.** `idProgram::AllocDef` gave a
    function-scope vector three 8-byte float slots (24 bytes), while callers
    push 16 (`E_EVENT_SIZEOF_VEC`). Every parameter after a vector was read 8
    bytes off.
    - **What it broke:** the roadhouse script `DistanceToXY( vector, vector )`
      returned garbage, so the grandfather conversation (`WaitDistance`) never
      went on and the level could not be finished. Fixed in
      `src/game/script/Script_Program.cpp`.
    - **How it was found:** `g_debugPlayerCanSee` and `g_debugTriggers` (they
      log the conditions scripted scenes wait on) showed that the trigger fired
      and the distance was fine, but `playerCanSee` was never reached.
    - **It is not Switch-specific:** it hits every 64-bit build, and every
      script function taking a vector followed by other parameters.
    - **Lesson:** when a scripted scene stalls, log what it waits on before
      suspecting triggers or animation.
15. **Precise trigger test.** `ClipContents` can miss the player.
    `idEntity::TouchTriggers` already fell back to bounds, but
    `hhTrigger::IsEncroaching` did not, so `isSimpleBox 0` triggers were
    touched and then dropped. Both now accept the player by bounds.
16. **`sys.trigger()` passes a NULL activator in OpenPrey.** Any target that
    uses its activator must check it. `hhTarget_EndLevel` crashed at the end of
    the roadhouse.
17. **Keep the GL driver out of per-frame allocation.** Mesa 20.1's nouveau
    crashed in its suballocator under hundreds of `glBufferData` calls per
    frame. Reusing same-size storage was not enough. Carving blocks out of
    large buffers written through unsynchronized maps, with fences guarding
    reuse, takes the driver out of the loop. A crash inside the driver during
    a burst of allocations points at the allocation pattern, not at the
    feature that is on screen (shadows here).
18. **GUI text fields take Backspace as a character.** `idEditWindow` handles
    Backspace in its `SE_CHAR` branch; a `SE_KEY` Backspace is ignored.

## 6. Performance: what is known

The full reference is the Performance section of
[switch-port.md](switch-port.md). Summary:

- **Progress:**
  - Original: 10-15 fps.
  - Now, light scenes: ~30 fps.
  - Heavy scenes (the bar full of NPCs, ~2000 draws): ~9-10 fps.
  - Loading: 118 s → ~50 s.
- **The back end (submitting draws to the driver) is 75-85% of the frame.**
- **There are two kinds of scene:**
  - **GPU bound** (bathroom): half the resolution doubles the frame rate.
    Dynamic resolution handles it.
  - **CPU/driver bound** (bar): ~40 µs per draw, and the resolution changes
    nothing. Dynamic resolution now notices and undoes the drop.
- **Ruled out:**
  - ARB parameter cache (no gain);
  - vertex buffer re-creation (small);
  - Mesa 26 (rendering glitches; speed now equal to Mesa 20.1);
  - SaltyNX (replaced by the native monitor).
- **Measured with `r_perfGpuSync 1`, in the bar:** ~25 ms of CPU plus
  ~19-28 ms of GPU per frame. Both CPU and GPU are close to the 33 ms budget.
- **The CPU clock helps little:** even at ~2.4 GHz the frame rate barely rose.
  Cores 0 and 3 stay busy and cores 1-2 stay idle. Core 3 belongs to the
  system, which runs the GPU driver service.
- **Also ruled out:** `r_useIndexBuffers 1` and swap interval 0/1.
- **Research points to the driver.** nouveau (the base of Mesa on the Switch)
  does not use Zcull, compressed render targets or the GPU's tiled cache, and
  its OpenGL costs much more CPU than deko3d. Sources in the Performance
  section of switch-port.md.
- **Mesa 26 retested (2026-09-29):**
  - with threads on the right cores, it performs like Mesa 20.1;
  - GLthread helped little in the numbers;
  - the rendering glitches remain (section 0).
- **Decision:** lock at 30 fps, with the CPU at 1224 MHz in profile 3 to hold
  a minimum in heavy scenes.
- **Realistic expectations:**
  - a locked 30 fps in heavy scenes needs a lower per-draw cost (back end on
    its own thread, driver work);
  - a steady 60 fps would need a new renderer on the native API (deko3d), like
    the official Doom 3. Months of work.

### Useful cvars

| Cvar | Default | Use |
|---|---|---|
| `com_showFPS 1` | 0 | fps and 3D resolution on screen |
| `com_logPerf 1` | 0 | One performance line per second in the log |
| `com_logHitches` | 100 | Logs frames above N ms, with a breakdown |
| `r_perfGpuSync 1` | 0 | Diagnostic: splits CPU and GPU time (lowers the frame rate) |
| `r_fpsLock` | 30 | 30 = locked; 0 = unlocked (1 to 19 mean 30) |
| `r_dynamicResolution` | 1 | Dynamic resolution; checks each drop and undoes it when it did not help |
| `r_dynamicResolutionMin` | 50 | Lowest resolution, in % |
| `r_renderScale` | 100 | Highest resolution, in % |
| `r_switchPerfProfile` | 3 | 0 = default; 1 = GPU 384 MHz; 2 = GPU 460.8; 3 = GPU 460.8 + RAM 1600. CPU: set 1224 or 1785 MHz in sys-clk / Horizon-OC |
| `r_vertexPages` | 1 | Vertex cache blocks in shared 8 MB buffers, no driver allocation per block (read at startup) |
| `r_switchGLThread` | 1 | Mesa 26 build only: GL driver on its own thread (core 2); applies after `vid_restart` |
| `r_useIndexBuffers` | 0 | Tested: no gain on the Switch |
| `image_compressTextures` | 1 | DXT textures (2 also compresses normal maps) |
| `r_cacheProgramParms` | 1 | Parameter cache (no measured effect) |
| `g_debugTriggers` | 0 | Logs trigger touches, rejections and bounds fallbacks |
| `in_gyro` | 1 | 0 = off; 1 = always; 2 = only while aiming with ZL |

**How to measure:**

1. Ask the tester for `com_logPerf 1`, `r_fpsLock 0` and
   `r_dynamicResolution 0`.
2. Always measure in the same places: standing in the bathroom, in the bar full
   of NPCs, and turning the camera in the hallway.
3. Change one cvar at a time.

Status Monitor screenshots complete the data.

### Mesa 20.1 built from source

`OpenPrey.nro` uses devkitPro's Mesa 20.1 (package `switch-mesa` 20.1.0-5).
pacman only ships that driver prebuilt. To be able to modify it, the official
recipe is reproduced in `tools/switch/mesa20/build.sh`, with instructions in
that folder's `README.md`.

- **What the script does:** downloads the Mesa sources and devkitPro's
  patches, checks the checksums, applies two build-compat patches of ours
  (no driver change) and builds.
- **Output:** `.tmp/mesa20-build/mesa20-install/`. The Mesa installed in
  devkitPro is not touched.
- **Verified:** the same exported functions as the pacman package (`libEGL`
  9588, `libglapi` 1644, `libGLESv2` 358).
- **Using it in OpenPrey:** a separate build dir with
  `-Dswitch_mesa_sdk=<that folder>/opt/devkitpro/portlibs/switch` and
  `-Dswitch_variant=mesa20-src`, which gives `OpenPrey-mesa20-src.nro`.
- **Using it in another devkitPro port:** put that folder's `include/` and
  `lib/` ahead of the portlibs.
- **Pitfall:** do not export `MESON_RSP_THRESHOLD` when building Mesa. Without
  meson's response files, archiving `libEGL.a` exceeds the Windows command
  line limit.

## 7. Roadmap for porting another game (id Tech 4 or similar)

The order that worked here:

1. **Study the engine:** build system, platform layer (`src/sys/*`), how the
   game module is loaded (a DLL?), which graphics API it uses.
2. **Test the hardware before the game:** a small program (like
   `tools/switch/gltest`) that confirms the GL version, extensions and stencil
   on the console.
3. **Meson cross file** with `host_machine.system = 'horizon'` and
   `__SWITCH__`. Build the parts in order: idlib → game → engine.
4. **A platform layer of its own** (`src/sys/switch/`), instead of filling the
   Linux code with `#ifdef`s: files, time, threads, input, video, networking
   as a stub.
5. **DLLs become a static object** (the `make_game_object.py` technique).
6. **GL without libGL:** everything goes through `eglGetProcAddress`.
7. **Diagnostics before optimizing:**
   - crash handler with a backtrace;
   - error file written early;
   - log written line by line;
   - the ELF of every build kept.
8. **Look for 64-bit bugs** (`long`, pointers stored in `int`).
9. **Playable first, fast later:**
   - clock profiles;
   - threads spread over the cores;
   - compressed textures;
   - dynamic resolution;
   - post-processing off;
   - only then deeper optimizations, **always measured** and behind a cvar
     that can turn them off.
10. **Controls:** a modern FPS scheme, versioned binds and optional gyro.
11. **Clean exit:** stop every thread and restore the system services.

## 8. Pending

- The items in section 0: the grandfather conversation, the Mesa 26 glitches
  and the CPU clock.
- Measure profile 3 against profile 4 (CPU 1224 against 1785 MHz) once the
  clock sticks (`CPU clock check` in the log).
- Render back end on its own thread (core 2), behind a cvar.
- Carry the `Pvs.cpp` fix and the trigger fix over to OpenPrey-GameLibs.
- Loading time: ~28 s is still images.
- Audio: check which OpenAL Soft backend is used. Multiplayer: real sockets.
