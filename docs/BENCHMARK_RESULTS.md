# Benchmark Results

The measured comparison of Norn's queue structures on one machine.

## Environment

| Field | Value |
| --- | --- |
| CPU | Apple M1 |
| OS | macOS 26.5.2 (arm64) |
| Compiler | Apple Clang 21.0.0 |
| Commit | `5d73d0eb` |
| Build type | Release |
| Queue capacity | 1024 |
| Benchmark library | Google Benchmark v1.9.1 |

## Results

Batch size 100,000 items. Throughput is items per second as reported by the
harness; per-item cost is derived from that same value so the two columns
agree.

| Structure | Configuration | ns/item | ops/s |
| --- | --- | --- | --- |
| Bounded mutex queue | single-thread push/pop | 68.1 | 14.68M |
| SPSC ring | unpadded | 73.3 | 13.64M |
| SPSC ring | padded (cache-line separated) | 59.5 | 16.81M |
| MPMC ring | 1x1 | 32.7 | 30.56M |
| MPMC ring | 2x2 | 152.7 | 6.55M |
| MPMC ring | 4x4 | 168.2 | 5.95M |
| Hazard-pointer queue | MPSC, 1x1 | 194.6 | 5.14M |

## What the numbers show

- Padded SPSC beats unpadded SPSC on this machine, consistent with the
  cache-line-separation hypothesis.
- The hazard-pointer queue is the slowest end-to-end, about 2.6x the unpadded
  SPSC ring per item, because it allocates heap nodes and publishes hazard
  pointers on the pop path.
- MPMC 1x1 is the fastest single-pair config; throughput drops as producer and
  consumer threads contend.

## Caveats

Thread creation, retry loops, and scheduler yields are part of the timed
workload. The numbers compare designs on this machine, not absolute
cycles-per-operation claims. The hazard-pointer config is MPSC 1x1 because the
demonstration vehicle is MPSC by contract. Re-run with
`tools/run_benchmark.py` on explicitly identified hardware before treating any
number as portable.