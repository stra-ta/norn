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
