# Hardware-Aware Performance Campaign

This campaign explains the existing queues with measured scheduling, placement, retry, and memory-order evidence.
It does not redesign the queues or treat a benchmark as a correctness test.

## Scope and artifact policy

The campaign keeps the historical Google Benchmark binaries unchanged.
`norn_hardware_benchmarks` is a separate steady-state executable because the historical benchmarks include queue construction, thread creation, and joining in the timed iteration.
Raw JSON is written atomically below ignored `results/hardware/<environment>/` paths.
This avoids committing large traces or host-specific detail.
The compact, curated pilot summary is [`docs/evidence/hardware/2026-08-22-pilot/summary.json`](evidence/hardware/2026-08-22-pilot/summary.json).

The pilot started from clean commit `6f4583454b23b80661a8cb51487903e86e939c04`.
The baseline freeze was commit `64851af4a2a1667937e3a1cf1b526d0e66ce72dd`.
At the freeze, default, Release, Debug, ASan, UBSan, and TSan built and passed all 33 existing tests on the native M1.
The legacy benchmark run was recorded under `results/hardware/baseline-64851af4/`.
Its load average was high, so it is a harness check rather than publishable evidence.

## Method

Each hardware case creates workers before timing.
Each repetition constructs a fresh queue, releases persistent workers through a command epoch, waits for complete producer and consumer counts, and then destroys the queue after all workers finish.
Queue construction, thread creation, affinity setup, and JSON generation are outside the timed region.

The manifest at [`tools/hardware_campaign_manifest.json`](../tools/hardware_campaign_manifest.json) defines the questions and controls.
The default pilot uses two warmups and nine measured repetitions with 100,000 items per producer.
Case order is randomized from a recorded seed.
Every run records the exact command, commit, dirty state, compiler, OS, kernel, CPU count, topology, placement read-back, process CPU time, voluntary and involuntary context switches, per-sample retries, yields, spin steps, fairness, throughput, and per-item elapsed cost.

The runner reports a median, mean, population standard deviation, minimum, and maximum without discarding samples.
It marks failed operation totals or failed required affinity read-back as invalid.
Performance results never participate in CTest pass or fail criteria.

Run a native Release campaign:

```sh
cmake --preset release
cmake --build --preset release --target norn_hardware_benchmarks
python3 tools/run_hardware_campaign.py \
  --binary build/release/norn_hardware_benchmarks \
  --output results/hardware/m1/<run-id>.json
```

Add `--perf` on Linux to probe each requested event before collecting it.
If an event is unsupported or permission-denied, the JSON records that fact and still retains the throughput run.
No command changes `perf_event_paranoid`, capabilities, scheduler policy, or host configuration.

## Affinity and topology

On Linux, every worker uses `pthread_setaffinity_np` and immediately reads its effective mask back with `pthread_getaffinity_np`.
A requested placement is valid only when the effective singleton mask matches the requested CPU.
The runner obtains permitted CPUs from `sched_getaffinity(0)` and topology from `/sys/devices/system/cpu/cpu*/topology/`.
It can select same-logical-CPU, SMT-sibling, distinct-core, and cross-package pairs only when the permitted topology demonstrates that relation.

On macOS, the campaign records `sysctl` CPU and cache-line metadata but reports affinity as unsupported.
macOS scheduler hints are not treated as hard CPU affinity.
Therefore, M1 cases are correctly labeled scheduler-managed and unpinned.

The Lima guest exposed four distinct vCPUs in one package with no SMT siblings and no cross-package pair.
It is explicitly labeled virtualized.
Its distinct-core pinning verifies Linux scheduling controls but cannot prove M1 physical cache, package, or NUMA effects.

## Pilot results

The table shows medians in millions of items per second.
All values use the new steady-state harness, so they must not be compared directly with the legacy README table.

| Case | Native M1 | Linux ARM64 VM | Interpretation |
| --- | ---: | ---: | --- |
| SPSC unpadded, unpinned | 31.32 | 22.28 | Both are scheduler-managed baseline cases. |
| SPSC padded, unpinned | 60.41 | 12.67 | M1 pilot favors padding, but its wide spread and host load make the size exploratory. The VM does not reproduce the direction. |
| SPSC padded, same logical CPU | N/A | 0.17 | Time-sharing both roles on one vCPU is dramatically slower. This is scheduler evidence, not a cache-line comparison. |
| SPSC padded, distinct cores | N/A | 18.54 | Pinning can change the VM result. Virtual topology prevents a physical-cache conclusion. |
| SPSC seq_cst, unpinned | 22.78 | 25.12 | The two environments disagree on direction, so neither establishes a portable ordering-cost claim. |
| MPMC 1x1 | 133.99 | 135.10 | Both pilots show a high single-pair baseline. |
| MPMC 2x2 | 21.65 | 18.16 | Both pilots show a substantial contention cliff. |
| MPMC 4x4, tight retry | 6.47 | 5.55 | Additional contenders reduce throughput further. |
| MPMC 4x4, yield | 9.60 | 26.99 | Yielding can improve this loaded pilot, but it is not a universal winner. |
| MPMC 4x4, bounded retry | 9.80 | 16.50 | The VM result is strong but virtualized and variable. |
| MPMC 4x4, exponential retry | 11.57 | 17.38 | Same conclusion as bounded retry. |

The native M1 pilot ran at a high host load and has broad sample spreads, including the padded SPSC case.
The only supported native conclusion is that this workload had a repeatable-looking direction worth rerunning under controlled load.
The Linux VM supports the control-path conclusion that affinity read-back and topology skip logic work.
It does not support a claim about bare-metal ARM64 cache hierarchy or exact CPU placement costs.

## Contention, retries, and backoff

The hardware benchmark counts failed queue attempts around `try_push` and `try_pop`.
For MPMC, this measures retry-loop pressure seen by the caller.
It is not a direct count of failed reservation CAS operations inside `mpmc_queue`, because adding that instrumentation to the production queue would change the path being measured.

The runner also records process CPU time and `getrusage` context switches.
Those values can show a throughput-versus-CPU-efficiency tradeoff between tight spinning and yielding.
They cannot assign cost precisely to useful instructions, CAS retries, cache misses, or scheduler migration.
That decomposition requires valid counters or a separate, controlled probe.

The backoff modes live only in the benchmark loop.
`tight` retries immediately.
`yield` calls `std::this_thread::yield()` after every failed attempt.
`bounded` yields after 64 consecutive failures.
`exponential` uses an increasing compiler-fence spin interval capped at 64 steps, then yields.
No queue API or queue synchronization rule changes between these cases.

## Memory-order experiment

`spsc_queue_seq_cst<T, Capacity, Padded>` is an isolated stronger-order alias.
The default `spsc_queue` remains acquire/release.
The producer constructs the payload before the write-index publication store.
The consumer acquire-loads that write index before reading the payload.
The consumer destroys the payload before the read-index publication store, and the producer acquire-loads that index before reusing the slot.

Replacing those acquire and release operations with `seq_cst` strengthens, rather than weakens, the required happens-before edges and leaves the linearization points unchanged.
Owner-local index loads remain relaxed because no other thread writes that index.
The experiment has basic, FIFO, two-thread transfer, and sanitizer coverage.
There is no relaxed experimental variant in the normal benchmark targets.

## Perf and counter availability

The requested compact `perf stat` event set is cycles, instructions, branches, branch misses, cache references, cache misses, context switches, CPU migrations, and task clock.
Native macOS has no `perf` tool in this campaign.
The Ubuntu ARM64 VM has `perf`, but all requested events are blocked by `perf_event_paranoid=4`.
The pilot therefore reports each counter unavailable instead of inventing cache-miss or instruction-cost explanations.

When a qualifying Linux host permits counters, compare only interleaved control and treatment cases with the same build, affinity, batch size, and repetition policy.
Treat `perf stat` as aggregate process evidence.
It does not prove that one source line caused a counter difference.

## Architecture comparison and limits

Native M1 results are real M1 hardware measurements with scheduler-managed placement.
Linux ARM64 results are virtualized measurements.
GitHub-hosted x86-64 CI runs only a functional campaign smoke test and must not be treated as a performance dataset.
No x86-64 performance comparison is published yet.

The shared behavioral shape in this pilot is MPMC degradation from 1x1 to 2x2 and 4x4.
The padding and seq_cst directions do not agree across the native M1 and VM samples.
That disagreement is evidence against a universal performance claim, not a reason to choose the preferred result.

## Next evidence needed

- Repeat the native M1 SPSC and backoff cases under low host load with more randomized interleaving.
- Run the Linux manifest on bare metal with accessible perf events before attributing differences to cache misses or migrations.
- Add an explicitly labeled x86-64 bare-metal or dedicated-host campaign.
- Keep VM and hosted-CI output as functional and scheduler-control evidence only.
