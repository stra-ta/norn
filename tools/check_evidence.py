#!/usr/bin/env python3
"""Validate Norn's committed evidence manifest and generated figures."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path


REQUIRED = {
    "commit",
    "dirty",
    "compiler",
    "kernel",
    "cpu",
    "architecture",
    "build_type",
    "seed",
    "command",
}


def load_generator(root: Path):
    location = root / "tools/generate_benchmark_figure.py"
    spec = importlib.util.spec_from_file_location("norn_benchmark_figure", location)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load generator: {location}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_manifest(root: Path) -> None:
    manifest_path = root / "results/manifest.json"
    document = json.loads(manifest_path.read_text(encoding="utf-8"))
    if document.get("schema_version") != 1:
        raise SystemExit("results manifest schema_version must be 1")
    artifacts = document.get("committed_evidence")
    if not isinstance(artifacts, list) or not artifacts:
        raise SystemExit("results manifest has no committed evidence")
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            raise SystemExit("results manifest contains a non-object artifact")
        path = artifact.get("path")
        if not isinstance(path, str) or not (root / path).is_file():
            raise SystemExit(f"missing committed evidence: {path}")
        provenance = artifact.get("provenance")
        if not isinstance(provenance, dict) or set(provenance) != REQUIRED:
            raise SystemExit(f"incomplete provenance for {path}")
        if not isinstance(provenance["dirty"], bool):
            raise SystemExit(f"dirty must be boolean for {path}")
        if not isinstance(provenance["command"], list) or not provenance["command"]:
            raise SystemExit(f"command must be a non-empty argv list for {path}")
        for generated in artifact.get("generated", []):
            if not (root / generated).is_file():
                raise SystemExit(f"missing generated artifact: {generated}")


def check_generated_figures(root: Path) -> None:
    generator = load_generator(root)
    evidence = root / "docs/evidence/benchmarks/legacy-5d73d0eb.json"
    output = root / "docs/BENCHMARKS.svg"
    if output.read_text(encoding="utf-8") != generator.render(generator.read_measurements(evidence)):
        raise SystemExit(f"generated figure is stale: {output}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    check_manifest(args.root)
    check_generated_figures(args.root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
