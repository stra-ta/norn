# Norn

[![CI](https://github.com/stra-ta/norn/actions/workflows/ci.yml/badge.svg)](https://github.com/stra-ta/norn/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Norn is a low-level C++20 concurrency and memory-management library for latency-sensitive systems.

It ships small, well-specified building blocks: cache-line utilities, backoff policies, bounded SPSC and MPMC ring buffers, and a hazard-pointer-based MPSC linked queue.

The repository is also the library's lab: co-located tests, benchmarks, and hardware experiments that build, measure, and explain these designs under the C++ memory model.

Norn is pre-1.0 software under active development.
APIs may still change across the 0.x series, and nothing here should be read as a production-maturity or long-term-stability claim.

![Norn architecture](docs/ARCHITECTURE.svg)

![Norn difficulty ladder](docs/PROGRESSION.svg)

## Build and test

Requirements:

- CMake 3.25 or newer.
- Make or Ninja.
- A C++20 compiler.

Build the default configuration and run the full test suite:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default --output-on-failure
```

Project warnings are errors by default for GCC and Clang:

```text
-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
```

Catch2 and Google Benchmark are fetched by CMake from pinned source archives with verified hashes.

### Sanitizers

The repository includes Debug, Release, ASan, UBSan, and TSan presets:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure

cmake --preset ubsan
cmake --build --preset ubsan
ctest --preset ubsan --output-on-failure

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan --output-on-failure
```

GCC's ThreadSanitizer does not support `std::atomic_thread_fence` in this setup.
Apple Clang TSan and the other GCC sanitizer configurations are covered by the verification work.

## Install and consume

Install a configured build to a prefix:

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix <prefix>
```

Consume it from any project with CMake config mode:

```cmake
find_package(norn CONFIG REQUIRED)

target_link_libraries(my_app PRIVATE norn::norn)
```

Configure the consuming project with `-DCMAKE_PREFIX_PATH=<prefix>` when the prefix is outside CMake's standard search paths.

`norn::norn` declares a dependency on the full library.
`norn::core` declares a core-only dependency tier, although both targets use the package's shared installed header tree.

## Core primitives

The `norn/core/` headers provide the small pieces the queues are built from:

- `norn::cache_line_size`: the destructive interference size, taken from `std::hardware_destructive_interference_size` when the toolchain provides it and 64 otherwise.
- `norn::cache_aligned<T>`: a wrapper that aligns its `value` member to one cache line.
- `norn::isolated_atomic<T>`: `cache_aligned<std::atomic<T>>`, an atomic isolated onto its own cache line.
- `norn::cpu_relax()`: an architecture-specific pause hint (`pause` on x86, `yield` on ARM64, a signal fence elsewhere).
- `norn::backoff::{tight, yield, bounded, exponential}`: wait strategies for retry loops.

Every backoff policy is default-constructible and exposes the same two-operation contract: `reset()` restores the initial state before a new contention episode, and `operator()` performs exactly one wait step.
Stateful policies (`bounded`, `exponential`) should be reset between episodes; stateless policies accept `reset()` as a no-op.
Policies do no logging, no allocation, and no timing.

## What Norn contains

| Structure | Thread model | Storage | What it shows |
| --- | --- | --- | --- |
| `mutex_queue<T>` | Multiple producers and consumers | `std::deque` | A simple blocking reference implementation with close and drain semantics |
| `bounded_mutex_queue<T, N>` | Multiple producers and consumers | Fixed ring | A bounded locking baseline |
| `spsc_ring<T, N>` | One producer, one consumer | Pre-allocated ring | Ownership-based atomic publication |
| `spsc_ring_padded<T, N>` | One producer, one consumer | Pre-allocated ring | The cost and effect of cache-line separation |
| `mpmc_ring<T, N>` | Multiple producers and consumers | Pre-allocated ring | Sequence numbers, CAS reservation, and weak progress semantics |
| `mpsc_linked_queue<T>` | Multiple producers, one consumer | Heap nodes | Hazard-pointer protection and deferred reclamation |

The 0.1 names `spsc_queue`, `spsc_queue_padded`, `spsc_queue_seq_cst`, and `mpmc_queue` remain available as aliases of the canonical types above and are supported through the 0.x series.
For the hazard-pointer queue, `mpsc_queue` remains the retained implementation name in `norn/hazard_pointer.hpp`, and the canonical name `mpsc_linked_queue` aliases it until its decomposition in 0.3.

**Benchmark output (hardware campaign)**: per-sample `workers` array with per-thread `pushes`, `pops`, `retries`, `yields`, `spin_steps`, `producer_fairness`, `consumer_fairness`; campaign medians for all counter fields.

The bounded rings avoid individual node allocation and reclamation.
The hazard-pointer queue pays that cost deliberately so the lifetime problem is visible in code and tests.

![Norn memory model](docs/MEMORY-MODEL.svg)

## Baseline queue benchmarks

Measured on an Apple M1 with Apple Clang 21.0.0 at commit `5d73d0eb`, Release build, batch size 100,000 items.
Throughput is items per second from the harness; per-item cost is derived from the same value so the two columns agree.

![Norn throughput evidence](docs/BENCHMARKS.svg)


| Structure | Configuration | ns/item | ops/s |
| --- | --- | --- | --- |
| Bounded mutex queue | single-thread push/pop | 68.1 | 14.68M |
| SPSC ring | unpadded | 73.3 | 13.64M |
| SPSC ring | padded (cache-line separated) | 59.5 | 16.81M |
| MPMC ring | 1x1 | 32.7 | 30.56M |
| MPMC ring | 2x2 | 152.7 | 6.55M |
| MPMC ring | 4x4 | 168.2 | 5.95M |
| Hazard-pointer queue | MPSC, 1x1 | 194.6 | 5.14M |

Build the Release benchmarks:

```sh
cmake --preset release
cmake --build --preset release
```

The main comparison binary includes the bounded mutex baseline, unpadded and padded SPSC queues, and the hazard-pointer queue:

```sh
python3 tools/run_benchmark.py \
  --binary build/release/norn_benchmarks \
  --output results/queue-comparison.json \
  --compiler c++ \
  --build-type Release
```

The bounded MPMC configurations have their own benchmark binary:

```sh
python3 tools/run_benchmark.py \
  --binary build/release/norn_mpmc_benchmarks \
  --output results/mpmc.json \
  --compiler c++ \
  --build-type Release
```

Thread creation, retry loops, and scheduler yields are part of the timed workload.
The numbers compare designs on this machine, not absolute cycles-per-operation claims.

The expected shape shows up: padded SPSC beats unpadded, consistent with the cache-line-separation hypothesis, and the hazard-pointer queue runs about 2.6x the unpadded SPSC ring per item because it allocates nodes and publishes hazard pointers on the pop path.
MPMC 1x1 is the fastest single-pair config; throughput drops as producer and consumer threads contend.

Full results and metadata in [docs/BENCHMARK_RESULTS.md](docs/BENCHMARK_RESULTS.md).

The legacy table measures the original harness and is not a universal hardware claim.

### Hardware campaign

The focused hardware campaign reruns the queues under a steady-state harness that excludes thread setup from the timed region, and adds Linux affinity read-back, retry and scheduler accounting, and safe `seq_cst` comparison cases.
The measurements come from the same machines, but the numbers are not directly comparable with the baseline table above.

Three findings worth reading first:

- **Steady-state MPMC 1x1 reaches about 134M ops/s** on both native M1 (133.99M) and the Linux ARM64 VM (135.10M), far above the baseline's 30.56M, because the steady-state harness does not time queue construction or thread creation.
- **The MPMC contention cliff is shared across environments**: 1x1 at ~134M drops to ~21.7M at 2x2 and ~6.5M at 4x4 tight.
Yielding or bounded/exponential backoff recovers part of the loss (4x4 yield reaches ~9.6M on M1).
- **Cache-line padding is not portable**: the M1 pilot favors padded SPSC (60.41M vs 31.32M unpadded), but the virtualized Linux ARM64 pilot reverses it (12.67M vs 22.28M unpadded).
Pinning both producer and consumer to one vCPU collapses padded SPSC to 0.17M, while distinct-core pinning recovers to 18.54M.

Read [docs/HARDWARE_PERFORMANCE.md](docs/HARDWARE_PERFORMANCE.md) for methodology, raw-result locations, the full pilot table, and caveats.

## Lifecycles

![Queue lifecycles](docs/LIFECYCLE.svg)

The ring buffers move through a small, fixed sequence of ownership states: reserve, publish, consume, and reuse.
The hazard-pointer queue adds a separate lifetime protocol: protect a node before reading it, validate the source, retire it after unlinking, and reclaim it only after a scan finds no active protection.

## Example

The SPSC ring has the smallest API:

```cpp
#include <norn/queue/spsc_ring.hpp>

norn::spsc_ring<int, 1024> queue;

queue.try_push(42);

if (auto value = queue.try_pop()) {
  // consume *value
}
```

The SPSC contract is strict: exactly one thread owns producer operations and exactly one thread owns consumer operations.
Use `mpmc_ring` or a mutex-backed queue when that ownership model does not fit.

## Important distinctions

### spsc_ring

The producer owns the write index and the consumer owns the read index.
Each side publishes progress with release stores and observes the other side with acquire loads.
Values live in pre-allocated slots, and the queue manages object lifetimes explicitly.

Under this single-producer, single-consumer ownership contract, `try_push` and `try_pop` are bounded sequences of loads and stores with no loops and no waiting on the peer, so the queue is wait-free for as long as each side honors its role.

### mpmc_ring

The bounded MPMC ring uses per-slot sequence numbers and CAS-based reservation.
A sequence number tells a producer when a slot can be reused and tells a consumer when a value is published.

The ring is mutex-free, non-blocking, bounded, and pre-allocated with no per-operation allocation, but it is **not formally lock-free**.
A producer paused after reserving a slot can create a false-empty hole.
A consumer paused after claiming a slot can create a false-full result.
These behaviors are intentional, documented, and tested.

### mpsc_linked_queue

The linked MPSC queue allocates a heap node per push and requires an explicit caller-owned `norn::hazard_domain` at construction; every operating thread registers with that domain.

Hazard pointers protect dynamically allocated nodes while a thread is reading them.
The reader publishes a pointer, validates that the shared pointer has not changed, and clears the protection when finished.
A retired node is reclaimed only after a scan finds no active hazard pointer for it.

Progress therefore depends on node allocation succeeding and on scan work retiring nodes, so no formal lock-free or wait-free claim is made.
The implementation currently lives in `norn/hazard_pointer.hpp`, and the demonstration is MPSC rather than MPMC: that keeps the reclamation example focused while `mpmc_ring` covers the many-to-many case.

### Mutex queues

`mutex_queue` and `bounded_mutex_queue` are blocking reference implementations with close-and-drain semantics: `close()` rejects future pushes, wakes blocked operations, and lets consumers drain accepted values.
They anchor correctness comparisons and benchmark baselines rather than competing on throughput.

## Verification

The suite covers:

- Mutex queue behavior, close-and-drain semantics, and bounded capacity.
- SPSC boundaries, wraparound, move-only values, and two-thread transfer.
- MPMC boundaries, wraparound, exactly-once consumption, and progress holes.
- Core primitives: cache-line sizing, cache-aligned and isolated atomics, `cpu_relax`, and backoff policies.
- Queue API compatibility between the 0.1 names and the canonical 0.2 types.
- Parameterized MPMC and SPSC stress histories.
- Hazard-pointer publication, registration, reclamation, move-only values, and queue stress.

**Hardware campaign evidence** (CTest `hardware_campaign_tool` and `hardware_binary_json_contract`):
- Per-worker retry, yield, spin-step, and fairness counts in every sample.
- Full MPMC unpinned matrix: 1x1/2x2/4x4 × tight/yield/bounded/exponential backoff (1M items each).
- Campaign summary medians for retries, yields, spin steps, producer/consumer fairness.
- Multi-worker pinned cases guarded against misleading distinct-core placement.
- Binary JSON contract test validating output shape, sample completeness, and worker-aggregate reconciliation.

The project has been verified with:

| Environment | Toolchain | Coverage |
| --- | --- | --- |
| macOS ARM64 | Apple Clang 21 | Default, Release, ASan, UBSan, TSan |
| Ubuntu ARM64 in Lima | GNU GCC 13.3 | Default, Release, ASan, UBSan |
| Ubuntu x86-64 in GitHub Actions | GCC and Clang | Debug/Release matrix and Clang sanitizers |

GitHub Actions runs the native x86-64 build matrix and sanitizer jobs.
Local Linux verification uses a disposable ARM64 Lima copy of the source tree.

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): the 0.2 architecture record, covering library and lab responsibilities and project boundaries.
- [`docs/ARCHITECTURE_WRITEUP.md`](docs/ARCHITECTURE_WRITEUP.md): the complete architecture and performance overview.
- [`docs/MPMC_DESIGN.md`](docs/MPMC_DESIGN.md): slot states, reservations, memory orders, and progress limits.
- [`docs/HAZARD_POINTER_DESIGN.md`](docs/HAZARD_POINTER_DESIGN.md): publication, scanning, ABA, and reclamation.
- [`docs/CORRECTNESS_CAMPAIGN.md`](docs/CORRECTNESS_CAMPAIGN.md): parameterized histories, progress demonstrations, and results.
- [`docs/MEMORY_MODEL.md`](docs/MEMORY_MODEL.md): synchronization and linearization notes.
- [`docs/BENCHMARKING.md`](docs/BENCHMARKING.md): benchmark methodology and caveats.
- [`docs/BENCHMARK_RESULTS.md`](docs/BENCHMARK_RESULTS.md): measured metrics and environment.
- [`docs/HARDWARE_PERFORMANCE.md`](docs/HARDWARE_PERFORMANCE.md): affinity, false sharing, contention, backoff, ordering, and counter evidence (includes per-worker statistics and full backoff matrix).
- [`docs/INTERVIEW_NOTES.md`](docs/INTERVIEW_NOTES.md): questions grounded in the implementation.

## Limitations

- `mpmc_ring` is not formally lock-free despite using atomics and no mutex.
- `mpsc_linked_queue` makes no formal lock-free or wait-free claim, and its progress depends on allocation and scan work.
- Benchmark setup is part of the timed workload in the baseline harness.
- Native x86-64 verification comes from GitHub Actions; local Linux verification is ARM64.
- The Linux ARM64 hardware pilot is virtualized, and GitHub Actions is a functional smoke gate rather than a performance environment.
- Norn is a pre-1.0 library and lab, not a drop-in production dependency.

## Out of scope

Norn deliberately excludes:

- Schedulers and coroutine runtimes.
- Executors and thread pools.
- Networking, sockets, and I/O contexts.
- Any general-purpose threading framework.

## License

Norn is released under the [MIT License](LICENSE).
