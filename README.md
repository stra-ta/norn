# Norn

C++20 concurrency structures with an emphasis on memory-model correctness, measurable performance, and documented tradeoffs.

## Status

**M4 - bounded MPMC queue**

The repository contains unbounded and bounded mutex queues, a bounded SPSC ring buffer,
and a bounded MPMC ring queue.
The SPSC queue is the first lock-free structure and uses explicit object lifetime management.
The MPMC queue is the first multi-producer, multi-consumer structure and uses the Vyukov
sequence-number ring scheme with documented weak-semantics tradeoffs.
Memory reclamation structures do not exist yet.

The local Apple `g++` command resolves to Apple Clang, not GNU GCC.
Real GNU GCC verification was performed with GCC 13.3.0 on Ubuntu 24.04 LTS (aarch64) inside a disposable Lima VM: default, release, ASan, UBSan, and TSan configurations all pass with strict warnings promoted to errors.
The same configurations run in Ubuntu CI for both GCC and Clang.

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
