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

Later milestones will document operation contracts, invariants, linearization points, progress guarantees, stress coverage, sanitizer limitations, and history-checking results.
