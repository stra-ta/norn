#include <benchmark/benchmark.h>

#include <cstddef>
#include <thread>

#include "norn/mutex_queue.hpp"
#include "norn/spsc_queue.hpp"

namespace {

template <typename Queue>
void spsc_throughput(benchmark::State& state) {
  const auto batch = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    Queue queue;
    std::thread producer([&] {
      for (std::size_t value = 0; value < batch; ++value) {
        while (!queue.try_push(value)) {
          std::this_thread::yield();
        }
      }
    });
    std::thread consumer([&] {
      for (std::size_t value = 0; value < batch; ++value) {
        while (true) {
          auto result = queue.try_pop();
          if (result.has_value()) {
            benchmark::DoNotOptimize(*result);
            break;
          }
          std::this_thread::yield();
        }
      }
    });
    producer.join();
    consumer.join();
    benchmark::DoNotOptimize(queue.empty());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(batch));
}

void bounded_mutex_queue_throughput(benchmark::State& state) {
  norn::bounded_mutex_queue<std::size_t, 1024> queue;
  for (auto _ : state) {
    benchmark::DoNotOptimize(queue.try_push(static_cast<std::size_t>(state.iterations())));
    std::size_t value = 0;
    benchmark::DoNotOptimize(queue.try_pop(value));
    benchmark::DoNotOptimize(value);
  }
}

}  // namespace

BENCHMARK(bounded_mutex_queue_throughput)->UseRealTime();
BENCHMARK_TEMPLATE(spsc_throughput, norn::spsc_queue<std::size_t, 1024>)
    ->Arg(100'000)
    ->UseRealTime();
BENCHMARK_TEMPLATE(spsc_throughput, norn::spsc_queue_padded<std::size_t, 1024>)
    ->Arg(100'000)
    ->UseRealTime();
