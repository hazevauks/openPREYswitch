#!/usr/bin/env python3
"""Package the Nintendo Switch build of openPREY as a release zip.

The zip is extracted at the root of the SD card:

  switch/openprey/OpenPrey.nro
  switch/openprey/basepr/...    the OpenPrey overlay, from basepy/
  switch/openprey/base/         a note: the player copies their Prey .pk4 files here

The overlay follows the install rules in basepy/meson.build (the same files
`meson install` stages in .install/basepr); keep the two in sync. No game data
is packaged.

Usage:
  package_release.py [--nro builddir-switch/OpenPrey.nro] [--version X]
                     [--output .tmp/release]

The zip is written to <output>/openPREY-switch-<version>.zip. The version
defaults to the project version in meson.build.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

REPO_ROOT = Path(__file__).resolve().parents[2]
OVERLAY_SOURCE = REPO_ROOT / "basepy"
SD_ROOT = "switch/openprey"
PACKAGE_STEM = "openPREY-switch"

# basepy/meson.build: directories installed whole
OVERLAY_DIRS = ("glprogs", "maps", "materials", "script", "strings")

# guis/ is installed without guis/assets/, except these files
OVERLAY_GUI_ASSETS = (
    "guis/assets/menu/background_left.tga",
    "guis/assets/menu/background_right.tga",
    "guis/assets/menu/background_top.tga",
    "guis/assets/menu/background_bottom.tga",
    "guis/assets/loading/loading.tga",
    "guis/assets/loading/roadhouse.tga",
    "guis/assets/loading/loading_left.tga",
    "guis/assets/loading/loading_right.tga",
    "guis/assets/loading/loading_top.tga",
    "guis/assets/loading/loading_bottom.tga",
    "guis/assets/loading/roadhouse_left.tga",
    "guis/assets/loading/roadhouse_right.tga",
    "guis/assets/loading/roadhouse_top.tga",
    "guis/assets/loading/roadhouse_bottom.tga",
)

# never packaged, wherever they turn up
IGNORED_NAMES = {".DS_Store", "Thumbs.db", "desktop.ini"}

PK4_NOTE_NAME = "PUT_YOUR_PREY_PK4_FILES_HERE.txt"
PK4_NOTE = """\
Copy every .pk4 file from the "base" folder of your Prey (2006) PC install
into this folder, for example from:

  C:\\Program Files (x86)\\Human Head Studios\\Prey\\base

Retail CD/DVD copies have pak000.pk4 to pak004.pk4 and game00.pk4.

openPREY does not include any game data.
"""


def project_version() -> str:
    text = (REPO_ROOT / "meson.build").read_text(encoding="utf-8")
    match = re.search(r"project\s*\(.*?version\s*:\s*'([^']+)'", text, re.DOTALL)
    if not match:
        raise SystemExit("error: no project version found in meson.build")
    return match.group(1)


def overlay_files() -> list[tuple[Path, str]]:
    """(source file, path inside basepr/) for every overlay file."""
    files: list[tuple[Path, str]] = []

    def add_tree(root: Path, skip: Path | None = None) -> None:
        if not root.is_dir():
            raise SystemExit(f"error: missing overlay directory {root}")
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.name in IGNORED_NAMES:
                continue
            if skip is not None and skip in path.parents:
                continue
            files.append((path, path.relative_to(OVERLAY_SOURCE).as_posix()))

    for name in OVERLAY_DIRS:
        add_tree(OVERLAY_SOURCE / name)
    add_tree(OVERLAY_SOURCE / "guis", skip=OVERLAY_SOURCE / "guis" / "assets")
    for name in OVERLAY_GUI_ASSETS:
        path = OVERLAY_SOURCE / name
        if not path.is_file():
            raise SystemExit(f"error: missing overlay file {path}")
        files.append((path, name))

    return sorted(files, key=lambda item: item[1])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Package the Switch build of openPREY as a release zip.")
    parser.add_argument("--nro", type=Path, default=REPO_ROOT / "builddir-switch" / "OpenPrey.nro",
                        help="NRO to package (default: builddir-switch/OpenPrey.nro)")
    parser.add_argument("--version", help="version in the zip name (default: the meson.build project version)")
    parser.add_argument("--output", type=Path, default=REPO_ROOT / ".tmp" / "release",
                        help="output directory (default: .tmp/release)")
    args = parser.parse_args(argv)

    if not args.nro.is_file():
        print(f"error: {args.nro} not found; build it first (meson compile -C builddir-switch)", file=sys.stderr)
        return 1

    version = args.version or project_version()
    overlay = overlay_files()
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / f"{PACKAGE_STEM}-{version}.zip"

    with ZipFile(archive, "w", compression=ZIP_DEFLATED, compresslevel=9) as zf:
        zf.write(args.nro, f"{SD_ROOT}/OpenPrey.nro")
        for source, name in overlay:
            zf.write(source, f"{SD_ROOT}/basepr/{name}")
        zf.writestr(f"{SD_ROOT}/base/{PK4_NOTE_NAME}", PK4_NOTE.replace("\n", "\r\n"))

    print(f"{archive}")
    print(f"  OpenPrey.nro ({args.nro.stat().st_size / (1024 * 1024):.1f} MB, from {args.nro})")
    print(f"  basepr/: {len(overlay)} files")
    print(f"  base/{PK4_NOTE_NAME}")
    print(f"  zip size: {archive.stat().st_size / (1024 * 1024):.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
