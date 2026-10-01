# Nintendo Switch port handbook (OpenPrey)

Working guide for continuing the OpenPrey Switch port and for starting similar
ports. It collects what was learned in practice: rules, environment, test
loop, pitfalls and measurements.

Technical reference for the port: [switch-port.md](switch-port.md).

**Before starting work on the port** (people and coding assistants alike):

> Read `docs-dev/switch-port-handbook.md` and `docs-dev/switch-port.md` before starting.

---

## 0. Open issues (read first)

Updated after every test round. When an issue is solved, it leaves this
section and the lesson goes to section 5.

**Story progress (2026-10-01):**

- **A run from the start of the game** (build 09f2f4e, LTO) went through the
  roadhouse and feedingtowera with a steady frame rate, above 30 fps even in
  the bar.
- Later rounds went on through feedingtowerb, Tommy's death, the Land of the
  Ancients (lotaa, Spirit Walk) and back to feedingtowerc, where Tommy fell out
  of the map past the pod tunnel (below: the collision was not Prey's).
- The port is about to have its first release. What is left is optimization
  and quality of life.

**Solved and confirmed on hardware (2026-10-01):**

- the driver crash in feedingtowera: vertex pages (lesson 17);
- the savegame name keyboard: clicking the field opens it (lesson 18);
- the crash in a scripted teleport after loading a save: `sys.trigger()`
  passes the local player again (lesson 16);
- the settings menu (- button) and the console through it;
- the LTO build: above 30 fps in the bar with shadows off and the CPU at
  1224 MHz (Horizon-OC). LTO is the default (`b_lto` in the cross file);
- savegames rejected at random (lesson 22): a save from an older build loaded
  with the "old checksum" warning, and a new autosave loaded without it;
- out of memory across map changes (lesson 19): feedingtowerc to lotaa
  released 351 MB of sounds and 290 MB of images, and the heap went from
  1697 to 1149 MB in use.

### Death in the pod tunnel of feedingtowerc (to diagnose)

- **Report (2026-10-01, build b069c22):** with Prey's own collision loaded
  (the log matched the shipped `.cm`, bounds 7220 x 6956 x 8128), Tommy no
  longer falls or walks through walls (next issue), but he still dies in the
  tunnel under the pod hole, at (238, -1029, 80). The build before died at
  (238, -1030, 74) too. The physics state is normal up to the death: on the
  floor, no fall, no spirit walk.
- **Checked in the map data and ruled out at that point:** `trigger_hurt`,
  death volumes (`trigger_deathresurrection`, `hhSafeDeathVolume`), movers,
  the slab paths, portals, possession, egg spawners and script kills (no
  shipped script kills the player). The tunnel's materials are plain flesh
  (`matter_flesh`), and the only volumes there are two `trigger_portal`
  (visibility). The three fodders woken by `trigger_once_96` bite for 7 (14
  at most with DDA), so they cannot kill in one hit.
- **Change (diagnostics):** every death logs `player killed at (...): damage
  N by <attacker>, inflictor <inflictor>; last hit: <damage def> from ... by
  ...` and the entities within 256 units; a death volume logs `death volume
  '<name>' kills the player`; `g_debugPlayerPhysics 1` also logs each hit
  (`player damage: ...`, with the DDA scale) and the player's health once
  per second.
- **Missing enemies (2026-10-01):** a retail playthrough video shows two
  enemies in the pod room before the tunnel; the tester never had them. The
  room has exactly three creatures: `monster_mutilated_male_4` (patrols the
  monitors), `ftdMutilatedWorker1` (the can worker) and `monster_crawler_21`.
  The mutilated are enemies (team 7, 150 health, melee). With the rebuilt
  collision they spawned on missing floor (the tester stood at z 254.6 at
  (-357, -754), next to `monster_mutilated_male_4` at (-325, -711)) and fell.
  The "Possession" autosave is written a moment after the level starts, so it
  stored them already sinking: loading it with the shipped collision, all three
  start inside the real floor and fall out of the world ("clip model outside
  world bounds" at z -3200).
- **Both tunnel deaths started from that autosave.** No run that entered
  feedingtowerc fresh has reached the tunnel yet, so the death may come from
  state that autosave stored with the rebuilt collision.
- **Test:**
  1. load a save from the end of lotaa, so feedingtowerc starts fresh with
     the shipped collision (the two mutilated should be in the pod room);
  2. type `g_debugPlayerPhysics 1` in the console;
  3. go down the pod hole and walk the tunnel;
  4. send the log. If Tommy dies, the `player killed` line names what killed
     him.

### Walking through walls and falling out of feedingtowerc (fixed, collision confirmed)

- **Confirmed (2026-10-01):** the log showed `removing the rebuilt
  maps/game/feedingtowerc.cm to load the shipped one`, then `collision
  data:` equal to the shipped file (445 models, 55790 vertices, 47652
  polygons, 3953 brushes, 17341 nodes), `map bounds are (7220.0, 6956.0,
  8128.0)`, and `done.` for the two AAS files the map ships. Tommy walked the
  room and the tunnel floor (z 80.3) without falling. No crash.

- **Report (2026-10-01):** after the pod opens the flesh wall
  (`trigger_pod_gack_3`) at the start of game/feedingtowerc, Tommy walks
  through walls and dies, in the tunnel it opens and in the room next to it.
  Reloading the autosave did not help.
- **Log (`g_debugPlayerPhysics 1`):** the player's state was normal (gravity,
  box, contents, clip mask, linked) until he stood at z 254.6, under the clip
  brushes (266-272) and floor (268) of the shipped collision, and then fell
  to z -8203. The map load printed `map bounds are (524288.0, 524291.2,
  524288.0)`, while the shipped world model is ~7200 x 6900 x 8100, and
  `collision data:` did not match the shipped `.cm` (39792 polygons and
  373827 nodes against 47652 and 17341).
- **Cause (lesson 23):** the engine's map CRC was Quake 4's. No shipped `.cm`
  or `.aas` matched it, so every map ran on collision rebuilt from the `.map`
  (holes, giant polygons) and monsters had no navigation data. The rebuilt
  `.cm` files were written to `basepr/maps/game/` and hid the pk4 copies on
  later loads, without a message.
- **Change:** the Doom 3 CRC (`src/idlib/mapfile.cpp`), and
  `LoadCollisionModelFile` removes a stale rebuilt `.cm` so the shipped one
  loads. The AAS parser now also reads Prey's 1.07 area layout (it expected
  OpenQ4's 1.08 and had never run on a Prey file). Details in switch-port.md,
  "Map data: collision and AAS".
- **Test:**
  1. load a save from before the pod (or the "Possession" autosave);
  2. check the log: `removing the rebuilt maps/game/feedingtowerc.cm to load
     the shipped one` once, `map bounds are` about (7208, 6944, 8096), and
     `collision data:` with 47652 polygons;
  3. go through the tunnel and the room next to it;
  4. watch the monsters: with navigation data they should move around
     obstacles and chase properly on every map. This AAS code runs on the
     Switch for the first time, so a crash or stuck monsters right after a
     map load point here; `[Load AAS]` lines followed by `done.` mean the file
     parsed.
  - A save from before the fix keeps its clip models by name, so it loads
    with the new collision.

### Deathwalk (not implemented)

- When Tommy dies he does not go to the Deathwalk; the game continues from the
  last save. Retail Prey appends a deathwalk level (`deathwalk1-3`) to most
  maps; in OpenPrey `shouldappendlevel` is never set and the engine side
  (`AppendMap` for collision, the render world) does not exist, so
  `hhPlayer::Killed` sees no deathwalk map and kills Tommy for good.

**Not a fix: `com_fixedTic 1`.** It runs exactly one 60 Hz game tick per
rendered frame, so at 30 fps the game runs at half speed. The frame rate rises
because each frame simulates less. `vid_restart` also leaves the screen black
on the Switch; restart the game instead.

### Out of memory on the third map, graphics errors and crashes after loading saves (memory confirmed, the rest to confirm)

- **Reports (2026-10-01, build 7b7bc42):**
  - "Out of memory (17711349 bytes requested)" loading feedingtowerb in a run
    from the start. The heap had taken 3110 of its 3115 MB from the system;
    the 31 MB "in use" in the file was measured after the shutdown.
  - A crash loading a save: the savegame's script checksum did not match, and
    the rejected load deleted objects that were only constructed
    (`hhWeaponRifle::~hhWeaponRifle` → `ZoomOut` on members `Spawn` sets).
  - A crash in `R_GlobalShaderOverride`: `renderEntity.overlayShader` was
    0x30.
  - Graphics errors from the middle of the second area (Sphaira forwarder),
    and a slower frame than the previous build.
- **Causes found:**
  1. **Sounds** were decoded to PCM, kept twice (engine and OpenAL), and never
     released (everything never-purge). Prey's audio decodes to ~2.7 GB, 2.2
     GB of it music. Each map piled up on the last (switch-port.md,
     "Memory").
  2. **Savegame restore** re-created static render entities with a plain
     `new`. Fields the save does not carry (`overlayShader`, masks,
     `globalLight`...) held garbage: models drawn twice with random
     materials, and the crash when the pointer was invalid. That explains the
     graphics errors and part of the slowdown after loading.
  3. **Rejected savegames** crashed instead of restarting the map.
- **Changes:**
  - sounds: the engine's PCM copy is freed after the upload to OpenAL;
    `BeginLevelLoad` releases the last level's samples (except menu sounds and
    what is playing); they come back when the new level looks up their
    shaders or plays them;
  - restore: `ReadRenderEntity` / `ReadRenderLight` set the unsaved fields,
    and static render entities are cleared first;
  - a rejected save no longer deletes the half-built objects (they leak once)
    and logs the checksums; the session restarts the map with the player's
    persistent data, as the engine intended;
  - the log of the previous session is kept as
    `basepr/logs/openprey-previous.log`, and `openprey_error.txt` keeps the heap
    as it was at a FatalError.
- **Test:**
  1. play from a save through two or three map changes;
  2. check that music and sounds play normally after each change;
  3. send `openprey.log`. The `sounds: released` and `memory before/after
     loading` lines should show the memory going back down at each change.
  - After loading a save, the graphics should be clean and the frame rate
    as good as in a fresh run.

### PGO training run (retry)

- **2026-09-30:** the first instrumented build crashed at startup, inside a
  static constructor, in libgcov's indirect-call profiler (`gcov_topn_add_value`).
  Value profiling keeps its state in thread-local variables. Both stages now
  use `-fno-profile-values`, which records only branch and call counts, the
  bulk of what PGO uses.
- `OpenPrey-pgo.nro` is the instrumented build (`builddir-switch-pgo/`,
  `-Dswitch_pgo=generate`). It runs slower while it records.
- **Run:**
  1. play 10-20 minutes of normal play (menus, a map load, combat, busy
     areas);
  2. **quit from the game's menu**, since the profiles are only written at a
     clean exit;
  3. copy `sdmc:/switch/openprey/pgo/` into `.tmp/pgo-data/`;
  4. build with `-Dswitch_pgo=use` (switch-port.md, "LTO and PGO") and compare
     with the LTO build at the same spots.

### Slow loads (fix to confirm)

- **Cached loads:** feedingtowera from a save took 78.5 s. Of that, 45 s were
  images, and 364 MB of OS reads took 21.6 s, about 17 MB/s.
- **Cause:** newlib's 1 KB stdio buffer made each KB a separate file service
  call.
- **Change (build after 6524fe0):** a 64 KB buffer for every file the engine
  opens, pk4s included.
- **Test:** load the same save and compare `N images loaded in T seconds`,
  `OS reads: N MB in T s` and the total `msec to load`.
- **First visits** still compress textures: 70-100 s of images per new map.
  The idea is to compress on cores 1-2.
- **Collision rebuilds on first visits** (79 s in feedingtowerb) came from the
  map CRC (lesson 23). With the shipped `.cm` loading, first visits skip them.

### Front end at 60-120 ms late in feedingtowerb (to measure)

- **Symptom:** near the end of the 2026-09-30 log, 6-7 fps for over a minute
  with shadows off:
  - front end 60-120 ms, game 10-22 ms, back end ~20-35 ms;
  - ~1100-2300 draws, only ~50-180 paged blocks per frame.

  The GPU was not the limit.
- **Change (diagnostics):** every `hitch` line is followed by `hitch front
  end:`, which gives:
  - the R_RenderView phases in ms: find (portals and culling), lights, models
    (dynamic models and interactions), sort;
  - views rendered (mirrors, portals and cameras count);
  - visible entities and lights;
  - md5 models generated, entity callbacks and interactions created.
- **Test:** go back to that spot, then send the log and describe what is on
  screen.
- **Likely candidates:**
  - many subviews (portals or mirrors);
  - many animated models skinned on the CPU (the SIMD path is generic C, no
    NEON);
  - interaction creation.

### Rendering glitches in the Mesa 26 build (parked)

- Mesa 26 is no longer being tested. The port stays on Mesa 20.1, which can
  also be built from source (section 6).
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

- **Branches:** work happens on `switch-port` in `hazevauks/openPREYswitch`,
  which merges into `main` for releases. Fixes that help every platform can
  also go upstream to `themuffinator/OpenPrey` as pull requests.
- **Never commit game files** (`.pk4`, textures, audio). Players use their own
  copy of Prey.
- `src/game` mirrors the OpenPrey-GameLibs repository (rule from `AGENTS.md`).
  Game code changes have to be carried over there too. **Pending:** the
  `long` to `int` fix in `src/game/Pvs.cpp`.
- Temporary files go in `.tmp/`, which git ignores.
- Meson is the official build system. Changed the workflow? Update
  `docs-dev/switch-port.md` in the same commit.
- Everything written (READMEs, docs, commits) is in English.

## 2. Environment (Windows)

| Item | Where / how |
|---|---|
| devkitPro | The devkitPro install folder, `<devkitPro>` below (the installer's default is `C:\devkitPro`) |
| Build shell | `<devkitPro>\msys2\usr\bin\bash.exe`, **always with `MSYSTEM=MSYS`** (in MinGW mode the msys `meson`/`python` do not run) |
| `/opt/devkitpro` path | Mapped in `<devkitPro>\msys2\etc\fstab`; keep a backup of the original. If the devkitPro folder moves, fix that line. |
| Python | If Windows has none, use `<devkitPro>/msys2/usr/bin/python3.exe`. |
| Cross file | `tools/switch/meson/switch-cross.ini` (`host_machine.system = 'horizon'`, `-D__SWITCH__`) |
| Build dirs | `builddir-switch/` (main), `builddir-switch-mesa26/` (Mesa 26 experiment), `builddir-switch-mesa20/` (Mesa 20.1 built from source), `builddir-switch-pgo/` (PGO: instrumented or optimized) |

### Building

From Git Bash or any shell, with `<devkitPro>` and `<repo>` in MSYS form
(`/c/devkitPro`, `/c/path/to/openPREYswitch`):

```bash
MSYSTEM=MSYS <devkitPro>/msys2/usr/bin/bash.exe -lc "cd <repo> && export MESON_RSP_THRESHOLD=2147483647 && meson compile -C builddir-switch"
```

- `MESON_RSP_THRESHOLD=2147483647` is **required**. Without it, long link
  lines go into a response file, MSYS2 does not convert its paths, and the
  link fails.
- A full build takes about 30 minutes. Touching shared headers (for example
  `RenderSystem.h`) recompiles everything.
- With LTO (the default), every change also relinks everything, about 10
  minutes. For quick experiments use a dir configured with `-Db_lto=false`.
- Configuring from scratch:
  `meson setup builddir-switch --cross-file tools/switch/meson/switch-cross.ini -Dbuildtype=release`
- Output: `builddir-switch/OpenPrey.nro`. With symbols:
  `builddir-switch/OpenPrey-client_arm64.elf`.

### Releasing each build (always in this order)

1. Build.
2. **Archive the ELF:** copy it to `.tmp/elf-builds/OpenPrey-<commit>.elf`.
   Crashes cannot be read without the ELF of the same build.
3. Copy the NRO to `switch/openprey/` on the test SD card (or a staging copy
   of the card under `.tmp/`).
4. Commit with a descriptive message and `git push origin switch-port`.
5. Write a short numbered test plan for testers, with the exact console
   commands.

### Public releases

The root `README.md` is the release page: installation, controls, tips,
known issues and support. Keep its known issues in step with section 0.

1. Merge `switch-port` into `main`.
2. Build from `main` and archive the ELF, as above.
3. Package: `python3 tools/switch/package_release.py` writes
   `.tmp/release/openPREY-switch-<version>.zip` (switch-port.md, "Homebrew
   entry and release zip").
4. Test the zip as a player would: extract it into an empty SD card folder,
   add the `.pk4` files, and start through title override.
5. Tag the commit `v<version>` and publish a GitHub release with the zip, the
   changes and the known issues.

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
  - `basepr/logs/openprey.log`: written line by line, so the last line is the
    last event before a hang.
  - `basepr/logs/openprey-previous.log`: the session before the current one.
    Starting the game renames the last log to this, so a crash or error can be
    reported after restarting.
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
- **Settings menu:** the **−** button opens and closes it (fps, frame rate
  lock, shadows, gyro, look speed...). **Y** there opens the **game
  console**; in the console, **A** opens the system keyboard and **−** closes
  it.

## 4. Switch code map

| File | Role |
|---|---|
| `src/sys/switch/switch_main.cpp` | `main`, engine thread (16 MB stack), paths, events, crash handler, clocks (`r_switchPerfProfile` GPU/memory profiles, loading boost), `com_logPerf`/`com_logHitches`, clean exit |
| `switch_glimp.cpp` | EGL/Mesa, GL 4.3 compat context at 1280x720, swap, frame rate lock (`r_fpsLock`, `Switch_PaceFrame`) |
| `switch_input.cpp` | Controls (game/menu/console/settings), versioned default binds (`in_switchControlScheme`), system keyboard, touch |
| `switch_settings.cpp` | Settings menu overlay (the − button) and versioned settings defaults (`com_switchSettings`) |
| `switch_gyro.cpp` | Gyro aiming (`in_gyro*`) |
| `switch_threads.cpp` | Threads and locks; `__wrap_pthread_create` places each thread on a core |
| `switch_net.cpp` | Loopback-only networking (stub) |
| `tools/switch/make_game_object.py` | "Fake DLL": links the game into one object (`ld -r`) exporting only `GetGameAPI`; with LTO it generates the game's code there |
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
16. **Keep the activator the shipped scripts expect.** Upstream OpenPrey made
    `sys.trigger()` pass a NULL activator. Targets written for Prey's SDK
    behavior (the local player) crashed one by one: level end, vehicle target,
    scripted teleport. Single player passes the local player again, and the
    handlers that dereferenced it unchecked were fixed. An audit of every
    `EV_Activate` handler (140 of them) found the rest at once.
17. **Keep the GL driver out of per-frame allocation.** Mesa 20.1's nouveau
    crashed in its suballocator under hundreds of `glBufferData` calls per
    frame. Reusing same-size storage was not enough. Carving blocks out of
    large buffers written through unsynchronized maps, with fences guarding
    reuse, takes the driver out of the loop. A crash inside the driver during
    a burst of allocations points at the allocation pattern, not at the
    feature that is on screen (shadows here).
18. **GUI text fields take Backspace as a character.** `idEditWindow` handles
    Backspace in its `SE_CHAR` branch; a `SE_KEY` Backspace is ignored.
19. **Measure memory by subsystem before guessing.** The heap line alone did
    not say what filled 3 GB. What did was a cached map load reading only
    364 MB, textures included, while the heap held ~2 GB, and then the
    decoded size of the game's audio. Sounds were decoded in full, kept twice
    and never released.
20. **Savegames do not carry every struct field.** Anything restored into
    memory that `new` did not clear keeps garbage in the fields the save
    leaves out. The renderer read it as overlay materials and masks. Set such
    fields in the Read function itself.
21. **Do not destroy objects that were only constructed.** Prey destructors
    assume `Spawn` ran. A rejected savegame deleted the objects it had just
    created and crashed in `hhWeaponRifle::ZoomOut`.
22. **`long` is 8 bytes on the Switch.** Code written for Windows (4-byte
    `long`) breaks in quiet ways. Hash digests declared `unsigned long[4]`
    were half garbage, so savegame checksums changed between runs. Grep for
    `long` in anything that hashes, serializes or does bit tricks.
23. **Check that the engine accepts the game's own data files.** OpenQ4's
    `idMapFile` computed Quake 4's map CRC, so every shipped Prey `.cm` was
    "out of date" (rebuilt from the `.map`, with holes) and every `.aas` was
    rejected with a developer-only message (monsters without navigation).
    Nothing failed loudly: the rebuilt `.cm` was saved and hid the real one.
    Recomputing the CRC from the retail files in a script settled it. A file
    that is always rejected also hides bugs in the code that would read it:
    the AAS parser expected OpenQ4's layout and had never run on a Prey file,
    so accepting the files meant checking that parser too. When
    something about the world is wrong (collision, navigation), compare what
    loaded with the shipped file first: `collision data:` against the `.cm`,
    `map bounds are` against the map.

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
| `in_gyro` | 0 | 0 = off; 1 = always; 2 = only while aiming with ZL |
| `g_noclip` | 0 | Noclip in single player (settings menu); follows the `noclip` command |
| `g_debugPlayerPhysics` | 0 | Logs the player's position, velocity, box, contents and clip mask once per second |
| `r_shadows` | 0 | Stencil shadows; off by default on the Switch (much faster) |
| `in_joystickInvertLook` | 0 | Invert the right stick's vertical look |
| `com_switchSettings` | - | Internal: settings defaults version of the config |

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

- The items in section 0.
- **Carry the game-code fixes over to OpenPrey-GameLibs** (`src/game` mirrors
  it). Upstream PRs are optional. The fixes are:
  - `Script_Program.cpp`: 64-bit vector parameters;
  - `Script_Thread.cpp`: `sys.trigger` activator;
  - `Misc.cpp`, `Item.cpp`, `Trigger.cpp`: NULL activators;
  - `game_trigger.cpp` and `game_targets.cpp`: triggers and targets;
  - `Pvs.cpp`;
  - the `g_debugPlayerCanSee` diagnostics;
  - `SaveGame.cpp` and `prey_game.cpp`: unsaved render entity/light fields
    and the rejected-save leak; `Script_Program.cpp`: old-checksum savegames;
  - `game_player.cpp`: `g_debugPlayerPhysics` and `g_noclip`;
  - if GameLibs carries its own idlib: the MD4/MD5 digests and the Doom 3 map
    CRC (`mapfile.cpp`).
- **Deathwalk:** level appending (section 0).
- **Performance:**
  - PGO training run and optimized build;
  - the heavy front end in late feedingtowerb;
  - render back end on its own thread.
- **Loading:** first-visit texture compression on cores 1-2.
- Audio: check which OpenAL Soft backend is used. Multiplayer: real sockets.
