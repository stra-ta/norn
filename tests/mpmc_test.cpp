#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "norn/mpmc_queue.hpp"

namespace {

// Blocks in its constructor until the gate opens when should_block is true.
// Used to hold a producer deterministically inside the reservation-to-publication
// window while other values pass through without blocking.
struct gated_value {
  explicit gated_value(int input, bool should_block, std::atomic<bool>& entered,
                       std::atomic<bool>& gate) noexcept
      : number(input), gate_(gate) {
    if (should_block) {
      entered.store(true, std::memory_order_release);
      while (!gate_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
  }
  gated_value(const gated_value&) = delete;
  gated_value& operator=(const gated_value&) = delete;
  gated_value(gated_value&&) = default;
  gated_value& operator=(gated_value&&) = delete;
  int number;
  std::atomic<bool>& gate_;
};

// Blocks in its move constructor until the gate opens when should_block is true.
// Used to hold a consumer deterministically inside the claim-to-release window:
// the queue's optional pop path claims the slot with a CAS and then
// move-constructs the returned optional in place from std::move(*element), which
// cannot be elided, so this constructor runs in the consumer thread after the
// claim CAS.
struct gated_move {
  explicit gated_move(int input, bool should_block, std::atomic<bool>& entered,
                      std::atomic<bool>& gate) noexcept
      : number(input), block_on_move_(should_block), entered_(entered), gate_(gate) {}
  gated_move(const gated_move&) = delete;
  gated_move& operator=(const gated_move&) = delete;
  gated_move(gated_move&& other) noexcept
      : number(other.number), block_on_move_(other.block_on_move_), entered_(other.entered_),
        gate_(other.gate_) {
    if (block_on_move_) {
      entered_.store(true, std::memory_order_release);
      while (!gate_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
  }
  int number;
  bool block_on_move_;
  std::atomic<bool>& entered_;
  std::atomic<bool>& gate_;
};

}  // namespace

TEST_CASE("MPMC queue reports full and empty boundaries") {
  norn::mpmc_queue<int, 2> queue;
  REQUIRE(queue.try_push(1));
  REQUIRE(queue.try_push(2));
  REQUIRE_FALSE(queue.try_push(3));
  REQUIRE_FALSE(queue.empty());
  REQUIRE(queue.try_pop() == 1);
  REQUIRE(queue.try_pop() == 2);
  REQUIRE_FALSE(queue.try_pop().has_value());
  REQUIRE(queue.empty());
}

TEST_CASE("MPMC queue preserves single-producer FIFO order") {
  constexpr int count = 10'000;
  norn::mpmc_queue<int, 64> queue;
  std::atomic<bool> consumer_succeeded = true;

  std::thread producer([&] {
    for (int value = 0; value < count && consumer_succeeded.load(std::memory_order_relaxed);) {
      if (queue.try_push(value)) {
        ++value;
      } else {
        std::this_thread::yield();
      }
    }
  });
  std::thread consumer([&] {
    int nullopt_streak = 0;
    for (int expected = 0; expected < count;) {
      auto value = queue.try_pop();
      if (!value.has_value()) {
        // A long continuous empty streak means the producer stopped making
        // progress. Fail instead of spinning forever.
        if (++nullopt_streak > 10'000'000) {
          consumer_succeeded.store(false, std::memory_order_relaxed);
          return;
        }
        std::this_thread::yield();
      } else {
        nullopt_streak = 0;
        if (*value != expected) {
          consumer_succeeded.store(false, std::memory_order_relaxed);
          return;
        }
        ++expected;
      }
    }
  });

  producer.join();
  consumer.join();
  REQUIRE(consumer_succeeded.load(std::memory_order_relaxed));
  REQUIRE(queue.empty());
}

TEST_CASE("MPMC queue consumes every value exactly once across multiple producers and consumers") {
  constexpr std::size_t producers = 3;
  constexpr std::size_t consumers = 3;
  constexpr std::size_t values_per_producer = 2'000;
  constexpr std::size_t total = producers * values_per_producer;
  norn::mpmc_queue<std::size_t, 8> queue;
  std::vector<std::atomic<bool>> seen(total);
  for (auto& flag : seen) {
    flag.store(false, std::memory_order_relaxed);
  }
  std::atomic<std::size_t> consumed{0};
  std::atomic<bool> duplicate = false;
  std::atomic<bool> failed = false;

  // Consumers must run concurrently with the producers: with a bounded queue,
  // producers can only complete their pushes while consumers drain.
  // Attempt caps keep a queue regression from hanging the test runner; a
  // correct run never approaches them.
  std::vector<std::thread> consumer_threads;
  for (std::size_t consumer = 0; consumer < consumers; ++consumer) {
    consumer_threads.emplace_back([&] {
      std::size_t empty_streak = 0;
      while (consumed.load(std::memory_order_relaxed) < total && !failed.load(std::memory_order_relaxed)) {
        auto value = queue.try_pop();
        if (!value.has_value()) {
          if (++empty_streak > 50'000'000) {
            failed.store(true, std::memory_order_relaxed);
            return;
          }
          std::this_thread::yield();
          continue;
        }
        empty_streak = 0;
        if (*value >= total) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        if (seen[*value].exchange(true, std::memory_order_relaxed)) {
          duplicate.store(true, std::memory_order_relaxed);
        }
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::vector<std::thread> producer_threads;
  for (std::size_t producer = 0; producer < producers; ++producer) {
    producer_threads.emplace_back([&, producer] {
      const std::size_t first = producer * values_per_producer;
      std::size_t attempts = 0;
      for (std::size_t value = 0; value < values_per_producer && !failed.load(std::memory_order_relaxed);) {
        if (queue.try_push(first + value)) {
          ++value;
        } else {
          if (++attempts > 50'000'000) {
            failed.store(true, std::memory_order_relaxed);
            return;
          }
          std::this_thread::yield();
        }
      }
    });
  }
  for (auto& producer : producer_threads) {
    producer.join();
  }

  // After every producer has joined, every accepted value is published:
  // try_push returns true only after the publish store. The consumers drain
  // the remainder. Never terminate on empty() or on a single failed pop,
  // both of which can be wrong under the weak semantics.
  for (auto& consumer : consumer_threads) {
    consumer.join();
  }

  REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
  REQUIRE_FALSE(duplicate.load(std::memory_order_relaxed));
  REQUIRE(consumed.load(std::memory_order_relaxed) == total);
  for (const auto& flag : seen) {
    REQUIRE(flag.load(std::memory_order_relaxed));
  }
}

TEST_CASE("MPMC queue supports move-only values") {
  norn::mpmc_queue<std::unique_ptr<int>, 2> queue;
  REQUIRE(queue.try_push(std::make_unique<int>(1)));
  REQUIRE(queue.try_push(std::make_unique<int>(2)));
  auto first = queue.try_pop();
  REQUIRE(first.has_value());
  REQUIRE(**first == 1);
  REQUIRE(queue.try_push(std::make_unique<int>(3)));
  auto second = queue.try_pop();
  REQUIRE(second.has_value());
  REQUIRE(**second == 2);
  REQUIRE(queue.try_pop().has_value());
  REQUIRE_FALSE(queue.try_pop().has_value());
}

TEST_CASE("MPMC queue reuses slots across wraparound") {
  norn::mpmc_queue<int, 3> queue;
  for (int value = 0; value < 10'000; ++value) {
    REQUIRE(queue.try_push(value));
    REQUIRE(queue.try_pop() == value);
  }
  REQUIRE(queue.empty());
}

TEST_CASE("reservation window with a blocked producer strands later items until it publishes") {
  constexpr int blocked = 42;
  norn::mpmc_queue<gated_value, 2> queue;
  std::atomic<bool> entered = false;
  std::atomic<bool> gate = false;
  std::atomic<bool> push_result = false;

  std::thread producer([&] { push_result.store(queue.emplace(blocked, true, entered, gate)); });

  for (int attempt = 0; attempt < 10'000 && !entered.load(std::memory_order_acquire); ++attempt) {
    std::this_thread::yield();
  }
  const bool saw_entered = entered.load(std::memory_order_acquire);

  // Collect every observation before asserting so a regression cannot abort
  // the runner while the producer thread is still inside the gate.
  // The blocked producer has reserved slot 0 but not published. The queue
  // is capacity 2, so the remaining producer has exactly one usable slot.
  const bool second_push = queue.try_push(gated_value(1, false, entered, gate));
  const bool third_push = queue.try_push(gated_value(2, false, entered, gate));

  // Consumers see the reservation hole as empty even though slot 1 holds a
  // published item: the documented false-empty window of the weak variant.
  // The failed third push is NOT false-full: under the reservation-CAS
  // linearization the reserved slot 0 already counts as an enqueued item, so
  // with slot 1 published the abstract queue is genuinely full.
  const auto early_pop = queue.try_pop();

  gate.store(true, std::memory_order_release);
  producer.join();

  REQUIRE(saw_entered);
  REQUIRE(push_result.load(std::memory_order_relaxed));
  REQUIRE(second_push);
  REQUIRE_FALSE(third_push);
  REQUIRE_FALSE(early_pop.has_value());

  // The hole closes and the stranded items drain in order.
  REQUIRE(queue.try_pop()->number == blocked);
  REQUIRE(queue.try_pop()->number == 1);
  REQUIRE_FALSE(queue.try_pop().has_value());
  REQUIRE(queue.empty());
}

// A consumer blocked in its move-out after claiming the slot produces the
// asymmetric false-full behavior: other consumers advance past the held
// position, while a producer sees the held slot as full even though the item
// was already consumed at the claim linearization point, so capacity exists.
// The held value is emplaced rather than pushed: pushing a block-on-move value
// would trigger the blocking move while moving the temporary into the slot.
TEST_CASE("claim window with a blocked consumer advances others but shows false-full to producers") {
  constexpr int held = 1;
  norn::mpmc_queue<gated_move, 2> queue;
  std::atomic<bool> entered = false;
  std::atomic<bool> gate = false;

  // Slot 0: the value the gated consumer will claim and then block in the move.
  REQUIRE(queue.emplace(held, true, entered, gate));
  // Slot 1: a plain value for the second consumer to pop while the gate is closed.
  REQUIRE(queue.emplace(2, false, entered, gate));

  std::atomic<int> held_number{0};
  std::atomic<bool> held_popped{false};
  std::thread gated_consumer([&] {
    auto result = queue.try_pop();
    if (result.has_value()) {
      held_number.store(result->number, std::memory_order_relaxed);
      held_popped.store(true, std::memory_order_release);
    }
  });

  for (int attempt = 0; attempt < 10'000 && !entered.load(std::memory_order_acquire); ++attempt) {
    std::this_thread::yield();
  }
  const bool saw_entered = entered.load(std::memory_order_acquire);

  // The gate-closed checks below can only run if the gated consumer actually
  // entered its move-out: otherwise the main thread's own probe could claim
  // the gated slot and block on the still-closed gate. On the failure path we
  // open the gate immediately so it is reachable from every branch.
  bool advanced_popped = false;
  int advanced_number = 0;
  bool saw_false_full = false;
  if (saw_entered) {
    // While the gate stays closed, the gated consumer holds slot 0's claim.
    // A second consumer can still advance past the held position and pop slot 1.
    if (auto adv = queue.try_pop(); adv.has_value()) {
      advanced_popped = true;
      advanced_number = adv->number;
    }

    // A producer probing the held slot sees false-full: the item was already
    // consumed at the claim linearization point, so capacity exists, yet the
    // unreleased slot reports full. Cap the attempts so a regression cannot hang.
    bool pushed_through = false;
    std::size_t attempts = 0;
    while (attempts++ < 1'000'000) {
      if (queue.emplace(3, false, entered, gate)) {
        pushed_through = true;
        break;
      }
      std::this_thread::yield();
    }
    saw_false_full = !pushed_through;
  }

  gate.store(true, std::memory_order_release);
  gated_consumer.join();

  REQUIRE(saw_entered);
  REQUIRE(held_popped.load(std::memory_order_acquire));
  REQUIRE(held_number.load(std::memory_order_relaxed) == held);
  REQUIRE(advanced_popped);
  REQUIRE(advanced_number == 2);
  REQUIRE(saw_false_full);

  // With the gate open and the slot released, the producer's push succeeds
  // and the queue drains completely.
  REQUIRE(queue.emplace(3, false, entered, gate));
  const auto drained = queue.try_pop();
  REQUIRE(drained.has_value());
  REQUIRE(drained->number == 3);
  REQUIRE_FALSE(queue.try_pop().has_value());
  REQUIRE(queue.empty());
}

TEST_CASE("sequence comparison classifies near-wrap values") {
  constexpr std::size_t max = static_cast<std::size_t>(-1);
  constexpr std::size_t half = std::size_t{1} << (sizeof(std::size_t) * 8 - 1);
  REQUIRE(norn::detail::sequence_relation(5, 5) == 0);
  REQUIRE(norn::detail::sequence_relation(6, 5) == 1);
  REQUIRE(norn::detail::sequence_relation(4, 5) == -1);
  REQUIRE(norn::detail::sequence_relation(0, max) == 1);
  REQUIRE(norn::detail::sequence_relation(max, 0) == -1);
  REQUIRE(norn::detail::sequence_relation(max - 1, max) == -1);
  REQUIRE(norn::detail::sequence_relation(half, 0) == -1);
  REQUIRE(norn::detail::sequence_relation(half - 1, 0) == 1);
}