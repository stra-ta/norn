#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>

#include "norn/queue/mpmc_ring.hpp"
#include "norn/queue/spsc_ring.hpp"

// ---------------------------------------------------------------------------
// Canonical SPSC ring
// ---------------------------------------------------------------------------

TEST_CASE("spsc_ring basic push and pop") {
  norn::spsc_ring<int, 4> ring;
  REQUIRE(ring.capacity() == 4);
  REQUIRE(ring.empty());
  REQUIRE_FALSE(ring.try_pop().has_value());

  REQUIRE(ring.try_push(1));
  REQUIRE(ring.try_push(2));
  REQUIRE_FALSE(ring.empty());

  REQUIRE(ring.try_push(3));
  REQUIRE(ring.try_push(4));
  REQUIRE_FALSE(ring.try_push(5));  // full

  int popped = 0;
  REQUIRE(ring.try_pop(popped));
  REQUIRE(popped == 1);
  REQUIRE(ring.try_pop(popped));
  REQUIRE(popped == 2);

  // The freed slot becomes reusable.
  REQUIRE(ring.try_push(5));

  auto third = ring.try_pop();
  REQUIRE(third == std::optional<int>(3));
  auto fourth = ring.try_pop();
  REQUIRE(fourth == std::optional<int>(4));
  auto fifth = ring.try_pop();
  REQUIRE(fifth == std::optional<int>(5));
  REQUIRE(ring.empty());
  REQUIRE_FALSE(ring.try_pop().has_value());
}

TEST_CASE("spsc_ring emplace and move-only values") {
  norn::spsc_ring<std::unique_ptr<int>, 2> ring;

  REQUIRE(ring.emplace(std::make_unique<int>(10)));
  std::unique_ptr<int> moved = std::make_unique<int>(20);
  REQUIRE(ring.try_push(std::move(moved)));

  auto first = ring.try_pop();
  REQUIRE(first.has_value());
  REQUIRE(**first == 10);
  auto second = ring.try_pop();
  REQUIRE(second.has_value());
  REQUIRE(**second == 20);
  REQUIRE(ring.empty());
}

TEST_CASE("padded spsc_ring isolates indices on their own cache lines") {
  static_assert(alignof(norn::spsc_index<true>) >= norn::cache_line_size);
  REQUIRE(alignof(norn::spsc_ring<int, 4, true>) >= norn::cache_line_size);
}

// ---------------------------------------------------------------------------
// Canonical MPMC ring
// ---------------------------------------------------------------------------

TEST_CASE("mpmc_ring basic push and pop") {
  norn::mpmc_ring<int, 8> ring;
  REQUIRE(ring.capacity() == 8);
  REQUIRE(ring.empty());
  REQUIRE_FALSE(ring.try_pop().has_value());

  REQUIRE(ring.try_push(1));
  REQUIRE(ring.try_push(2));
  REQUIRE(ring.emplace(3));
  REQUIRE_FALSE(ring.empty());

  int popped = 0;
  REQUIRE(ring.try_pop(popped));
  REQUIRE(popped == 1);
  auto second = ring.try_pop();
  REQUIRE(second == std::optional<int>(2));
  auto third = ring.try_pop();
  REQUIRE(third == std::optional<int>(3));
  REQUIRE(ring.empty());
  REQUIRE_FALSE(ring.try_pop().has_value());
}

TEST_CASE("mpmc_ring supports move-only values") {
  norn::mpmc_ring<std::unique_ptr<int>, 4> ring;

  std::unique_ptr<int> moved = std::make_unique<int>(42);
  REQUIRE(ring.try_push(std::move(moved)));

  auto value = ring.try_pop();
  REQUIRE(value.has_value());
  REQUIRE(**value == 42);
  REQUIRE(ring.empty());
}
