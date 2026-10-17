# Norn

[![CI](https://github.com/stra-ta/norn/actions/workflows/ci.yml/badge.svg)](https://github.com/stra-ta/norn/actions/workflows/ci.yml)

A header-only C++20 lab for bounded queues and memory ordering. Four queue
types share one publication protocol, and a permanent ThreadSanitizer
experiment shows what happens when that protocol is weakened by exactly one
memory order.

## Structures

| Structure | Ownership | Storage | Contract |
| --- | --- | --- | --- |
| `mutex_queue<T>` | Multiple producers and consumers | `std::deque` | Blocking reference queue with close-and-drain semantics |
| `bounded_mutex_queue<T, N>` | Multiple producers and consumers | Fixed deque | Blocking bounded baseline |
| `spsc_ring<T, N>` | One producer, one consumer | Pre-allocated ring | Acquire/release publication |
| `mpmc_ring<T, N>` | Multiple producers and consumers | Vyukov sequence-number ring | CAS reservation with documented false-empty and false-full gaps |

`include/norn/core/` holds the cache-line placement helpers both rings share.
The padded SPSC variant (`spsc_ring_padded`) separates the two indices onto
their own cache lines as a placement experiment; see `docs/MEMORY_MODEL.md`.

Progress claims are deliberately narrow:
The SPSC ring is wait-free only under its one-producer, one-consumer contract.
The MPMC ring is mutex-free and non-blocking, but Norn makes no formal
lock-free claim for it: a preempted thread holding a reserved-but-unpublished
slot strands up to `Capacity - 1` published items until it resumes.
`docs/MPMC_DESIGN.md` records the design and its limits.

## The weakened-order experiment

`experiment/weakened_order_probe.cpp` runs the real `mpmc_ring` with one
producer and one consumer under ThreadSanitizer, twice from the same source:
the ordered control keeps the release publication store and must run clean;
the weakened target flips that one store to relaxed and must report a data
race on the payload access. Run it with the `tsan` preset; the details,
recorded results, and toolchain caveats are in
[`experiment/README.md`](experiment/README.md).

## Historical performance evidence

| Structure | Configuration | ns/item | ops/s |
| --- | --- | ---: | ---: |
| Bounded mutex queue | single-thread push/pop | 68.1 | 14.68M |
| SPSC ring | padded | 59.5 | 16.81M |
| MPMC ring | 1 producer, 1 consumer | 32.7 | 30.56M |
| MPMC ring | 4 producers, 4 consumers | 168.2 | 5.95M |
| Hazard-pointer queue | MPSC, 1 producer | 194.6 | 5.14M |

Apple M1, macOS arm64, Apple Clang 21.0.0, Release, commit `5d73d0eb`,
100,000-item batches. Thread setup, retries, and scheduler yields are
included, so these compare structures on that machine rather than claiming
portable per-operation cost. The raw manifest lives at
[`docs/evidence/benchmarks/legacy-5d73d0eb.json`](docs/evidence/benchmarks/legacy-5d73d0eb.json).

The benchmark programs and evidence-campaign tooling left the repository in
0.3; the committed JSONs under `docs/evidence/` remain the raw record,
annotated with their exact environment. No performance claim is made beyond
that labeled historical run.

## Build

Requirements are CMake 3.25 or newer, a Make or Ninja generator, and a C++20
compiler.

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default --output-on-failure
```

Presets: `default`, `debug`, `release`, `asan`, `ubsan`, `tsan`.
The project applies `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`
to its own targets; installed consumers do not inherit those flags.

## Verification

- `tools/verify.sh` configures, builds, and runs the suite in an isolated
  Debug directory; `tools/full_confidence.sh` repeats that under ASan and
  UBSan.
- CTest proves queue contracts: capacity boundaries, wraparound, move-only
  values, FIFO and exactly-once histories, close-and-drain behavior,
  blocked-consumer wakeups, and the reservation-window and claim-window
  demonstrations.
- `tools/check_memory_order.py` enforces the acquire/release contract on the
  ring sources and self-tests that it catches a release-to-relaxed mutation.
- The `tsan` preset builds and tests both weakened-order probe variants; the
  weakened test passes only when TSan names the race.
- `tests/package_consumer` installs the package into a private prefix and
  builds two external consumers against it with `CMakePrefixPath` alone.

## Documentation

- [`GUIDE.md`](GUIDE.md) - build, verify, install, and the experiment runbook
- [`docs/MEMORY_MODEL.md`](docs/MEMORY_MODEL.md) - memory-order reasoning for every structure
- [`docs/MPMC_DESIGN.md`](docs/MPMC_DESIGN.md) - the Vyukov ring design, gaps, and constraints
- [`docs/CORRECTNESS.md`](docs/CORRECTNESS.md) - what the tests prove per structure
- [`docs/BUG_DIARY.md`](docs/BUG_DIARY.md) - defects found during development and their lessons
- [`docs/INTERVIEW_NOTES.md`](docs/INTERVIEW_NOTES.md) - questions and answers grounded in this implementation

## Changes in 0.3

The lab is rescoped to the structures its study actually needs:

- Removed the hazard-pointer reclamation family and the MPSC linked queue
  built on it. The design note and the tail-lag bug lesson remain recoverable
  in git history; `docs/BUG_DIARY.md` keeps the lesson summary.
- Removed the google/benchmark harness and the hardware evidence-campaign
  tooling. Committed evidence JSONs are untouched and still carry their
  provenance metadata.
- Removed the 0.1 compatibility headers (`norn/spsc_queue.hpp`,
  `norn/mpmc_queue.hpp`, `norn/cache_line.hpp`, `norn/hazard_pointer.hpp`).
  Names are canonical from 0.2 onward: `norn::spsc_ring` and `norn::mpmc_ring`.
- Added the permanent weakened-order probe under `experiment/`.

Nothing else moved: the two rings, the mutex reference queues, their tests,
and the memory-order gate keep their semantics on this branch.

## Limitations

- `mpmc_ring` is not formally lock-free, and its weak semantics return
  observably wrong `empty()`/full results while a thread is inside its
  reservation or claim window. The gaps are documented, not hidden.
- `spsc_ring` requires exactly one producer and one consumer; the destructor
  must not run concurrently with either.
- No `close()` on the rings: draining multiple-producer runs requires joining
  producers first, as the stress protocols show.
- Performance conclusions are valid only for the labeled historical run;
  nothing here is a portable performance claim.

## License

Norn is released under the [MIT License](LICENSE).
