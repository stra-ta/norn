# Norn

C++20 concurrency structures with an emphasis on memory-model correctness, measurable performance, and documented tradeoffs.

## Status

**M6 - hazard-pointer reclamation**

The repository contains unbounded and bounded mutex queues, a bounded SPSC ring buffer,
a bounded MPMC ring queue, and hazard-pointer reclamation.
The hazard-pointer library provides a domain, per-thread records with guarded slots,
an RAII guard, and a demonstration Michael-Scott queue that exercises the full
publication-scan-retire lifecycle.
Memory reclamation structures are implemented and documented.

M5 is a correctness campaign, not a feature: parameterized exactly-once and 1P1C FIFO
histories, deterministic demonstrations of the MPMC reservation-hole progress
limitation, mutex close-and-drain protocols, and an educational weakened-order probe
showing the memory-order sensitivity under TSan.
See `docs/CORRECTNESS_CAMPAIGN.md` for the campaign design and results.

The local Apple `g++` command resolves to Apple Clang, not GNU GCC.
Real GNU GCC verification was performed with GCC 13.3.0 on Ubuntu 24.04 LTS (aarch64) inside a disposable Lima VM: default, release, ASan, UBSan, and TSan configurations all pass with strict warnings promoted to errors.
The same configurations run in Ubuntu CI for both GCC and Clang; native x86-64 runs execute in CI on push and have not been pushed yet.

## Build

Requirements: CMake 3.25+, Make or Ninja, and a C++20 compiler.

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Dependencies are fetched by CMake for the test and benchmark targets.

To record benchmark metadata alongside Google Benchmark JSON output:

```sh
cmake --preset release
cmake --build --preset release
python3 tools/run_benchmark.py --output results/mutex-queue.json --compiler c++ --build-type Release
python3 tools/run_benchmark.py --binary build/release/norn_mpmc_benchmarks --output results/mpmc.json --compiler c++ --build-type Release
```

## Roadmap

1. M0: infrastructure
2. M1: mutex reference queues
3. M2: bounded SPSC ring buffer
4. M3: SPSC performance analysis
5. M4: bounded MPMC queue
6. M5: concurrency correctness campaign
7. M6: hazard-pointer reclamation
8. M7: benchmark comparison
9. M8: architecture and performance writeup

See `docs/ARCHITECTURE.md` for scope and `docs/INTERVIEW_NOTES.md` for learning goals.
