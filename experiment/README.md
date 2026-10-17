# The weakened-order probe

The MPMC ring's correctness rests on one store. After constructing a payload,
the producer publishes it with

```cpp
cell->sequence.store(pos + 1, std::memory_order_release);
```

and the consumer's acquire load of that number is the happens-before edge that
orders the payload write before the payload read. This experiment asks what
happens when that edge is removed.

`weakened_order_probe.cpp` runs the real `norn::mpmc_ring` with one producer
and one consumer, passing a guarded payload through the queue. The same source
builds two targets:

| target | publication store | expected under ThreadSanitizer |
| --- | --- | --- |
| `norn_probe_ordered` | `memory_order_release` | clean run, exit 0 |
| `norn_probe_weakened` | `memory_order_relaxed` (one-line change) | data race on the payload access; probe aborts on a corrupted payload |

The weakened variant is the exact adversarial change the queue's memory-order
contract guards against: one memory order on one store. Nothing else differs.

## Run it

```sh
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan -R tsan_probe --output-on-failure
```

`tsan_probe_ordered` must pass. `tsan_probe_weakened` passes only when its
output contains a `WARNING: ThreadSanitizer: data race` report; the
`check_race_report.sh` wrapper inspects the text instead of trusting the exit
code, so a clean weakened run is a visible failure in both directions.

The guard check means the corrupted variant usually also aborts with its own
diagnostic, on any sanitizer or none.

## Recorded results

First demonstrated on the 0.2 tree with a disposable scratch copy of the
header, outside the repository. The full campaign record is in git history
(`docs/CORRECTNESS_CAMPAIGN.md`, removed with the campaign tooling in 0.3).
The experiment lives in the repository from 0.3 so the probe and its controls
are repeatable in one command.

| variant | platform / toolchain | result |
| --- | --- | --- |
| ordered control | macOS ARM64, Apple Clang TSan | clean, exit 0 |
| ordered control | Linux ARM64, GNU GCC 13.3 TSan | clean, exit 0 |
| weakened publish store | macOS ARM64, Apple Clang TSan | data race on payload access reported; probe aborted on corrupted payload |
| weakened publish store | Linux ARM64, GNU GCC 13.3 TSan | data race on payload access reported; exit 66 |

0.3 in-repository runs:

| variant | platform / toolchain | result |
| --- | --- | --- |
| ordered control | Linux ARM64 container (Docker, `gcc:13`, GNU GCC 13.5.0) | clean, exit 0 |
| weakened publish store | Linux ARM64 container (Docker, `gcc:13`, GNU GCC 13.5.0) | `WARNING: ThreadSanitizer: data race` on the payload storage in `optional`; exit 66 |

Toolchain caveat, recorded 2026-10-17: Apple Clang 21.0.0 with
`-fsanitize=thread` on macOS 26.6 (arm64) loads the runtime and instruments the
binary but does not report even a plain two-thread race on the same host. Treat
macOS TSan output from this toolchain as non-evidence for this experiment; the
Linux GCC results above are the detection evidence, and the CI thread-sanitizer
job re-verifies the probes on every change.

## The same flip on the SPSC ring

`spsc_ring` has the same shape: the producer's release store of `write_index_`
orders the payload write before the consumer's read. Changing that one store
to relaxed produces the same report. That variant is left as an exercise; the
MPMC probe carries the recorded evidence.
