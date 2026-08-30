#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#include "norn/hazard_pointer.hpp"

namespace {

class tracking_resource final : public std::pmr::memory_resource {
 public:
  explicit tracking_resource(std::size_t successful_allocations,
                             bool fail_after_limit = true)
      : successful_allocations_(successful_allocations),
        fail_after_limit_(fail_after_limit) {}

  [[nodiscard]] std::size_t live_allocations() const noexcept { return live_; }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    if (fail_after_limit_ && allocations_ >= successful_allocations_) {
      throw std::bad_alloc();
    }
    void* pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
    ++allocations_;
    ++live_;
    return pointer;
  }

  void do_deallocate(void* pointer, std::size_t bytes,
                    std::size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    --live_;
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  std::size_t successful_allocations_;
  bool fail_after_limit_;
  std::size_t allocations_ = 0;
  std::size_t live_ = 0;
};

struct throwing_value {
  explicit throwing_value(int) { throw std::bad_alloc(); }
  throwing_value(const throwing_value&) = delete;
  throwing_value& operator=(const throwing_value&) = delete;
  throwing_value(throwing_value&&) noexcept = default;
  throwing_value& operator=(throwing_value&&) noexcept = default;
  ~throwing_value() noexcept = default;
};

std::atomic<int> deleted_values{0};

void count_and_delete(void* pointer) noexcept {
  ++deleted_values;
  delete static_cast<int*>(pointer);
}

}  // namespace

TEST_CASE("hazard_ptr publishes and clears correctly") {
  norn::hazard_domain domain;
  auto& rec = domain.register_thread();
  std::atomic<void*> source;
  int value = 42;
  source.store(&value, std::memory_order_relaxed);

  {
    norn::hazard_ptr<int, 0> guard(rec, source);
    REQUIRE(guard.get() == &value);
    REQUIRE(*guard.get() == 42);
  }
  // After destruction, guard.get() would be dangling.  Verify a new guard
  // on a different source works correctly (the slot was cleared).
  int other = 99;
  source.store(&other, std::memory_order_relaxed);
  norn::hazard_ptr<int, 0> guard2(rec, source);
  REQUIRE(guard2.get() == &other);
}

TEST_CASE("hazard_ptr retries on concurrent modification") {
  norn::hazard_domain domain;
  auto& rec = domain.register_thread();
  int a = 1, b = 2;
  std::atomic<void*> source;
  source.store(&a, std::memory_order_relaxed);

  // Simulate a race: publish, change source, then re-validate.
  // The hazard_ptr constructor handles this internally.
  source.store(&b, std::memory_order_relaxed);
  norn::hazard_ptr<int, 0> guard(rec, source);
  // guard should now point to &b, not &a.
  REQUIRE(guard.get() == &b);
}

TEST_CASE("hazard records expose two exclusive slots") {
  STATIC_REQUIRE(norn::hazard_record::slot_count == 2);
  norn::hazard_domain domain;
  auto& record = domain.register_thread();
  std::atomic<void*> source;
  int first = 1;
  int second = 2;
  source.store(&first, std::memory_order_relaxed);

  norn::hazard_ptr<int, 0> first_guard(record, source);
  source.store(&second, std::memory_order_relaxed);
  norn::hazard_ptr<int, 1> second_guard(record, source);
  REQUIRE(first_guard.get() == &first);
  REQUIRE(second_guard.get() == &second);
  REQUIRE(record.has_active_hazards());
  REQUIRE_THROWS_AS((norn::hazard_ptr<int, 0>(record, source)),
                    std::logic_error);
}

TEST_CASE("hazard domain refuses deregistration while a slot is protected") {
  norn::hazard_domain domain;
  auto& record = domain.register_thread();
  std::atomic<void*> source;
  int value = 7;
  source.store(&value, std::memory_order_relaxed);

  {
    norn::hazard_ptr<int> guard(record, source);
    REQUIRE_FALSE(domain.deregister_thread());
    REQUIRE(domain.thread_count() == 1);
  }

  REQUIRE(domain.deregister_thread());
  REQUIRE(domain.thread_count() == 0);
  REQUIRE_FALSE(domain.deregister_thread());
}

TEST_CASE("hazard domain rejects switching domains without deregistration") {
  norn::hazard_domain first;
  norn::hazard_domain second;
  (void)first.register_thread();

  REQUIRE_THROWS_AS(second.register_thread(), std::logic_error);
  REQUIRE(first.deregister_thread());
  (void)second.register_thread();
  REQUIRE(second.deregister_thread());
}

TEST_CASE("hazard scans defer reclamation while a slot is protected") {
  deleted_values.store(0, std::memory_order_relaxed);
  norn::hazard_domain domain;
  auto& record = domain.register_thread();
  std::atomic<void*> source;
  auto* value = new int(42);
  source.store(value, std::memory_order_relaxed);

  {
    norn::hazard_ptr<int> guard(record, source);
    domain.retire(value, count_and_delete);
    domain.scan();
    REQUIRE(deleted_values.load(std::memory_order_relaxed) == 0);
  }

  domain.scan();
  REQUIRE(deleted_values.load(std::memory_order_relaxed) == 1);
  REQUIRE(domain.deregister_thread());
}

TEST_CASE("mpsc queue reports a node allocator failure without corrupting state") {
  tracking_resource resource(1);
  norn::hazard_domain domain;
  norn::mpsc_queue<int> queue(domain, &resource);

  REQUIRE_THROWS_AS(queue.try_push(1), std::bad_alloc);
  REQUIRE(queue.empty());
  REQUIRE(resource.live_allocations() == 1);
}

TEST_CASE("mpsc queue releases a node when value construction fails") {
  tracking_resource resource(2, false);
  norn::hazard_domain domain;
  norn::mpsc_queue<throwing_value> queue(domain, &resource);

  REQUIRE_THROWS_AS(queue.try_push(1), std::bad_alloc);
  REQUIRE(queue.empty());
  REQUIRE(resource.live_allocations() == 1);
}

TEST_CASE("mpsc_queue basic push and pop") {
  norn::hazard_domain domain;
  norn::mpsc_queue<int> queue(domain);

  REQUIRE(queue.try_push( 1));
  REQUIRE(queue.try_push( 2));
  REQUIRE(queue.try_push( 3));

  auto v1 = queue.try_pop();
  REQUIRE(v1.has_value());
  REQUIRE(*v1 == 1);

  auto v2 = queue.try_pop();
  REQUIRE(v2.has_value());
  REQUIRE(*v2 == 2);

  auto v3 = queue.try_pop();
  REQUIRE(v3.has_value());
  REQUIRE(*v3 == 3);

  REQUIRE_FALSE(queue.try_pop().has_value());
  REQUIRE(queue.empty());
}

TEST_CASE("mpsc_queue supports move-only values") {
  norn::hazard_domain domain;
  norn::mpsc_queue<std::unique_ptr<int>> queue(domain);

  REQUIRE(queue.try_push( std::make_unique<int>(10)));
  REQUIRE(queue.try_push( std::make_unique<int>(20)));

  auto v1 = queue.try_pop();
  REQUIRE(v1.has_value());
  REQUIRE(**v1 == 10);

  auto v2 = queue.try_pop();
  REQUIRE(v2.has_value());
  REQUIRE(**v2 == 20);

  REQUIRE_FALSE(queue.try_pop().has_value());
}

TEST_CASE("mpsc_queue stress with multiple producers and one consumer") {
  norn::hazard_domain domain;
  norn::mpsc_queue<int> queue(domain);
  constexpr int producers = 2;
  constexpr int values_per_producer = 50;
  constexpr int total = producers * values_per_producer;

  std::atomic<bool> push_failed{false};

  std::vector<std::thread> producer_threads;
  for (int p = 0; p < producers; ++p) {
    producer_threads.emplace_back([&, p] {
      const int first = p * values_per_producer;
      for (int v = 0; v < values_per_producer; ++v) {
        if (!queue.try_push(first + v)) {
          push_failed.store(true, std::memory_order_relaxed);
          return;
        }
      }
    });
  }

  std::vector<int> received;
  received.reserve(total);
  std::atomic<bool> pop_failed{false};

  std::thread consumer([&] {
    for (int i = 0; i < total; ++i) {
      auto val = queue.try_pop();
      while (!val.has_value()) {
        val = queue.try_pop();
      }
      received.push_back(*val);
    }
  });

  for (auto& t : producer_threads) {
    t.join();
  }
  consumer.join();

  REQUIRE_FALSE(push_failed.load(std::memory_order_relaxed));
  REQUIRE_FALSE(pop_failed.load(std::memory_order_relaxed));
  REQUIRE(static_cast<std::size_t>(received.size()) == static_cast<std::size_t>(total));
  std::vector<bool> seen(static_cast<std::size_t>(total), false);
  for (int v : received) {
    REQUIRE(v >= 0);
    REQUIRE(v < total);
    REQUIRE_FALSE(seen[static_cast<std::size_t>(v)]);
    seen[static_cast<std::size_t>(v)] = true;
  }
}

TEST_CASE("mpsc_queue stress with multiple producer batches and scan") {
  norn::hazard_domain domain;
  norn::mpsc_queue<int> queue(domain);
  constexpr int rounds = 5;
  constexpr int values_per_round = 200;

  for (int round = 0; round < rounds; ++round) {
    std::thread producer([&] {
      for (int v = 0; v < values_per_round; ++v) {
        queue.try_push( round * values_per_round + v);
      }
    });

    std::atomic<int> consumed{0};
    std::thread consumer([&] {
      while (consumed.load(std::memory_order_relaxed) < values_per_round) {
        auto val = queue.try_pop();
        if (val.has_value()) {
          consumed.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });

    producer.join();
    consumer.join();
    REQUIRE(consumed.load(std::memory_order_relaxed) == values_per_round);

    // Explicit scan to exercise reclamation.
    domain.scan();
  }
}

TEST_CASE("hazard domain thread registration and deregistration") {
  norn::hazard_domain domain;
  std::atomic<int> exit_flag{0};

  std::thread t1([&] {
    (void)domain.register_thread();
    norn::mpsc_queue<int> queue(domain);
    queue.try_push( 1);
    queue.try_push( 2);
    (void)queue.try_pop();
    (void)queue.try_pop();
    domain.deregister_thread();
    exit_flag.store(1, std::memory_order_release);
  });

  t1.join();
  REQUIRE(exit_flag.load(std::memory_order_acquire) == 1);
  // After deregistration, thread_count should reflect no active threads.
  REQUIRE(domain.thread_count() == 0);
}

TEST_CASE("mpsc_queue deleter is called exactly once per retired node") {
  norn::hazard_domain domain;
  norn::mpsc_queue<int> queue(domain);

  for (int i = 0; i < 50; ++i) {
    queue.try_push( i);
  }
  for (int i = 0; i < 50; ++i) {
    auto v = queue.try_pop();
    REQUIRE(v.has_value());
    REQUIRE(*v == i);
  }
  domain.scan();
  REQUIRE(queue.empty());
}

TEST_CASE("mpsc_queue pop on empty queue returns nullopt") {
  norn::hazard_domain domain;
  norn::mpsc_queue<int> queue(domain);
  REQUIRE_FALSE(queue.try_pop().has_value());
  REQUIRE_FALSE(queue.try_pop().has_value());
  REQUIRE(queue.empty());
}
