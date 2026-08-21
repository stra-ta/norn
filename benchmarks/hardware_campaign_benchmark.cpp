#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <utility>
#include <vector>

#include "norn/mpmc_queue.hpp"
#include "norn/spsc_queue.hpp"
#include "support/thread_control.hpp"

namespace {

enum class backoff_mode { tight, yield, bounded, exponential };

struct options {
  std::string queue = "spsc";
  std::size_t items = 100'000;
  std::size_t producers = 1;
  std::size_t consumers = 1;
  std::size_t repetitions = 7;
  std::size_t warmups = 0;
  backoff_mode backoff = backoff_mode::tight;
  std::vector<int> producer_cpus;
  std::vector<int> consumer_cpus;
  bool require_affinity = false;
};

struct worker_stats {
  std::size_t pushes = 0;
  std::size_t pops = 0;
  std::size_t retries = 0;
  std::size_t yields = 0;
  std::size_t spin_steps = 0;
};

struct sample {
  double wall_seconds = 0.0;
  double cpu_seconds = 0.0;
  long voluntary_context_switches = 0;
  long involuntary_context_switches = 0;
  std::size_t pushes = 0;
  std::size_t pops = 0;
  std::size_t retries = 0;
  std::size_t yields = 0;
  std::size_t spin_steps = 0;
  double producer_fairness = 0.0;
  double consumer_fairness = 0.0;
  bool complete = false;
};

struct backoff {
  explicit backoff(backoff_mode mode) : mode_(mode) {}

  void failed(worker_stats& stats) {
    ++failures_;
    ++stats.retries;
    switch (mode_) {
      case backoff_mode::tight:
        return;
      case backoff_mode::yield:
        ++stats.yields;
        std::this_thread::yield();
        return;
      case backoff_mode::bounded:
        if (failures_ % 64U == 0U) {
          ++stats.yields;
          std::this_thread::yield();
        }
        return;
      case backoff_mode::exponential: {
        const std::size_t exponent = std::min<std::size_t>(failures_ - 1U, 6U);
        const std::size_t steps = std::size_t{1} << exponent;
        for (std::size_t step = 0; step < steps; ++step) {
          std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        stats.spin_steps += steps;
        if (failures_ >= 64U) {
          ++stats.yields;
          std::this_thread::yield();
          failures_ = 0;
        }
        return;
      }
    }
  }

  void succeeded() { failures_ = 0; }

 private:
  backoff_mode mode_;
  std::size_t failures_ = 0;
};

std::string_view backoff_name(backoff_mode mode) {
  switch (mode) {
    case backoff_mode::tight:
      return "tight";
    case backoff_mode::yield:
      return "yield";
    case backoff_mode::bounded:
      return "bounded";
    case backoff_mode::exponential:
      return "exponential";
  }
  return "unknown";
}

double seconds(const timeval& value) {
  return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
}

long context_switch_delta(long after, long before) {
  return after >= before ? after - before : 0;
}

double jain_fairness(const std::vector<worker_stats>& stats, std::size_t first, std::size_t count,
                     bool producer) {
  if (count == 0) {
    return 0.0;
  }
  double sum = 0.0;
  double squared_sum = 0.0;
  for (std::size_t index = first; index < first + count; ++index) {
    const double operations = static_cast<double>(producer ? stats[index].pushes : stats[index].pops);
    sum += operations;
    squared_sum += operations * operations;
  }
  if (squared_sum == 0.0) {
    return 0.0;
  }
  return sum * sum / (static_cast<double>(count) * squared_sum);
}

std::vector<int> parse_cpu_list(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  std::vector<int> cpus;
  std::size_t start = 0;
  while (start < value.size()) {
    const std::size_t end = value.find(',', start);
    const std::string token(value.substr(start, end == std::string_view::npos ? value.size() - start
                                                                                : end - start));
    const int cpu = std::stoi(token);
    if (cpu < 0) {
      throw std::invalid_argument("CPU IDs must be non-negative");
    }
    cpus.push_back(cpu);
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return cpus;
}

std::size_t parse_size(std::string_view value, std::string_view name) {
  const unsigned long long parsed = std::stoull(std::string(value));
  if (parsed == 0U) {
    throw std::invalid_argument(std::string(name) + " must be positive");
  }
  return static_cast<std::size_t>(parsed);
}

backoff_mode parse_backoff(std::string_view value) {
  if (value == "tight") {
    return backoff_mode::tight;
  }
  if (value == "yield") {
    return backoff_mode::yield;
  }
  if (value == "bounded") {
    return backoff_mode::bounded;
  }
  if (value == "exponential") {
    return backoff_mode::exponential;
  }
  throw std::invalid_argument("unknown backoff mode");
}

void print_usage() {
  std::cout << "usage: norn_hardware_benchmarks [options]\n"
               "  --queue spsc|spsc-padded|spsc-seq-cst|mpmc\n"
               "  --items <positive count> --producers <count> --consumers <count>\n"
               "  --repetitions <count> --warmups <count> --backoff tight|yield|bounded|exponential\n"
               "  --producer-cpus <comma-separated IDs> --consumer-cpus <comma-separated IDs>\n"
               "  --require-affinity\n";
}

options parse_options(int argc, char** argv) {
  options parsed;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      print_usage();
      std::exit(0);
    }
    if (argument == "--require-affinity") {
      parsed.require_affinity = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(argument));
    }
    const std::string_view value(argv[++index]);
    if (argument == "--queue") {
      parsed.queue = value;
    } else if (argument == "--items") {
      parsed.items = parse_size(value, argument);
    } else if (argument == "--producers") {
      parsed.producers = parse_size(value, argument);
    } else if (argument == "--consumers") {
      parsed.consumers = parse_size(value, argument);
    } else if (argument == "--repetitions") {
      parsed.repetitions = parse_size(value, argument);
    } else if (argument == "--warmups") {
      parsed.warmups = static_cast<std::size_t>(std::stoull(std::string(value)));
    } else if (argument == "--backoff") {
      parsed.backoff = parse_backoff(value);
    } else if (argument == "--producer-cpus") {
      parsed.producer_cpus = parse_cpu_list(value);
    } else if (argument == "--consumer-cpus") {
      parsed.consumer_cpus = parse_cpu_list(value);
    } else {
      throw std::invalid_argument("unknown option " + std::string(argument));
    }
  }
  const bool spsc = parsed.queue == "spsc" || parsed.queue == "spsc-padded" ||
                    parsed.queue == "spsc-seq-cst";
  if (!spsc && parsed.queue != "mpmc") {
    throw std::invalid_argument("unknown queue");
  }
  if (spsc && (parsed.producers != 1U || parsed.consumers != 1U)) {
    throw std::invalid_argument("SPSC queues require exactly one producer and one consumer");
  }
  return parsed;
}

template <typename Queue, bool IsMpmc>
std::pair<std::vector<sample>, std::vector<norn::benchmark_support::affinity_result>> run_campaign(
    const options& config) {
  const std::size_t workers = config.producers + config.consumers;
  std::vector<worker_stats> stats(workers);
  std::vector<norn::benchmark_support::affinity_result> affinities(workers);
  auto configured = std::make_unique<std::atomic<std::size_t>[]>(workers);
  auto finished = std::make_unique<std::atomic<std::size_t>[]>(workers);
  for (std::size_t index = 0; index < workers; ++index) {
    configured[index].store(0, std::memory_order_relaxed);
    finished[index].store(0, std::memory_order_relaxed);
  }
  std::atomic<Queue*> active_queue{nullptr};
  std::atomic<std::size_t> command{0};
  std::atomic<std::size_t> producers_finished{0};

  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (std::size_t index = 0; index < workers; ++index) {
    threads.emplace_back([&, index] {
      const bool producer = index < config.producers;
      const std::vector<int>& eligible_cpus = producer ? config.producer_cpus : config.consumer_cpus;
      const std::size_t role_index = producer ? index : index - config.producers;
      const std::vector<int> assigned_cpu = eligible_cpus.empty()
                                                ? std::vector<int>{}
                                                : std::vector<int>{eligible_cpus[role_index % eligible_cpus.size()]};
      affinities[index] = norn::benchmark_support::apply_affinity(assigned_cpu);
      configured[index].store(1, std::memory_order_release);
      const std::size_t total_rounds = config.warmups + config.repetitions;
      for (std::size_t repetition = 1; repetition <= total_rounds; ++repetition) {
        while (command.load(std::memory_order_acquire) < repetition) {
          std::this_thread::yield();
        }
        Queue* const queue = active_queue.load(std::memory_order_acquire);
        worker_stats local{};
        backoff retry(config.backoff);
        if constexpr (IsMpmc) {
          if (producer) {
            for (std::size_t value = 0; value < config.items;) {
              if (queue->try_push(value)) {
                ++value;
                ++local.pushes;
                retry.succeeded();
              } else {
                retry.failed(local);
              }
            }
            producers_finished.fetch_add(1, std::memory_order_release);
          } else {
            for (;;) {
              auto value = queue->try_pop();
              if (value.has_value()) {
                ++local.pops;
                retry.succeeded();
              } else if (producers_finished.load(std::memory_order_acquire) == config.producers) {
                auto final_value = queue->try_pop();
                if (final_value.has_value()) {
                  ++local.pops;
                  retry.succeeded();
                } else {
                  break;
                }
              } else {
                retry.failed(local);
              }
            }
          }
        } else if (producer) {
          for (std::size_t value = 0; value < config.items;) {
            if (queue->try_push(value)) {
              ++value;
              ++local.pushes;
              retry.succeeded();
            } else {
              retry.failed(local);
            }
          }
        } else {
          for (std::size_t value = 0; value < config.items;) {
            auto result = queue->try_pop();
            if (result.has_value()) {
              ++value;
              ++local.pops;
              retry.succeeded();
            } else {
              retry.failed(local);
            }
          }
        }
        stats[index] = local;
        finished[index].store(repetition, std::memory_order_release);
      }
    });
  }

  for (std::size_t index = 0; index < workers; ++index) {
    while (configured[index].load(std::memory_order_acquire) == 0U) {
      std::this_thread::yield();
    }
  }

  std::vector<sample> samples;
  samples.reserve(config.repetitions);
  const std::size_t total_rounds = config.warmups + config.repetitions;
  for (std::size_t repetition = 1; repetition <= total_rounds; ++repetition) {
    Queue queue;
    producers_finished.store(0, std::memory_order_relaxed);
    active_queue.store(&queue, std::memory_order_release);
    rusage before{};
    rusage after{};
    getrusage(RUSAGE_SELF, &before);
    const auto start = std::chrono::steady_clock::now();
    command.store(repetition, std::memory_order_release);
    for (std::size_t index = 0; index < workers; ++index) {
      while (finished[index].load(std::memory_order_acquire) < repetition) {
        std::this_thread::yield();
      }
    }
    const auto finish = std::chrono::steady_clock::now();
    getrusage(RUSAGE_SELF, &after);

    sample current;
    current.wall_seconds = std::chrono::duration<double>(finish - start).count();
    current.cpu_seconds = (seconds(after.ru_utime) - seconds(before.ru_utime)) +
                          (seconds(after.ru_stime) - seconds(before.ru_stime));
    current.voluntary_context_switches = context_switch_delta(after.ru_nvcsw, before.ru_nvcsw);
    current.involuntary_context_switches = context_switch_delta(after.ru_nivcsw, before.ru_nivcsw);
    for (const worker_stats& worker : stats) {
      current.pushes += worker.pushes;
      current.pops += worker.pops;
      current.retries += worker.retries;
      current.yields += worker.yields;
      current.spin_steps += worker.spin_steps;
    }
    current.producer_fairness = jain_fairness(stats, 0, config.producers, true);
    current.consumer_fairness = jain_fairness(stats, config.producers, config.consumers, false);
    current.complete = current.pushes == config.producers * config.items &&
                       current.pops == config.producers * config.items;
    if (repetition > config.warmups) {
      samples.push_back(current);
    }
  }

  for (std::thread& thread : threads) {
    thread.join();
  }
  return {std::move(samples), std::move(affinities)};
}

void print_json_string(std::string_view value) {
  std::cout << '"';
  for (const char character : value) {
    if (character == '"' || character == '\\') {
      std::cout << '\\';
    }
    std::cout << character;
  }
  std::cout << '"';
}

void print_cpu_array(const std::vector<int>& cpus) {
  std::cout << '[';
  for (std::size_t index = 0; index < cpus.size(); ++index) {
    if (index != 0) {
      std::cout << ',';
    }
    std::cout << cpus[index];
  }
  std::cout << ']';
}

void print_results(const options& config, const std::vector<sample>& samples,
                   const std::vector<norn::benchmark_support::affinity_result>& affinities) {
  bool affinity_valid = true;
  for (const auto& affinity : affinities) {
    if (config.require_affinity && affinity.status != "applied") {
      affinity_valid = false;
    }
  }
  std::cout << std::setprecision(12);
  std::cout << "{\"schema_version\":1,\"queue\":";
  print_json_string(config.queue);
  std::cout << ",\"items_per_producer\":" << config.items << ",\"producers\":" << config.producers
            << ",\"consumers\":" << config.consumers << ",\"capacity\":1024,\"backoff\":";
  print_json_string(backoff_name(config.backoff));
  std::cout << ",\"affinity_valid\":" << (affinity_valid ? "true" : "false") << ",\"affinity\":[";
  for (std::size_t index = 0; index < affinities.size(); ++index) {
    if (index != 0) {
      std::cout << ',';
    }
    std::cout << "{\"worker\":" << index << ",\"role\":";
    print_json_string(index < config.producers ? "producer" : "consumer");
    std::cout << ",\"status\":";
    print_json_string(affinities[index].status);
    std::cout << ",\"effective_cpus\":";
    print_cpu_array(affinities[index].effective_cpus);
    std::cout << '}';
  }
  std::cout << "],\"samples\":[";
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const sample& current = samples[index];
    if (index != 0) {
      std::cout << ',';
    }
    const double throughput = current.wall_seconds > 0.0
                                  ? static_cast<double>(current.pops) / current.wall_seconds
                                  : 0.0;
    const double nanoseconds_per_item = current.pops > 0U
                                            ? current.wall_seconds * 1'000'000'000.0 /
                                                  static_cast<double>(current.pops)
                                            : 0.0;
    std::cout << "{\"wall_seconds\":" << current.wall_seconds << ",\"cpu_seconds\":"
              << current.cpu_seconds << ",\"cpu_utilization\":"
              << (current.wall_seconds > 0.0 ? current.cpu_seconds / current.wall_seconds : 0.0)
              << ",\"throughput_items_per_second\":" << throughput << ",\"ns_per_item\":"
              << nanoseconds_per_item << ",\"voluntary_context_switches\":"
              << current.voluntary_context_switches << ",\"involuntary_context_switches\":"
              << current.involuntary_context_switches << ",\"pushes\":" << current.pushes
              << ",\"pops\":" << current.pops << ",\"retries\":" << current.retries
              << ",\"yields\":" << current.yields << ",\"spin_steps\":" << current.spin_steps
              << ",\"producer_fairness\":" << current.producer_fairness
              << ",\"consumer_fairness\":" << current.consumer_fairness
              << ",\"complete\":" << (current.complete ? "true" : "false") << '}';
  }
  std::cout << "]}\n";
}

int run(int argc, char** argv) {
  const options config = parse_options(argc, argv);
  if (config.queue == "spsc") {
    const auto [samples, affinities] = run_campaign<norn::spsc_queue<std::size_t, 1024>, false>(config);
    print_results(config, samples, affinities);
  } else if (config.queue == "spsc-padded") {
    const auto [samples, affinities] =
        run_campaign<norn::spsc_queue_padded<std::size_t, 1024>, false>(config);
    print_results(config, samples, affinities);
  } else if (config.queue == "spsc-seq-cst") {
    const auto [samples, affinities] =
        run_campaign<norn::spsc_queue_seq_cst<std::size_t, 1024>, false>(config);
    print_results(config, samples, affinities);
  } else {
    const auto [samples, affinities] = run_campaign<norn::mpmc_queue<std::size_t, 1024>, true>(config);
    print_results(config, samples, affinities);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "norn_hardware_benchmarks: " << error.what() << '\n';
    return 2;
  }
}
