#!/usr/bin/env python3
"""Run a Norn benchmark and attach reproducibility metadata to its JSON."""

import argparse
import json
import platform
import subprocess
import sys
import tempfile
from pathlib import Path


def command_output(command: list[str]) -> str:
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unavailable"


def cpu_brand() -> str:
    macos = command_output(["sysctl", "-n", "machdep.cpu.brand_string"])
    if macos != "unavailable":
        return macos
    lscpu = command_output(["lscpu"]).splitlines()
    for line in lscpu:
        if line.lower().startswith("model name"):
            return line.split(":", 1)[1].strip()
    return command_output(["sh", "-c", "grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/release/norn_benchmarks")
    parser.add_argument("--output", required=True)
    parser.add_argument("--compiler", default="c++")
    parser.add_argument("--build-type", default="Release")
    args, benchmark_args = parser.parse_known_args()

    with tempfile.NamedTemporaryFile(mode="w+", suffix=".json", delete=False) as raw_file:
        raw_path = Path(raw_file.name)
    try:
        command = [args.binary, "--benchmark_format=json", *benchmark_args]
        completed = subprocess.run(command, check=False, text=True, capture_output=True)
        if completed.returncode != 0:
            sys.stderr.write(completed.stderr)
            return completed.returncode
        raw_path.write_text(completed.stdout)
        results = json.loads(completed.stdout)
        metadata = {
            "git_commit": command_output(["git", "rev-parse", "HEAD"]),
            "git_dirty": bool(command_output(["git", "status", "--porcelain"])),
            "compiler": command_output([args.compiler, "--version"]).splitlines()[0],
            "os": platform.platform(),
            "architecture": platform.machine(),
            "cpu": cpu_brand(),
            "build_type": args.build_type,
            "queue_capacity": 1024,
            "producer_count": None,
            "consumer_count": None,
            "benchmark_configurations": {
                "bounded_mutex_queue_throughput": {"producer_count": 0, "consumer_count": 0},
                "spsc_throughput": {"producer_count": 1, "consumer_count": 1},
                "mpmc_throughput<1, 1>": {"producer_count": 1, "consumer_count": 1},
                "mpmc_throughput<2, 2>": {"producer_count": 2, "consumer_count": 2},
                "mpmc_throughput<4, 4>": {"producer_count": 4, "consumer_count": 4},
            },
        }
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps({"metadata": metadata, "results": results}, indent=2) + "\n")
    finally:
        raw_path.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
