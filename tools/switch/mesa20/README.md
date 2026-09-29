# Mesa 20.1 for Switch, built from source

`build.sh` rebuilds devkitPro's `switch-mesa` 20.1.0-5 (the OpenGL driver
`OpenPrey.nro` and most devkitPro Switch ports link) from source. It follows
devkitPro's PKGBUILD. The goal is a driver we can modify and re-test: the
package from pacman is binary only.

What it does:

1. Downloads Mesa 20.1.0-rc3 and devkitPro's recipe files. The recipe is
   pinned to pacman-packages commit `c7876f72`, and every download is checked
   against the PKGBUILD's sha256.
2. Applies devkitPro's patches: the Switch EGL platform, and nouveau on
   `libdrm_nouveau`.
3. Applies two patches of ours. They only let the 2020 sources build with
   today's devkitPro and do not change the driver:
   - `python312-compat.patch`: Python 3.12 removed `distutils`, which Mesa's
     mako version check used.
   - `newlib-timespec_get.patch`: current newlib declares `timespec_get()`
     without implementing it, which clashed with Mesa's static fallback. The
     fallback now has its own name.
4. Builds with devkitPro's `meson-cross.sh` and stages the install in
   `<work>/mesa20-install/opt/devkitpro/portlibs/switch` (`include/`, `lib/`
   with `libEGL.a`, `libglapi.a`, `libGLESv2.a`, pkg-config and CMake files).
   The system portlibs are not touched.

## Requirements

Run it from the devkitPro MSYS2 shell with `MSYSTEM=MSYS`. It needs these
pacman packages: `switch-dev`, `switch-libdrm_nouveau`, `switch-pkg-config`,
`dkp-meson-scripts`, `dkp-toolchain-vars`, `python-mako` and `patch`.

Do not export `MESON_RSP_THRESHOLD` here. The OpenPrey build needs it, but
archiving `libEGL.a` needs response files, or the command line exceeds the
Windows limit.

```bash
tools/switch/mesa20/build.sh            # work dir: .tmp/mesa20-build
```

A full build takes about 20 minutes (~700 steps).

## Using it

**OpenPrey:** configure a separate build dir:

```bash
meson setup builddir-switch-mesa20 --cross-file tools/switch/meson/switch-cross.ini \
    -Dbuildtype=release -Dswitch_variant=mesa20-src \
    -Dswitch_mesa_sdk=<work>/mesa20-install/opt/devkitpro/portlibs/switch
```

It produces `OpenPrey-mesa20-src.nro`, titled "OpenPrey (mesa20-src)", which
can sit next to `OpenPrey.nro` on the SD card.

Checked against the pacman package: the same exported functions
(`libEGL.a` 9588, `libglapi.a` 1644, `libGLESv2.a` 358). Sizes differ by
about 1% because today's compiler is newer.

**Another devkitPro port:** put the staged `include/` and `lib/` ahead of the
portlibs, with `-I`/`-L`, or `PKG_CONFIG_PATH=<staged>/lib/pkgconfig`. Link
`-lEGL -lglapi -ldrm_nouveau` as with the pacman package. To replace the
system copy instead, copy the staged tree over
`/opt/devkitpro/portlibs/switch`. `pacman -S switch-mesa` restores the
original.
