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
| Platform layer `src/sys/switch/` | Not started. The engine does not link yet. |
| GL loading (qgl/GLEW through `eglGetProcAddress`) | Not started |
| OpenGL capability probe (`tools/switch/gltest`) | Builds; needs a run on hardware |

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

Useful partial targets while the port is incomplete:

- `ninja -C builddir-switch libopenprey_game_idlib.a`
- `ninja -C builddir-switch basepy/libgame_arm64.a`

## Design decisions

- **Static game module.** Switch homebrew cannot `dlopen` a separate game library,
  so on `horizon` the game is built as a static library and linked into the engine
  (`basepy/meson.build`).
- **No GLEW / libGL.** devkitPro ships Mesa (nouveau) as EGL + glapi, with no libGL.
  Every GL entry point, including GL 1.x, must be resolved through
  `eglGetProcAddress`. `ID_GL_HARDLINK` is therefore not defined on Switch.
- **Platform defines.** `__SWITCH__` (from the cross file) selects the Switch block
  in `src/sys/sys_public.h`. Shared Linux/Mac paths in `idlib` also cover
  `__SWITCH__`, so behavior such as `idMath::FtoiFast` rounding matches Linux.
- **Own platform folder.** `src/sys/posix/` depends on `sys/mman.h`, POSIX signals,
  and `sys/uio.h`, which newlib/libnx do not provide. The Switch backend lives in
  `src/sys/switch/` and reuses the POSIX pieces that work, instead of adding
  `#ifdef`s to the Linux/macOS code.

## Next steps

1. Run `tools/switch/gltest` on hardware to confirm that a compatibility-profile
   context, ARB programs, and S3TC work on Switch Mesa.
2. `src/sys/switch/`: `main`, time, files, paths, threads, and events (SDL2).
3. GL loader: resolve `gl*`/GLEW entry points through `eglGetProcAddress`.
4. `GLimp_*` on SDL2 + EGL, controller input, and audio.
5. Package an `.nro` (`nacptool`/`elf2nro`) from the linked ELF.
