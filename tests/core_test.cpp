#include <catch2/catch_test_macros.hpp>

#include <atomic>

#include "norn/core/alignment.hpp"
#include "norn/core/atomic.hpp"
#include "norn/core/cache_line.hpp"

// ---------------------------------------------------------------------------
// cache_line
// ---------------------------------------------------------------------------

TEST_CASE("cache_line_size is a positive power of two") {
  REQUIRE(norn::cache_line_size > 0);
  REQUIRE((norn::cache_line_size & (norn::cache_line_size - 1)) == 0);
}

// ---------------------------------------------------------------------------
// cache_aligned
// ---------------------------------------------------------------------------

TEST_CASE("cache_aligned alignment is at least cache_line_size") {
  REQUIRE(alignof(norn::cache_aligned<int>) >= norn::cache_line_size);
}

TEST_CASE("cache_aligned stores and retrieves a value") {
  norn::cache_aligned<int> a{42};
  REQUIRE(a.value == 42);
  a.value = 7;
  REQUIRE(a.value == 7);
}

// ---------------------------------------------------------------------------
// isolated_atomic
// ---------------------------------------------------------------------------

TEST_CASE("isolated_atomic alignment is at least cache_line_size") {
  REQUIRE(alignof(norn::isolated_atomic<int>) >= norn::cache_line_size);
}

TEST_CASE("isolated_atomic load and store") {
  norn::isolated_atomic<int> a;
  a.value.store(10, std::memory_order_relaxed);
  REQUIRE(a.value.load(std::memory_order_relaxed) == 10);
  a.value.store(20, std::memory_order_relaxed);
  REQUIRE(a.value.load(std::memory_order_relaxed) == 20);
}
