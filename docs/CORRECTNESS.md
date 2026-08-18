# Correctness

M1 verifies mutex-backed queue behavior, including FIFO order, capacity boundaries,
close-and-drain behavior, move-only values, and multi-threaded production and consumption.

M2 verifies SPSC FIFO order, capacity boundaries, repeated wraparound, move-only values,
and a two-thread sequence oracle.

The SPSC producer publishes a constructed object by advancing `write_index_`.
The consumer publishes completed destruction by advancing `read_index_`.
These are the operation publication points for the bounded ring protocol.

The concurrent contract is exactly one producer and one consumer.
The producer owns `write_index_`; the consumer owns `read_index_`.
The destructor requires both threads to have stopped.
Each occupied slot contains exactly one live object, and each object is destroyed exactly once after the consumer has moved it out.

Successful push linearizes when the producer release-publishes the new write index.
Successful pop linearizes when the consumer release-publishes the new read index.
Failed full and empty operations linearize at their capacity and availability checks respectively.

M4 verifies the bounded MPMC queue with capacity boundaries, repeated wraparound,
move-only values, single-producer FIFO order, and exactly-once consumption across
multiple producers and consumers.
The exactly-once drain protocol joins all producers before draining, because a true
`try_push` return happens only after the publish store, and never terminates on
`empty()` or on a single failed pop, both of which can be wrong under the weak
semantics.

A deterministic gated-construction test holds one producer inside the
reservation-to-publication window and verifies the documented false-empty and
false-full behavior plus in-order drain of the stranded items after the gate opens.

A producer claims a slot with a CAS on `enqueue_pos_` and publishes the object with a
release store on the slot sequence.
A consumer claims with a CAS on `dequeue_pos_` and publishes slot reuse with a release
store after destroying the moved-from residue.
The sequence release/acquire pairing is the operation publication mechanism for the
ring protocol.

Successful operations linearize at their reservation CAS.
Failed operations linearize at their probe only under quiescence, meaning no thread is
inside the window between its reservation CAS and its publication store.

Later milestones will document operation contracts, invariants, linearization points, progress guarantees, stress coverage, sanitizer limitations, and history-checking results.
