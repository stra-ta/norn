#!/usr/bin/env python3
"""Focused contract tests for hardware campaign topology and evidence handling."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("hardware_campaign", ROOT / "tools" / "run_hardware_campaign.py")
assert SPEC is not None and SPEC.loader is not None
CAMPAIGN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAMPAIGN)


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

    def test_atomic_write_retains_every_repetition(self) -> None:
        content = {"records": [{"raw": {"samples": [{"complete": True}, {"complete": True}]}}]}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "evidence.json"
            CAMPAIGN.atomic_write(output, content)
            self.assertEqual(json.loads(output.read_text()), content)

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


if __name__ == "__main__":
    unittest.main()
