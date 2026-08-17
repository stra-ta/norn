# Benchmarking

M1 provides a single-thread bounded mutex-queue push/pop baseline.
It is a reference measurement, not an optimization target.

M3 adds a two-thread SPSC batch benchmark for the unpadded and cache-line-separated index layouts.
The batch amortizes thread creation, but the current harness still includes thread creation and join in each measured iteration.
That limitation is intentional for the first comparison and will be addressed before making performance claims.
Queue construction, destruction, retry loops, and scheduler yields are also included in the current timed workload.
Results are therefore useful for a first relative comparison, not a final cycles-per-operation claim.

Hypothesis: separating the producer-owned and consumer-owned indices may reduce cache-line invalidation when the two threads run on different cores.
The result is hardware- and topology-dependent, so padding is retained only if the measurement supports it.

`tools/run_benchmark.py` wraps Google Benchmark JSON with git, compiler, operating-system, architecture, CPU, build, capacity, and producer/consumer metadata.
Google Benchmark supports JSON/CSV formats, repetitions, and warmup controls.

Future measurements will record machine, compiler, flags, topology, configuration, git revision, warmup policy, repetitions, and raw machine-readable output.
Performance claims will be tied to a hypothesis and measured on explicitly identified hardware.
