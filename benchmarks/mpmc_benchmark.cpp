#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "norn/mpmc_queue.hpp"

namespace {

template <std::size_t Producers, std::size_t Consumers>
void mpmc_throughput(benchmark::State& state) {
  const auto batch = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    norn::mpmc_queue<std::size_t, 1024> queue;
    std::atomic<std::size_t> consumed{0};
    const std::size_t total = batch * Producers;

    std::vector<std::thread> producer_threads;
    for (std::size_t producer = 0; producer < Producers; ++producer) {
      producer_threads.emplace_back([&] {
        for (std::size_t value = 0; value < batch;) {
          if (queue.try_push(value)) {
            ++value;
          } else {
            std::this_thread::yield();
          }
        }
      });
    }
    std::vector<std::thread> consumer_threads;
    for (std::size_t consumer = 0; consumer < Consumers; ++consumer) {
      consumer_threads.emplace_back([&] {
        while (consumed.load(std::memory_order_relaxed) < total) {
          auto result = queue.try_pop();
          if (result.has_value()) {
            benchmark::DoNotOptimize(*result);
            consumed.fetch_add(1, std::memory_order_relaxed);
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
    benchmark::DoNotOptimize(consumed.load());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(batch * Producers));
}

}  // namespace

BENCHMARK_TEMPLATE(mpmc_throughput, 1, 1)->Arg(100'000)->UseRealTime();
BENCHMARK_TEMPLATE(mpmc_throughput, 2, 2)->Arg(100'000)->UseRealTime();
BENCHMARK_TEMPLATE(mpmc_throughput, 4, 4)->Arg(100'000)->UseRealTime();