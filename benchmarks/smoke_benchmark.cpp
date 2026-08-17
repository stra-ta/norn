#include <benchmark/benchmark.h>

static void norn_benchmark_target(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(state.iterations());
  }
}
BENCHMARK(norn_benchmark_target);
