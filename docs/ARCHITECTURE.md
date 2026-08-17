# Architecture

Norn is intentionally developed in milestones.

M0 establishes reproducible build and verification infrastructure without committing to a public concurrent-queue API.

The planned scope is a mutex reference queue, bounded SPSC queue, bounded MPMC queue, and one carefully documented memory-reclamation implementation.
Networking, schedulers, coroutine runtimes, and general-purpose threading abstractions are out of scope.
