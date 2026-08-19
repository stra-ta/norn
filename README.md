# Norn

[![CI](https://github.com/wheevu/norn/actions/workflows/ci.yml/badge.svg)](https://github.com/wheevu/norn/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Norn is a C++20 concurrency lab for building, measuring, and explaining queues under the C++ memory model.


It starts with simple mutex-backed queues, moves through bounded SPSC and MPMC rings, and ends with an unbounded node-based queue that needs hazard-pointer reclamation.

Every step has tests, design notes, and an explanation of the tradeoffs.

![Norn architecture](docs/ARCHITECTURE.svg)

## What Norn contains

| Structure | Thread model | Storage | What it shows |
| --- | --- | --- | --- |
| `mutex_queue<T>` | Multiple producers and consumers | `std::deque` | A simple reference implementation with close and drain semantics |
| `bounded_mutex_queue<T, N>` | Multiple producers and consumers | Fixed ring | A bounded locking baseline |
| `spsc_queue<T, N>` | One producer, one consumer | Pre-allocated ring | Ownership-based atomic publication |
| `spsc_queue_padded<T, N>` | One producer, one consumer | Pre-allocated ring | The cost and effect of cache-line separation |
| `mpmc_queue<T, N>` | Multiple producers and consumers | Pre-allocated ring | Sequence numbers, CAS reservation, and weak progress semantics |
| `mpsc_queue<T>` | Multiple producers, one consumer | Heap nodes | Hazard-pointer protection and deferred reclamation |

The bounded queues avoid individual node allocation and reclamation.
The hazard-pointer queue pays that cost deliberately so the lifetime problem is visible in code and tests.****

## Lifecycles

![Queue lifecycles](docs/LIFECYCLE.svg)

The ring buffers move through a small, fixed sequence of ownership states: reserve, publish, consume, and reuse.
The hazard-pointer queue adds a separate lifetime protocol: protect a node before reading it, validate the source, retire it after unlinking, and reclaim it only after a scan finds no active protection.

## Example

The SPSC queue has the smallest API:

```cpp
#include <norn/spsc_queue.hpp>

norn::spsc_queue<int, 1024> queue;

queue.try_push(42);

if (auto value = queue.try_pop()) {
  // consume *value
}
```

The SPSC contract is strict: exactly one thread owns producer operations and exactly one thread owns consumer operations.
Use `mpmc_queue` or a mutex-backed queue when that ownership model does not fit.

## Important distinctions

### SPSC ring buffer

The producer owns the write index and the consumer owns the read index.
Each side publishes progress with release stores and observes the other side with acquire loads.
Values live in pre-allocated slots, and the queue manages object lifetimes explicitly.

### MPMC ring buffer

The bounded MPMC queue uses per-slot sequence numbers and CAS-based reservation.
A sequence number tells a producer when a slot can be reused and tells a consumer when a value is published.

The queue is mutex-free and non-blocking, but it is **not formally lock-free**.
A producer paused after reserving a slot can create a false-empty hole.
A consumer paused after claiming a slot can create a false-full result.
These behaviors are intentional, documented, and tested.

### Hazard pointers

Hazard pointers protect dynamically allocated nodes while a thread is reading them.
The reader publishes a pointer, validates that the shared pointer has not changed, and clears the protection when finished.
A retired node is reclaimed only after a scan finds no active hazard pointer for it.

The hazard-pointer demonstration is MPSC rather than MPMC.
That keeps the reclamation example focused while the bounded MPMC ring covers the many-to-many case.

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
cmake --build --preset asan
ctest --preset asan --output-on-failure

cmake --build --preset ubsan
ctest --preset ubsan --output-on-failure

cmake --build --preset tsan
ctest --preset tsan --output-on-failure
```

GCC's ThreadSanitizer does not support `std::atomic_thread_fence` in this setup.
Apple Clang TSan and the other GCC sanitizer configurations are covered by the verification work.

## Benchmarks

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

Thread creation, queue construction, retry loops, joins, and scheduler yields are part of the timed workload.
The comparison is useful for understanding design costs, not for claiming universal nanoseconds-per-operation numbers.

The first local comparison showed the expected shape: the hazard-pointer queue was roughly twice as slow as the bounded SPSC ring buffer because it allocates nodes and publishes hazard pointers on the pop path.

## Verification

The current suite contains 33 tests covering:

- Mutex queue behavior, close-and-drain semantics, and bounded capacity.
- SPSC boundaries, wraparound, move-only values, and two-thread transfer.
- MPMC boundaries, wraparound, exactly-once consumption, and progress holes.
- Parameterized MPMC and SPSC stress histories.
- Hazard-pointer publication, registration, reclamation, move-only values, and queue stress.

The project has been verified with:

| Environment | Toolchain | Coverage |
| --- | --- | --- |
| macOS ARM64 | Apple Clang 21 | Default, Release, ASan, UBSan, TSan |
| Ubuntu ARM64 in Lima | GNU GCC 13.3 | Default, Release, ASan, UBSan |
| Ubuntu x86-64 in GitHub Actions | GCC and Clang | Debug/Release matrix and Clang sanitizers |

GitHub Actions runs the native x86-64 build matrix and sanitizer jobs.
Local Linux verification uses a disposable ARM64 Lima copy of the source tree.

## Other notes

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): milestone scope and project boundaries.
- [`docs/ARCHITECTURE_WRITEUP.md`](docs/ARCHITECTURE_WRITEUP.md): the complete architecture and performance overview.
- [`docs/MPMC_DESIGN.md`](docs/MPMC_DESIGN.md): slot states, reservations, memory orders, and progress limits.
- [`docs/HAZARD_POINTER_DESIGN.md`](docs/HAZARD_POINTER_DESIGN.md): publication, scanning, ABA, and reclamation.
- [`docs/CORRECTNESS_CAMPAIGN.md`](docs/CORRECTNESS_CAMPAIGN.md): parameterized histories, progress demonstrations, and results.
- [`docs/MEMORY_MODEL.md`](docs/MEMORY_MODEL.md): synchronization and linearization notes.
- [`docs/BENCHMARKING.md`](docs/BENCHMARKING.md): benchmark methodology and caveats.
- [`docs/INTERVIEW_NOTES.md`](docs/INTERVIEW_NOTES.md): questions grounded in the implementation.

## Limitations

- `mpmc_queue` is not formally lock-free despite using atomics and no mutex.
- The hazard-pointer demonstration is MPSC, not MPMC.
- Benchmark setup is part of the timed workload.
- Native x86-64 verification comes from GitHub Actions; local Linux verification is ARM64.
- This is an educational concurrency project, not a drop-in production queue library.

## License

Norn is released under the [MIT License](LICENSE).
