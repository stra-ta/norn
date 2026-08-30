#pragma once

#include <atomic>
#include <stdexcept>

#include "norn/hazard/domain.hpp"

namespace norn {

// RAII guard protecting one pointer through a hazard_record slot.  A slot is
// single-owner: constructing a second guard for the same slot is rejected
// instead of silently replacing the first protection.
template <typename T, int Index = 0>
class hazard_ptr {
 public:
  hazard_ptr(hazard_record& record, std::atomic<void*>& source)
      : record_(record), value_(nullptr) {
    static_assert(Index >= 0 && Index < hazard_record::slot_count);
    if (!record_.template claim_slot<Index>()) {
      throw std::logic_error("hazard pointer slot is already in use");
    }
    try {
      value_ = record_.template protect<Index, T>(source);
    } catch (...) {
      record_.template clear<Index>();
      throw;
    }
  }

  ~hazard_ptr() { record_.template clear<Index>(); }

  hazard_ptr(const hazard_ptr&) = delete;
  hazard_ptr& operator=(const hazard_ptr&) = delete;
  hazard_ptr(hazard_ptr&&) = delete;
  hazard_ptr& operator=(hazard_ptr&&) = delete;

  [[nodiscard]] T* get() const noexcept { return value_; }
  [[nodiscard]] T& operator*() const noexcept { return *value_; }
  [[nodiscard]] T* operator->() const noexcept { return value_; }

 private:
  hazard_record& record_;
  T* value_;
};

}  // namespace norn
