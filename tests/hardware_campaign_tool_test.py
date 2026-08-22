#!/usr/bin/env python3
"""Focused contract tests for hardware campaign topology and evidence handling."""

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("hardware_campaign", ROOT / "tools" / "run_hardware_campaign.py")
assert SPEC is not None and SPEC.loader is not None
CAMPAIGN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAMPAIGN)


def find_hardware_binary() -> Path | None:
    override = os.environ.get("NORN_HARDWARE_BINARY")
    if override:
        candidate = Path(override)
        if candidate.exists():
            return candidate
    candidates = [
        Path.cwd() / "norn_hardware_benchmarks",
        ROOT / "build" / "release" / "norn_hardware_benchmarks",
        ROOT / "build" / "default" / "norn_hardware_benchmarks",
        ROOT / "build" / "debug" / "norn_hardware_benchmarks",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


class HardwareCampaignToolTest(unittest.TestCase):
    def test_manifest_covers_campaign_measurements(self) -> None:
        manifest = json.loads((ROOT / "tools" / "hardware_campaign_manifest.json").read_text())
        milestones = {case["milestone"] for case in manifest["cases"]}
        self.assertIn("H3", milestones)
        self.assertIn("H5", milestones)
        self.assertIn("H6", milestones)
        self.assertIn("H7", milestones)

    def test_sparse_cpu_lists_are_parsed(self) -> None:
        self.assertEqual(CAMPAIGN.parse_cpu_list("1,3-5,7"), [1, 3, 4, 5, 7])

    def test_distinct_core_selection_stays_in_effective_mask(self) -> None:
        topology = {
            "allowed_cpus": [2, 6, 10],
            "cpus": {
                "2": {"core_id": "0", "package_id": "0", "thread_siblings": [2, 6]},
                "6": {"core_id": "0", "package_id": "0", "thread_siblings": [2, 6]},
                "10": {"core_id": "1", "package_id": "0", "thread_siblings": [10]},
            },
        }
        producers, consumers, reason = CAMPAIGN.choose_placement("distinct-core", topology)
        self.assertIsNone(reason)
        self.assertEqual(producers, [2])
        self.assertEqual(consumers, [10])

    def test_missing_topology_has_a_stable_skip_reason(self) -> None:
        _, _, reason = CAMPAIGN.choose_placement("distinct-core", {"allowed_cpus": [], "cpus": {}})
        self.assertEqual(reason, "Linux topology is unavailable")

    def test_partial_topology_never_claims_a_core_or_package_relationship(self) -> None:
        topology = {
            "allowed_cpus": [0, 1],
            "cpus": {
                "0": {"core_id": None, "package_id": None, "thread_siblings": [0]},
                "1": {"core_id": None, "package_id": None, "thread_siblings": [1]},
            },
        }
        self.assertEqual(
            CAMPAIGN.choose_placement("distinct-core", topology)[2], "incomplete CPU core topology"
        )
        self.assertEqual(
            CAMPAIGN.choose_placement("cross-package", topology)[2], "incomplete CPU package topology"
        )

    def test_atomic_write_retains_every_repetition(self) -> None:
        content = {"records": [{"raw": {"samples": [{"complete": True}, {"complete": True}]}}]}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "evidence.json"
            CAMPAIGN.atomic_write(output, content)
            self.assertEqual(json.loads(output.read_text()), content)

    def test_summary_includes_median_retry_yield_spin_and_fairness(self) -> None:
        samples = [
            {
                "throughput_items_per_second": 100.0,
                "complete": True,
                "retries": 4,
                "yields": 1,
                "spin_steps": 10,
                "producer_fairness": 0.9,
                "consumer_fairness": 0.8,
            },
            {
                "throughput_items_per_second": 200.0,
                "complete": True,
                "retries": 6,
                "yields": 3,
                "spin_steps": 20,
                "producer_fairness": 0.7,
                "consumer_fairness": 0.6,
            },
            {
                "throughput_items_per_second": 300.0,
                "complete": True,
                "retries": 8,
                "yields": 5,
                "spin_steps": 30,
                "producer_fairness": 0.5,
                "consumer_fairness": 0.4,
            },
        ]
        summary = CAMPAIGN.summary(samples)
        self.assertEqual(summary["retry_median"], 6)
        self.assertEqual(summary["yield_median"], 3)
        self.assertEqual(summary["spin_steps_median"], 20)
        self.assertAlmostEqual(summary["producer_fairness_median"], 0.7)
        self.assertAlmostEqual(summary["consumer_fairness_median"], 0.6)
        # Backward-compatible keys remain intact.
        self.assertEqual(summary["sample_count"], 3)
        self.assertTrue(summary["all_complete"])

    def test_distinct_core_placement_rejects_multi_role_cases(self) -> None:
        topology = {
            "allowed_cpus": [2, 6, 10],
            "cpus": {
                "2": {"core_id": "0", "package_id": "0", "thread_siblings": [2, 6]},
                "6": {"core_id": "0", "package_id": "0", "thread_siblings": [2, 6]},
                "10": {"core_id": "1", "package_id": "0", "thread_siblings": [10]},
            },
        }
        producers, consumers, reason = CAMPAIGN.choose_placement(
            "distinct-core", topology, producers=2, consumers=2
        )
        self.assertEqual(reason, "distinct-core placement does not yet support multiple producers or consumers")
        self.assertEqual(producers, [])
        self.assertEqual(consumers, [])
        # Single-role distinct-core placement still resolves to distinct cores.
        producers, consumers, reason = CAMPAIGN.choose_placement(
            "distinct-core", topology, producers=1, consumers=1
        )
        self.assertIsNone(reason)
        self.assertEqual(producers, [2])
        self.assertEqual(consumers, [10])

    def test_campaign_summary_keeps_status_and_all_measurement_metadata(self) -> None:
        records = [
            {
                "case": {"id": "complete"},
                "status": "complete",
                "summary": {"sample_count": 9, "all_complete": True},
            },
            {"case": {"id": "skipped"}, "status": "skipped", "reason": "unsupported"},
        ]
        self.assertEqual(
            CAMPAIGN.campaign_summary(records),
            [
                {"id": "complete", "status": "complete", "sample_count": 9, "all_complete": True},
                {"id": "skipped", "status": "skipped", "reason": "unsupported"},
            ],
        )

    def test_require_complete_fails_for_an_inconclusive_case(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            completed = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "run_hardware_campaign.py"),
                    "--binary",
                    "/usr/bin/false",
                    "--output",
                    str(Path(directory) / "campaign.json"),
                    "--case",
                    "h3-spsc-unpadded-unpinned",
                    "--require-complete",
                ],
                check=False,
            )
            self.assertEqual(completed.returncode, 1)


class HardwareBinaryContractTest(unittest.TestCase):
    """Output-contract test for the hardware benchmark binary JSON.

    Verifies completeness (every sample complete, affinity marked valid) and the
    worker/statistics shape the campaign tool depends on. Skipped when the
    benchmark binary has not been built.
    """

    def test_hardware_binary_json_contract(self) -> None:
        binary = find_hardware_binary()
        if binary is None:
            self.skipTest("hardware benchmark binary not built")
        completed = subprocess.run(
            [
                str(binary),
                "--queue",
                "spsc",
                "--items",
                "1000",
                "--producers",
                "1",
                "--consumers",
                "1",
                "--repetitions",
                "3",
                "--warmups",
                "1",
                "--backoff",
                "tight",
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        report = json.loads(completed.stdout)
        self.assertEqual(report.get("schema_version"), 1)
        self.assertTrue(report.get("affinity_valid"))
        workers = report.get("affinity")
        self.assertIsInstance(workers, list)
        self.assertEqual(len(workers), 2)
        for entry in workers:
            self.assertIn(entry["role"], ("producer", "consumer"))
            self.assertIn("status", entry)
            self.assertIn("effective_cpus", entry)
        samples = report.get("samples")
        self.assertIsInstance(samples, list)
        self.assertGreater(len(samples), 0)
        required_metrics = (
            "throughput_items_per_second",
            "retries",
            "yields",
            "spin_steps",
            "producer_fairness",
            "consumer_fairness",
            "complete",
        )
        for sample in samples:
            self.assertTrue(sample.get("complete"))
            for metric in required_metrics:
                self.assertIn(metric, sample)
        # Every sample must carry a workers array that reconciles with the
        # declared producer/consumer totals and the aggregate sample counters.
        expected_worker_count = report.get("producers", 0) + report.get("consumers", 0)
        producer_count = report.get("producers", 0)
        required_worker_counters = ("pushes", "pops", "retries", "yields", "spin_steps")
        for sample in samples:
            workers = sample.get("workers")
            self.assertIsInstance(workers, list)
            # count producers + consumers
            self.assertEqual(len(workers), expected_worker_count)
            summed = {counter: 0 for counter in required_worker_counters}
            for index, worker in enumerate(workers):
                # sequential indexes
                self.assertEqual(worker.get("worker"), index)
                # producer/consumer roles
                self.assertEqual(
                    worker.get("role"),
                    "producer" if index < producer_count else "consumer",
                )
                # all required worker counters
                for counter in required_worker_counters:
                    self.assertIn(counter, worker)
                    summed[counter] += worker[counter]
            # sums matching aggregate sample counters
            for counter in required_worker_counters:
                self.assertEqual(sample.get(counter), summed[counter])


if __name__ == "__main__":
    if "--contract" in sys.argv:
        sys.argv.remove("--contract")
        suite = unittest.TestLoader().loadTestsFromTestCase(HardwareBinaryContractTest)
    else:
        suite = unittest.TestLoader().loadTestsFromTestCase(HardwareCampaignToolTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
