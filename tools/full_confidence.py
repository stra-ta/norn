#!/usr/bin/env python3
"""Run functional, ASan, and UBSan confidence suites in isolated builds."""

from __future__ import annotations

import argparse
import os
import platform
import subprocess
from pathlib import Path


def run(command: list[str], cwd: Path) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def run_profile(root: Path, build_root: Path, name: str, flags: list[str]) -> None:
    build_dir = build_root / name
    configure = [
        "cmake",
        "-S",
        str(root),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DNORN_BUILD_BENCHMARKS=OFF",
    ]
    if flags:
        joined = " ".join(flags)
        configure.extend([f"-DCMAKE_CXX_FLAGS={joined}", f"-DCMAKE_EXE_LINKER_FLAGS={joined}"])
    run(configure, root)
    run(["cmake", "--build", str(build_dir), "--parallel"], root)
    run(["ctest", "--test-dir", str(build_dir), "--output-on-failure"], root)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path(os.environ.get("NORN_CONFIDENCE_DIR", root / "build/confidence")),
    )
    parser.add_argument(
        "--with-tsan",
        action="store_true",
        help="also run TSan when the local toolchain supports it",
    )
    args = parser.parse_args()
    build_root = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    profiles = [
        ("debug", []),
        ("asan", ["-fsanitize=address", "-fno-omit-frame-pointer"]),
        ("ubsan", ["-fsanitize=undefined", "-fno-omit-frame-pointer"]),
    ]
    if args.with_tsan:
        if platform.system() == "Darwin":
            print("TSan skipped on Darwin: this repository's local toolchain does not support it.")
        else:
            profiles.append(("tsan", ["-fsanitize=thread"]))
    for name, flags in profiles:
        run_profile(root, build_root, name, flags)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
