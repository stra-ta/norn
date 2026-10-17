# Norn change guide

## Scope

Norn is a header-only C++20 lab for bounded queues and memory ordering.
The shipped surface is `include/norn/`: two atomic rings (`spsc_ring`,
`mpmc_ring`), two mutex reference queues (`mutex_queue`,
`bounded_mutex_queue`), and the shared cache-line helpers under
`include/norn/core/`. Tests live in `tests/`, the weakened-order TSan
experiment in `experiment/`, and the one-command verification in `tools/`.

## Invariants

- `spsc_ring` has exactly one producer and one consumer. The producer owns
  `write_index_`; the consumer owns `read_index_`. Each side observes the
  other's index only through acquire loads, and each publishes with a release
  store. The destructor must not run concurrently with either side.
- `mpmc_ring` is bounded and mutex-free, but makes no formal lock-free claim.
  Its weak semantics produce false-empty from a producer hole and false-full
  from a consumer hole; docs/MPMC_DESIGN.md is the authority.
- `NORN_EXPERIMENT_WEAKENED_PUBLISH` exists only for the experiment targets.
  Any build that defines it knowingly removes the release publication store;
  it must never leak into library consumers or tests outside `experiment/`.
- The queue contracts live beside tests and sanitizers. A benchmark claim has
  no home in the repository anymore; historical measurements are under
  `docs/evidence/` with their provenance labels.
- CTest proves functional behavior. Performance numbers are never part of a
  pass condition.

## Commands

Use the one-command checks for normal changes:

```sh
tools/verify.sh
tools/full_confidence.sh
```

For focused CMake work:

```sh
cmake -S . -B /tmp/norn-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/norn-build
ctest --test-dir /tmp/norn-build --output-on-failure
```

The `tsan` preset builds the suite and both weakened-order probe variants.
It is the only preset where `tsan_probe_weakened` is expected to pass, and
only on a toolchain whose TSan detects races; check the caveat in
`experiment/README.md` before interpreting a local macOS run.

## Documentation

- `README.md` records the structure set and the historical evidence labels.
- `GUIDE.md` records build, verification, and install paths.
- `docs/MEMORY_MODEL.md` records the memory-order reasoning per structure.
- `docs/MPMC_DESIGN.md` records the Vyukov ring design and its limits.
- `docs/CORRECTNESS.md` records what the tests prove.
- `docs/BUG_DIARY.md` records real defects and their lessons.
- `experiment/README.md` records the probe design and run results.
- `docs/evidence/` keeps the committed historical evidence JSONs.

## Change rules

Keep memory-order changes paired with a litmus or adversarial test; the
weakened-order probe is the standing adversarial suite for the MPMC
publication edge, and `tools/check_memory_order.py` guards the source-level
contract.
Label any hardware-specific finding with its exact environment, or leave it
out.
Do not re-add performance targets, campaign tooling, or unsourced claims
without a committed artifact and a reason the lab needs them.
Keep the tests warning-clean: the project builds with `-Werror` on GCC and
Clang.

## Compatibility

The 0.1 compatibility headers were removed in 0.3 with a migration note in
`README.md`; require the same note-and-test treatment for any future breaking
change to public names.
