#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "norn/mpmc_queue.hpp"
#include "norn/spsc_queue.hpp"

// M5 campaign Proof 1 and the SPSC stress from Proof 5.
// The manifests are fixed and identical across platforms, toolchains, and build
// configurations. The identifiers tag each entry for reproduction; they do not
// generate inputs at runtime.

namespace {

struct run_spec {
  std::uint32_t id;
  std::size_t producers;
  std::size_t consumers;
  std::size_t capacity;
  std::size_t values_per_producer;
};

constexpr std::size_t kAttemptCap = 1'000'000;

// The fixed MPMC manifest: 24 runs sweeping producers 1-8, consumers 1-8,
// capacities 2/8/64, and values per producer 100-5000.
constexpr run_spec kMpmcManifest[] = {
    {0x1a2b3c4d, 1, 1, 2, 500},   // 1P1C FIFO, smallest capacity
    {0x2b3c4d5e, 1, 1, 8, 2000},  // 1P1C FIFO
    {0x3c4d5e6f, 1, 1, 64, 5000}, // 1P1C FIFO, large
    {0x4d5e6f70, 1, 2, 8, 1000},
    {0x5e6f7081, 1, 4, 8, 1000},
    {0x6f708192, 1, 8, 2, 100},   // consumer-heavy, tiny capacity
    {0x708192a3, 2, 1, 8, 1000},
    {0x8192a3b4, 2, 2, 8, 1500},
    {0x92a3b4c5, 2, 4, 2, 500},
    {0xa3b4c5d6, 3, 3, 8, 2000},
    {0xb4c5d6e7, 4, 4, 8, 2000},
    {0xc5d6e7f8, 4, 4, 64, 3000},
    {0xd6e7f809, 4, 8, 2, 200},   // consumer-heavy, tiny capacity
    {0xe7f8091a, 8, 1, 8, 1000},  // producer-heavy
    {0xf8091a2b, 8, 2, 8, 1000},
    {0x091a2b3c, 8, 4, 8, 500},
    {0x1a2b3c4e, 8, 8, 2, 100},   // heavy both sides, tiny capacity
    {0x2b3c4d5f, 8, 8, 8, 500},
    {0x3c4d5e60, 8, 8, 64, 1000},
    {0x4d5e6f71, 2, 2, 64, 5000}, // large values
    {0x5e6f7082, 1, 1, 64, 5000}, // 1P1C FIFO, large
    {0x6f708193, 3, 3, 2, 100},   // small capacity churn
    {0x708192a4, 6, 6, 8, 1000},
    {0x8192a3b5, 5, 3, 64, 3000},
};

struct spsc_spec {
  std::uint32_t id;
  std::size_t capacity;
  bool padded;
  std::size_t values_per_producer;
};

// SPSC stress: capacities 8 and 64, plus the padded variant at 64.
constexpr spsc_spec kSpscManifest[] = {
    {0x91a2b3c6, 8, false, 2000},
    {0xa2b3c4d7, 64, false, 5000},
    {0xb3c4d5e8, 64, true, 5000},
};

template <std::size_t Capacity>
void run_mpmc_history(const run_spec& spec, bool check_fifo) {
  const std::size_t total = spec.producers * spec.values_per_producer;
  norn::mpmc_queue<std::size_t, Capacity> queue;
  std::vector<std::atomic<bool>> seen(total);
  for (auto& flag : seen) {
    flag.store(false, std::memory_order_relaxed);
  }
  std::atomic<std::size_t> consumed{0};
  std::atomic<bool> failed{false};
  std::atomic<bool> duplicate{false};

  // Consumers run concurrently with producers: a bounded queue can only accept
  // values while someone drains. The exactly-once oracle validates every value
  // against the closed range before use.
  std::vector<std::thread> consumer_threads;
  for (std::size_t c = 0; c < spec.consumers; ++c) {
    consumer_threads.emplace_back([&] {
      std::size_t empty_streak = 0;
      std::size_t expected = 0;
      while (consumed.load(std::memory_order_relaxed) < total &&
             !failed.load(std::memory_order_relaxed)) {
        auto value = queue.try_pop();
        if (!value.has_value()) {
          if (++empty_streak > kAttemptCap) {
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
        // FIFO oracle: only valid for single-producer, single-consumer runs.
        // With multiple consumers, a consumer can claim value n and stall while
        // another claims and records n + 1, so completion order is not a valid
        // FIFO signal. With multiple producers, reservation order is not push
        // order, so values need not be globally ascending.
        if (check_fifo && *value != expected) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        ++expected;
        if (seen[*value].exchange(true, std::memory_order_relaxed)) {
          duplicate.store(true, std::memory_order_relaxed);
        }
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::vector<std::thread> producer_threads;
  for (std::size_t p = 0; p < spec.producers; ++p) {
    producer_threads.emplace_back([&, p] {
      const std::size_t first = p * spec.values_per_producer;
      std::size_t attempts = 0;
      for (std::size_t value = 0; value < spec.values_per_producer &&
                              !failed.load(std::memory_order_relaxed);) {
        if (queue.try_push(first + value)) {
          ++value;
          attempts = 0;
        } else if (++attempts > kAttemptCap) {
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
  for (auto& consumer : consumer_threads) {
    consumer.join();
  }

  INFO("id=" << spec.id << " producers=" << spec.producers
               << " consumers=" << spec.consumers << " capacity=" << Capacity
               << " values=" << spec.values_per_producer);
  REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
  REQUIRE_FALSE(duplicate.load(std::memory_order_relaxed));
  REQUIRE(consumed.load(std::memory_order_relaxed) == total);
  for (const auto& flag : seen) {
    REQUIRE(flag.load(std::memory_order_relaxed));
  }
}

void run_mpmc_dispatch(const run_spec& spec) {
  const bool check_fifo = spec.producers == 1 && spec.consumers == 1;
  switch (spec.capacity) {
    case 2:
      run_mpmc_history<2>(spec, check_fifo);
      break;
    case 8:
      run_mpmc_history<8>(spec, check_fifo);
      break;
    default:
      run_mpmc_history<64>(spec, check_fifo);
      break;
  }
}

template <std::size_t Capacity, bool Padded>
void run_spsc_history(const spsc_spec& spec) {
  norn::spsc_queue<std::size_t, Capacity, Padded> queue;
  const std::size_t count = spec.values_per_producer;
  std::atomic<bool> failed{false};
  std::atomic<std::size_t> consumed{0};

  std::thread producer([&] {
    std::size_t attempts = 0;
    for (std::size_t value = 0; value < count && !failed.load(std::memory_order_relaxed);) {
      if (queue.try_push(value)) {
        ++value;
        attempts = 0;
      } else if (++attempts > kAttemptCap) {
        failed.store(true, std::memory_order_relaxed);
        return;
      } else {
        std::this_thread::yield();
      }
    }
  });
  std::thread consumer([&] {
    std::size_t expected = 0;
    std::size_t empty_streak = 0;
    while (consumed.load(std::memory_order_relaxed) < count &&
           !failed.load(std::memory_order_relaxed)) {
      auto value = queue.try_pop();
      if (!value.has_value()) {
        if (++empty_streak > kAttemptCap) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        std::this_thread::yield();
        continue;
      }
      empty_streak = 0;
      // SPSC: a single producer's pushes must be consumed in ascending order.
      if (*value != expected) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
      ++expected;
      consumed.fetch_add(1, std::memory_order_relaxed);
    }
  });

  producer.join();
  consumer.join();

  INFO("id=" << spec.id << " capacity=" << Capacity << " padded=" << Padded
               << " values=" << spec.values_per_producer);
  REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
  REQUIRE(consumed.load(std::memory_order_relaxed) == count);
}

void run_spsc_dispatch(const spsc_spec& spec) {
  if (spec.capacity == 8) {
    run_spsc_history<8, false>(spec);
  } else if (spec.padded) {
    run_spsc_history<64, true>(spec);
  } else {
    run_spsc_history<64, false>(spec);
  }
}

}  // namespace

TEST_CASE("MPMC manifest histories consume every value exactly once") {
  for (const auto& spec : kMpmcManifest) {
    run_mpmc_dispatch(spec);
  }
}

TEST_CASE("SPSC manifest histories preserve ascending order") {
  for (const auto& spec : kSpscManifest) {
    run_spsc_dispatch(spec);
  }
}