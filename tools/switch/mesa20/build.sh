#!/bin/bash
#
# Rebuilds devkitPro's switch-mesa 20.1.0-5 (the Mesa every devkitPro Switch
# port links, OpenPrey.nro included) from source, following its PKGBUILD, and
# stages the result in <work>/mesa20-install. The installed portlibs are not
# touched.
#
# Run from the devkitPro MSYS2 shell (MSYSTEM=MSYS). Needs, via pacman:
#   switch-dev switch-libdrm_nouveau switch-pkg-config dkp-meson-scripts
#   dkp-toolchain-vars python-mako patch
# Do NOT export MESON_RSP_THRESHOLD here: linking libEGL.a needs response
# files, or the archiver command line exceeds the Windows limit.
#
# Usage: tools/switch/mesa20/build.sh [work dir, default .tmp/mesa20-build]

set -e

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
work="${1:-$repo/.tmp/mesa20-build}"
mkdir -p "$work"
work="$(cd "$work" && pwd)"
cd "$work"

# devkitPro pacman-packages, pinned to the commit that made switch-mesa 20.1.0-5
recipe=https://raw.githubusercontent.com/devkitPro/pacman-packages/c7876f72c1f3353725ab8468f0ef9180280a8c31/switch/mesa
fetch() {	# url file sha256
	if [ ! -f "$2" ]; then
		curl -fL "$1" -o "$2.part" && mv "$2.part" "$2"
	fi
	echo "$3 *$2" | sha256sum -c -
}
fetch https://archive.mesa3d.org/older-versions/20.x/mesa-20.1.0-rc3.tar.xz mesa-20.1.0.tar.xz \
	c90b75ea34302ebde9b81b87c5642fa864c40fe9c4ad34ce0793170c1413168d
fetch $recipe/switch-mesa-20.1.0-5.patch switch-mesa-20.1.0-5.patch \
	950f93d3e5b6ae9c5a42c2918623fe9a80d7ee398d92d2e73abec86b62d75916
fetch $recipe/gl_XML.py.patch gl_XML.py.patch \
	a9bc326195b3fe29709e079466a8b2162a2ac9409f694eaad5494490155e2dd4
fetch $recipe/glX_XML.py.patch glX_XML.py.patch \
	1475defcdf8600690eaddeee28d8f01310635c1273fb541f668e8789714d36ca
fetch $recipe/OpenGLConfig.cmake OpenGLConfig.cmake \
	016b39536fa560b38cbb456e3e9146439401c0ea29f26400ea065bb9d76f2518

rm -rf mesa-20.1.0-rc3
tar xf mesa-20.1.0.tar.xz
cd mesa-20.1.0-rc3

# devkitPro's patches (Switch EGL platform, nouveau on libdrm_nouveau, build fixes)
patch -p1 -i ../switch-mesa-20.1.0-5.patch
patch -p1 -i ../gl_XML.py.patch
patch -p1 -i ../glX_XML.py.patch
# ours: build with today's devkitPro (Python 3.12, current newlib); no driver change
patch -p1 -i "$here/python312-compat.patch"
patch -p1 -i "$here/newlib-timespec_get.patch"

unset MESON_RSP_THRESHOLD
/opt/devkitpro/meson-cross.sh switch ../crossfile.txt build -Db_ndebug=true
ninja -C build

stage="$work/mesa20-install"
rm -rf "$stage"
DESTDIR="$stage" ninja -C build install
portlibs="$stage/opt/devkitpro/portlibs/switch"
sed -i "s,-lEGL,-lEGL -ldrm_nouveau," "$portlibs/lib/pkgconfig/egl.pc"
install -Dm644 "$work/OpenGLConfig.cmake" "$portlibs/lib/cmake/OpenGL/OpenGLConfig.cmake"

echo "Mesa 20.1 staged in $portlibs"
