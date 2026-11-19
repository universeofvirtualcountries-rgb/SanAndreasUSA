#!/usr/bin/env python3
"""Fail if a .cpp file on disk is missing from a build's source list.

This is the guard for the class of bug that already bit us twice: a source file
compiles fine in isolation (tools/arm_compile_check.sh compiles everything), but
the linker never sees it because it was never added to CMakeLists.txt /
Android.mk. The undefined-symbol error appears only in CI, on a device build.

Checks
------
* every .cpp under src/, mod/ and plugin-sdk/renderware/imgui is listed in
  CMakeLists.txt (MOD_SOURCES + CORE_SOURCES), Android.mk and the ARM check;
* all lists agree with each other;
* every listed file exists.

Usage: python3 tools/check_source_lists.py [--verbose]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEARCH_DIRS = ("src", "mod", "plugin-sdk/renderware/imgui")


def files_on_disk() -> set[str]:
    found: set[str] = set()
    for rel in SEARCH_DIRS:
        base = ROOT / rel
        if not base.is_dir():
            continue
        for path in base.rglob("*.cpp"):
            found.add(path.relative_to(ROOT).as_posix())
    return found


def _cmake_set(text: str, name: str) -> list[str]:
    """Tokens of `set(NAME ... )` — CMake multi-line lists need no backslashes."""
    match = re.search(r"set\(" + re.escape(name) + r"\b(.*?)\n\s*\)", text, re.S)
    if not match:
        return []
    body = match.group(1).replace("\\\n", " ")
    body = re.sub(r"#[^\n]*", "", body)
    return [token for token in body.split() if token]


def cmake_lists() -> set[str]:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    core = {e for e in _cmake_set(text, "CORE_SOURCES") if not e.startswith("${")}
    mod = {e for e in _cmake_set(text, "MOD_SOURCES") if not e.startswith("${")}
    return core | mod


def _mk_block(text: str, marker: str) -> list[str]:
    """Tokens of a make assignment, following trailing backslashes."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if marker not in line:
            continue
        tokens = line.split(marker, 1)[1].rstrip("\\").split()
        if not line.rstrip().endswith("\\"):
            return tokens
        for follow in lines[index + 1 :]:
            stripped = follow.strip()
            if stripped.startswith("#"):
                continue
            continued = stripped.endswith("\\")
            tokens.extend(stripped.rstrip("\\").split())
            if not continued:
                break
        return tokens
    return []


def android_mk() -> set[str]:
    text = (ROOT / "Android.mk").read_text(encoding="utf-8")
    return {token for token in _mk_block(text, "LOCAL_SRC_FILES") if token.endswith(".cpp")}


def arm_check() -> set[str]:
    text = (ROOT / "tools/arm_compile_check.sh").read_text(encoding="utf-8")
    match = re.search(r"^SOURCES=\(\n(.*?)^\)", text, re.S | re.M)
    return set(match.group(1).split()) if match else set()


def report(label: str, missing: set[str], extra: set[str], verbose: bool) -> int:
    problems = 0
    for path in sorted(missing):
        print(f"  MISSING in {label}: {path}")
        problems += 1
    for path in sorted(extra):
        print(f"  STALE in {label} (file does not exist): {path}")
        problems += 1
    if verbose and not problems:
        print(f"  {label}: ok")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    disk = files_on_disk()
    cmake = cmake_lists()
    ndk = android_mk()
    arm = arm_check()

    problems = 0
    problems += report("CMakeLists.txt", disk - cmake, cmake - disk, args.verbose)
    problems += report("Android.mk", disk - ndk, ndk - disk, args.verbose)
    problems += report("tools/arm_compile_check.sh", disk - arm, arm - disk, args.verbose)

    if problems:
        print(f"source lists out of sync: {problems} problem(s)")
        return 1

    print(f"source lists in sync ({len(disk)} .cpp files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
