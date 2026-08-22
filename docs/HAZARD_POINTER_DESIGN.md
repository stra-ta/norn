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

The scan (`hazard_domain::scan_locked`) runs while the domain mutex is held for
its entire duration.
It does not take a snapshot of records and it does not release the lock mid-scan.
The scanner performs these phases:

Phase 0: Issue `std::atomic_thread_fence(std::memory_order_seq_cst)`.
  This fence pairs with the reader's seq_cst fence: together they ensure that if
  the reader has published and validated a pointer, the scanner will observe the
  publication.
  Without this fence, the scanner's acquire-loads could see a stale null value
  even after the reader's release-store is globally visible, because the C++
  memory model does not establish a synchronizes-with relationship between a
  release-store and a subsequent acquire-load that reads the old value.
Phase 1: Collect all published hazard values into a `std::unordered_set<void*>`.
  Iterate the domain's active record list and insert each non-null slot value,
  reading slots with acquire loads.
  Deregistration cannot interleave because `deregister_thread()` takes the same
  mutex, so the active list is stable for the whole scan and every scanned
  record stays alive.
Phase 2: Compact the domain's shared retired list in place.
  Walk the list once: an entry whose pointer is present in the collected set is
  kept, and an unprotected entry is reclaimed by invoking its stored deleter and
  dropped.
  The list is then resized to the number of surviving entries.

The reader's seq_cst fence (step 3 of protect) and the scanner's seq_cst fence
(phase 0 of scan) together establish the ordering that guarantees a validated
pointer appears in the scanner's collection if the scanner runs after the
publication.
If the scanner runs before the publication, the reader's re-validation (step 4 of
protect) detects the source change and retries with the new pointer.

Scan cost: building the hazard set is expected O(N * H), including the
unordered_set's allocation and hashing work, where N is the number of active
records and H is the fixed two slots per record.
The compaction walk performs expected O(R) hash lookups and O(R) bookkeeping
over the R retired entries.
A full scan therefore costs expected O(N * H + R), with the unordered_set
allocation counted as part of the scan cost.
Scanning is not lock-free: it holds the domain mutex from start to finish.

## Architecture

### `norn::hazard_domain`

Shared state for all threads using hazard pointers.
Owns the active records, the zombie (deregistered) records, and the shared
retired list, and provides retire and scan.
All mutation of that state goes through a single `std::mutex` (`mu_`).

- `register_thread() -> hazard_record&` returns the calling thread's existing
  `thread_local` record when it is already registered with this domain;
  otherwise it allocates a fresh record and appends it to the active list under
  the mutex.
- `deregister_thread()` removes the record from the active list, transfers any
  record-local retired entries to the domain's shared retired list, clears the
  record's domain pointer, and moves the record to the zombie list, all under
  the mutex.
- `retire(void* ptr, void(*deleter)(void*) noexcept)` appends the pair to the
  domain's shared retired list under the mutex and automatically runs
  `scan_locked()` once that list reaches
  `max(64, 2 * active_thread_count * slots_per_record)` entries.
- `scan()` takes the mutex and forces one full scan pass regardless of the
  threshold.
- `thread_count()` returns the number of active registered records, read under
  the mutex.

The domain is not copyable or movable.
Registration, deregistration, retirement, scanning, and thread counting all use
the same `std::mutex`; contention is limited to thread start and exit,
retirement, and scan passes, not the readers' protect and clear path.

### `norn::hazard_record`

Per-thread state registered with a domain.
Contains exactly two `std::atomic<void*>` hazard slots
(`hazard_record::kSlots == 2`), matching the Michael-Scott-derived pop path's
dual protection.
Heap-allocated by the domain at registration and owned by the domain until domain
destruction.
The record is not copyable or movable.
Only the owning thread may publish or clear hazard slots.

`protect<Index, T>(std::atomic<void*>& source) -> T*` performs the five-step
publication protocol described above.
`clear<int Index>()` stores null into the slot (relaxed order).

The record also exposes a public `retired_` vector with a matching
`retire_entry` helper.
Nothing in the current queue appends to it during normal operation, since
`mpsc_queue` retires exclusively through `domain.retire`; the vector remains the
transfer surface that `deregister_thread()` drains into the domain's retired
list.

Records are created via `thread_local` storage: each thread has a lazily-allocated
record that is registered with the domain on first use.
The domain maintains two lists under its mutex: an active list
(scanned) and a zombie list (deregistered records awaiting domain destruction).
Deregistration removes a record from the active list, transfers its retired nodes
to the domain for reclamation during the next scan, clears the record's domain
pointer, and moves it to the zombie list.
Records in the zombie list are retained until domain destruction and are never
scanned; their hazard slots are irrelevant because
the owning thread has exited and will not dereference any protected pointer.

### `norn::hazard_ptr<T>`

RAII guard protecting a single pointer via a hazard slot.
Construction calls `record.protect(source)`; destruction calls `record.clear()`.
`get()` returns the typed pointer.

Not copyable or movable: moving would require transferring the hazard slot
atomically, adding complexity without benefit.

### Retirement

`domain.retire(ptr, deleter)` appends the pair to the domain's shared retired
list under the domain mutex.
It may perform a scan: when the shared list reaches
`max(64, 2 * active_thread_count * slots_per_record)` entries, `retire` calls
`scan_locked()` before returning.
An explicit `domain.scan()` forces a pass regardless of the threshold.
After retire, the pointer must not be accessed unless a live hazard_ptr guard
protects it.
If a thread needs to read a node's contents after retiring it, it must hold a
hazard_ptr guard before calling retire.

Deleters are `void(*)(void*) noexcept`.
If a custom deleter cannot be noexcept, the caller wraps it in a noexcept lambda
that static_asserts or documents the constraint.

Thread deregistration: when a thread exits, `deregister_thread()` is called.
Its retired nodes are transferred to the domain for reclamation during the next
scan by another thread.
Records remain stable during scans because the domain owns every active and
zombie record until domain destruction, and `scan_locked()` runs under the same
mutex as registration and deregistration changes.
Records are not moved once created.

## Demonstration vehicle: the MPSC linked queue

M6 implements an unbounded multiple-producer, single-consumer linked queue whose
linking and helping steps are derived from the Michael-Scott design.
A node-based structure is exactly what motivates hazard pointers: a pop unlinks
and retires a node while a producer may still be helping through the tail.

Norn makes no formal lock-free claim for this queue.
Producers allocate a heap node per push, retirement and scanning run under the
domain's mutex with allocation on the scan path, and progress therefore depends
on node allocation succeeding and on scan work retiring nodes.
The specialization to a single consumer keeps the reclamation example focused;
the bounded `mpmc_ring` covers the many-to-many case.

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

Pop runs entirely on the single consumer thread; producers never modify `head_`.

1. Register the calling thread with the domain to obtain its hazard record
   (an already-registered thread reuses its record).
2. Loop:
   a. Protect `head_` via hazard slot 0 by constructing a
      `hazard_ptr<node, 0>` guard; the guard's construction runs the five-step
      publication protocol and yields the validated head pointer `h`.
   b. Protect the value of `h->next` via hazard slot 1 by constructing a
      `hazard_ptr<node, 1>` guard; this yields the validated successor
      pointer `next`.
   c. Reload `head_` (acquire) and check it still equals `h`.
      If it does not, validation failed: destroy both guards and retry.
   d. If `next` is null, the queue is empty: return `std::nullopt`.
   e. Load `tail_` (acquire). If `h == tail`, tail is lagging behind the
      already-linked chain: CAS `tail_` from `t` to `next`, then retry.
      This helping step happens before any retirement because retiring the
      node that `tail_` points to would let a scan free a node a producer is
      about to dereference while helping through the tail.
   f. CAS `head_` from `h` to `next`.
      Given single-consumer validation in step c this CAS is defensive and
      expected to succeed; on failure, destroy both guards and retry.
3. Only after the CAS in step f succeeds:
   a. Move-construct the return value from the payload storage of `next`
      via `std::launder`; `T` is nothrow-move-constructible, so this cannot
      throw and the node is privately owned by the consumer at this point.
   b. Retire `h`, passing `delete_node` when the node carries a payload and
      `delete_sentinel` for the sentinel.
   c. Return the value.

Both guards are RAII objects: every exit path, including retries and the
empty return, destroys them and clears both hazard slots (relaxed order).

### Empty check

`empty()` loads `head_` (acquire) and returns whether `head_->next` is null.
It does not compare against `tail_` and does not publish a hazard pointer:
only the single consumer unlinks or frees nodes, so no other thread can free
the node `head_` points to, and producers only append successors atomically.
Inside `try_pop`, emptiness is decided after full validation: a protected,
validated successor of null (step d above) means no real node is linked.

## Constraints

- Enforced by `static_assert` in `mpsc_queue`: `T` must be nothrow
  move-constructible and nothrow-destructible.
  Pop move-constructs its return value only after the head CAS succeeded and
  the node is privately owned by the consumer, so the move cannot throw out
  of `try_pop` and no partially moved node is ever observable.
- Exactly one thread may consume: `try_pop` and `empty()` assume no other
  consumer advances `head_` or frees nodes.
  Any number of producers may call `try_push`.
- A `hazard_domain` passed to an `mpsc_queue` must outlive the queue and
  every operation on it; the queue stores a plain reference and registers
  threads with the domain lazily on the first `try_push` or `try_pop`.
- Destruction must be quiescent: no pushes or pops may be in flight while the
  queue or its domain is destroyed.
  The domain destructor reclaims all retired-but-unreclaimed entries and
  destroys every active and deregistered (zombie) record, reclaiming any
  record-local retired entries they still hold.
- Each `retire` appends the pair to the domain's shared retired list under
  its mutex and automatically runs a scan once that list reaches
  `max(64, 2 * active_thread_count * slots_per_record)` entries.
  An explicit `scan()` forces a pass regardless of the threshold.
  Nothing remains leaked: whatever no scan reclaimed is reclaimed when the
  domain is destroyed.
- Custom deleters must match `void(*)(void*) noexcept`, which is the
  parameter type of `retire` and the stored deleter type of a retired entry.
  The queue supplies `delete_node` and `delete_sentinel`, which destroy the
  payload (payload nodes only) and free the node.
- After retiring a pointer, a thread must not access it again unless a live
  `hazard_ptr` guard protects it: the entry sits in the shared retired list,
  and the retire call itself may trigger a scan that frees every entry not
  published in a hazard slot.

## Scope limitations

- M6 is an MPSC linked queue with hazard-pointer reclamation; it makes no
  formal lock-free claim, since progress depends on allocation and scan work.
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
- Linked MPSC queue stress test (multiple producer threads, one consumer, many
  items).
- ASan catches use-after-free or double-free.
- TSan exercises the ordering paths (TSan detects data races on atomic accesses;
  it does not prove weak-memory correctness by itself).
- GCC strict compilation on both platforms.
- Weakened-order probe in disposable scratch: relax the publication store (remove
  seq_cst fence) and verify with a stress test that the incorrect ordering can
  cause observable misbehavior under ASan or TSan.

## Decisions to confirm

- The MPSC linked queue (Michael-Scott-derived linking) as the demonstration
  vehicle, with no formal lock-free claim.
- 2 hazard slots per thread (sufficient for MS pop's dual protection).
- Single-collect scan (correctness from the reader's fence, not scanner double-
  collection).
- Sentinel node with aligned byte storage (no T value).
- Retirement-triggered scanning: `retire` runs `scan_locked()` automatically
  once the shared retired list reaches
  `max(64, 2 * active_count * slots_per_record)`, with an explicit `scan()`
  escape hatch.
- `void(*)(void*) noexcept` deleters.
- Non-movable hazard_ptr.
- All three HP types in `include/norn/hazard_pointer.hpp`.