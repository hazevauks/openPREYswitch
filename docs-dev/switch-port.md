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
| `OpenPrey.nro` | Links and packages; not yet run on hardware |
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

This folder is also `fs_savepath`: configs, saves and the log
(`basepr/logs/openprey.log`, enabled by default) are written there. A fatal error
also writes `openprey_error.txt` to the folder.

## Controls

The in-game mapping follows the Windows SDL3 backend so binds carry over (see the
header of `src/sys/switch/switch_input.cpp`). The left stick moves, the right stick
looks, and **+** opens the menu. In menus, the left stick moves the cursor, **A**
clicks, **B** goes back, and touching the screen clicks where you touch.

Buttons have no default binds yet. Bind them from the console or `autoexec.cfg`,
for example `bind JOY15 _attack`.

## Next steps

1. First boot on hardware: read `basepr/logs/openprey.log` and `openprey_error.txt`.
2. Audio: check which OpenAL Soft backend the devkitPro build uses.
3. Default controller binds, and a software keyboard for the console.
4. Performance on the Tegra X1 (docked resolution, post-processing defaults).
5. Multiplayer: real sockets in `switch_net.cpp`.
