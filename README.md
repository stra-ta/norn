# Norn

C++20 concurrency structures with an emphasis on memory-model correctness, measurable performance, and documented tradeoffs.

## Status

**M0 - repository foundation**

The repository currently contains only build, test, benchmark, documentation, and CI scaffolding.
No concurrent data structure has been implemented yet.

## Build

Requirements: CMake 3.25+, Make or Ninja, and a C++20 compiler.

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Dependencies are fetched by CMake for the test and benchmark targets.

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
