# Benchmarking

M1 provides a single-thread bounded mutex-queue push/pop baseline.
It is a reference measurement, not an optimization target.

M3 adds a two-thread SPSC batch benchmark for the unpadded and cache-line-separated index layouts.
The batch amortizes thread creation, but the current harness still includes thread creation and join in each measured iteration.
That limitation is intentional for the first comparison and will be addressed before making performance claims.
Queue construction, destruction, retry loops, and scheduler yields are also included in the current timed workload.
Results are therefore useful for a first relative comparison, not a final cycles-per-operation claim.

M4 adds a multi-thread MPMC batch benchmark for 1x1, 2x2, and 4x4 producer/consumer
configurations, with the same per-iteration thread setup cost and the same caveats as
the SPSC benchmark.
Retry-on-full producer loops and retry-on-empty consumer loops are part of the timed
workload, so the numbers compare configurations rather than absolute throughput.

Hypothesis: separating the producer-owned and consumer-owned indices may reduce cache-line invalidation when the two threads run on different cores.
The result is hardware- and topology-dependent, so padding is retained only if the measurement supports it.

`tools/run_benchmark.py` wraps Google Benchmark JSON with git, compiler, operating-system, architecture, CPU, build, capacity, and producer/consumer metadata.
Google Benchmark supports JSON/CSV formats, repetitions, and warmup controls.

M7 adds the hazard-pointer queue to the comparison.
The HP queue is unbounded and allocates individual heap nodes, so it incurs per-node
allocation, hazard publication fences, and scan overhead on every pop.
On the local test machine, the HP queue runs at roughly twice the latency of the
bounded SPSC ring buffer for a 100,000-item batch, which is the expected cost of
the reclamation machinery.
These numbers are not performance claims; they are the first data point for the
educational comparison.

Performance claims will be tied to a hypothesis and measured on explicitly identified hardware.
