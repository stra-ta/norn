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