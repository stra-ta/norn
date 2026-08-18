# Memory Model

M1 structures use a mutex and condition variables rather than custom atomics.
The mutex protects queue contents and the closed flag.
Condition-variable waits always re-check their predicates after wakeup.

This is a blocking reference implementation, not a lock-free design.

## SPSC queue

The producer exclusively modifies its logical write index.
The consumer exclusively modifies its logical read index.

The producer acquires the consumer's read index before deciding whether the ring is full.
After constructing an element, it publishes the new write index with release semantics.
The consumer acquires that write index before reading the element.

The consumer destroys the element before publishing its new read index with release semantics.
The producer's acquire load of that index therefore prevents it from reusing a slot before the consumer has finished with the old object.

The producer and consumer local-index loads are relaxed because each index has one owning thread.
The cross-thread loads are acquire operations because they observe slot availability and lifetime publication.

The queue requires exactly one producer and exactly one consumer for concurrent operations.
Only the producer writes `write_index_`, and only the consumer writes `read_index_`.
The destructor must not run concurrently with either operation.

For a logical slot index `i`, whose physical storage location is `i % Capacity`, the invariant is:

- `read_index_ <= i < write_index_` means the slot contains a live, published `T`.
- `i < read_index_` means the previous object has been destroyed and the slot is reusable after the producer observes the release store.
- `i >= write_index_` means the slot has no live object for the current traversal.

Successful `emplace` linearizes at the release store publishing `write_index_`.
A full `emplace` linearizes at its failed capacity check.
Successful `try_pop` linearizes at the release store publishing `read_index_` after destruction.
An empty `try_pop` linearizes at its failed equality check.

`empty()` is a diagnostic snapshot, not a synchronization or reservation operation.
When called concurrently it may become stale immediately; callers must use `try_push` and `try_pop` for decisions.

## Cache-line separation

The producer and consumer run on different cores in the concurrent workload, so their private state should not share a cache line.
If both indices sat on one line, every producer update would invalidate the line on the consumer's core and vice versa, forcing repeated refetches.
This is the false-sharing hypothesis: separating the two indices may measurably reduce cache-line invalidation.

The unpadded layout places `read_index_` and `write_index_` adjacently, so they are expected to share a line.
The padded variant is `spsc_queue<T, Capacity, true>` (`spsc_queue_padded`), whose index fields are each aligned to `std::hardware_destructive_interference_size` (with a 64-byte fallback when the feature-test macro is unavailable).
Each index therefore occupies its own cache line, and each core's writes stay on a line that only its owning thread touches.

Alignment does not change the memory-order protocol: both variants publish indices with the same release stores and observe them with the same acquire loads.
Padding only changes the physical placement of the atomic state.
The effectiveness of separation is hardware- and topology-dependent, so the padded layout is retained only if the measurement supports it.

## MPMC queue

The bounded MPMC queue is a Vyukov-style ring of sequence-numbered slots.
The full design, including the reservation protocol, slot state machine, wraparound comparison, and documented limitations, is in `docs/MPMC_DESIGN.md`.
This section records the memory-order reasoning.

Producers claim a slot by winning a relaxed CAS on `enqueue_pos_`.
Consumers claim a slot by winning a relaxed CAS on `dequeue_pos_`.
The counters only need atomic monotonic advancement and CAS uniqueness, so relaxed order suffices for them.

The slot sequence number is the synchronization backbone.
A producer constructs the payload and then publishes it with a release store of `sequence = pos + 1`.
A consumer acquires that value before reading the payload, so the release/acquire pair establishes the happens-before from construction to consumption.

The consumer destroys the moved-from residue and then releases the slot with a release store of `sequence = pos + Capacity`.
A producer's acquire load of that value happens-before its next write to the slot, so the slot is never reused before the consumer is done with the old object.

Successful operations linearize at their reservation CAS.
Failed operations linearize at their probe only under quiescence, meaning no thread is inside the window between its reservation CAS and its publication store.
While a producer is inside that window, consumers can observe false-empty and producers false-full: the weak semantics of this queue trade exact empty and full answers for non-blocking operations.

`empty()` is a reservation-gap snapshot, not an availability check.
The counters can differ while no item is pop-able because `enqueue_pos_` advances at reservation time rather than publication time.
Callers must not use it to infer drain completion.

The queue is lock-free at the system level where the position and sequence atomics are lock-free, which the implementation enforces with a static assert.
It is not wait-free: under contention a thread may retry its reservation CAS without an upper bound.
A thread paused inside the reservation-to-publication window stalls the queue at its position, stranding up to `Capacity - 1` published items, until it resumes.

This document will record, for each structure, atomic state, ownership, publication mechanisms, happens-before relationships, linearization points, and invariants.
Memory orders will be justified by the synchronization relationship they establish, not selected by convention.
