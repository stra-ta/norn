# Norn guide

Norn is a header-only C++20 library for studying bounded queues and memory
ordering. It is a pre-1.0 lab, not a production-maturity claim.

## Build

Requirements are CMake 3.25 or newer, a Make or Ninja generator, and a C++20
compiler.

```sh
cmake --preset default      # Debug build with tests
cmake --build --preset default
ctest --preset default --output-on-failure
```

Presets: `default`, `debug`, `release`, `asan`, `ubsan`, `tsan`.
The `tsan` preset also enables the weakened-order probes
(`NORN_TSAN_PROBES=ON`).

The project applies `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`
to its own targets. Installed consumers do not inherit those project-only
warning flags.

## Verify

One-command verification for normal changes:

```sh
tools/verify.sh            # Debug suite in build/verify
tools/full_confidence.sh   # Debug, ASan, and UBSan, each isolated
```

The direct forms are the CMake presets above. Use a temporary build directory
when an existing CMake cache points at another checkout:

```sh
cmake -S . -B /tmp/norn-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/norn-build
ctest --test-dir /tmp/norn-build --output-on-failure
```

What the suite covers: capacity boundaries, wraparound, move-only values, FIFO
and exactly-once histories, close-and-drain semantics, blocked-consumer and
blocked-producer wakeups, the reservation-window and claim-window
demonstrations, and the source-level memory-order contract
(`tools/check_memory_order.py`, with a mutation self-test).

## The weakened-order experiment

```sh
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan -R tsan_probe --output-on-failure
```

`tsan_probe_ordered` must pass; `tsan_probe_weakened` passes only when TSan
reports the payload data race. See [`experiment/README.md`](experiment/README.md)
for the design, the recorded results, and the toolchain caveat about which
TSan builds can actually detect the race.

## Install and consume

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix <prefix>
```

Consume it from CMake:

```cmake
find_package(norn CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE norn::norn)
```

Configure the consumer with `-DCMAKE_PREFIX_PATH=<prefix>` when the prefix is
outside the default search paths. `tests/package_consumer` runs this path as a
CTest test: it installs into a private prefix and builds two external
consumers (one against `norn::core`, one against `norn::norn`) with no source
include paths.

## Reading the docs

Read the documentation in this order for a full pass over the kept code:

1. [`docs/CORRECTNESS.md`](docs/CORRECTNESS.md) - what each test proves
2. [`docs/MEMORY_MODEL.md`](docs/MEMORY_MODEL.md) - the memory-order reasoning per structure
3. [`docs/MPMC_DESIGN.md`](docs/MPMC_DESIGN.md) - the Vyukov ring: slot state machine, linearization, gaps
4. [`docs/BUG_DIARY.md`](docs/BUG_DIARY.md) - real defects found and their lessons
5. [`docs/INTERVIEW_NOTES.md`](docs/INTERVIEW_NOTES.md) - questions and answers grounded in the code
6. [`experiment/README.md`](experiment/README.md) - the weakened-order probe

## Limitations

- `mpmc_ring` is not formally lock-free despite using atomics and no mutex.
- The MPMC weak semantics produce false-empty from a producer hole and
  false-full from a consumer hole while a thread is mid-operation.
- `spsc_ring` requires exactly one producer and one consumer, and the
  destructor must not run concurrently with either.
- Performance conclusions come only from the labeled historical run
  (`docs/evidence/`); no current performance claim is made.

## License

Norn is released under the [MIT License](LICENSE).
