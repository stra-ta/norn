// External consumer of the installed norn::norn target: exercises a canonical
// queue type across two real threads, built only against the installed
// package. The threading dependency must arrive transitively through
// norn::core, so no explicit Threads link happens here.

#include <iostream>
#include <optional>
#include <thread>

#include <norn/core/backoff.hpp>
#include <norn/queue/spsc_ring.hpp>

int main() {
  norn::spsc_ring<int, 8> ring;
  constexpr int produced_value = 7;

  std::thread producer([&ring, produced_value] {
    norn::backoff::tight backoff;
    while (!ring.try_push(produced_value)) {
      backoff();
    }
  });

  norn::backoff::tight backoff;
  std::optional<int> popped;
  while (!popped) {
    popped = ring.try_pop();
    if (!popped) {
      backoff();
    }
  }

  producer.join();
  if (!popped || *popped != produced_value) {
    return 1;
  }
  std::cout << "queue consumer ok\n";
  return 0;
}
