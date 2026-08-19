# Bug Diary

Only real defects discovered during development belong here.

## M6: Tail-lag false-empty in the Michael-Scott queue

**Observed behavior.** During implementation of the hazard-pointer
Michael-Scott queue, a pop could report the queue empty (return `nullopt`)
immediately after a push had already linked its node.
The failure was intermittent and only appeared under concurrency;
single-threaded tests never reproduced it.

**Wrong assumption.** `head_ == tail_` was treated as "the queue is empty."
Head and tail start on a shared sentinel, and push links the new node in two
steps: CAS `tail_->next` from null to the new node, then CAS `tail_` forward.
Between those two CASes, `head_ == tail_` is true while a real node is already
linked.
A pop arriving in that window concluded the queue was empty and dropped a
value that was, in fact, enqueued.

**Fix.** Emptiness is now `head_ == tail_ && head_->next == nullptr`.
When `head_ == tail_` but `head_->next` is non-null, tail is lagging:
pop helps advance tail (CAS `tail_` from `old_tail` to `next`) and retries
instead of returning empty.
The rule is documented in `docs/HAZARD_POINTER_DESIGN.md` (pop step 7 and the
empty-check section).

**Verification.** Multi-producer single-consumer stress tests assert
exactly-once consumption counts.
All 33 tests pass under ASan, UBSan, and TSan on macOS ARM64 (Apple Clang) and
Linux ARM64 (GNU GCC 13.3).

**Lesson.** A compound invariant is only valid when every component is
observed from the same consistent state.
Pointer-equality checks that hold in a quiescent state can hold transiently
mid-operation, so emptiness must be derived from the link structure, and
intermediate states need helping steps, not early returns.