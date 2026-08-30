#!/usr/bin/env python3
"""Configure, build, and run Norn's functional verification suite."""

from __future__ import annotations

import argparse
import os
import subprocess
from pathlib import Path


def run(command: list[str], cwd: Path) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path(os.environ.get("NORN_BUILD_DIR", root / "build/verify")),
    )
    parser.add_argument("--build-type", default="Debug", choices=("Debug", "Release"))
    parser.add_argument("--with-benchmarks", action="store_true")
    args = parser.parse_args()
    build_dir = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    run(
        [
            "cmake",
            "-S",
            str(root),
            "-B",
            str(build_dir),
            f"-DCMAKE_BUILD_TYPE={args.build_type}",
            f"-DNORN_BUILD_BENCHMARKS={'ON' if args.with_benchmarks else 'OFF'}",
        ],
        root,
    )
    run(["cmake", "--build", str(build_dir), "--parallel"], root)
    run(["ctest", "--test-dir", str(build_dir), "--output-on-failure"], root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
