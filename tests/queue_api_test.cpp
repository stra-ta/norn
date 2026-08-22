#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

// Canonical 0.2 queue headers.
#include "norn/queue/mpmc_ring.hpp"
#include "norn/queue/mpsc_linked_queue.hpp"
#include "norn/queue/spsc_ring.hpp"

// 0.1 compatibility headers.
#include "norn/mpmc_queue.hpp"
#include "norn/spsc_queue.hpp"

// ---------------------------------------------------------------------------
// Name compatibility: 0.1 names are aliases for the canonical 0.2 types
// ---------------------------------------------------------------------------

static_assert(std::same_as<norn::spsc_queue<int, 8>, norn::spsc_ring<int, 8>>);
static_assert(std::same_as<norn::spsc_queue<int, 8, true>, norn::spsc_ring<int, 8, true>>);
static_assert(
    std::same_as<norn::spsc_queue<int, 8, true, true>, norn::spsc_ring<int, 8, true, true>>);
static_assert(std::same_as<norn::spsc_queue_padded<int, 8>, norn::spsc_ring_padded<int, 8>>);
static_assert(std::same_as<norn::spsc_queue_seq_cst<int, 8>, norn::spsc_ring_seq_cst<int, 8>>);
static_assert(
    std::same_as<norn::spsc_queue_seq_cst<int, 8, true>, norn::spsc_ring_seq_cst<int, 8, true>>);
static_assert(std::same_as<norn::mpmc_queue<int, 8>, norn::mpmc_ring<int, 8>>);
static_assert(std::same_as<norn::mpsc_linked_queue<int>, norn::mpsc_queue<int>>);

TEST_CASE("0.1 queue names are the canonical 0.2 types") {
  REQUIRE((std::same_as<norn::spsc_queue<int, 8>, norn::spsc_ring<int, 8>>));
  REQUIRE((std::same_as<norn::spsc_queue_padded<int, 8>, norn::spsc_ring_padded<int, 8>>));
  REQUIRE((std::same_as<norn::spsc_queue_seq_cst<int, 8>, norn::spsc_ring_seq_cst<int, 8>>));
  REQUIRE((std::same_as<norn::mpmc_queue<int, 8>, norn::mpmc_ring<int, 8>>));
  REQUIRE((std::same_as<norn::mpsc_linked_queue<int>, norn::mpsc_queue<int>>));
}

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

// ---------------------------------------------------------------------------
// Canonical MPSC linked queue
// ---------------------------------------------------------------------------

TEST_CASE("mpsc_linked_queue basic push and pop with an explicit hazard domain") {
  norn::hazard_domain domain;
  norn::mpsc_linked_queue<int> queue(domain);

  REQUIRE(queue.try_push(1));
  REQUIRE(queue.try_push(2));
  REQUIRE(queue.try_push(3));
  REQUIRE_FALSE(queue.empty());

  auto first = queue.try_pop();
  REQUIRE(first == std::optional<int>(1));
  auto second = queue.try_pop();
  REQUIRE(second == std::optional<int>(2));
  auto third = queue.try_pop();
  REQUIRE(third == std::optional<int>(3));

  REQUIRE(queue.empty());
  REQUIRE_FALSE(queue.try_pop().has_value());
}
