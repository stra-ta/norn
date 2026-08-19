# Norn: Architecture and Performance

## Project scope

Norn is a set of C++20 concurrency structures built incrementally to teach
memory-model correctness, measurable performance, and documented tradeoffs.
It covers mutex reference queues, bounded SPSC and MPMC ring buffers, and
hazard-pointer reclamation for an unbounded node-based queue.
Each milestone is verified independently and documented with its design rationale.

## Structure overview

| Structure | Bounded | Concurrency | Allocation | Reclamation | File |
| --- | --- | --- | --- | --- | --- |
| `mutex_queue<T>` | no | MP | unbounded | N/A (std::deque) | `mutex_queue.hpp` |
| `bounded_mutex_queue<T, N>` | yes | MP | pre-allocated ring | N/A | `mutex_queue.hpp` |
| `spsc_queue<T, N>` | yes | SPSC | pre-allocated ring | N/A | `spsc_queue.hpp` |
| `spsc_queue_padded<T, N>` | yes | SPSC | pre-allocated ring | N/A | `spsc_queue.hpp` |
| `mpmc_queue<T, N>` | yes | MPMC | pre-allocated ring | N/A | `mpmc_queue.hpp` |
| `mpsc_queue<T>` (HP) | no | MPSC | heap per push | hazard pointers | `hazard_pointer.hpp` |

## Memory ordering

Every structure in Norn has its memory orders documented and tested.

The SPSC and MPMC ring buffers use release stores and acquire loads for index
publication, with relaxed CAS on the position counters.
The Vyukov MPMC queue is mutex-free and non-blocking, but not formally lock-free:
a stalled thread in the reservation window can block logical progress through
the queue (false-empty from a stalled producer, false-full from a stalled consumer).

The hazard-pointer library uses the classic publication protocol:
release-store into the hazard slot, a seq_cst fence, then acquire-load of the
source for re-validation.
The scanner performs its own seq_cst fence before collecting hazard slots, forming
the paired-fence ordering that prevents premature reclamation under the C++
memory model.

## Performance

The bounded ring buffers outperform the hazard-pointer queue because they
pre-allocate their storage and avoid per-operation heap allocation and fence
overhead.
The padded SPSC variant is slightly faster than the unpadded version when the
two threads run on different cores, consistent with the false-sharing hypothesis.

The hazard-pointer queue is roughly twice as slow as the SPSC ring buffer for
100k-item batches on the local test machine.
This is the expected cost of node allocation, hazard publication, and the
seq_cst fences on every pop.
The comparison is educational; the absolute numbers are hardware- and
topology-dependent.

## Design decisions

**Bounded vs unbounded**: the ring buffers are bounded because the primary
learning goal is the memory-order protocol, not the reclamation problem.
Hazard pointers are introduced specifically to demonstrate the reclamation
problem that bounded queues avoid.

**Weak semantics in MPMC**: the Vyukov queue returns false on full/empty instead
of spinning, matching the `try_*` API of the SPSC queue.
This is documented as a deliberate tradeoff: non-blocking operations at the
cost of temporarily incorrect empty and full results under mid-operation stalls.

**Michael-Scott as the HP vehicle**: the Michael-Scott queue is the canonical
structure that motivates hazard pointers, with dual-hazard pop protection
(head and head->next) and tail-lag helping.
It demonstrates the full reclamation lifecycle: allocation, hazard publication,
retirement, scan, and reclamation.

## Correctness

The concurrency correctness campaign (M5) established:
- Exactly-once consumption under parameterized histories for the MPMC queue.
- Single-producer, single-consumer FIFO order.
- Deterministic demonstration of the reservation-hole progress limitation.
- Memory-order sensitivity via a weakened-order probe producing TSan races.
- Close-and-drain semantics for both mutex queue families.

The hazard-pointer library was verified with ASan, UBSan, and TSan across
two platforms (macOS ARM64, Linux ARM64 GCC), with the tail-lag bug in the
Michael-Scott queue found and fixed during implementation.

## Limitations and future work

- The HP queue is MPSC; an MPMC node-based queue with hazard pointers would
  require a more complex scan or a different reclamation scheme.
- Epoch-based reclamation is documented as an alternative for future work.
- The benchmark comparison includes thread creation in the timed workload;
  a production harness would separate setup from the critical path.
- x86-64 GCC verification relies on CI; local ARM64 verification is the
  primary evidence.
- Performance claims are pending a dedicated hardware measurement on bare metal.