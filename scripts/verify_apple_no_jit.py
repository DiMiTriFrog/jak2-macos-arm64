#!/usr/bin/env python3
"""Fail closed when an Apple AOT bundle still advertises or embeds JIT support."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


FORBIDDEN_TEXT = (
    b"com.apple.security.cs.allow-jit",
    b"MAP_JIT",
    b"pthread_jit_write_protect_np",
    b"vm_protect",
)


def run(command: list[str]) -> tuple[int, bytes, bytes]:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    return result.returncode, result.stdout, result.stderr


def bundle_binaries(target: Path) -> list[Path]:
    if target.is_file():
        return [target]

    candidates: list[Path] = []
    for relative in ("Contents/MacOS", "Contents/Frameworks", "Frameworks"):
        directory = target / relative
        if directory.is_dir():
            candidates.extend(path for path in directory.rglob("*") if path.is_file())
    if not candidates:
        raise SystemExit(f"no executable bundle contents found: {target}")
    return sorted(candidates)


def verify_entitlements(binary: Path, allow_unsigned: bool) -> list[str]:
    code, stdout, stderr = run(["codesign", "-d", "--entitlements", ":-", str(binary)])
    payload = stdout + stderr
    if code != 0 and not allow_unsigned:
        return [f"{binary}: codesign inspection failed (use --allow-unsigned only for local builds)"]
    if b"com.apple.security.cs.allow-jit" in payload or b"allow-jit" in payload:
        return [f"{binary}: JIT entitlement is present"]
    return []


def verify_macho(binary: Path) -> list[str]:
    code, stdout, stderr = run(["file", str(binary)])
    if code != 0:
        return [f"{binary}: file inspection failed: {stderr.decode(errors='replace').strip()}"]
    description = stdout.decode(errors="replace")
    if "Mach-O" not in description:
        return []

    payload = binary.read_bytes()
    problems = [f"{binary}: forbidden JIT marker {marker!r}" for marker in FORBIDDEN_TEXT if marker in payload]

    load_code, load_stdout, load_stderr = run(["otool", "-l", str(binary)])
    if load_code != 0:
        problems.append(f"{binary}: otool inspection failed: {load_stderr.decode(errors='replace').strip()}")
    elif b"__JIT" in load_stdout or b"JIT" in load_stdout:
        problems.append(f"{binary}: executable load command contains a JIT-labelled region")

    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--allow-unsigned", action="store_true")
    args = parser.parse_args()

    target = args.bundle.resolve()
    if not target.exists():
        print(f"target does not exist: {target}", file=sys.stderr)
        return 2

    problems: list[str] = []
    for binary in bundle_binaries(target):
        problems.extend(verify_macho(binary))
        problems.extend(verify_entitlements(binary, args.allow_unsigned))

    if problems:
        for problem in problems:
            print(f"FAIL: {problem}", file=sys.stderr)
        return 1

    print(f"PASS: no JIT markers or JIT entitlement found in {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
