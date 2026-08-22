# Architecture

This document records what Norn 0.2 actually is.
It describes the library and lab split, the current code paths, packaging targets, and the boundary between shipped surface and future work.

## Library and lab

Norn is a low-level C++20 concurrency and memory-management library for latency-sensitive systems, packaged as header-only C++20 with tested contracts.

The same repository is also its lab: `tests/`, `benchmarks/`, and `tools/` co-locate correctness suites, benchmark harnesses, and hardware experiments next to the code they measure.

The library side promises stability discipline: canonical headers, compatibility aliases through 0.x, and documented limits.

The lab side promises evidence discipline: measured claims carry methodology, raw results, and caveats in `docs/`.

## Current shape

The shipped library lives under `include/norn/`:

- `core/cache_line.hpp`, `core/alignment.hpp`, `core/atomic.hpp`, `core/cpu_relax.hpp`, and `core/backoff.hpp` provide the cache-line constant, `cache_aligned<T>`, `isolated_atomic<T>`, `cpu_relax()`, and the backoff policies (`tight`, `yield`, `bounded`, `exponential`).
- `queue/spsc_ring.hpp` provides the bounded SPSC ring and its padded and seq-cst variants.
- `queue/mpmc_ring.hpp` provides the bounded MPMC sequence-number ring.
- `queue/mpsc_linked_queue.hpp` provides the canonical MPSC name as an alias for `mpsc_queue` in `hazard_pointer.hpp`.
- `hazard_pointer.hpp` provides `hazard_domain`, `hazard_record`, `hazard_ptr`, and the `mpsc_queue` implementation; decomposing this file into its own area is future work.
- `mutex_queue.hpp` provides the blocking reference queues.
- `spsc_queue.hpp`, `mpmc_queue.hpp`, and `cache_line.hpp` are compatibility headers that alias the 0.1 names to the canonical types.

The lab lives in `tests/`, `benchmarks/`, and `tools/`: Catch2 unit and stress tests, Google Benchmark harnesses, and the hardware campaign runner with its manifest and Python tooling.

Design notes, correctness-campaign results, and benchmark evidence live in `docs/`.

## Memory management paths

There are three real paths today, and each is deliberate:

- The bounded rings (`spsc_ring`, `mpmc_ring`) pre-allocate all storage at construction, perform no per-operation allocation, and manage object lifetimes explicitly; destruction drains remaining items and requires quiescence.
- `mpsc_linked_queue` allocates a heap node per push and defers reclamation through hazard pointers against an explicit caller-owned `norn::hazard_domain`; retirement triggers scans once a threshold is reached.
- The mutex queues use `std::deque` behind a mutex and condition variables, prioritizing clarity over performance.

## Progress guarantees

- `spsc_ring`: bounded, pre-allocated, no per-operation allocation; acquire/release publication; wait-free under the single-producer, single-consumer ownership contract.
- `mpmc_ring`: bounded, pre-allocated, no per-operation allocation; mutex-free and non-blocking, but not formally lock-free because a thread paused inside a reservation or claim window blocks progress through that slot.
- `mpsc_linked_queue`: MPSC over heap nodes with an explicit hazard domain; progress depends on allocation success and scan work, so no formal lock-free claim is made.
- `mutex_queue` and `bounded_mutex_queue`: blocking close-and-drain reference implementations.

## Compatibility

The 0.1 names remain available through the entire 0.x series.
`spsc_queue`, `spsc_queue_padded`, `spsc_queue_seq_cst`, and `mpmc_queue` alias their canonical ring types, while `mpsc_queue` remains the retained implementation name that the canonical `mpsc_linked_queue` aliases until 0.3.

`tests/queue_api_test.cpp` asserts type identity across these names and their targets, so the compat layer cannot silently drift.

## Packaging

Norn installs version 0.2.0 as a CMake config package.

Consumers use config mode discovery and link the interface targets:

```cmake
find_package(norn CONFIG REQUIRED)

target_link_libraries(my_app PRIVATE norn::norn)
```

Two targets are exported:

- `norn::norn`: declares a dependency on the full library, including queues and hazard pointers.
- `norn::core`: declares the core-only dependency tier.

Both targets are header-only INTERFACE libraries requiring C++20.
They use one shared installed header tree, so the target choice records dependency intent rather than hiding headers at compile time.

## Graduation rule

A lab experiment graduates into the shipped library when it satisfies four conditions:

1. It has a canonical header in an area directory under `include/norn/`.
2. It has API tests and stress or adversarial coverage in `tests/`.
3. Its contract and known limits are documented in `docs/`.
4. Performance claims are backed by measured evidence from the lab.

Until all four hold, the work stays in the lab.
Decomposing hazard pointers out of `norn/hazard_pointer.hpp` is the next candidate under this rule.

## Future work

Planned items, none of which exist yet in the shipped surface:

- A physical separation of the lab (benchmarks, campaigns, tooling) from the installable library tree.
- Decomposition of hazard pointers out of `norn/hazard_pointer.hpp`.
- Memory pools.
- Channels.
- Wait strategies beyond the current backoff policies.
- Telemetry.

## Out of scope

Norn deliberately excludes schedulers, networking, coroutine runtimes, executors and thread pools, I/O contexts, sockets, and any general-purpose threading framework.
