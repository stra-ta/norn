# Norn

[![CI](https://github.com/stra-ta/norn/actions/workflows/ci.yml/badge.svg)](https://github.com/stra-ta/norn/actions/workflows/ci.yml)

A header-only C++20 lab for bounded queues, memory ordering, and hazard-pointer reclamation.

![Norn library boundaries](docs/ARCHITECTURE.svg)

## Historical queue comparison

| Structure | Configuration | ns/item | ops/s |
| --- | --- | ---: | ---: |
| Bounded mutex queue | single-thread push/pop | 68.1 | 14.68M |
| SPSC ring | padded | 59.5 | 16.81M |
| MPMC ring | 1 producer, 1 consumer | 32.7 | 30.56M |
| MPMC ring | 4 producers, 4 consumers | 168.2 | 5.95M |
| Hazard-pointer queue | MPSC, 1 producer | 194.6 | 5.14M |

Apple M1, macOS arm64, Apple Clang 21.0.0, Release, commit `5d73d0eb`, 100,000-item batches.
Thread setup, retries, and scheduler yields are included, so these compare structures on that machine rather than claiming portable per-operation cost.

Norn keeps functional verification separate from performance measurement.
The queue contracts live beside tests, sanitizers, source-level memory-order checks, and reproducible hardware campaigns.

<table>
  <tr>
    <td><img src="docs/LIFECYCLE.svg" alt="Queue and hazard-pointer lifecycles"></td>
    <td><img src="docs/BENCHMARKS.svg" alt="Historical queue throughput with provenance"></td>
  </tr>
</table>

<table>
  <tr>
    <td><img src="docs/PROGRESSION.svg" alt="Norn queue difficulty progression"></td>
    <td><img src="docs/MEMORY-MODEL.svg" alt="Norn memory-order relationships"></td>
  </tr>
</table>

## Structures

- Bounded SPSC and MPMC rings
- Mutex-backed reference queues
- A hazard-protected MPSC linked queue
- Cache-line placement and retry-policy utilities

The SPSC ring is wait-free only under its one-producer, one-consumer contract.
Norn makes no formal lock-free claim for the MPMC or linked queues.

[Build, install, verify, benchmark, and read the limits](GUIDE.md).

- [Correctness contracts](docs/CORRECTNESS.md)
- [Hazard-pointer design](docs/HAZARD_POINTER_DESIGN.md)
- [Measurement method](docs/BENCHMARKING.md)

## Build

See [GUIDE.md](GUIDE.md) for build presets and dependencies.

## Verification

Functional CI and performance evidence are separate. See [GUIDE.md](GUIDE.md) and `LAB_RULES.md` / `EVIDENCE.md` in `stra-ta/.github` for manifest provenance and the one-command suite (`./scripts/verify.sh` / `./scripts/confidence.sh` or `tools/verify.sh`).

## Limitations

CI is functional only. Performance evidence requires a committed manifest with machine metadata (commit, compiler, kernel, CPU, arch, build type, seed, argv) and a link from the claim to that artifact. See `stra-ta/.github` for lab-wide caveats.

