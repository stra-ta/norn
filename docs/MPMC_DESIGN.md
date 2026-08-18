# MPMC Queue Design (M4)

## Scope

M4 adds a bounded, multi-producer, multi-consumer queue based on the Vyukov bounded ring scheme.
The design is sequence numbers plus CAS-based slot reservation on a fixed ring.
This document records the design before implementation.

## API

`norn::mpmc_queue<T, Capacity>` is a header-only template in `include/norn/mpmc_queue.hpp`.

- `emplace(Args&&...) -> bool` constructs a value in place and returns whether it was accepted.
- `try_push(const T&)` and `try_push(T&&)` forward to `emplace`.
- `try_pop(T&) -> bool` and `try_pop() -> std::optional<T>` remove one value.
- `empty()` is a reservation-gap snapshot with the semantics described below, not an availability check.
- `capacity()` is a static constant.
- The queue is neither copyable nor movable, matching the other Norn queues.
- `Capacity` must be at least 2.

`Capacity` must be at least 2 because the sequence-number states collide when `Capacity == 1`.
The filled state for a slot is `sequence == p + 1`, and the empty state seen by the next
producer pass over the same physical slot is `sequence == p + Capacity`.
Those two values are equal exactly when `Capacity == 1`, so a producer could overwrite a
live object before the consumer released it.
The constraint is enforced with a static assert.

`Capacity` need not be a power of two.
The physical index is logical position modulo `Capacity`.

The queue uses weak, non-blocking semantics: a failed attempt returns false instead of
spinning.
This matches the `try_*` API of the SPSC queue.
The consequences for linearizability and progress are documented below.

There is no `close()` in the initial M4 scope: draining with multiple producers requires
tracking whether any producer is still in flight, which needs epoch-style accounting.
That is deferred and documented as a limitation.

Dequeue order follows reservation order on the ring, not invocation order.
Two producers that both call `try_push` may reserve in either order, so the queue is not
FIFO across producers.
A single producer sees its own pushes consumed in order, because its reservations are
strictly increasing.

## Slot state and sequence numbers

The ring has `Capacity` slots.
Each slot holds a `std::atomic<std::size_t>` sequence number plus storage for one `T`.
Slot `i` is initialized with `sequence[i] == i`, so the first producer pass sees every
slot in its empty state.
`enqueue_pos` and `dequeue_pos` start at zero.

A slot's sequence number records the logical position of the operation that most recently
used the slot, not a value that wraps with the ring.
Sequence numbers grow monotonically for the lifetime of the queue.

Slot at physical index `i` is the home of logical positions `i`, `i + Capacity`,
`i + 2 * Capacity`, and so on.
Let `p` be the producer's current claim position and `c` the consumer's current claim position.

- `sequence == p` means the slot is empty and available to the producer claiming `p`.
- `sequence == p + 1` means the slot is filled, published, and available to the consumer claiming `p`.
- `sequence < p` means the slot is behind the producer: from the producer's view the queue is full at this position.
- `sequence > p + 1` means the slot is ahead of the producer: the producer's view is stale.

The consumer compares against `c + 1` instead of `p`.
The producer compares against `p` instead of `p + 1`.

## Cell storage and object lifetime

Each slot stores `T` in `alignas(T) std::byte` storage, following the SPSC pattern.
The producer placement-news `T` into the slot after its reservation and the consumer
destroys the moved-from residue before releasing the slot.
All access to the stored object goes through `std::launder`, because the storage is
reused across multiple object lifetimes.
Teardown destroys any live, unclaimed objects; it requires quiescence, matching the SPSC
destructor contract.

## Wraparound and the sequence comparison

Sequence numbers are compared with the modular unsigned difference, not with direct signed casts.
The code computes `diff = sequence - pos` in unsigned arithmetic (modulo `2^w`, where `w` is
the width of `std::size_t`), then interprets the result as an offset in the circular order:

- `diff == 0` means the slot is in the expected state for this position.
- `diff` at or above the midpoint of the range, `diff >= 2^(w-1)`, means the slot is behind this position.
- `diff` below the midpoint means the slot is ahead of this position.

This avoids implementation-defined signed conversions entirely.
The legitimate differences in a live queue are bounded by roughly `Capacity` plus the number
of threads in flight, so they are always far below `2^(w-1)`.
The constraint is that `Capacity` stays below `2^(w-1)`; on a 64-bit `std::size_t` that is 2^63.

The physical index uses unsigned modulo, so index wrap is harmless as well.

## Producer reservation

The producer loads `enqueue_pos` with relaxed order and probes the slot at that position.
It acquires the slot's sequence number.

If the sequence equals the position, the producer tries a relaxed `compare_exchange_strong`
that advances `enqueue_pos` by one.
The CAS winner has exclusively reserved the slot.
A losing producer reloads the position and retries.

Strong CAS is specified deliberately: a strong CAS fails only when the counter value has
actually changed, which means another thread advanced it.
Weak CAS may fail spuriously without any counter movement, which would break the
"every failed CAS coincides with another thread's progress" reasoning below.

If the sequence is behind the position, the producer's slot is not available in this pass
and the push returns false.
If the sequence is ahead, the producer's view is stale and it reloads the position.

The winner constructs `T` in the slot and then publishes the item with a release store
of `sequence = pos + 1`.
The item becomes observable only at that store.

## Consumer reservation

The consumer is symmetric.
It loads `dequeue_pos` with relaxed order, acquires the slot's sequence number, and
compares it against `pos + 1`.

On a match it claims the slot with a relaxed `compare_exchange_strong` that advances
`dequeue_pos`.
It moves the value out, destroys the residue, and releases the slot with
`sequence = pos + Capacity`.
That store makes the slot available to the producer that will next claim logical position
`pos + Capacity`.

If the sequence is behind, the consumer's slot has no item in this pass and the pop
returns false.
If it is ahead, the consumer's view is stale and it reloads.

## Linearization points

A successful enqueue linearizes at its successful CAS on `enqueue_pos`.
A successful dequeue linearizes at its successful CAS on `dequeue_pos`.

The failed operations need a precise statement.
Under quiescence, meaning no thread is inside the window between its reservation CAS and
its publication store (whether paused or merely slow), a behind sequence number at the
probe is an accurate full or empty signal, and the failed operation linearizes at its
probe.
While any producer is inside that window, the weak semantics can report false-empty or
false-full: a consumer probing a slot whose producer has reserved but not yet published
sees the slot as empty even though other producers may have published items elsewhere in
the ring.
This is a real linearizability gap of the weak variant, not a theoretical edge.
The operation itself is never blocked waiting on another thread; it either returns false
or retries, which is the progress property described below.

The tradeoff is explicit: weak semantics keep every operation non-blocking and
immediately returning, at the cost of potentially incorrect empty and full results
while a thread is inside the reservation window.
The alternative, spinning until the stalled slot resolves, restores accuracy but makes
the operation block on the paused thread.
Norn chooses the weak variant to match the `try_*` API, and documents the gap.

## CAS loops

Each operation is a loop: load position, probe slot, decide, attempt CAS.
Because the CAS is strong, a failed CAS means another thread advanced the counter, so
each retry coincides with system-wide progress.
Retries are O(1) in expectation.
There is no hard upper bound on retries under adversarial scheduling; a thread that keeps
losing races can retry indefinitely while other threads keep progressing.

Producers contend on `enqueue_pos`; consumers contend on `dequeue_pos`.
That contention is intrinsic to a bounded MPMC design and cannot be padded away.

## Memory orders

The slot sequence number is the synchronization backbone.
Loads use acquire and stores use release.

The CAS operations on `enqueue_pos` and `dequeue_pos` are strong with relaxed order.
Relaxed is sufficient because the counters only need atomic monotonic advancement and
CAS uniqueness, while the data happens-before is established by the slot sequence
release/acquire pairing.
The failure memory order of the CAS is also relaxed, since a failed attempt only
triggers a re-probe.

`empty()` reads `dequeue_pos` with relaxed order and `enqueue_pos` with relaxed order.
It reports whether the reservation counters are equal.
Because `enqueue_pos` advances at reservation time rather than publication time, the
counters can differ while no item is actually available to pop: a producer paused between
reservation and publication makes `empty()` report non-empty with nothing pop-able.
`empty()` is therefore a reservation-gap snapshot, not an availability or drain check.

## Progress guarantee

The queue is mutex-free and its API is non-blocking: the `try_*` operations never wait
on a lock or condition variable and always return.
It is not formally lock-free, and it is not wait-free.

The formal definition of lock-free requires that infinitely often some method call
finishes in a finite number of steps.
This queue does not satisfy that definition, for two reasons.
A thread can starve in the reservation CAS loop: it may lose races indefinitely while
other threads keep winning, so a single call has no completion bound.
More importantly, a preempted thread that holds a reserved-but-unpublished slot blocks
the queue's logical progress at that position: consumers report false-empty, producers
report false-full, and up to `Capacity - 1` published items stay unreachable until the
thread resumes.
During that window the weak semantics also return results that violate linearizability,
so the affected operations are not merely slow but observably incorrect.

What the queue does guarantee: every `try_*` call returns without blocking, every
successful call linearizes at its reservation CAS, and a thread that is not starved by
other threads completes its operation.
The uncontended single-producer, single-consumer case degrades to a single probe per
operation.

The atomics used by the queue are lock-free on the platforms this project targets;
`std::atomic<std::size_t>::is_always_lock_free` is enforced with a static assert.
That property concerns the hardware primitives, not the progress guarantee of the
queue algorithm itself.

## Paused mid-operation

A producer that wins the reservation CAS and is descheduled before its publish store
leaves the slot reserved but unpublished.
Consumers at that position see a behind sequence number and report empty.
Producers behind that position report full once the rest of the ring is reserved.
Up to `Capacity - 1` published items behind the hole become unreachable until the paused
producer resumes, and every operation against the stalled frontier returns false in the
meantime.

The stall is bounded in space (one hole plus up to `Capacity - 1` stranded items) but
unbounded in time.
It is the direct consequence of the weak linearizability gap: the queue state is
temporarily inconsistent, not corrupted.
When the paused thread resumes and publishes, the hole closes and all stranded items
become reachable in order.

A consumer paused after claiming but before releasing the slot has a different effect.
Other consumers can continue past it, because they probe their own positions.
Producers report full for the held slot, which is accurate: the slot genuinely is not
reusable until the consumer releases it.

These are the fundamental limitations of the scheme and are documented rather than hidden.

## Exception safety

Construction happens after the producer's reservation, and the move-out happens after
the consumer's reservation.
A throw at either point would strand the slot: the slot is claimed, the counter has
advanced past it, and no other thread can ever complete it.

Therefore the queue requires:

- the constructor used by `emplace` to be noexcept, enforced with a static assert;
- `T` to be nothrow-move-constructible for the `std::optional` pop path;
- `T` to be nothrow-move-assignable for the out-parameter pop path; and
- `T`'s destructor to be noexcept, which is the default for well-formed types.

The destructor of the queue itself must not run concurrently with any producer or
consumer operation, matching the SPSC contract.

This contrasts with the SPSC queue, where construction precedes publication and the
consumer moves out before advancing its own index, so a throw leaves a retryable state.
MPMC cannot do that because the CAS claim must precede the payload operation to
guarantee that exactly one thread takes the slot.
A throwing payload operation after a shared claim is unrecoverable, so the operations
are constrained to not throw.

## Data layout and padding

`enqueue_pos` and `dequeue_pos` sit on separate cache lines so producers and consumers
do not false-share the two counters.
The alignment uses `std::hardware_destructive_interference_size` with the same 64-byte
fallback as the SPSC queue.
Separating the counters removes false sharing between the two counter groups, but it
cannot remove the intrinsic CAS contention within each group.

Each cell keeps its sequence number adjacent to its data, matching the classic layout.
Adjacent-slot false sharing between producer and consumer is accepted for the dense
variant.
A padded-cell variant is deferred until a measurement motivates it, following the M3
policy of retaining padding only when the benchmark supports it.

## Planned verification

Tests will cover capacity boundaries, wraparound, single and multiple producer and
consumer configurations, move-only values, and full and empty edges.
ASan, UBSan, and TSan runs will follow the existing preset structure.

The exactly-once oracle needs an explicit protocol because the queue has no `close()`.
Producers push a known closed set of value IDs, retrying on false returns until every ID
is accepted.
After all producers join, every accepted value is published, because a true return from
`try_push` happens only after the publish store.
Consumers then pop until the per-ID consumption count reaches the total, and any
duplicate or missing ID fails the test.
Termination never relies on `empty()` or on a single failed pop, since both can be wrong
under the weak semantics.

Two deterministic tests exercise the paths that ordinary stress leaves to chance.
A gated-construction test uses a value type whose constructor blocks until released: one
producer enters the reservation window and stalls, other producers and consumers operate
around it, and the test verifies the false-empty and false-full behavior plus in-order
drain of the stranded items after the gate opens.
The capacity is 2 and the thread count exceeds the capacity so the full path is forced.
A modular-comparison unit test drives the sequence comparison helper with values near
`SIZE_MAX` to verify the half-range threshold logic without running 2^64 operations.

A single-producer, single-consumer test verifies that one producer's pushes are consumed
in order.

The implementation enforces its preconditions with static asserts: `Capacity >= 2`,
`Capacity` below half the `size_t` range, lock-free position and sequence atomics,
noexcept construction for `emplace`, nothrow moves for the pop paths, and a noexcept
destructor for `T`.

A throughput benchmark matrix (1x1, 2x2, 4x4) will use the existing benchmark tooling,
with producer and consumer counts recorded in the metadata.
The existing `benchmark_configurations` family map supports MPMC entries without schema
rework, one key per configuration with explicit counts.
Real GNU GCC verification will be repeated on the disposable Linux VM.

## Decisions to confirm

- Accept the weak, non-blocking semantics with their documented linearizability gap.
- Accept the `Capacity >= 2` constraint.
- Accept the noexcept-construction, nothrow-move, and nothrow-destruction constraints as the honest cost of the scheme.
- Accept that the queue is mutex-free and non-blocking but not formally lock-free or wait-free.
- Defer `close()` and drain termination until a later milestone.
- Defer padded cells until a measurement motivates them.
- Keep arbitrary capacity via modulo instead of requiring a power of two.