# Norn

A header-only C++20 lab for bounded queues, memory ordering, and hazard-pointer reclamation.

![Norn library boundaries](docs/ARCHITECTURE.svg)

Norn keeps functional verification separate from performance measurement.
The queue contracts live beside tests, sanitizers, source-level memory-order checks, and reproducible hardware campaigns.

<table>
  <tr>
    <td><img src="docs/LIFECYCLE.svg" alt="Queue and hazard-pointer lifecycles"></td>
    <td><img src="docs/BENCHMARKS.svg" alt="Historical queue throughput with provenance"></td>
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
