#!/usr/bin/env python3
"""Run the hardware campaign with full metadata and explicit skip reasons."""

import argparse
import json
import os
import platform
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path


PERF_EVENTS = [
    "cycles",
    "instructions",
    "branches",
    "branch-misses",
    "cache-references",
    "cache-misses",
    "context-switches",
    "cpu-migrations",
    "task-clock",
]


def command_output(command: list[str]) -> str | None:
    try:
        completed = subprocess.run(command, check=True, text=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return completed.stdout.strip()


def git_metadata() -> dict[str, object]:
    commit = command_output(["git", "rev-parse", "HEAD"])
    dirty_output = command_output(["git", "status", "--porcelain"])
    return {
        "commit": commit,
        "dirty": dirty_output != "" if dirty_output is not None else None,
    }


def parse_cpu_list(value: str) -> list[int]:
    cpus: list[int] = []
    for part in value.strip().split(","):
        if not part:
            continue
        if "-" in part:
            start, end = (int(component) for component in part.split("-", maxsplit=1))
            cpus.extend(range(start, end + 1))
        else:
            cpus.append(int(part))
    return sorted(set(cpus))


def file_text(path: Path) -> str | None:
    try:
        return path.read_text().strip()
    except OSError:
        return None


def linux_topology() -> dict[str, object]:
    allowed = sorted(os.sched_getaffinity(0))
    entries: dict[str, dict[str, object]] = {}
    for cpu in allowed:
        topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        entries[str(cpu)] = {
            "core_id": file_text(topology / "core_id"),
            "package_id": file_text(topology / "physical_package_id"),
            "thread_siblings": parse_cpu_list(file_text(topology / "thread_siblings_list") or ""),
        }
    return {"allowed_cpus": allowed, "cpus": entries}


def macos_metadata() -> dict[str, object]:
    names = ["machdep.cpu.brand_string", "hw.physicalcpu", "hw.logicalcpu", "hw.cachelinesize"]
    return {name: command_output(["sysctl", "-n", name]) for name in names}


def environment() -> dict[str, object]:
    data: dict[str, object] = {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "os": platform.platform(),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "python": sys.version.splitlines()[0],
        "cpu_count": os.cpu_count(),
        "load_average": list(os.getloadavg()) if hasattr(os, "getloadavg") else None,
        "compiler": (command_output(["c++", "--version"]) or "unavailable").splitlines()[0],
        "git": git_metadata(),
    }
    if platform.system() == "Linux":
        data["topology"] = linux_topology()
        data["lscpu"] = command_output(["lscpu", "--json"])
        data["virtualization"] = command_output(["systemd-detect-virt", "--vm"])
        data["virtualized"] = data["virtualization"] not in (None, "none")
    elif platform.system() == "Darwin":
        data["topology"] = macos_metadata()
        data["virtualized"] = False
    else:
        data["topology"] = {"status": "unsupported"}
        data["virtualized"] = None
    return data


def choose_placement(placement: str, topology: dict[str, object]) -> tuple[list[int], list[int], str | None]:
    if placement == "unpinned":
        return [], [], None
    allowed = topology.get("allowed_cpus")
    cpus = topology.get("cpus")
    if not isinstance(allowed, list) or not isinstance(cpus, dict) or not allowed:
        return [], [], "Linux topology is unavailable"
    if placement == "same-cpu":
        return [allowed[0]], [allowed[0]], None
    if placement == "distinct-cpu":
        if len(allowed) < 2:
            return [], [], "fewer than two allowed CPUs"
        return [allowed[0]], [allowed[1]], None
    if placement == "distinct-core":
        first = allowed[0]
        first_info = cpus.get(str(first), {})
        for candidate in allowed[1:]:
            candidate_info = cpus.get(str(candidate), {})
            if (
                candidate_info.get("package_id") == first_info.get("package_id")
                and candidate_info.get("core_id") != first_info.get("core_id")
            ):
                return [first], [candidate], None
        return [], [], "no distinct physical cores in one package are available"
    if placement == "smt-siblings":
        for producer in allowed:
            siblings = cpus.get(str(producer), {}).get("thread_siblings", [])
            for consumer in siblings:
                if consumer != producer and consumer in allowed:
                    return [producer], [consumer], None
        return [], [], "no allowed SMT sibling pair"
    if placement == "cross-package":
        first = allowed[0]
        first_package = cpus.get(str(first), {}).get("package_id")
        for candidate in allowed[1:]:
            if cpus.get(str(candidate), {}).get("package_id") != first_package:
                return [first], [candidate], None
        return [], [], "no allowed cross-package CPU pair"
    return [], [], f"unknown placement {placement}"


def perf_available_events() -> tuple[list[str], dict[str, str]]:
    if shutil.which("perf") is None:
        return [], {event: "perf-unavailable" for event in PERF_EVENTS}
    available: list[str] = []
    unavailable: dict[str, str] = {}
    for event in PERF_EVENTS:
        completed = subprocess.run(
            ["perf", "stat", "-x,", "-e", event, "--", "true"], text=True, capture_output=True
        )
        if completed.returncode == 0:
            available.append(event)
        else:
            unavailable[event] = completed.stderr.strip() or "unsupported-or-permission-denied"
    return available, unavailable


def parse_perf(output: str) -> dict[str, object]:
    counters: dict[str, object] = {}
    for line in output.splitlines():
        fields = line.split(",")
        if len(fields) < 3:
            continue
        value, _, event = (field.strip() for field in fields[:3])
        counters[event] = value
    return counters


def run_case(command: list[str], perf: bool) -> tuple[dict[str, object] | None, dict[str, object]]:
    if not perf:
        completed = subprocess.run(command, text=True, capture_output=True)
        metadata = {"returncode": completed.returncode, "stderr": completed.stderr.strip()}
        if completed.returncode != 0:
            return None, metadata
        return json.loads(completed.stdout), metadata
    events, unavailable = perf_available_events()
    if not events:
        return None, {"returncode": None, "perf": {"available": [], "unavailable": unavailable}}
    completed = subprocess.run(
        ["perf", "stat", "-x,", "-e", ",".join(events), "--", *command], text=True, capture_output=True
    )
    metadata = {
        "returncode": completed.returncode,
        "stderr": completed.stderr.strip(),
        "perf": {"available": events, "unavailable": unavailable, "counters": parse_perf(completed.stderr)},
    }
    if completed.returncode != 0:
        return None, metadata
    return json.loads(completed.stdout), metadata


def summary(samples: list[dict[str, object]]) -> dict[str, object]:
    if not samples:
        return {"status": "no-samples"}
    values = [float(sample["throughput_items_per_second"]) for sample in samples]
    sorted_values = sorted(values)
    middle = len(sorted_values) // 2
    median = sorted_values[middle] if len(sorted_values) % 2 else (sorted_values[middle - 1] + sorted_values[middle]) / 2
    return {
        "sample_count": len(samples),
        "throughput_median_items_per_second": median,
        "throughput_min_items_per_second": min(values),
        "throughput_max_items_per_second": max(values),
        "throughput_mean_items_per_second": statistics.fmean(values),
        "throughput_population_stdev_items_per_second": statistics.pstdev(values),
        "all_complete": all(bool(sample["complete"]) for sample in samples),
    }


def atomic_write(path: Path, content: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as temporary:
        json.dump(content, temporary, indent=2, sort_keys=True)
        temporary.write("\n")
        temporary_path = Path(temporary.name)
    temporary_path.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/release/norn_hardware_benchmarks")
    parser.add_argument("--manifest", default="tools/hardware_campaign_manifest.json")
    parser.add_argument("--output", required=True)
    parser.add_argument("--case", action="append", dest="case_ids")
    parser.add_argument("--repetitions", type=int)
    parser.add_argument("--seed", type=int, default=20260822)
    parser.add_argument("--perf", action="store_true")
    args = parser.parse_args()

    manifest = json.loads(Path(args.manifest).read_text())
    available_cases = manifest["cases"]
    selected = [case for case in available_cases if not args.case_ids or case["id"] in args.case_ids]
    unknown = set(args.case_ids or []) - {case["id"] for case in selected}
    if unknown:
        parser.error("unknown case IDs: " + ", ".join(sorted(unknown)))
    random.Random(args.seed).shuffle(selected)
    machine = environment()
    records: list[dict[str, object]] = []
    for case in selected:
        record: dict[str, object] = {"case": case, "status": "skipped"}
        if case.get("linux_only") and platform.system() != "Linux":
            record["reason"] = "Linux-only affinity placement"
            records.append(record)
            continue
        producer_cpus: list[int] = []
        consumer_cpus: list[int] = []
        if platform.system() == "Linux":
            producer_cpus, consumer_cpus, reason = choose_placement(case["placement"], machine["topology"])
            if reason is not None:
                record["reason"] = reason
                records.append(record)
                continue
        command = [
            args.binary,
            "--queue",
            case["queue"],
            "--items",
            str(case.get("items", manifest["defaults"]["items"])),
            "--producers",
            str(case["producers"]),
            "--consumers",
            str(case["consumers"]),
            "--repetitions",
            str(args.repetitions or case.get("repetitions", manifest["defaults"]["repetitions"])),
            "--warmups",
            str(case.get("warmup_runs", manifest["defaults"]["warmup_runs"])),
            "--backoff",
            case.get("backoff", manifest["defaults"]["backoff"]),
        ]
        if producer_cpus:
            command.extend(["--producer-cpus", ",".join(str(cpu) for cpu in producer_cpus)])
            command.extend(["--consumer-cpus", ",".join(str(cpu) for cpu in consumer_cpus), "--require-affinity"])
        raw, execution = run_case(command, args.perf)
        record["command"] = command
        record["execution"] = execution
        if raw is None:
            record["status"] = "inconclusive"
        elif not raw["affinity_valid"] or not all(sample["complete"] for sample in raw["samples"]):
            record["status"] = "invalid"
            record["raw"] = raw
        else:
            record["status"] = "complete"
            record["raw"] = raw
            record["summary"] = summary(raw["samples"])
        records.append(record)
    atomic_write(
        Path(args.output),
        {
            "schema_version": 1,
            "manifest": manifest,
            "environment": machine,
            "order_seed": args.seed,
            "records": records,
        },
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
