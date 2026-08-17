# Architecture

Norn is intentionally developed in milestones.

M0 established reproducible build and verification infrastructure without committing to a public concurrent-queue API.

M1 adds mutex-backed unbounded and bounded queues.
They are intentionally straightforward reference implementations for correctness comparisons and benchmark baselines.
Their shutdown contract is: `close()` rejects future pushes, wakes blocked operations, and allows consumers to drain accepted values.

The planned scope is a mutex reference queue, bounded SPSC queue, bounded MPMC queue, and one carefully documented memory-reclamation implementation.
Networking, schedulers, coroutine runtimes, and general-purpose threading abstractions are out of scope.
