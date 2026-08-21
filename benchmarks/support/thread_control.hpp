#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace norn::benchmark_support {

struct affinity_result {
  std::string status;
  std::vector<int> effective_cpus;
};

inline std::string cpu_list(const std::vector<int>& cpus) {
  std::ostringstream stream;
  for (std::size_t index = 0; index < cpus.size(); ++index) {
    if (index != 0) {
      stream << ',';
    }
    stream << cpus[index];
  }
  return stream.str();
}

inline std::vector<int> current_affinity() {
#if defined(__linux__)
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (pthread_getaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) {
    return {};
  }
  std::vector<int> cpus;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &mask)) {
      cpus.push_back(cpu);
    }
  }
  return cpus;
#else
  return {};
#endif
}

inline affinity_result apply_affinity(const std::vector<int>& requested) {
#if defined(__linux__)
  if (requested.empty()) {
    return {"not-requested", current_affinity()};
  }
  cpu_set_t mask;
  CPU_ZERO(&mask);
  for (int cpu : requested) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
      return {"invalid-cpu", {}};
    }
    CPU_SET(cpu, &mask);
  }
  const int set_status = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
  if (set_status != 0) {
    return {"set-failed-" + std::to_string(set_status), current_affinity()};
  }
  const std::vector<int> effective = current_affinity();
  std::vector<int> expected = requested;
  std::sort(expected.begin(), expected.end());
  expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
  return {effective == expected ? "applied" : "narrowed", effective};
#else
  static_cast<void>(requested);
  return {"unsupported", {}};
#endif
}

}  // namespace norn::benchmark_support
