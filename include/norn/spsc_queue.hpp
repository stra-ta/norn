#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <utility>

#include "norn/cache_line.hpp"

namespace norn {

// Bounded SPSC queue.
// The producer owns write_index_ and the consumer owns read_index_.
// Each side observes the other side's index only through acquire loads.
template <bool Padded>
struct spsc_index {
  std::atomic<std::size_t> value{0};
};

template <>
struct alignas(cache_line_size) spsc_index<true> {
  std::atomic<std::size_t> value{0};
};

// `SequentiallyConsistent` is an isolated diagnostic configuration used by the
// hardware campaign. The default preserves the acquire/release protocol.
template <typename T, std::size_t Capacity, bool Padded = false, bool SequentiallyConsistent = false>
class spsc_queue {
  static_assert(Capacity > 0, "queue capacity must be positive");

  struct slot {
    alignas(T) std::byte storage[sizeof(T)];
  };

 public:
  spsc_queue() = default;
  spsc_queue(const spsc_queue&) = delete;
  spsc_queue& operator=(const spsc_queue&) = delete;

  ~spsc_queue() { clear(); }

  bool try_push(const T& value) { return emplace(value); }

  bool try_push(T&& value) { return emplace(std::move(value)); }

  template <typename... Args>
  bool emplace(Args&&... args) {
    const std::size_t write = write_index_.value.load(std::memory_order_relaxed);
    const std::size_t read = read_index_.value.load(peer_load_order);
    if (write - read == Capacity) {
      return false;
    }

    // Construction happens before the release publication below. If
    // construction throws, write_index_ is unchanged and the slot is reused.
    ::new (static_cast<void*>(storage_[write % Capacity].storage)) T(
        std::forward<Args>(args)...);
    write_index_.value.store(write + 1, publication_store_order);
    return true;
  }

  bool try_pop(T& value) {
    const std::size_t read = read_index_.value.load(std::memory_order_relaxed);
    const std::size_t write = write_index_.value.load(peer_load_order);
    if (read == write) {
      return false;
    }

    T* element = pointer_at(read);
    value = std::move(*element);
    element->~T();
    read_index_.value.store(read + 1, publication_store_order);
    return true;
  }

  [[nodiscard]] std::optional<T> try_pop() {
    const std::size_t read = read_index_.value.load(std::memory_order_relaxed);
    const std::size_t write = write_index_.value.load(peer_load_order);
    if (read == write) {
      return std::nullopt;
    }

    T* element = pointer_at(read);
    // Finish the move before ending the slot's lifetime. If T's move
    // constructor throws, the source object remains alive and the consumer
    // can retry without advancing read_index_.
    std::optional<T> value(std::in_place, std::move(*element));
    element->~T();
    read_index_.value.store(read + 1, publication_store_order);
    return value;
  }

  [[nodiscard]] bool empty() const noexcept {
    return read_index_.value.load(std::memory_order_relaxed) ==
            write_index_.value.load(peer_load_order);
  }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  static constexpr std::memory_order peer_load_order =
      SequentiallyConsistent ? std::memory_order_seq_cst : std::memory_order_acquire;
  static constexpr std::memory_order publication_store_order =
      SequentiallyConsistent ? std::memory_order_seq_cst : std::memory_order_release;

  T* pointer_at(std::size_t index) noexcept {
    return std::launder(reinterpret_cast<T*>(storage_[index % Capacity].storage));
  }

  void clear() noexcept {
    const std::size_t read = read_index_.value.load(std::memory_order_relaxed);
    const std::size_t write = write_index_.value.load(std::memory_order_relaxed);
    for (std::size_t index = read; index != write; ++index) {
      pointer_at(index)->~T();
    }
  }

  std::array<slot, Capacity> storage_{};
  spsc_index<Padded> read_index_;
  spsc_index<Padded> write_index_;
};

template <typename T, std::size_t Capacity>
using spsc_queue_padded = spsc_queue<T, Capacity, true>;

// Stronger-order experimental alias. It remains semantically equivalent to the
// acquire/release baseline because seq_cst loads and stores retain the required
// release/acquire synchronization relations.
template <typename T, std::size_t Capacity, bool Padded = false>
using spsc_queue_seq_cst = spsc_queue<T, Capacity, Padded, true>;

}  // namespace norn
