# M5 Concurrency Correctness Campaign

## Purpose and scope

M5 is not a feature milestone.
It is a campaign that demonstrates, with executable evidence, the correctness properties
the queue documentation claims: exactly-once behavior, single-producer single-consumer
FIFO order, deterministic demonstration of the MPMC progress limitation, and confidence
in the weak-memory ordering across architectures.

The primary target is the MPMC queue.
The SPSC queue gets a contract-respecting parameterized stress, and the mutex queues get a
deterministic close-and-drain test.
The campaign outcome is scoped to what the tests demonstrate; it does not claim formal
classification proofs.

## Outcome statement

Source inspection confirms that MPMC and SPSC contain no mutex or condition-variable
usage; the mutex queues are the only blocking structures.
The campaign tests demonstrate the behavioral properties below:

- MPMC operations are non-blocking with respect to queue synchronization; a stalled
  producer in the reservation-to-publication window demonstrably produces false-empty
  observations and strands published items until it resumes, and a stalled consumer
  after claiming demonstrably produces false-full observations at the held slot while
  other consumers advance.
- Parameterized histories of the MPMC queue consume every value exactly once with no
  duplicates, across the swept producer, consumer, and capacity configurations.
- Single-producer, single-consumer (1P1C) histories are consumed in ascending order.
- The mutex queues drain every accepted value exactly once after close and reject
  pushes after close.

Formal linearizability of arbitrary concurrent histories is analytically documented in
the design notes and is not proven by this campaign; a full history checker is future
work.

## Proof 1 - Parameterized exactly-once and 1P1C FIFO histories

A stress harness runs many small histories from a fixed, committed manifest and
checks exactly-once consumption and 1P1C FIFO order.

The harness uses a fixed manifest of exactly 24 runs.
Each entry is a tuple of a run identifier, producer count, consumer count, capacity,
and values per producer.
Capacity is a compile-time template parameter, so the manifest instantiates the queue at
capacities 2, 8, and 64 and passes the other parameters at runtime.
The manifest is committed with the test source and is identical across platforms,
toolchains, and build configurations.

Run parameters:

- producer count from 1 to 8;
- consumer count from 1 to 8;
- capacity from the set {2, 8, 64};
- values per producer from 100 to 5,000.

Each run uses the drain protocol established in the M4 tests: consumers run concurrently
with producers, producers retry on false until every value is accepted, producers join,
then consumers drain the remainder.

The exactly-once oracle validates every popped value against the closed value range
before use, tracks per-ID consumption with atomic flags, detects duplicates, and checks
that the consumed total equals the expected total.
The existing oracle's missing range validation is fixed in this campaign.

The FIFO oracle is restricted to 1P1C runs.
With multiple consumers, a consumer can claim value n and stall while another consumer
claims and records n + 1, so completion-record order is not a valid FIFO signal.
With multiple producers, reservation order is not global push order, so completion
values need not be globally ascending.
1P1C runs have neither ambiguity and observe ascending order.

The manifest entries are fixed and are not generated from the identifiers at runtime.
Thread scheduling is not controlled; the same manifest is executed under five build
configurations on two platforms, so scheduling varies with the environment, and any
failure is reproduced by rerunning the same manifest entry.
Every manifest entry runs in all five build configurations (default, release, asan,
ubsan, tsan) on macOS and ARM64 Linux; the native CI matrix runs the suite on push.

Retry budgets are per-value attempt caps of 1,000,000 attempts that reset on progress,
so a correct run never approaches them and a regression fails the run instead of hanging
the suite.
CTest gets an explicit 300-second timeout per test so a hung thread fails the run.
A smoke measurement during implementation validates the runtime budget: the full
manifest completes in roughly two minutes per build configuration.
The manifest is trimmed only if a configuration exceeds the budget on every platform,
and any trimmed manifest is committed together with the measured runtimes.

## Proof 2 - Weak-memory confidence across architectures

The same manifest histories run on ARM64 (executed) and x86-64 (native CI, pending the
first push).

The host machine is ARM64, so a local x86-64 Lima guest would run under emulation and
the full sanitizer matrix on it would be slow.
The x86-64 evidence is therefore split:

- a budgeted emulated x86-64 run of the default and Release suites, gated by a TSan
  startup smoke test, was planned but NOT executed: the host had insufficient free disk
  for the emulated guest (see Results);
- the existing CI matrix (native x86-64 ubuntu-latest, GCC and Clang, plus Clang
  sanitizer jobs) is the native x86-64 signal for the same suite once pushed.

The ARM64 runs use the existing Lima instance with GNU GCC.
The macOS runs use the local Apple Clang.

Configuration coverage per platform:

- macOS and ARM64 Linux run default, Release, ASan, UBSan, and TSan (executed).
- Emulated x86-64 would run default and Release only, plus the TSan startup smoke; not executed.
- Native x86-64 CI runs the build matrix and the Clang sanitizer jobs on push (pending).

Release is included where practical because optimized execution changes timing and is
relevant to concurrency tests.
Cross-architecture coverage increases confidence in the memory orders; it does not
prove memory-model correctness, which is established analytically in the design notes.

## Proof 3 - Progress behavior demonstration

The reservation-hole behavior already has one deterministic test.
M5 extends it into a documented progress-demonstration suite with written expectations
that reference the design note.

Producer-hole test: a producer blocked in the reservation-to-publication window produces
false-empty observations and strands published items until the gate opens.
The test asserts a failed pop while a published item is present, and the in-order drain
after the gate opens.
The failed push at the reserved frontier is not called false-full: under the
reservation-CAS linearization the reserved slot already counts as an enqueued item, so
the failure is consistent with a genuinely full abstract queue.

Consumer-hole test: a consumer blocked after claiming but before releasing the slot
shows the asymmetric behavior: other consumers advance past the held position, and
producers see false-full at the held slot.
The held slot's item is already consumed at the claim linearization point, so capacity
exists and the producer's full result is observably wrong.
The test uses a value whose move constructor spins on an atomic gate.
The optional pop path claims the slot with a CAS and then move-constructs the returned
`std::optional` in place from `std::move(*element)`; construction from the existing slot
object cannot be elided, so the move constructor deterministically runs in the consumer
thread after the claim CAS.
The protocol: the consumer thread pops the gated value and signals an entered flag;
while the gate stays closed, another consumer pops a plain published value and a
producer attempts pushes with a 1,000,000-attempt cap, expecting a failure at the held
slot.
The gate opens before any join or failure exit, so no thread is ever left blocked; a
producer that exhausts its cap records the failure, the gate opens, and the test
requires the recorded observations.

Each test records the observed behavior against the documented claims, so the progress
limitation is demonstrated rather than asserted.

## Proof 4 - Memory-order sensitivity, educational and non-merged

A weakened-order variant of the MPMC queue shows what breaks when publication ordering
is relaxed.
The variant changes exactly one line: the producer's publish store from release to
relaxed.
The payload write then has no happens-before edge to the consumer's read, which is a
data race under the C++ memory model.

The weakened variant lives in a disposable scratch directory outside the repository,
never in the implementation.
A probe program compiles against it and runs under TSan.

The pass criterion is the validated TSan race report on the payload access, not
observable corruption.
Observable corruption is timing-dependent and not guaranteed, particularly on x86-64,
where a relaxed store commonly compiles to the same hardware store as a release store.
The evidence captured is the changed line, the TSan race report, and any corruption
observed, recorded in the campaign results.

## Proof 5 - Mutex queue close-and-drain and SPSC stress

The mutex queues get a deterministic close-and-drain protocol covering both
`mutex_queue` and `bounded_mutex_queue`.

- Accepted-before-close: consumers start concurrently with producers, because a bounded
  queue cannot accept more offered values than its capacity without concurrent draining.
  Producers push unique value IDs with retry until accepted and record the accepted set,
  then join.
  The main thread calls `close()`.
  Consumers then pop until nullopt and the consumed set is compared against the accepted
  set: every accepted value exactly once, nothing else.
  Because close() runs after all producers joined, it cannot race a push; the
  serialization of push and close by the queue's mutex is established by inspection.
- Rejected-after-close: after close, additional pushes return false.
- Woken-by-close: a consumer signals a started flag before calling `pop()` on an empty
  queue and must not have returned within a bounded window; the main thread then calls
  `close()`.
  The consumer's `pop()` returns nullopt only after close.
  This cannot prove the consumer is inside the condition-variable wait without
  instrumenting the queue, so it is the strongest deterministic wake check available,
  mirroring the started-flag pattern of the existing smoke tests.

The SPSC queue gets a 1P1C stress with the same manifest discipline: queue
capacities 8 and 64, the padded variant at 64, values per producer from 100 to 5,000,
and the exactly-once plus ascending-order oracle.
Producer and consumer share a cancellation flag: the producer checks it in its retry
loop, and the consumer sets it when its empty streak exceeds the 1,000,000-attempt cap.
Both threads exit on the flag, so joins always complete and a regression fails the run
instead of hanging the suite.

## Run matrix

| Suite | macOS Clang | ARM64 GCC | x86-64 emulated GCC | CI x86-64 GCC/Clang |
| --- | --- | --- | --- | --- |
| existing 18 tests | x | x | not run | x (on push) |
| MPMC manifest histories | x | x | not run | x (on push) |
| progress suite | x | x | not run | x (on push) |
| mutex close-and-drain + SPSC stress | x | x | not run | x (on push) |
| sanitizer configs | ASan, UBSan, TSan | ASan, UBSan, TSan | not run | Clang ASan/UBSan/TSan |
| Release build | x | x | not run | x (on push) |

CI selects `CMAKE_BUILD_TYPE=Release` directly in its workflow rather than through the
CMake presets.

## Results

Campaign executed on the M5 working tree on top of commit `e5eaa8f` (uncommitted M5
changes included), on the platforms below.
The manifest and oracles are the ones defined above; no manifest trimming was
needed, because every configuration finished well inside the two-minute budget.

### Commands

The exact commands executed on macOS (Apple Clang, no compiler override), per
configuration:

```sh
cmake --preset default && cmake --build --preset default -j8 && ctest --preset default
cmake --preset release && cmake --build --preset release -j8 && ctest --preset release
cmake --preset asan && cmake --build --preset asan -j8 && ctest --preset asan
cmake --preset ubsan && cmake --build --preset ubsan -j8 && ctest --preset ubsan
cmake --preset tsan && cmake --build --preset tsan -j8 && ctest --preset tsan
```

The exact commands executed on Linux ARM64 (GNU GCC 13.3.0), per configuration, with
the TSan configuration limited to the test targets:

```sh
cmake --preset default -DCMAKE_CXX_COMPILER=g++ && cmake --build --preset default -j4 && ctest --preset default
cmake --preset release -DCMAKE_CXX_COMPILER=g++ && cmake --build --preset release -j4 && ctest --preset release
cmake --preset asan -DCMAKE_CXX_COMPILER=g++ && cmake --build --preset asan -j4 && ctest --preset asan
cmake --preset ubsan -DCMAKE_CXX_COMPILER=g++ && cmake --build --preset ubsan -j4 && ctest --preset ubsan
cmake --preset tsan -DCMAKE_CXX_COMPILER=g++ -DNORN_BUILD_BENCHMARKS=OFF && cmake --build --preset tsan -j4 && ctest --preset tsan
```

The weakened-order probe compiled and ran with, on Linux:

```sh
g++ -fsanitize=thread -O1 -g -std=c++20 -I. probe.cpp -o probe
./probe
```

and on macOS with `clang++` in place of `g++`.

### Platform matrix

| Platform | Compiler | OS / kernel | Configurations | Result | Suite duration |
| --- | --- | --- | --- | --- | --- |
| macOS ARM64 (local) | Apple Clang 21.0.0 | macOS 26.5.2 (Darwin 25.5) | default | 24/24 | 0.76 s |
| macOS ARM64 (local) | Apple Clang 21.0.0 | macOS 26.5.2 (Darwin 25.5) | release | 24/24 | 0.44 s |
| macOS ARM64 (local) | Apple Clang 21.0.0 | macOS 26.5.2 (Darwin 25.5) | asan | 24/24 | 3.09 s |
| macOS ARM64 (local) | Apple Clang 21.0.0 | macOS 26.5.2 (Darwin 25.5) | ubsan | 24/24 | 0.98 s |
| macOS ARM64 (local) | Apple Clang 21.0.0 | macOS 26.5.2 (Darwin 25.5) | tsan | 24/24 | 13.32 s |
| Linux ARM64 (Lima `weir-linux`) | GNU GCC 13.3.0 | Ubuntu 24.04.4 LTS, kernel 6.8.0-134-generic | default | 24/24 | 0.56 s |
| Linux ARM64 (Lima `weir-linux`) | GNU GCC 13.3.0 | Ubuntu 24.04.4 LTS, kernel 6.8.0-134-generic | release | 24/24 | 0.47 s |
| Linux ARM64 (Lima `weir-linux`) | GNU GCC 13.3.0 | Ubuntu 24.04.4 LTS, kernel 6.8.0-134-generic | asan | 24/24 | 57.31 s |
| Linux ARM64 (Lima `weir-linux`) | GNU GCC 13.3.0 | Ubuntu 24.04.4 LTS, kernel 6.8.0-134-generic | ubsan | 24/24 | 0.68 s |
| Linux ARM64 (Lima `weir-linux`) | GNU GCC 13.3.0 | Ubuntu 24.04.4 LTS, kernel 6.8.0-134-generic | tsan | 24/24 | 1.86 s |
| Linux x86-64 (native) | GCC and Clang | ubuntu-latest (CI) | CI build matrix + Clang sanitizer jobs | pending first push | - |
| Linux x86-64 (emulated) | - | - | - | not run | - |

The emulated x86-64 run was dropped during execution: the host had 3.8 GiB free disk,
insufficient for a 100 GiB emulated guest, so the x86-64 evidence stays CI-based.

The 24 tests comprise the 18 existing tests, the two new mutex close-and-drain tests,
the blocked-consumer wake test, the consumer-hole progress test, and the two
manifest history suites (24 MPMC runs and 3 SPSC runs).

### Weakened-order probe (Proof 4)

The probe ran in disposable scratch outside the repository (`/tmp` on the host, copied
into the VM), never in the implementation tree.
The weakened variant changes exactly one line of `mpmc_queue.hpp`: the producer's
publish store from `memory_order_release` to `memory_order_relaxed`.

| Variant | Platform / toolchain | TSan result |
| --- | --- | --- |
| correct header (control) | macOS, Apple Clang TSan | clean, exit 0 |
| correct header (control) | Linux ARM64, GCC TSan | clean, exit 0 |
| weakened publish store | macOS, Apple Clang TSan | data race on payload access reported; probe aborted on corrupted payload |
| weakened publish store | Linux ARM64, GCC TSan | data race on payload access reported; exit 66 |

The TSan report names the payload access: a read of size 8 in the consumer thread's
optional move-construction versus a write of size 8 in the producer's `emplace`.
The negative controls confirm the race is caused by the weakened ordering, not by the
probe itself.

### Progress demonstrations (Proof 3)

- Producer-hole test: the blocked producer in the reservation-to-publication window
  produces a false-empty observation while a published item is present, and the failed
  push at the reserved frontier is consistent with a genuinely full abstract queue
  under the reservation-CAS linearization.
- Consumer-hole test: the blocked consumer in its claim-to-release window lets another
  consumer advance past the held position, while a producer sees false-full at the
  held slot; after the gate opens, the slot releases and the queue drains completely.

## Results and reproducibility

Campaign results are recorded in this document and capture, per platform: the git
commit and dirty state, the exact manifest, compiler and sanitizer versions,
OS and kernel, architecture, the commands run, and durations.
This follows the reproducibility metadata pattern of `tools/run_benchmark.py`.

## Decisions to confirm

- Accept the scoped outcome: exactly-once, 1P1C FIFO, progress demonstration with the
  corrected asymmetry (false-empty from the producer hole, false-full from the consumer
  hole), close-and-drain, and cross-architecture coverage, with formal linearizability
  analysis deferred.
- Accept that mutex-freedom is established by source inspection, not by a test.
- Accept the fixed 24-run manifest with capacities {2, 8, 64}, per-value 1,000,000
  attempt caps, 300-second CTest timeouts, and the global trimming policy.
- Accept the budgeted emulated x86-64 run gated by a TSan smoke test, with CI as the
  native x86-64 signal.
  The emulated run was not executed (insufficient host disk); CI remains the x86-64
  signal.
- Accept the disposable-scratch isolation policy for the weakened-order variant, with
  the TSan report as the pass criterion.
- Accept the deterministic mutex close-and-drain protocol and the SPSC 1P1C stress as
  the family coverage.