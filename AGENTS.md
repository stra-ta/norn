# Norn change guide

## Scope

Norn is a header-only C++20 concurrency library and its correctness and
measurement lab.
The shipped surface is under `include/norn/`; tests, benchmarks, and campaign
tools live beside it.

## Invariants

- `spsc_ring` has one producer and one consumer.
- `mpmc_ring` is bounded and mutex-free, but makes no formal lock-free claim.
- `mpsc_linked_queue` is MPSC and requires a caller-owned `hazard_domain`.
- A thread must deregister from its hazard domain before switching domains or
  exiting while a record is active.
- Hazard guards own one of the two record slots for their lifetime.
- A benchmark is performance evidence only; CTest proves functional behavior.
- Hardware results include commit, dirty state, compiler, kernel, CPU,
  architecture, build type, seed, and command metadata.

## Commands

Use the one-command checks for normal changes:

```sh
tools/verify.sh
tools/full_confidence.sh
```

The direct forms are `python3 tools/verify.py` and
`python3 tools/full_confidence.py`.
Use a temporary build directory when an existing CMake cache points at another
checkout.

For focused CMake work:

```sh
cmake -S . -B /tmp/norn-build -DCMAKE_BUILD_TYPE=Debug -DNORN_BUILD_BENCHMARKS=OFF
cmake --build /tmp/norn-build
ctest --test-dir /tmp/norn-build --output-on-failure
```

For benchmark evidence, build the Release target and run the committed
manifest through `tools/run_hardware_campaign.py`.
Keep generated host-specific output under ignored `results/` paths.

## Documentation

`docs/ARCHITECTURE.md` records module boundaries.
`docs/HAZARD_POINTER_DESIGN.md` records the hazard protocol and lifecycle.
`docs/CORRECTNESS.md` and `docs/CORRECTNESS_CAMPAIGN.md` record test contracts.
`docs/BENCHMARKING.md` and `docs/HARDWARE_PERFORMANCE.md` record measurement
method and limits.
`results/manifest.json` records the schema and provenance required for measured
artifacts.

## Change rules

Preserve compatibility headers unless a migration note and tests accompany a
breaking change.
Keep memory-order changes paired with a litmus or adversarial test.
Keep old raw evidence and label hardware-specific findings with their exact
environment.
Do not promote a performance result, progress guarantee, or production claim
without a committed artifact that supports it.

## Lab-wide contracts

- See https://github.com/stra-ta/.github/blob/main/LAB_RULES.md and https://github.com/stra-ta/.github/blob/main/EVIDENCE.md and https://github.com/stra-ta/.github/blob/main/COMPATIBILITY.md for lab-wide naming, evidence, and schema contracts.
- Per https://github.com/stra-ta/.github/blob/main/CONTRIBUTING.md, contributions require the target repo's AGENTS.md, README, and relevant design note, preserve repo boundaries, add the narrowest regression test, run one-command verification, and keep performance claims tied to committed manifests.
