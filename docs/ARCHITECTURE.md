# Architecture

Norn is intentionally developed in milestones.

M0 established reproducible build and verification infrastructure without committing to a public concurrent-queue API.

M1 adds mutex-backed unbounded and bounded queues.
They are intentionally straightforward reference implementations for correctness comparisons and benchmark baselines.
Their shutdown contract is: `close()` rejects future pushes, wakes blocked operations, and allows consumers to drain accepted values.

M2 and M3 add a bounded SPSC ring buffer and its performance analysis, including a
padded index variant separated per cache line and a two-thread batch benchmark for both
layouts.

M4 adds a bounded MPMC queue using the Vyukov sequence-number ring scheme: producers and
consumers claim slots with CAS, and each slot's sequence number publishes the payload
and slot reuse with release stores.
It uses weak, non-blocking semantics and has no `close()`; its design, linearizability
properties, and progress guarantees are documented in `docs/MPMC_DESIGN.md`.

The planned scope is a mutex reference queue, bounded SPSC queue, bounded MPMC queue, and one carefully documented memory-reclamation implementation.
Networking, schedulers, coroutine runtimes, and general-purpose threading abstractions are out of scope.
