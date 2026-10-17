// External consumer of the installed norn::core target: exercises the
// canonical core helpers straight from the installed headers.

#include <atomic>
#include <cstddef>
#include <iostream>

#include <norn/core/atomic.hpp>
#include <norn/core/cache_line.hpp>

int main() {
  static_assert(norn::cache_line_size > 0);
  static_assert(alignof(norn::cache_aligned<int>) >= norn::cache_line_size);

  norn::isolated_atomic<int> counter{};
  counter.value.store(41, std::memory_order_relaxed);
  counter.value.fetch_add(1, std::memory_order_relaxed);

  if (counter.value.load(std::memory_order_relaxed) != 42) {
    return 1;
  }
  std::cout << "core consumer ok\n";
  return 0;
}
