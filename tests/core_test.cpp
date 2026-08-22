#include <catch2/catch_test_macros.hpp>

#include <atomic>

#include "norn/cache_line.hpp"
#include "norn/core/alignment.hpp"
#include "norn/core/atomic.hpp"
#include "norn/core/backoff.hpp"
#include "norn/core/cache_line.hpp"
#include "norn/core/cpu_relax.hpp"

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

// ---------------------------------------------------------------------------
// cpu_relax
// ---------------------------------------------------------------------------

TEST_CASE("cpu_relax is noexcept and callable") {
  STATIC_REQUIRE(noexcept(norn::cpu_relax()));
  norn::cpu_relax();
}

// ---------------------------------------------------------------------------
// backoff policies
// ---------------------------------------------------------------------------

namespace {

// Test-only Operations seam: counts wait actions instead of performing them,
// so sequence oracles never touch clocks, sleeps, or the real scheduler.
struct counting_wait {
  static inline int relax_calls = 0;
  static inline int yield_calls = 0;

  static void relax() noexcept { ++relax_calls; }
  static void yield() noexcept { ++yield_calls; }

  static void reset_counters() noexcept {
    relax_calls = 0;
    yield_calls = 0;
  }
};

}  // namespace

TEST_CASE("tight policy performs exactly one relax per call and reset stays usable") {
  counting_wait::reset_counters();
  norn::backoff::detail::tight_policy<counting_wait> b;

  b();
  b();
  b();
  REQUIRE(counting_wait::relax_calls == 3);
  REQUIRE(counting_wait::yield_calls == 0);

  b.reset();

  b();
  REQUIRE(counting_wait::relax_calls == 4);
  REQUIRE(counting_wait::yield_calls == 0);
}

TEST_CASE("yield policy performs exactly one scheduler yield per call") {
  counting_wait::reset_counters();
  norn::backoff::detail::yield_policy<counting_wait> b;

  b();
  b();
  b();
  REQUIRE(counting_wait::yield_calls == 3);
  REQUIRE(counting_wait::relax_calls == 0);

  b.reset();

  b();
  REQUIRE(counting_wait::yield_calls == 4);
  REQUIRE(counting_wait::relax_calls == 0);
}

TEST_CASE("bounded policy relaxes for 63 calls then yields once per 64-call cycle") {
  counting_wait::reset_counters();
  norn::backoff::detail::bounded_policy<counting_wait> b;

  // Calls 1-63: one relax each.
  for (int i = 0; i < 63; ++i) {
    b();
    REQUIRE(counting_wait::relax_calls == i + 1);
    REQUIRE(counting_wait::yield_calls == 0);
  }

  // Call 64: a single yield with no relax.
  b();
  REQUIRE(counting_wait::relax_calls == 63);
  REQUIRE(counting_wait::yield_calls == 1);

  // Call 65: the cycle restarted from call 1.
  b();
  REQUIRE(counting_wait::relax_calls == 64);
  REQUIRE(counting_wait::yield_calls == 1);

  // Explicit reset returns to call-1 behavior.
  b.reset();
  counting_wait::reset_counters();
  b();
  REQUIRE(counting_wait::relax_calls == 1);
  REQUIRE(counting_wait::yield_calls == 0);
}

TEST_CASE("exponential policy doubles spins up to 64 then yields on invocation 64") {
  counting_wait::reset_counters();
  norn::backoff::detail::exponential_policy<counting_wait> b;

  int previous_relaxes = 0;
  const int expected_deltas[] = {1, 2, 4, 8, 16, 32, 64};
  for (int delta : expected_deltas) {
    b();
    REQUIRE(counting_wait::relax_calls - previous_relaxes == delta);
    REQUIRE(counting_wait::yield_calls == 0);
    previous_relaxes = counting_wait::relax_calls;
  }

  // Steps saturate at 64 relaxes through invocation 63.
  for (int i = 8; i <= 63; ++i) {
    b();
    REQUIRE(counting_wait::relax_calls - previous_relaxes == 64);
    REQUIRE(counting_wait::yield_calls == 0);
    previous_relaxes = counting_wait::relax_calls;
  }
  REQUIRE(previous_relaxes == 3711);  // 1+2+4+...+32 + 57*64

  // Invocation 64 still relaxes its saturated step, then yields once and
  // wraps back to the first step.
  b();
  REQUIRE(counting_wait::relax_calls - previous_relaxes == 64);
  REQUIRE(counting_wait::relax_calls == 3775);
  REQUIRE(counting_wait::yield_calls == 1);

  // Invocation 65 behaves like invocation 1 again.
  b();
  REQUIRE(counting_wait::relax_calls == 3776);
  REQUIRE(counting_wait::yield_calls == 1);

  // Explicit reset returns to the first step.
  b.reset();
  counting_wait::reset_counters();
  b();
  REQUIRE(counting_wait::relax_calls == 1);
  REQUIRE(counting_wait::yield_calls == 0);
}

TEST_CASE("backoff policies are nothrow callable and nothrow resettable") {
  norn::backoff::tight tight;
  norn::backoff::yield yielding;
  norn::backoff::bounded bounded;
  norn::backoff::exponential exponential;

  STATIC_REQUIRE(noexcept(tight()));
  STATIC_REQUIRE(noexcept(tight.reset()));
  STATIC_REQUIRE(noexcept(yielding()));
  STATIC_REQUIRE(noexcept(yielding.reset()));
  STATIC_REQUIRE(noexcept(bounded()));
  STATIC_REQUIRE(noexcept(bounded.reset()));
  STATIC_REQUIRE(noexcept(exponential()));
  STATIC_REQUIRE(noexcept(exponential.reset()));
}
