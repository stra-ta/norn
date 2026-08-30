#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace norn {

class hazard_domain;
class hazard_record;

struct hp_retired_entry {
  void* ptr;
  void (*deleter)(void*) noexcept;
};

// A thread may have one active hazard record.  Registering with another
// domain without first deregistering is rejected so a record cannot be left
// active in the first domain while the thread uses the second one.
namespace detail {
inline thread_local hazard_record* tls_record = nullptr;
}  // namespace detail

class hazard_record {
  friend class hazard_domain;

  static constexpr int kSlots = 2;
  std::atomic<void*> slots_[kSlots]{};
  std::atomic<bool> claimed_[kSlots]{};

 public:
  static constexpr int slot_count = kSlots;

  // This vector is retained as the record-to-domain transfer surface.  Queue
  // users should retire through hazard_domain::retire instead.
  std::vector<hp_retired_entry> retired_;
  hazard_domain* domain_ = nullptr;

  hazard_record() = default;

  hazard_record(const hazard_record&) = delete;
  hazard_record& operator=(const hazard_record&) = delete;
  hazard_record(hazard_record&&) = delete;
  hazard_record& operator=(hazard_record&&) = delete;

  template <int SlotIndex, typename U>
  U* protect(std::atomic<void*>& source) {
    static_assert(SlotIndex >= 0 && SlotIndex < kSlots);
    U* ptr = nullptr;
    for (;;) {
      ptr = static_cast<U*>(source.load(std::memory_order_acquire));
      slots_[SlotIndex].store(ptr, std::memory_order_release);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      void* current = source.load(std::memory_order_acquire);
      if (current == ptr) {
        break;
      }
      slots_[SlotIndex].store(nullptr, std::memory_order_relaxed);
      ptr = static_cast<U*>(current);
    }
    return ptr;
  }

  template <int SlotIndex>
  bool claim_slot() noexcept {
    static_assert(SlotIndex >= 0 && SlotIndex < kSlots);
    bool expected = false;
    return claimed_[SlotIndex].compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
  }

  template <int SlotIndex>
  void clear() noexcept {
    static_assert(SlotIndex >= 0 && SlotIndex < kSlots);
    slots_[SlotIndex].store(nullptr, std::memory_order_relaxed);
    claimed_[SlotIndex].store(false, std::memory_order_release);
  }

  void retire_entry(void* ptr, void (*deleter)(void*) noexcept) {
    retired_.push_back({ptr, deleter});
  }

  [[nodiscard]] std::size_t retired_count() const noexcept {
    return retired_.size();
  }

  [[nodiscard]] bool has_active_hazards() const noexcept {
    for (int slot = 0; slot < kSlots; ++slot) {
      if (claimed_[slot].load(std::memory_order_acquire) ||
          slots_[slot].load(std::memory_order_acquire) != nullptr) {
        return true;
      }
    }
    return false;
  }
};

class hazard_domain {
 public:
  hazard_domain() = default;

  hazard_domain(const hazard_domain&) = delete;
  hazard_domain& operator=(const hazard_domain&) = delete;
  hazard_domain(hazard_domain&&) = delete;
  hazard_domain& operator=(hazard_domain&&) = delete;

  ~hazard_domain() {
    std::lock_guard lock(mu_);
    for (auto& entry : retired_) {
      entry.deleter(entry.ptr);
    }
    retired_.clear();
    for (auto* record : active_) {
      if (detail::tls_record == record) {
        detail::tls_record = nullptr;
      }
      destroy_record_locked(record);
    }
    active_.clear();
    for (auto* record : zombies_) {
      if (detail::tls_record == record) {
        detail::tls_record = nullptr;
      }
      destroy_record_locked(record);
    }
    zombies_.clear();
  }

  hazard_record& register_thread() {
    if (detail::tls_record != nullptr) {
      if (detail::tls_record->domain_ == this) {
        return *detail::tls_record;
      }
      throw std::logic_error(
          "thread is already registered with another hazard_domain; "
          "deregister it before switching domains");
    }

    auto* record = new hazard_record;
    record->domain_ = this;
    try {
      std::lock_guard lock(mu_);
      active_.push_back(record);
    } catch (...) {
      delete record;
      throw;
    }
    detail::tls_record = record;
    return *record;
  }

  // Returns false when the calling thread has no record for this domain or
  // still owns a hazard pointer.  A failed deregistration leaves the record
  // active so a caller can release its guard and retry safely.
  bool deregister_thread() {
    auto* record = detail::tls_record;
    if (record == nullptr || record->domain_ != this) {
      return false;
    }
    std::lock_guard lock(mu_);
    if (record->has_active_hazards()) {
      return false;
    }
    active_.erase(
        std::remove(active_.begin(), active_.end(), record), active_.end());
    for (auto& entry : record->retired_) {
      retired_.push_back(entry);
    }
    record->retired_.clear();
    record->domain_ = nullptr;
    zombies_.push_back(record);
    detail::tls_record = nullptr;
    return true;
  }

  void retire(void* ptr, void (*deleter)(void*) noexcept) {
    if (ptr == nullptr || deleter == nullptr) {
      throw std::invalid_argument("hazard_domain::retire requires a pointer and deleter");
    }
    std::lock_guard lock(mu_);
    retired_.push_back({ptr, deleter});
    const std::size_t threshold =
        std::max<std::size_t>(64, 2 * active_.size() * hazard_record::kSlots);
    if (retired_.size() >= threshold) {
      scan_locked();
    }
  }

  void scan() {
    std::lock_guard lock(mu_);
    scan_locked();
  }

  [[nodiscard]] std::size_t thread_count() {
    std::lock_guard lock(mu_);
    return active_.size();
  }

 private:
  static void destroy_record_locked(hazard_record* record) noexcept {
    for (auto& entry : record->retired_) {
      entry.deleter(entry.ptr);
    }
    delete record;
  }

  void scan_locked() {
    if (retired_.empty()) {
      return;
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::unordered_set<void*> hazards;
    for (auto* record : active_) {
      for (int slot = 0; slot < hazard_record::kSlots; ++slot) {
        void* value = record->slots_[slot].load(std::memory_order_acquire);
        if (value != nullptr) {
          hazards.insert(value);
        }
      }
    }
    std::size_t kept = 0;
    for (std::size_t index = 0; index < retired_.size(); ++index) {
      if (hazards.count(retired_[index].ptr)) {
        retired_[kept++] = retired_[index];
      } else {
        retired_[index].deleter(retired_[index].ptr);
      }
    }
    retired_.resize(kept);
  }

  std::mutex mu_;
  std::vector<hazard_record*> active_;
  std::vector<hazard_record*> zombies_;
  std::vector<hp_retired_entry> retired_;
};

}  // namespace norn
