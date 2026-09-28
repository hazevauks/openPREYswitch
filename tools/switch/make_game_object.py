#!/usr/bin/env python3
"""Build the statically linked game module for the Nintendo Switch.

Switch homebrew cannot load a separate game library, but the game code is
written for DLL semantics (GAME_DLL): it keeps its own copies of the engine
interface globals (sys, common, ...) and its own idlib, and exposes only
GetGameAPI. To keep that behavior identical to the desktop builds, this
script:

  1. relocatably links the game and game-idlib archives into one object
     (like a DLL, every archive member is kept), then
  2. localizes every defined symbol except GetGameAPI, so nothing in the game
     can clash with, or bind to, the engine's symbols of the same name.

COMDAT groups are dissolved in step 1 so the game's private copies of inline
functions and vtables never replace the engine's at the final link.

Usage:
  make_game_object.py <output.o> <objcopy> <c++ compiler...> -- <archive.a...>
"""

from __future__ import annotations

import os
import subprocess
import sys

EXPORTED_SYMBOL = "GetGameAPI"


def main(argv: list[str]) -> int:
    if "--" not in argv or len(argv) < 5:
        print(__doc__, file=sys.stderr)
        return 2
    split = argv.index("--")
    output, objcopy, *cxx = argv[:split]
    archives = argv[split + 1:]
    if not cxx or not archives:
        print(__doc__, file=sys.stderr)
        return 2

    partial = output + ".partial.o"
    link = cxx + [
        "-r",
        "-nostdlib",
        # Dissolve COMDAT groups (inline functions, vtables, templates). Left as
        # groups, the final link would keep the game's localized copy of a group
        # and discard the engine's, leaving engine references unresolved.
        "-Wl,--force-group-allocation",
        "-Wl,--whole-archive",
        *archives,
        "-Wl,--no-whole-archive",
        "-o",
        partial,
    ]
    localize = [objcopy, f"--keep-global-symbol={EXPORTED_SYMBOL}", partial, output]

    for command in (link, localize):
        result = subprocess.run(command)
        if result.returncode != 0:
            print("failed: " + " ".join(command), file=sys.stderr)
            return result.returncode

    os.remove(partial)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
