#include <algorithm>
#include <iostream>
#include <vector>

#include "support/thread_control.hpp"

int main() {
  using norn::benchmark_support::apply_affinity;
  using norn::benchmark_support::current_affinity;

#if defined(__linux__)
  const std::vector<int> original = current_affinity();
  if (original.empty()) {
    std::cerr << "unable to inspect current affinity\n";
    return 1;
  }
  const auto result = apply_affinity({original.front()});
  if (result.status != "applied" || result.effective_cpus != std::vector<int>{original.front()}) {
    std::cerr << "affinity read-back mismatch: " << result.status << '\n';
    return 1;
  }
  const auto restored = apply_affinity(original);
  if (restored.status != "applied" || restored.effective_cpus != original) {
    std::cerr << "affinity restoration mismatch: " << restored.status << '\n';
    return 1;
  }
#else
  const auto result = apply_affinity({0});
  if (result.status != "unsupported") {
    std::cerr << "non-Linux affinity must be unsupported\n";
    return 1;
  }
#endif
  return 0;
}
