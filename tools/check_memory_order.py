#!/usr/bin/env python3
"""Check the source-level memory-order contract and its mutation guard."""

from __future__ import annotations

import argparse
from pathlib import Path


REQUIRED = {
    "include/norn/queue/spsc_ring.hpp": (
        "std::memory_order_acquire",
        "std::memory_order_release",
    ),
    "include/norn/queue/mpmc_ring.hpp": (
        "sequence.load(std::memory_order_acquire)",
        "sequence.store(pos + 1, std::memory_order_release)",
        "sequence.store(pos + Capacity, std::memory_order_release)",
    ),
    "include/norn/hazard/domain.hpp": (
        "slots_[SlotIndex].store(ptr, std::memory_order_release)",
        "std::atomic_thread_fence(std::memory_order_seq_cst)",
    ),
}


def contract_holds(root: Path) -> bool:
    return all(
        all(pattern in (root / relative).read_text(encoding="utf-8") for pattern in patterns)
        for relative, patterns in REQUIRED.items()
    )


def contract_holds_text(texts: dict[str, str]) -> bool:
    return all(all(pattern in texts[relative] for pattern in patterns)
               for relative, patterns in REQUIRED.items())


def self_test(root: Path) -> None:
    texts = {
        relative: (root / relative).read_text(encoding="utf-8")
        for relative in REQUIRED
    }
    if not contract_holds_text(texts):
        raise SystemExit("baseline memory-order contract is incomplete")
    mutated = dict(texts)
    relative = "include/norn/queue/spsc_ring.hpp"
    mutated[relative] = mutated[relative].replace(
        "std::memory_order_release", "std::memory_order_relaxed", 1
    )
    if contract_holds_text(mutated):
        raise SystemExit("mutation guard failed to catch a release-to-relaxed mutation")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if not contract_holds(args.root):
        raise SystemExit("memory-order source contract failed")
    if args.self_test:
        self_test(args.root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
