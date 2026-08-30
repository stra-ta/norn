#pragma once

#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "norn/hazard/pointer.hpp"

namespace norn {

// Multiple-producer, single-consumer linked queue over heap nodes.  The
// caller owns the hazard domain and, when supplied, the memory resource for
// the complete lifetime of the queue and all deferred reclamation.
template <typename T>
class mpsc_queue {
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "mpsc_queue requires nothrow move-constructible T");
  static_assert(std::is_nothrow_destructible_v<T>,
                "mpsc_queue requires nothrow destructible T");

  struct node {
    alignas(T) std::byte storage[sizeof(T)];
    std::atomic<void*> next{nullptr};
    bool has_value = false;
    std::pmr::memory_resource* resource;

    explicit node(std::pmr::memory_resource* node_resource) noexcept
        : resource(node_resource) {}
  };

 public:
  explicit mpsc_queue(
      hazard_domain& domain,
      std::pmr::memory_resource* resource = std::pmr::new_delete_resource())
      : domain_(domain), resource_(resource) {
    if (resource_ == nullptr) {
      throw std::invalid_argument("mpsc_queue requires a memory resource");
    }
    sentinel_ = allocate_node();
    head_.store(sentinel_, std::memory_order_relaxed);
    tail_.store(sentinel_, std::memory_order_relaxed);
  }

  mpsc_queue(const mpsc_queue&) = delete;
  mpsc_queue& operator=(const mpsc_queue&) = delete;

  ~mpsc_queue() {
    // Destruction requires quiescence.  The domain must outlive this queue so
    // every deferred node has a valid resource and deleter.
    node* current = static_cast<node*>(head_.load(std::memory_order_relaxed));
    while (current != nullptr) {
      node* next =
          static_cast<node*>(current->next.load(std::memory_order_relaxed));
      destroy_node(current);
      current = next;
    }
  }

  template <typename... Args>
  bool try_push(Args&&... args) {
    auto* record = &domain_.register_thread();
    node* new_node = allocate_node();
    try {
      new_node->has_value = true;
      ::new (static_cast<void*>(new_node->storage)) T(
          std::forward<Args>(args)...);
    } catch (...) {
      // A throwing payload constructor must not leak the node allocated above.
      new_node->has_value = false;
      destroy_node(new_node);
      throw;
    }

    for (;;) {
      hazard_ptr<node, 0> tail_guard(*record, tail_);
      node* tail = tail_guard.get();
      void* next_raw = tail->next.load(std::memory_order_acquire);

      void* tail_expected = tail;
      if (next_raw != nullptr) {
        node* next = static_cast<node*>(next_raw);
        tail_.compare_exchange_strong(
            tail_expected, next, std::memory_order_release,
            std::memory_order_relaxed);
        continue;
      }

      if (tail->next.compare_exchange_strong(
              next_raw, static_cast<void*>(new_node),
              std::memory_order_release, std::memory_order_relaxed)) {
        void* tail_expected2 = tail;
        tail_.compare_exchange_strong(
            tail_expected2, static_cast<void*>(new_node),
            std::memory_order_release, std::memory_order_relaxed);
        return true;
      }
    }
  }

  std::optional<T> try_pop() {
    auto* record = &domain_.register_thread();

    for (;;) {
      hazard_ptr<node, 0> head_guard(*record, head_);
      node* head = head_guard.get();
      hazard_ptr<node, 1> next_guard(*record, head->next);
      node* next_value = next_guard.get();

      if (head != static_cast<node*>(head_.load(std::memory_order_acquire))) {
        continue;
      }

      if (next_value == nullptr) {
        return std::nullopt;
      }

      // If head == tail but next is non-null, tail is lagging.  Help advance
      // tail before attempting to pop so a producer cannot dereference a
      // retired node through the stale tail pointer.
      node* tail = static_cast<node*>(tail_.load(std::memory_order_acquire));
      if (head == tail) {
        void* expected = tail;
        tail_.compare_exchange_strong(
            expected, next_value, std::memory_order_release,
            std::memory_order_relaxed);
        continue;
      }

      void* expected = head;
      if (head_.compare_exchange_strong(
              expected, static_cast<void*>(next_value),
              std::memory_order_release, std::memory_order_relaxed)) {
        T value = std::move(
            *std::launder(reinterpret_cast<T*>(next_value->storage)));
        domain_.retire(head, head->has_value ? delete_node : delete_sentinel);
        return value;
      }
    }
  }

  [[nodiscard]] bool empty() const noexcept {
    node* head = static_cast<node*>(head_.load(std::memory_order_acquire));
    return head->next.load(std::memory_order_acquire) == nullptr;
  }

 private:
  node* allocate_node() {
    void* storage = resource_->allocate(sizeof(node), alignof(node));
    try {
      return ::new (storage) node(resource_);
    } catch (...) {
      resource_->deallocate(storage, sizeof(node), alignof(node));
      throw;
    }
  }

  static void destroy_node(node* value) noexcept {
    if (value == nullptr) {
      return;
    }
    if (value->has_value) {
      std::launder(reinterpret_cast<T*>(value->storage))->~T();
    }
    std::pmr::memory_resource* resource = value->resource;
    value->~node();
    resource->deallocate(value, sizeof(node), alignof(node));
  }

  static void delete_sentinel(void* pointer) noexcept {
    destroy_node(static_cast<node*>(pointer));
  }

  static void delete_node(void* pointer) noexcept {
    destroy_node(static_cast<node*>(pointer));
  }

  hazard_domain& domain_;
  std::pmr::memory_resource* resource_;
  node* sentinel_ = nullptr;
  std::atomic<void*> head_{nullptr};
  std::atomic<void*> tail_{nullptr};
};

template <typename T>
using mpsc_linked_queue = mpsc_queue<T>;

}  // namespace norn
