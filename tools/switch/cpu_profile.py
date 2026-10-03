#!/usr/bin/env python3
"""Names the addresses of the Switch CPU profile and adds them up.

com_cpuProfile 1 makes the Switch build write basepr/logs/openprey_cpuprofile.txt
(src/sys/switch/switch_profiler.cpp): every report lists, per thread, the code
lines it was sampled in, and the call stacks of all threads, as offsets into
the executable. This script maps them to functions with the ELF of the same
build and prints, per thread:

- where its time went by library (engine, game, Mesa GL front end, state
  tracker, nouveau driver, libnx, ...), when the linker map is given;
- the functions with the most time of their own;
- the functions with the most time including what they call;
- a call tree;
- for the engine thread, where it waited and which system calls it made.

    python3 tools/switch/cpu_profile.py openprey_cpuprofile.txt \\
        --elf .tmp/elf-builds/OpenPrey-<commit>.elf [--map .tmp/elf-builds/OpenPrey-<commit>.map]

The ELF (and map) must be the ones of the NRO that wrote the profile: keep both
with every build that goes to a console. --list shows the reports in the file,
--report N reads one of them (default: all of them added up).

A sample is one look at a thread, taken every 2 ms. Kinds: running (in its
own code), in a system call (having used CPU time since the last look), or
waiting (no CPU time since the last look). Only the engine thread is sampled
while it waits.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import os
import re
import shutil
import subprocess
import sys

REPORT_RE = re.compile(r"^\[cpu profile\] report (\d+): ([\d.]+) s, (\d+) frames, (\d+) us between samples, "
                       r"code 0x([0-9a-f]+) bytes; (\d+) code line and (\d+) stack samples dropped")
THREAD_RE = re.compile(r'^thread (\d+) "([^"]*)" start \+0x([0-9a-f]+): cpu ([\d.]+)%; samples (\d+): '
                       r"(\d+) running, (\d+) in system calls, (\d+) waiting")
STACKS_RE = re.compile(r"^stacks: (\d+) of (\d+) listed, with (\d+) of (\d+) samples")

KINDS = {"r": "running", "s": "in system calls", "w": "waiting"}

# Longest stack the profiler records (STACK_DEPTH): link register + return addresses.
STACK_DEPTH = 24


def find_tool(name: str, explicit: str | None) -> str:
    if explicit:
        return explicit
    for candidate in (name, name + ".exe"):
        path = shutil.which(candidate)
        if path:
            return path
    # Git Bash exports DEVKITPRO=/opt/devkitpro even where devkitPro lives elsewhere: try what exists.
    roots = [os.environ.get("DEVKITPRO", ""), r"C:\devkitPro", "/opt/devkitpro"]
    for root in roots:
        if not root:
            continue
        for candidate in (name + ".exe", name):
            path = os.path.join(root, "devkitA64", "bin", candidate)
            if os.path.exists(path):
                return path
    sys.exit(f"{name} not found: put devkitA64/bin on PATH, set DEVKITPRO, or pass --nm")


def short_name(name: str) -> str:
    """'idFoo::Bar(int*) [clone .part.0]' -> 'idFoo::Bar': clones are pieces of the same function."""
    name = re.sub(r" \[clone [^\]]*\]", "", name)
    name = re.sub(r"\.(part|isra|constprop|cold|lto_priv)\.\d+", "", name)
    name = re.sub(r"\.(cold|isra|constprop)$", "", name)
    depth = 0
    for i, c in enumerate(name):
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
        elif c == "(" and depth == 0 and not name.startswith("operator", max(0, i - 8), i):
            return name[:i]
    return name


class Symbols:
    def __init__(self, nm: str, elf: str):
        if not os.path.exists(elf):
            sys.exit(f"{elf} not found: pass the ELF of the NRO that wrote the profile with --elf")
        output = subprocess.run([nm, "-n", "-C", "--defined-only", elf], check=True, capture_output=True,
                                text=True, errors="replace").stdout
        self.starts: list[int] = []
        self.names: list[str] = []
        for line in output.splitlines():
            parts = line.split(" ", 2)
            if len(parts) != 3:
                continue
            if parts[1] not in ("T", "t", "W", "w") or parts[2].startswith("$"):
                continue
            address = int(parts[0], 16)
            if self.starts and self.starts[-1] == address:
                continue
            self.starts.append(address)
            self.names.append(short_name(parts[2]))
        if not self.starts:
            sys.exit(f"no function symbols in {elf}")

    def name(self, offset: int) -> str:
        index = bisect.bisect_right(self.starts, offset) - 1
        return self.names[index] if index >= 0 else "?"

    def caller(self, return_address: int) -> str:
        # A return address follows the call, which can be the last instruction of its function.
        return self.name(return_address - 1)


# Mesa is one archive (libEGL.a): its object names say which part of it a function belongs to.
MESA_PARTS = (
    (("main_", "vbo_", "math_", "program_", "drivers_", "glapi_", "mapi_"), "Mesa: GL front end (main, vbo)"),
    (("state_",), "Mesa: state tracker"),
    (("zink_",), "Mesa: Zink (GL on Vulkan)"),
    (("nvk_", "nvkmd_", "nak_", "vulkan_", "vk_", "wsi_"), "Mesa: NVK (Vulkan driver)"),
    (("nvc0_", "nv50_", "nouveau_"), "Mesa: nouveau GL driver"),
    (("codegen_",), "Mesa: nouveau shader compiler"),
    (("u_", "util_", "cso_", "translate_", "tgsi_", "hash_", "os_", "vl_", "half_", "sha1_", "string_", "draw_",
      "indices_", "pipebuffer_", "tc_", "threaded_"), "Mesa: gallium helpers"),
    (("glsl_", "ir_", "builtin_", "lower_", "opt_", "ast_", "link_", "linker_", "loop_", "generate_", "propagate_",
      "shader_", "hir_", "nir_", "spirv_", "compiler_"), "Mesa: shader compiler front end"),
)

LIBRARIES = {
    "libdrm_nouveau.a": "libdrm_nouveau (GPU command submission)",
    "libnx.a": "libnx (system calls, services)",
    "libopenal.a": "OpenAL Soft",
    "libSDL2.a": "SDL2",
    "libc.a": "C/C++ runtime",
    "libm.a": "C/C++ runtime",
    "libgcc.a": "C/C++ runtime",
    "libstdc++.a": "C/C++ runtime",
    "libsysbase.a": "C/C++ runtime",
    "libz.a": "zlib",
}


def library_of(obj: str) -> str:
    """The part of the program a linker map object path belongs to."""
    obj = obj.replace("\\", "/")
    match = re.search(r"([^/]+\.a)\(([^)]*)\)\s*$", obj)
    if not match:
        base = obj.rsplit("/", 1)[-1]
        if base.startswith("game_"):
            return "OpenPrey game"
        return "OpenPrey engine"
    archive, member = match.group(1), match.group(2)
    if archive in ("libEGL.a", "libglapi.a", "libGLESv2.a", "libGLESv1_CM.a", "libvulkan.a"):
        lowered = member.lower()
        if "horizon" in lowered:
            return "Mesa: Horizon GPU backend"
        if archive == "libglapi.a":
            return MESA_PARTS[0][1]
        if archive == "libvulkan.a":
            return "Mesa: NVK (Vulkan driver)"
        # meson names objects after their path: src_gallium_drivers_zink_zink_draw.cpp.o
        for part in ("zink", "nvk", "nouveau_vulkan", "nouveau_compiler"):
            if part in lowered:
                return "Mesa: Zink (GL on Vulkan)" if part == "zink" else "Mesa: NVK (Vulkan driver)"
        for prefixes, name in MESA_PARTS:
            if member.startswith(prefixes):
                return name
        return "Mesa: other"
    return LIBRARIES.get(archive, archive)


class LinkerMap:
    """Which object each piece of .text came from, read from the -Wl,-Map file."""

    def __init__(self, path: str):
        self.starts: list[int] = []
        self.ends: list[int] = []
        self.libraries: list[str] = []
        entry = re.compile(r"^\s+0x([0-9a-f]{8,16})\s+0x([0-9a-f]+)\s+(\S.*)$")
        sections = []
        in_map = False
        pending = False
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = raw.rstrip("\r\n")
                if not in_map:
                    in_map = line.startswith("Linker script and memory map")
                    continue
                if line.startswith(" .text"):
                    rest = line[1:].split(None, 1)
                    if len(rest) == 2:
                        match = entry.match(" " + rest[1])
                        if match:
                            sections.append((int(match.group(1), 16), int(match.group(2), 16), match.group(3)))
                        pending = False
                    else:
                        pending = True     # a long name: address, size and object are on the next line
                    continue
                if pending:
                    pending = False
                    match = entry.match(line)
                    if match:
                        sections.append((int(match.group(1), 16), int(match.group(2), 16), match.group(3)))
        for start, size, obj in sorted(s for s in sections if s[1] > 0):
            self.starts.append(start)
            self.ends.append(start + size)
            self.libraries.append(library_of(obj))

    def library(self, offset: int) -> str:
        index = bisect.bisect_right(self.starts, offset) - 1
        if index >= 0 and offset < self.ends[index]:
            return self.libraries[index]
        return "(unknown)"


def parse(path: str):
    reports = []
    current = None
    thread = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            match = REPORT_RE.match(line)
            if match:
                current = {
                    "index": int(match.group(1)), "seconds": float(match.group(2)), "frames": int(match.group(3)),
                    "interval_us": int(match.group(4)), "code": int(match.group(5), 16),
                    "dropped": (int(match.group(6)), int(match.group(7))),
                    "threads": {}, "stacks": [], "coverage": (0, 0),
                }
                reports.append(current)
                thread = None
                continue
            if current is None:
                continue
            match = THREAD_RE.match(line)
            if match:
                thread = {
                    "name": match.group(2), "start": int(match.group(3), 16), "cpu": float(match.group(4)),
                    "samples": int(match.group(5)), "r": int(match.group(6)), "s": int(match.group(7)),
                    "w": int(match.group(8)), "whole": "not sampled while waiting" not in line, "lines": [],
                }
                current["threads"][int(match.group(1))] = thread
                continue
            if line.startswith(" l ") and thread is not None:
                parts = line.split()
                thread["lines"].append((int(parts[1], 16), int(parts[2])))
                continue
            match = STACKS_RE.match(line)
            if match:
                current["coverage"] = (int(match.group(3)), int(match.group(4)))
                thread = None
                continue
            if line.startswith(" s "):
                parts = line.split()
                if len(parts) >= 5 and parts[2] in KINDS:
                    current["stacks"].append((int(parts[1]), parts[2], int(parts[3]), int(parts[4], 16),
                                              [int(x, 16) for x in parts[5:]]))
    if not reports:
        sys.exit(f"no [cpu profile] reports in {path} (was com_cpuProfile 1 set?)")
    return reports


def call_path(symbols: Symbols, pc: int, chain: list[int]) -> list[str]:
    """Function names from the sampled one out to the thread's start, repeats folded.

    chain[0] is the link register. It names the caller only in a function without a frame
    record of its own (a leaf, or one still in its prologue): elsewhere it is either the
    return address the first frame record also holds, or left over from a call the
    function made, pointing into the function itself.
    """
    path = [symbols.name(pc)]
    returns = chain[1:]
    if chain and chain[0] != 0 and (not returns or chain[0] != returns[0]):
        caller = symbols.caller(chain[0])
        if caller != path[0]:
            path.append(caller)
    for address in returns:
        name = symbols.caller(address)
        if name != path[-1]:
            path.append(name)
    if len(chain) >= STACK_DEPTH:
        path.append("(deeper)")
    return path


class Node:
    __slots__ = ("counts", "children")

    def __init__(self):
        self.counts = collections.Counter()   # kind -> samples
        self.children: dict[str, Node] = {}

    def total(self) -> int:
        return sum(self.counts.values())


def percent(part: float, whole: float) -> float:
    return 100.0 * part / whole if whole else 0.0


def print_tree(node: Node, name: str, total: int, depth: int, max_depth: int, minimum: float, out) -> None:
    samples = node.total()
    share = percent(samples, total)
    if share < minimum or depth > max_depth:
        return
    note = ""
    blocked = node.counts["w"] + node.counts["s"]
    if blocked and samples:
        note = f"   [{percent(node.counts['w'], total):.1f} waiting, {percent(node.counts['s'], total):.1f} in system calls]"
    out.write(f"  {share:5.1f}  {'  ' * depth}{name}{note}\n")
    for child_name, child in sorted(node.children.items(), key=lambda item: -item[1].total()):
        print_tree(child, child_name, total, depth + 1, max_depth, minimum, out)


def report_thread(slot, thread, stacks, symbols, linker_map, args, out) -> None:
    name = thread["name"] or f"<{symbols.name(thread['start'])}>"
    samples = thread["samples"]
    out.write(f"\n{'=' * 100}\n")
    out.write(f"thread {name}: {thread['cpu']:.1f}% of a core, {samples} samples: "
              f"{percent(thread['r'], samples):.0f}% running, {percent(thread['s'], samples):.0f}% in system calls, "
              f"{percent(thread['w'], samples):.0f}% waiting"
              f"{'' if thread['whole'] else ' (its waits are counted, not sampled)'}\n")
    running = thread["r"]

    # time of its own, from the code lines
    functions = collections.Counter()
    libraries = collections.Counter()
    library_of_function = {}
    listed = 0
    for offset, count in thread["lines"]:
        function = symbols.name(offset)
        functions[function] += count
        listed += count
        if linker_map:
            library = linker_map.library(offset)
            libraries[library] += count
            library_of_function.setdefault(function, library)
    if running:
        out.write(f"\nrunning: {running} samples; the code lines listed hold {percent(listed, running):.0f}% of them\n")
    if libraries:
        out.write("\n  by library (share of the running samples):\n")
        for library, count in libraries.most_common():
            if percent(count, running) >= 0.1:
                out.write(f"    {percent(count, running):5.1f}%  {library}\n")
    if functions:
        out.write("\n  by function, its own time (share of the running samples):\n")
        for function, count in functions.most_common(args.top):
            out.write(f"    {percent(count, running):5.1f}%  {function:<56.56}  {library_of_function.get(function, '')}\n")

    mine = [s for s in stacks if s[0] == slot]
    if not mine:
        return
    stack_samples = sum(s[2] for s in mine)
    recorded = thread["r"] + thread["s"] + (thread["w"] if thread["whole"] else 0)
    out.write(f"\ncall stacks: {stack_samples} of the thread's {recorded} recorded samples are in the stacks listed "
              f"({percent(stack_samples, recorded):.0f}%); shares below are of those\n")

    inclusive = collections.Counter()
    by_kind = {kind: collections.Counter() for kind in KINDS}
    kind_samples = collections.Counter()
    root = Node()
    for _, kind, count, pc, chain in mine:
        path = call_path(symbols, pc, chain)
        for function in set(path):
            inclusive[function] += count
        kind_samples[kind] += count
        if kind != "r":
            by_kind[kind][" <- ".join(path[:args.chain])] += count
        node = root
        node.counts[kind] += count
        for function in reversed(path):
            node = node.children.setdefault(function, Node())
            node.counts[kind] += count

    out.write("\n  by function, with everything it calls (waits included):\n")
    for function, count in inclusive.most_common(args.top):
        out.write(f"    {percent(count, stack_samples):5.1f}%  {function}\n")

    out.write(f"\n  call tree (share of the thread's samples; branches under {args.tree_min:g}% are left out):\n")
    for child_name, child in sorted(root.children.items(), key=lambda item: -item[1].total()):
        print_tree(child, child_name, stack_samples, 0, args.tree_depth, args.tree_min, out)

    for kind, title in (("w", "waiting (no CPU time: blocked in the system)"), ("s", "system calls that used CPU time")):
        if not kind_samples[kind]:
            continue
        out.write(f"\n  {title}: {percent(kind_samples[kind], stack_samples):.1f}% of the thread's samples, from\n")
        for chain_text, count in by_kind[kind].most_common(args.waits):
            out.write(f"    {percent(count, stack_samples):5.1f}%  {chain_text}\n")


def merge(reports):
    """All reports added up. A thread slot can change hands between reports: it is keyed with its name."""
    merged = {"seconds": 0.0, "frames": 0, "threads": {}, "stacks": []}
    keys = {}
    stacks = collections.Counter()
    for report in reports:
        merged["seconds"] += report["seconds"]
        merged["frames"] += report["frames"]
        mapping = {}
        for slot, thread in report["threads"].items():
            key = keys.setdefault((slot, thread["name"], thread["start"]), len(keys))
            mapping[slot] = key
            target = merged["threads"].setdefault(key, {
                "name": thread["name"], "start": thread["start"], "cpu": 0.0, "samples": 0, "r": 0, "s": 0, "w": 0,
                "whole": thread["whole"], "lines": collections.Counter(),
            })
            target["cpu"] += thread["cpu"] * report["seconds"]
            for field in ("samples", "r", "s", "w"):
                target[field] += thread[field]
            for offset, count in thread["lines"]:
                target["lines"][offset] += count
        for slot, kind, count, pc, chain in report["stacks"]:
            if slot in mapping:
                stacks[(mapping[slot], kind, pc, tuple(chain))] += count
    for thread in merged["threads"].values():
        thread["cpu"] /= merged["seconds"] or 1.0
        thread["lines"] = sorted(thread["lines"].items(), key=lambda item: -item[1])
    merged["stacks"] = [(slot, kind, count, pc, list(chain)) for (slot, kind, pc, chain), count in stacks.items()]
    return merged


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("profile", help="openprey_cpuprofile.txt from the SD card")
    parser.add_argument("--elf", help="ELF of the NRO that wrote the profile")
    parser.add_argument("--map", help="linker map of the same build, for the split by library")
    parser.add_argument("--nm", help="aarch64-none-elf-nm (default: PATH, then DEVKITPRO)")
    parser.add_argument("--list", action="store_true", help="list the reports in the file and exit")
    parser.add_argument("--report", type=int, help="read only this report (default: all added up)")
    parser.add_argument("--thread", help="only threads whose name contains this text")
    parser.add_argument("--top", type=int, default=40, help="functions listed per table (default 40)")
    parser.add_argument("--waits", type=int, default=15, help="wait and system call sites listed (default 15)")
    parser.add_argument("--chain", type=int, default=7, help="callers shown per wait site (default 7)")
    parser.add_argument("--tree-min", type=float, default=1.0, help="smallest call tree branch, in %% (default 1)")
    parser.add_argument("--tree-depth", type=int, default=40, help="deepest call tree level (default 40)")
    parser.add_argument("--out", help="write the result to this file instead of the screen")
    args = parser.parse_args()

    reports = parse(args.profile)
    if args.list:
        for report in reports:
            engine = [t for t in report["threads"].values() if t["whole"]]
            samples = engine[0]["samples"] if engine else 0
            print(f"report {report['index']}: {report['seconds']:.1f} s, {report['frames']} frames "
                  f"({report['frames'] / report['seconds']:.1f} fps), {samples} samples of the engine thread, "
                  f"{report['coverage'][0]} of {report['coverage'][1]} stack samples listed")
        return

    if not args.elf:
        sys.exit("--elf is needed: the ELF of the NRO that wrote the profile")
    if args.report is not None:
        chosen = [r for r in reports if r["index"] == args.report]
        if not chosen:
            sys.exit(f"no report {args.report} in {args.profile} (see --list)")
        reports = chosen

    symbols = Symbols(find_tool("aarch64-none-elf-nm", args.nm), args.elf)
    linker_map = LinkerMap(args.map) if args.map else None

    out = open(args.out, "w", encoding="utf-8", newline="\n") if args.out else sys.stdout

    # The profile gives the size of the code mapping: the ELF's last function has to end inside its last pages.
    code = reports[0]["code"]
    if not (symbols.starts[-1] < code <= symbols.starts[-1] + 0x10000):
        out.write(f"WARNING: the profile comes from a build with 0x{code:x} bytes of code, but the code of "
                  f"{args.elf} ends near 0x{symbols.starts[-1]:x}: the names below are probably wrong. "
                  f"Use the ELF of the build that wrote the profile.\n\n")

    merged = merge(reports)
    dropped_lines = sum(r["dropped"][0] for r in reports)
    dropped_stacks = sum(r["dropped"][1] for r in reports)
    out.write(f"{len(reports)} report(s), {merged['seconds']:.1f} s, {merged['frames']} frames "
              f"({merged['frames'] / (merged['seconds'] or 1.0):.1f} fps), one sample every "
              f"{reports[0]['interval_us'] / 1000:g} ms per thread\n")
    if dropped_lines or dropped_stacks:
        out.write(f"table overflow: {dropped_lines} code line and {dropped_stacks} stack samples were not recorded\n")

    out.write("\nthreads (CPU time in percent of one core):\n")
    order = sorted(merged["threads"].items(), key=lambda item: -item[1]["cpu"])
    for _, thread in order:
        name = thread["name"] or f"<{symbols.name(thread['start'])}>"
        out.write(f"  {thread['cpu']:5.1f}%  {name}\n")

    for slot, thread in order:
        name = thread["name"] or symbols.name(thread["start"])
        if args.thread and args.thread.lower() not in name.lower():
            continue
        if thread["samples"] == 0:
            continue
        report_thread(slot, thread, merged["stacks"], symbols, linker_map, args, out)

    if args.out:
        out.close()
        print(f"written to {args.out}")


if __name__ == "__main__":
    main()
