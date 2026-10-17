// Weakened-order probe, rehosted as a permanent experiment in Norn 0.3.
//
// The bounded MPMC ring's correctness rests on one store: after constructing a
// payload, the producer publishes it with a release store of the slot sequence
// number, and the consumer's acquire load of that number is the happens-before
// edge ordering the payload write before the payload read. This probe runs the
// real `norn::mpmc_ring` with one producer and one consumer, from the same
// source compiled twice:
//
//   default build                    -> the publish store keeps release
//   NORN_EXPERIMENT_WEAKENED_PUBLISH -> the same store becomes relaxed
//
// Under ThreadSanitizer the ordered target must run clean and the weakened
// target must report a data race on the payload access. The probe also
// validates every payload against a guard value, so a corrupted handoff aborts
// with a diagnostic even when TSan is not attached.
// See experiment/README.md for run commands and recorded results.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>

#include "norn/queue/mpmc_ring.hpp"

namespace {

constexpr std::uint64_t kItems = 1000;
constexpr std::uint64_t kGuardMask = 0x9e3779b97f4a7c15ull;

struct payload {
  std::uint64_t key = 0;
  std::uint64_t guard = 0;
};

[[noreturn]] void corrupted_payload(std::uint64_t key, std::uint64_t guard) {
  std::fprintf(stderr,
               "probe detected a corrupted payload: key=%llu guard=%llu "
               "(expected guard=%llu)\n",
               static_cast<unsigned long long>(key),
               static_cast<unsigned long long>(guard),
               static_cast<unsigned long long>(key ^ kGuardMask));
  std::abort();
}

}  // namespace

int main() {
  norn::mpmc_ring<payload, 2> ring;

  std::thread producer([&ring] {
    for (std::uint64_t key = 0; key < kItems;) {
      payload value;
      value.key = key;
      value.guard = key ^ kGuardMask;
      if (ring.try_push(value)) {
        ++key;
      } else {
        std::this_thread::yield();
      }
    }
  });

  std::thread consumer([&ring] {
    for (std::uint64_t expected = 0; expected < kItems;) {
      std::optional<payload> popped = ring.try_pop();
      if (!popped.has_value()) {
        std::this_thread::yield();
        continue;
      }
      if (popped->key != expected ||
          popped->guard != (expected ^ kGuardMask)) {
        corrupted_payload(popped->key, popped->guard);
      }
      ++expected;
    }
  });

  producer.join();
  consumer.join();

#if defined(NORN_EXPERIMENT_WEAKENED_PUBLISH)
  const char* variant = "weakened publish store";
#else
  const char* variant = "ordered publish store (control)";
#endif
  std::printf("probe ok: %s transferred %llu payloads\n", variant,
              static_cast<unsigned long long>(kItems));
  return 0;
}
