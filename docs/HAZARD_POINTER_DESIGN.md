# Hazard-Pointer Reclamation (M6)

## Purpose

M6 adds a hazard-pointer reclamation library to Norn.
Hazard pointers solve a problem that bounded queues do not have: in a node-based
lock-free structure, one thread may hold a pointer to a node while another thread
wants to free it.
Without a safe reclamation scheme, freeing the node causes a use-after-free.

The bounded SPSC and MPMC queues in Norn pre-allocate a fixed array of slots and
never free individual nodes during normal operation; they have no need for a
reclamation scheme.
Hazard pointers are needed for unbounded, node-based structures where nodes are
individually allocated and freed.

This milestone is the project's one documented memory-reclamation implementation.

## What hazard pointers are

A hazard pointer is a per-thread pointer-sized variable published in shared memory.
Before dereferencing a shared pointer, a thread stores the pointer in its hazard
slot, issues a full sequentially-consistent fence, and re-reads the source to
confirm the pointer has not changed (closing the TOCTOU race between publication
and dereference).
When done reading, the thread clears its hazard slot.

To reclaim a retired node, the system scans all threads' hazard slots once and
checks each retired pointer against the collected values.
If the node's address does not appear in any slot, no thread is currently accessing
it and it can be freed.

The correctness of reclamation depends on the reader's protocol, not on the
scanner's algorithm: the seq_cst fence in the reader ensures that if the reader
validated the pointer, the scanner will observe the hazard publication before
deciding to reclaim.

## The ABA problem and how hazard pointers address it

The ABA problem occurs when a lock-free CAS fails to detect a change because the
value was modified and then restored to its original address.
A freed node's memory can be reallocated at the same address, causing a CAS on a
now-stale pointer to succeed.

Hazard pointers prevent reclamation-induced ABA: as long as a thread holds a hazard
protection on a pointer, the node cannot be freed and its address cannot be reused.
However, hazard pointers do not prevent logical ABA where a live node is unlinked
and reinserted by a concurrent operation; that requires higher-level design.

Compared with epoch-based reclamation (EBR), a stalled hazard-pointer participant
pins only its explicitly protected nodes; a stalled EBR participant blocks
reclamation of all later retirements.
Hazard pointers pay per-access publication and O(N * H) scans; EBR pays a per-thread
epoch announcement but amortizes the scan cost.

## Publication protocol (the core correctness argument)

The reader performs these steps, in order:

1. Load the source pointer (acquire).
2. Store the pointer into hazard slot (release).
3. Issue `std::atomic_thread_fence(std::memory_order_seq_cst)`.
4. Re-read the source pointer (acquire).
5. If the re-read value differs from the stored value, clear the slot and retry
   from step 1.

The seq_cst fence in step 3 creates a store-load barrier.
Without it, on x86-64 (TSO), the store buffer can delay visibility of the hazard
publication until after the re-read in step 4.
With the fence, the publication is globally visible before the validation load,
and any scanner that starts after the publication will observe the hazard slot
value.
If the scanner started before the publication, it saw null for this slot, but the
reader's re-validation in step 4 will detect that the source has changed (because
the retiring thread advanced the source pointer before calling retire) and retry
with the new pointer.
This chain of ordering guarantees that a validated pointer is safe to dereference.

## Scan algorithm

The scanner performs these steps:

Phase 0: Issue `std::atomic_thread_fence(std::memory_order_seq_cst)`.
  This fence pairs with the reader's seq_cst fence: together they ensure that if
  the reader has published and validated a pointer, the scanner will observe the
  publication.
  Without this fence, the scanner's acquire-loads could see a stale null value
  even after the reader's release-store is globally visible, because the C++
  memory model does not establish a synchronizes-with relationship between a
  release-store and a subsequent acquire-load that reads the old value.
Phase 1: Under the domain's registration lock, take a snapshot of all registered
  records into a local array. Release the lock immediately after.
  Records that deregister after this point are not in the snapshot, but they
  remain valid for the scan's duration because the domain holds ownership of
  deregistered records until the next scan completes.
Phase 2: For each record in the snapshot and each hazard slot, collect
  h[r][s] = slot.load(acquire).
Phase 3: For each retired pointer p, if p does not appear in any collected h[r][s]
  value, reclaim p.

The reader's seq_cst fence (step 3 of protect) and the scanner's seq_cst fence
(phase 0 of scan) together establish the ordering that guarantees a validated
pointer appears in the scanner's collection if the scanner runs after the
publication.
If the scanner runs before the publication, the reader's re-validation (step 4 of
protect) detects the source change and retries with the new pointer.

Scan cost: collecting hazard slots is O(N * H).
Checking each retired pointer against the collected set is O(R * N * H) with a
linear scan, where R is the number of retired entries.
At the threshold R = Theta(N * H), this is O((N * H)^2).
For the expected N and H in Norn this is small and acceptable.
A hash set over the collected hazard values would reduce the membership check to
O(N * H + R) expected; this is a possible optimization if profiling shows the
linear scan is a bottleneck.

## Architecture

### `norn::hazard_domain`

Shared state for all threads using hazard pointers.
Owns the list of registered hazard records and provides retire and scan.

- `register_thread() -> hazard_record&` appends the calling thread's record
  (stored in `thread_local` storage) to the domain's list under a lightweight lock.
- `deregister_thread()` removes the record and transfers any unreclaimed retired
  nodes to the domain's internal retired list for the next scan.
- `retire(void* ptr, void(*deleter)(void*) noexcept)` appends the pair to the
  calling thread's local retired list.
- `scan()` collects hazard slots (Phase 1) and reclaims safe retired nodes
  (Phase 2). Called by each thread periodically after its retired list crosses a
  dynamic threshold.
- `thread_count()` returns the number of registered threads (used to size the
  threshold).

The domain is not copyable or movable.
Registration and deregistration use a spinlock that is only contended at thread
start and exit, not in the hot path.

### `norn::hazard_record`

Per-thread state registered with a domain.
Contains an array of `std::atomic<void*>` hazard slots (template parameter,
default 2 for the Michael-Scott queue).
Heap-allocated by the domain at registration and owned by the domain until domain
destruction.
The record is not copyable or movable.
Only the owning thread may publish or clear hazard slots.

`protect<int Index>(std::atomic<void*>& source) -> T*` performs the five-step
publication protocol described above.
`clear<int Index>()` stores null into the slot (relaxed order).

Records are created via `thread_local` storage: each thread has a lazily-allocated
record that is registered with the domain on first use.
The domain maintains two lists under its registration spinlock: an active list
(scanned) and a zombie list (deregistered records awaiting domain destruction).
Deregistration moves a record from the active list to the zombie list; the record's
retired nodes are transferred to the domain for reclamation during the next scan.
Records in the zombie list are not scanned; their hazard slots are irrelevant because
the owning thread has exited and will not dereference any protected pointer.

### `norn::hazard_ptr<T>`

RAII guard protecting a single pointer via a hazard slot.
Construction calls `record.protect(source)`; destruction calls `record.clear()`.
`get()` returns the typed pointer.

Not copyable or movable: moving would require transferring the hazard slot
atomically, adding complexity without benefit.

### Retirement

`domain.retire(ptr, deleter)` appends to the thread-local retired list.
It does not perform a scan.
After retire, the pointer must not be accessed unless a live hazard_ptr guard
protects it.
If a thread needs to read a node's contents after retiring it, it must hold a
hazard_ptr guard before calling retire.

After each retire, if the local retired list exceeds `max(64, 2 * thread_count()
* slots_per_thread)`, the thread calls `domain.scan()`.
The threshold is dynamic because the thread count can change between calls.

Deleters are `void(*)(void*) noexcept`.
If a custom deleter cannot be noexcept, the caller wraps it in a noexcept lambda
that static_asserts or documents the constraint.

Thread deregistration: when a thread exits, `deregister_thread()` is called.
Its retired nodes are transferred to the domain for reclamation during the next
scan by another thread.
Records remain stable during scans because the domain's registration lock is held
only during register/deregister (not during scan), and records are not moved once
created.

## Demonstration vehicle: Michael-Scott MPMC queue

M6 implements the Michael-Scott lock-free queue.
This is the classic structure that motivates hazard pointers and the most
interview-relevant example.

### Node layout

Each node holds its payload in aligned storage (not a direct `T` member) so the
sentinel node can be constructed without a valid `T` value:

```cpp
struct node {
  alignas(T) std::byte storage[sizeof(T)];
  std::atomic<node*> next{nullptr};
};
```

### Sentinel

The queue starts with a sentinel node shared by head and tail.
The sentinel's payload storage is uninitialized (no T value constructed).
Pop skips the sentinel: it only returns a value when `head != tail` or when the
first real node exists.

### Push (enqueue)

1. Allocate a new node, construct T in its storage.
2. Set `node->next` to null.
3. Loop:
   a. Protect `tail_` via hazard slot 0.
   b. Re-validate `tail_` has not changed (re-read source).
   c. Try to CAS `tail_->next` from null to the new node.
   d. If CAS succeeds, try to CAS `tail_` from old_tail to new_node.
      (If this CAS fails, another enqueue succeeded; that thread will advance
      tail. This is the standard MS help step.)
   e. If the CAS in (c) fails because `tail_->next` is not null, help advance
      tail by CASing `tail_` from old_tail to `tail_->next`.

### Pop (dequeue)

1. Protect `head_` via hazard slot 0.
2. Read `next = head_->next`.
3. Protect `next` via hazard slot 1.
4. Re-validate `head_` has not changed (re-read `head_`).
5. If `head_` changed, clear both slots and retry from step 1.
6. If `head_ == tail_` (both point to the sentinel, `next == null`), the queue
   is empty; clear both slots and return empty.
7. If `head_ == tail_` but `next != null`, tail is lagging. Help advance tail
   by CASing `tail_` from old_tail to `next`. Clear both slots and retry.
8. Read the value from `next` into a local variable (using move or copy, which
   may throw; if it throws, the node is not leaked because it remains linked
   and protected).
9. Try to CAS `head_` from old_head to `next`.
10. If CAS fails, clear both slots and retry from step 1.
11. If CAS succeeds, retire old_head.
12. Return the value read in step 8.

The payload is read (step 8) only after validating that this thread will win the
CAS (steps 4-7 confirmed the state is stable and this thread can proceed).
However, the actual CAS (step 9) may still fail if another thread sneaks in.
On CAS failure, the value read in step 8 is discarded (the local variable goes
out of scope); the node is not retired because the CAS failed.
This is safe: the value was read from a live, protected node, and discarding a
copy does not corrupt the queue state.
For move-only T, step 8 moves the value out of the node; on CAS failure the
moved-from node is left in a moved-from state, which is fine because the node
remains linked and the next retry will read the same (still-valid) node again.
Actually — this is incorrect: if step 8 moved the value out and the CAS fails,
the node is now in a moved-from state, and a future pop by another thread would
read garbage. Therefore: for move-only T, the payload must NOT be moved until
after the CAS succeeds. The design uses step 8 as a copy-or-move attempt
behind a check: if T is copyable, copy it in step 8; if T is move-only, defer
the move to step 12 (after the successful CAS).
The implementation uses `if constexpr` to choose the correct path.

### Empty check

`head_ == tail_` is empty only when `head_->next` is null (the queue has no
real nodes beyond the sentinel).
If `head_ == tail_` but `head_->next` is non-null, tail is lagging and the next
iteration of pop will help advance it.

## Constraints

- T must be nothrow-move-constructible (the pop path moves the payload out of the
  node after a successful CAS; throwing would leave the queue in an inconsistent
  state).
- T must be nothrow-move-assignable for the out-parameter pop path.
- The domain must outlive all threads registered with it.
- Threads must call `scan()` periodically; failure to do so leaks retired nodes.
- Custom deleters must be `void(*)(void*) noexcept`.
- A thread must not access a node after retiring it unless it holds an active
  hazard_ptr guard for that node.
- The retired list is pre-reserved at registration to avoid allocation during
  retire; if the pre-allocated capacity is exceeded, the thread calls scan()
  to reclaim space.

## Scope limitations

- M6 is a Michael-Scott MPMC queue with hazard-pointer reclamation.
- Epoch-based reclamation and RCU are documented as alternative schemes for future
  work.
- The implementation targets correctness and educational value, not raw performance;
  performance benchmarks are M7 scope.

## Tests

- Basic lifecycle: push, pop, verify values.
- Stress test: many items across multiple threads, exactly-once consumption, no
  use-after-free (ASan).
- Scan-frequency test: retire more nodes than the scan threshold.
- Thread registration and deregistration: threads start and exit during the stress.
- Move-only values to confirm proper deletion and no double-free.
- Deleter exactly-once: every retired node's deleter is called exactly once.
- TOCTOU test: one thread holds a hazard_ptr while another retires and scans;
  the scanner must retain the protected node.
- ASan, UBSan, and TSan verification.
- Weakened-order educational variant in disposable scratch (M5 pattern).

## Verification plan

- Unit tests for hazard_domain, hazard_record, and hazard_ptr.
- MS queue stress test (multiple threads, many items).
- ASan catches use-after-free or double-free.
- TSan exercises the ordering paths (TSan detects data races on atomic accesses;
  it does not prove weak-memory correctness by itself).
- GCC strict compilation on both platforms.
- Weakened-order probe in disposable scratch: relax the publication store (remove
  seq_cst fence) and verify with a stress test that the incorrect ordering can
  cause observable misbehavior under ASan or TSan.

## Decisions to confirm

- Michael-Scott MPMC as the demonstration vehicle.
- 2 hazard slots per thread (sufficient for MS pop's dual protection).
- Single-collect scan (correctness from the reader's fence, not scanner double-
  collection).
- Sentinel node with aligned byte storage (no T value).
- Dynamic scan threshold based on runtime thread count.
- `void(*)(void*) noexcept` deleters.
- Non-movable hazard_ptr.
- All three HP types in `include/norn/hazard_pointer.hpp`.