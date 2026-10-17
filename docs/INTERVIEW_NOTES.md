# Interview Notes

This notebook collects questions and answers grounded in Norn's kept
implementation: the two rings, the mutex reference queues, and the
weakened-order experiment.

## Design questions

- Why keep reference implementations before lock-free structures?
  The mutex queues make the contract observable (FIFO order, capacity
  boundaries, close-and-drain) before the atomic rings introduce weak
  semantics. They are also the baseline any future measurement would compare
  against.

- Which properties can sanitizers verify, and which require reasoning about
  the C++ memory model?
  Sanitizers verify tested executions: TSan detects conflicting unsynchronized
  accesses it observes; ASan and UBSan catch memory and undefined-behavior
  violations in those same runs. Nothing proves linearizability, progress
  guarantees, ABA safety, or absence of starvation. Those come from the
  source-level reasoning recorded in docs/MEMORY_MODEL.md and
  docs/MPMC_DESIGN.md.

- What is the false-sharing hypothesis, and how was it tested?
  Both SPSC roles write their own index but acquire-load the peer's index. If
  the indices share a cache line, publication invalidates the line the peer is
  polling. The padded type separates them by the configured cache-line size.
  The historical M1 run favored padding; the effect is
  hardware- and topology-dependent, so the claim stays labeled to that run.

- What are the consequences of using weak (non-blocking) semantics in the
  bounded MPMC ring?
  Every `try_*` call returns immediately even when another thread holds a
  slot mid-operation. While a producer sits between its reservation CAS and
  its publication store, consumers observe false-empty at that position; a
  consumer held in its claim-to-release window makes producers observe
  false-full. The operations are non-blocking but temporarily observably
  wrong, and a preempted thread can strand up to `Capacity - 1` published
  items.

- How does the reservation-CAS linearization affect what "full" and "empty"
  mean?
  A successful enqueue linearizes at its reservation CAS, so a reserved slot
  already counts as enqueued. The failed pushes at the reserved frontier are
  therefore genuinely full, not false-full; false-full only comes from a
  stalled consumer. `empty()` is a reservation-gap snapshot of the two
  counters, never an availability check.

- What does a randomized correctness campaign prove, and what does it not
  prove?
  The fixed manifest histories prove exactly-once consumption and single-
  producer FIFO on the runs they execute. They cannot prove correctness for
  untested interleavings; the linearization argument in
  docs/MPMC_DESIGN.md carries that proof, with its documented quiescence
  condition for failed operations.

## The weakened-order experiment

- How do you test memory-order sensitivity without rewriting the
  implementation?
  `experiment/weakened_order_probe.cpp` compiles the real `mpmc_ring` twice
  from one source. The weakened target defines
  `NORN_EXPERIMENT_WEAKENED_PUBLISH`, which changes exactly one memory order:
  the producer's payload publication store drops from release to relaxed.
  Under TSan the ordered control must run clean and the weakened target must
  report the payload data race.

- Why does weakening the publication store break the queue?
  The release store is the happens-before edge from the payload write to the
  consumer's acquire load of the sequence number. With a relaxed store the
  consumer's clock never gains the producer's epoch, so every payload read is
  unordered with its write and TSan reports the race; the guard check also
  catches actual corruption.

- What does that prove about the correct ring?
  That the release store is load-bearing: the ordered control transferring
  the same payloads cleanly is the negative control. One differing line
  isolates the cause.

- Why can x86 and ARM have different performance while both remain correct?
  The C++ memory model requires the same synchronization outcome, not
  identical instructions, fence costs, cache behavior, or scheduler policy.
  Correct acquire/release code can map to different hardware sequences and
  still establish the required happens-before edges.

- Which conclusions are portable?
  Queue correctness and the C++ synchronization argument are portable within
  the documented contracts. The false-empty/false-full window shape follows
  from the design, so it is portable as behavior. Any measurement, past or
  future, describes only the environment on its label.
