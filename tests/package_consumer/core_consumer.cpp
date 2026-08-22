// External consumer of the installed norn::core target: exercises the
// canonical core helpers straight from the installed headers.

#include <atomic>
#include <cstddef>
#include <iostream>

#include <norn/core/atomic.hpp>
#include <norn/core/backoff.hpp>
#include <norn/core/cache_line.hpp>
#include <norn/core/cpu_relax.hpp>

int main() {
  static_assert(norn::cache_line_size > 0);
  static_assert(alignof(norn::cache_aligned<int>) >= norn::cache_line_size);

  norn::isolated_atomic<int> counter{};
  counter.value.store(41, std::memory_order_relaxed);
  counter.value.fetch_add(1, std::memory_order_relaxed);

  norn::backoff::bounded backoff;
  backoff.reset();
  norn::cpu_relax();
  backoff();

  norn::backoff::yield yielding_backoff;
  yielding_backoff.reset();
  yielding_backoff();

  if (counter.value.load(std::memory_order_relaxed) != 42) {
    return 1;
  }
  std::cout << "core consumer ok\n";
  return 0;
}
