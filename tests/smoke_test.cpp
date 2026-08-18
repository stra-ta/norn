#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "norn/mutex_queue.hpp"
#include "norn/spsc_queue.hpp"

namespace {

// M5 campaign Proof 5: the accepted-before-close protocol for one mutex queue
// family. Consumers run concurrently with producers (a bounded queue can only
// accept values while someone drains), then close() is called after all
// producers joined, and consumers drain until nullopt. The consumed set must
// equal the accepted set exactly once.
template <typename Queue>
void run_close_and_drain(std::size_t producers, std::size_t consumers,
                         std::size_t values_per_producer) {
  const std::size_t total = producers * values_per_producer;
  Queue queue;
  std::vector<std::atomic<int>> seen(total);
  for (auto& count : seen) {
    count.store(0, std::memory_order_relaxed);
  }
  std::atomic<std::size_t> consumed{0};
  std::atomic<bool> failed{false};

  // Consumers run concurrently with producers (a bounded queue can only accept
  // values while someone drains). pop() blocks when empty; close() below wakes
  // every blocked pop, so consumers always terminate once the main thread
  // reaches close(), which it does because producers always terminate: they
  // either finish their values or hit the failure cap.
  std::vector<std::thread> consumer_threads;
  for (std::size_t c = 0; c < consumers; ++c) {
    consumer_threads.emplace_back([&] {
      while (consumed.load(std::memory_order_relaxed) < total &&
             !failed.load(std::memory_order_relaxed)) {
        auto value = queue.pop();
        if (!value.has_value()) {
          return;
        }
        if (static_cast<std::size_t>(*value) >= total) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        seen[static_cast<std::size_t>(*value)].fetch_add(1, std::memory_order_relaxed);
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::vector<std::thread> producer_threads;
  for (std::size_t p = 0; p < producers; ++p) {
    producer_threads.emplace_back([&, p] {
      const int first = static_cast<int>(p * values_per_producer);
      std::size_t attempts = 0;
      for (std::size_t value = 0; value < values_per_producer &&
                              !failed.load(std::memory_order_relaxed);) {
        // Bounded queues report full; unbounded queues never do. Retry keeps
        // the protocol uniform across both families. The cap resets on
        // progress, so a correct run never approaches it.
        if (queue.try_push(first + static_cast<int>(value))) {
          ++value;
          attempts = 0;
        } else if (++attempts > 1'000'000) {
          failed.store(true, std::memory_order_relaxed);
          return;
        } else {
          std::this_thread::yield();
        }
      }
    });
  }
  for (auto& producer : producer_threads) {
    producer.join();
  }

  // close() runs after all producers joined, so it cannot race a push; the
  // serialization of push and close by the queue's mutex is established by
  // inspection. Consumers wake on close and drain the remainder.
  queue.close();
  for (auto& consumer : consumer_threads) {
    consumer.join();
  }

  REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
  REQUIRE(consumed.load(std::memory_order_relaxed) == total);
  for (const auto& count : seen) {
    REQUIRE(count.load(std::memory_order_relaxed) == 1);
  }
}

}  // namespace

TEST_CASE("unbounded mutex queue drains every accepted value exactly once after close") {
  run_close_and_drain<norn::mutex_queue<int>>(4, 2, 500);
}

TEST_CASE("bounded mutex queue drains every accepted value exactly once after close") {
  run_close_and_drain<norn::bounded_mutex_queue<int, 16>>(4, 2, 500);
}

TEST_CASE("blocked consumer wakes when the queue closes") {
  norn::mutex_queue<int> queue;
  std::atomic<bool> pop_started = false;
  std::atomic<bool> pop_returned = false;
  std::optional<int> result;
  std::thread consumer([&] {
    pop_started.store(true, std::memory_order_release);
    result = queue.pop();
    pop_returned.store(true, std::memory_order_release);
  });
  for (int attempt = 0; attempt < 10'000 && !pop_started.load(std::memory_order_acquire); ++attempt) {
    std::this_thread::yield();
  }
  REQUIRE(pop_started.load(std::memory_order_acquire));
  // Best-effort proof that the consumer is inside the wait rather than about
  // to enter it after close(): the consumer must not have returned within a
  // bounded window, mirroring the blocking-push wake test. This cannot prove
  // the consumer is inside the condition-variable wait without instrumenting
  // the queue, but combined with close() below it is the strongest available
  // deterministic check.
  for (int attempt = 0; attempt < 1'000 && !pop_returned.load(std::memory_order_acquire); ++attempt) {
    std::this_thread::yield();
  }
  REQUIRE_FALSE(pop_returned.load(std::memory_order_acquire));
  queue.close();
  consumer.join();
  REQUIRE(pop_returned.load(std::memory_order_acquire));
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("unbounded mutex queue preserves FIFO order") {
  norn::mutex_queue<int> queue;
  REQUIRE(queue.try_push(1));
  REQUIRE(queue.try_push(2));
  REQUIRE(queue.pop() == 1);
  REQUIRE(queue.pop() == 2);
  int value = 0;
  REQUIRE_FALSE(queue.try_pop(value));
}

TEST_CASE("unbounded mutex queue drains after close and rejects pushes") {
  norn::mutex_queue<int> queue;
  REQUIRE(queue.try_push(7));
  queue.close();
  REQUIRE_FALSE(queue.try_push(8));
  REQUIRE(queue.pop() == 7);
  REQUIRE_FALSE(queue.pop().has_value());
}

TEST_CASE("bounded mutex queue reports full and supports move-only values") {
  norn::bounded_mutex_queue<std::unique_ptr<int>, 2> queue;
  REQUIRE(queue.try_push(std::make_unique<int>(1)));
  REQUIRE(queue.try_push(std::make_unique<int>(2)));
  REQUIRE_FALSE(queue.try_push(std::make_unique<int>(3)));

  auto first = queue.pop();
  REQUIRE(first.has_value());
  REQUIRE(**first == 1);
  REQUIRE(queue.try_push(std::make_unique<int>(3)));
  queue.close();
  REQUIRE(queue.pop().has_value());
  REQUIRE_FALSE(queue.push(std::make_unique<int>(4)));
}

TEST_CASE("bounded blocking operations wake when queue closes") {
  norn::bounded_mutex_queue<int, 1> queue;
  REQUIRE(queue.try_push(1));
  std::atomic<bool> push_started = false;
  std::atomic<bool> push_returned = false;
  std::atomic<bool> push_failed = false;
  std::thread producer([&] {
    push_started.store(true, std::memory_order_release);
    push_failed.store(!queue.push(2), std::memory_order_relaxed);
    push_returned.store(true, std::memory_order_release);
  });
  while (!push_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  for (int attempt = 0; attempt < 1'000 && !push_returned.load(std::memory_order_acquire); ++attempt) {
    std::this_thread::yield();
  }
  REQUIRE_FALSE(push_returned.load(std::memory_order_acquire));
  queue.close();
  producer.join();
  REQUIRE(push_returned.load(std::memory_order_acquire));
  REQUIRE(push_failed.load(std::memory_order_relaxed));
  REQUIRE(queue.pop() == 1);
  REQUIRE_FALSE(queue.pop().has_value());
}

TEST_CASE("unbounded mutex queue supports concurrent FIFO production") {
  constexpr int count = 10'000;
  norn::mutex_queue<int> queue;
  std::atomic<bool> producer_succeeded = true;
  std::thread producer([&] {
    for (int value = 0; value < count; ++value) {
      if (!queue.try_push(value)) {
        producer_succeeded.store(false, std::memory_order_relaxed);
        return;
      }
    }
    queue.close();
  });

  std::vector<int> values;
  while (auto value = queue.pop()) {
    values.push_back(*value);
  }
  producer.join();

  REQUIRE(producer_succeeded.load(std::memory_order_relaxed));
  REQUIRE(values.size() == count);
  for (int value = 0; value < count; ++value) {
    REQUIRE(values[static_cast<std::size_t>(value)] == value);
  }
}

TEST_CASE("bounded mutex queue supports multiple producers and consumers") {
  constexpr int producers = 4;
  constexpr int consumers = 4;
  constexpr int values_per_producer = 2'000;
  constexpr int total_values = producers * values_per_producer;
  norn::bounded_mutex_queue<int, 64> queue;
  std::vector<std::atomic<int>> seen(static_cast<std::size_t>(total_values));
  for (auto& count : seen) {
    count.store(0, std::memory_order_relaxed);
  }

  std::vector<std::thread> producer_threads;
  for (int producer = 0; producer < producers; ++producer) {
    producer_threads.emplace_back([&, producer] {
      for (int offset = 0; offset < values_per_producer; ++offset) {
        const int value = producer * values_per_producer + offset;
        while (!queue.push(value)) {
          std::this_thread::yield();
        }
      }
    });
  }
  std::vector<std::thread> consumer_threads;
  for (int consumer = 0; consumer < consumers; ++consumer) {
    consumer_threads.emplace_back([&] {
      while (auto value = queue.pop()) {
        if (*value < 0 || *value >= total_values) {
          continue;
        }
        seen[static_cast<std::size_t>(*value)].fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& producer : producer_threads) {
    producer.join();
  }
  queue.close();
  for (auto& consumer : consumer_threads) {
    consumer.join();
  }

  for (const auto& count : seen) {
    REQUIRE(count.load(std::memory_order_relaxed) == 1);
  }
}

TEST_CASE("SPSC queue reports empty and full boundaries") {
  norn::spsc_queue<int, 2> queue;
  REQUIRE(queue.empty());
  REQUIRE(queue.try_push(1));
  REQUIRE(queue.try_push(2));
  REQUIRE_FALSE(queue.try_push(3));
  REQUIRE_FALSE(queue.empty());
  REQUIRE(queue.try_pop() == 1);
  REQUIRE(queue.try_pop() == 2);
  REQUIRE_FALSE(queue.try_pop().has_value());
  REQUIRE(queue.empty());
}

TEST_CASE("padded SPSC queue preserves the same basic contract") {
  norn::spsc_queue_padded<int, 2> queue;
  REQUIRE(queue.try_push(1));
  REQUIRE(queue.try_push(2));
  REQUIRE_FALSE(queue.try_push(3));
  REQUIRE(queue.try_pop() == 1);
  REQUIRE(queue.try_pop() == 2);
  REQUIRE_FALSE(queue.try_pop().has_value());
}

TEST_CASE("SPSC queue preserves FIFO order through repeated wraparound") {
  norn::spsc_queue<int, 3> queue;
  for (int value = 0; value < 10'000; ++value) {
    REQUIRE(queue.try_push(value));
    REQUIRE(queue.try_pop() == value);
  }
}

TEST_CASE("SPSC queue supports move-only values and non-default construction") {
  struct value {
    explicit value(int input) : number(input) {}
    value() = delete;
    value(const value&) = delete;
    value& operator=(const value&) = delete;
    value(value&&) = default;
    value& operator=(value&&) = delete;
    int number;
  };

  norn::spsc_queue<value, 2> queue;
  REQUIRE(queue.emplace(42));
  auto value = queue.try_pop();
  REQUIRE(value.has_value());
  REQUIRE(value->number == 42);
}

TEST_CASE("SPSC queue transfers a validated sequence between two threads") {
  constexpr int count = 100'000;
  norn::spsc_queue<int, 64> queue;
  std::atomic<bool> consumer_succeeded = true;

  std::thread producer([&] {
    for (int value = 0; value < count;) {
      if (queue.try_push(value)) {
        ++value;
      } else {
        std::this_thread::yield();
      }
    }
  });
  std::thread consumer([&] {
    for (int expected = 0; expected < count;) {
      auto value = queue.try_pop();
      if (!value.has_value()) {
        std::this_thread::yield();
      } else if (*value != expected) {
        consumer_succeeded.store(false, std::memory_order_relaxed);
        return;
      } else {
        ++expected;
      }
    }
  });

  producer.join();
  consumer.join();
  REQUIRE(consumer_succeeded.load(std::memory_order_relaxed));
  REQUIRE(queue.empty());
}
