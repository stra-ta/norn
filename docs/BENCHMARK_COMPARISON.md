# Benchmark Comparison (M7)

## Purpose

M7 adds the hazard-pointer queue to the existing benchmark infrastructure so all
four queue families can be compared on the same machine, under the same conditions.

The four structures:

1. Bounded mutex queue (M1) — single-thread push/pop baseline
2. Bounded SPSC ring buffer (M2/M3) — unpadded and padded variants
3. Bounded MPMC ring buffer (M4) — 1x1, 2x2, 4x4
4. Unbounded HP queue (M6) — 1x1 (MPSC, multiple producers, single consumer)

## What is measured

Each benchmark creates the queue, pushes a batch of values, and pops them.
Thread creation, retry loops, and scheduler yields are part of the timed workload.
The numbers compare structures and configurations, not absolute throughput.

The HP benchmark uses 1x1 (single consumer) to match the MPSC contract of the
demonstration vehicle.
A2x2 or 4x4 configuration would require an MPMC queue, which is future work.

## Caveats

The HP queue allocates individual heap nodes and runs hazard publication/fences on
every pop.
It is expected to be slower than the bounded ring buffers, which pre-allocate
their storage and use simple atomics.
The comparison is educational, not an optimization target.

## Verification

- All existing benchmarks continue to pass.
- The new HP benchmark produces JSON output under tools/run_benchmark.py.
- Same two-platform matrix: macOS ARM64 + ARM64 Linux GCC.
- Release configuration for numbers; default for correctness.

## Decisions

- Use the existing batch size (100,000) for comparability.
- Add the HP benchmark to the existing mutex_queue_benchmark.cpp or a new file.
- Add metadata keys to tools/run_benchmark.py.
- Update docs/BENCHMARKING.md with M7 results.
