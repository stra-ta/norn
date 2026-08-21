# Interview Notes

This notebook collects questions and answers grounded in Norn's implementation and measurements.

M0 questions:

- Why keep reference implementations before lock-free structures?
- Which properties can sanitizers verify, and which require reasoning about the C++ memory model?
- Why must benchmark metadata be recorded with results?

M4 questions:

- What is the false-sharing hypothesis, and how was it tested?
- What are the consequences of using weak (non-blocking) semantics in a bounded MPMC queue?
- How does the reservation-CAS linearization affect what "full" and "empty" mean?

M5 questions:

- How do you demonstrate linearizability without a formal history checker?
- What does a randomized correctness campaign prove, and what does it not prove?
- How do you test memory-order sensitivity without modifying the implementation?

M6 questions:

- What problem do hazard pointers solve that bounded queues do not have?
- Why is the seq_cst fence required between hazard publication and source re-validation?
- What happens if a thread is paused after reserving a slot but before publishing the pointer?
- How does the scanner know it is safe to reclaim a retired node?
- Why is the Michael-Scott queue the canonical vehicle for demonstrating hazard pointers?

M7 questions:

- What is the cost of hazard-pointer reclamation compared to pre-allocated ring buffers?
- Why is the HP queue slower than SPSC, and what operations contribute to the overhead?

Hardware campaign questions:

- Why can false sharing hurt an SPSC queue?
  Both roles write their own index but acquire-load the other role's index.
  If the indices share a cache line, publication can invalidate the line the peer is polling.
  The padded type separates them by the configured cache-line size, which can reduce coherence traffic when roles run on separate cores.

- Why might padding help in one run and fail to help in another?
  It only addresses one source of cost.
  Queue capacity, cache hierarchy, physical placement, migration, host load, branch behavior, and the scheduler can dominate it.
  Norn's M1 pilot favored padding, while the virtualized ARM64 pilot did not, so the project documents the difference instead of generalizing it.

- What does CPU affinity change?
  Linux affinity constrains a worker's allowed CPU mask, which can reduce migration and make a placement experiment repeatable.
  It can also create an artificial bottleneck when both SPSC roles share one logical CPU.
  Norn verifies the effective mask after `pthread_setaffinity_np`; a requested mask without read-back is not evidence of placement.

- What can virtualization distort?
  A VM exposes vCPUs and virtual topology, not a trustworthy view of physical cache sharing, package distance, frequency behavior, or host scheduling.
  Norn's Lima ARM64 run is therefore scheduler-control evidence, not a bare-metal cache claim.

- Why does MPMC contention increase retry pressure?
  Producers race on `enqueue_pos_` and consumers race on `dequeue_pos_`.
  The per-slot sequence protocol makes each reservation safe, but more roles repeatedly observe unavailable or stale slot state and retry their `try_push` or `try_pop` loops.
  The campaign counts caller-visible retries without inserting queue-internal counters into the normal fast path.

- Why can throughput improve while CPU efficiency worsens?
  Tight spinning can reduce handoff delay by consuming more CPU time.
  Yielding can reduce wasted polling and improve fairness under oversubscription, but it can also add scheduling delay.
  Norn records both wall time and process CPU time because operations per second alone cannot show that tradeoff.

- Why might acquire/release and seq_cst benchmark similarly on one architecture?
  The compiler and hardware may implement the stronger order with little additional work for this pattern, or another cost may dominate it.
  A null difference is valid evidence for that build and workload only.
  Norn keeps the acquire/release baseline unchanged and benchmarks a separate seq_cst alias that preserves the same publication and consumption relation.

- Why can x86 and ARM have different performance while both remain correct?
  The C++ memory model requires the same synchronization outcome, not identical instructions, fence costs, cache behavior, or scheduler policy.
  Correct acquire/release code can map to different hardware sequences and still establish the required happens-before edges.

- Why is TSan insufficient to prove a lock-free algorithm correct?
  TSan can detect many conflicting unsynchronized accesses in a tested execution.
  It does not prove linearizability, progress, ABA safety, all weak-memory executions, or absence of starvation.
  Norn combines it with queue-specific invariants, stress histories, and documented memory-order reasoning.

- What does `perf stat` actually tell us?
  It reports aggregate counter estimates for a process or command.
  It can support a controlled comparison but cannot assign a counter to a source line or prove causation by itself.
  Norn's ARM64 VM had `perf_event_paranoid=4`, so the campaign records every requested event as unavailable rather than guessing.

- Which benchmark conclusions are portable?
  Queue correctness and the C++ synchronization argument are portable within their documented contracts.
  The MPMC contention shape is a useful hypothesis because both pilot environments show it, but its magnitude is not portable.
  Padding, affinity, backoff, and seq_cst conclusions require environment-qualified evidence.
