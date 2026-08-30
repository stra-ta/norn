#!/usr/bin/env python3
"""Regenerate the historical benchmark figure from committed JSON evidence."""

from __future__ import annotations

import argparse
import html
import json
from pathlib import Path


WIDTH = 1200
HEIGHT = 560
PLOT_LEFT = 330
PLOT_RIGHT = 1120
PLOT_TOP = 110
BAR_HEIGHT = 38
BAR_GAP = 26
MAX_VALUE = 32_000_000.0


def read_measurements(path: Path) -> list[dict[str, object]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    measurements = document.get("measurements")
    if not isinstance(measurements, list) or not measurements:
        raise ValueError("evidence file has no measurements")
    return measurements


def text(value: object) -> str:
    return html.escape(str(value), quote=True)


def render(measurements: list[dict[str, object]]) -> str:
    bars: list[str] = []
    for index, measurement in enumerate(measurements):
        y = PLOT_TOP + index * (BAR_HEIGHT + BAR_GAP)
        value = float(measurement["items_per_second"])
        width = (PLOT_RIGHT - PLOT_LEFT) * value / MAX_VALUE
        label = f"{measurement['structure']} / {measurement['configuration']}"
        bars.extend(
            [
                f'  <text class="label" x="20" y="{y + 24}">{text(label)}</text>',
                f'  <rect class="bar" x="{PLOT_LEFT}" y="{y}" width="{width:.3f}" height="{BAR_HEIGHT}" rx="6"/>',
                f'  <text class="value" x="{PLOT_LEFT + width + 12:.3f}" y="{y + 24}">{value / 1_000_000:.2f}M items/s</text>',
            ]
        )
    plot_bottom = PLOT_TOP + len(measurements) * (BAR_HEIGHT + BAR_GAP) - BAR_GAP
    ticks: list[str] = []
    for value in range(0, 33, 8):
        x = PLOT_LEFT + (PLOT_RIGHT - PLOT_LEFT) * value / 32
        ticks.extend(
            [
                f'  <line class="grid" x1="{x:.3f}" y1="{PLOT_TOP - 20}" x2="{x:.3f}" y2="{plot_bottom + 12}"/>',
                f'  <text class="tick" x="{x:.3f}" y="{plot_bottom + 42}">{value}M</text>',
            ]
        )
    return "\n".join(
        [
            '<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="560" viewBox="0 0 1200 560" role="img" aria-labelledby="title desc">',
            '  <title id="title">Norn historical queue benchmark comparison</title>',
            '  <desc id="desc">Historical throughput evidence from commit 5d73d0eb on an Apple M1. The figure is generated from committed JSON evidence.</desc>',
            "  <style>",
            "    .bg { fill: #fbfaf7; }",
            "    .title { fill: #172026; font: 700 24px -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }",
            "    .subtitle { fill: #59636a; font: 14px -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }",
            "    .label { fill: #29353b; font: 14px -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }",
            "    .value { fill: #1f6f68; font: 700 14px -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }",
            "    .tick { fill: #69737a; font: 12px -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; text-anchor: middle; }",
            "    .grid { stroke: #d9dfdc; stroke-width: 1; }",
            "    .bar { fill: #2d8a7d; }",
            "  </style>",
            '  <rect class="bg" width="1200" height="560"/>',
            '  <text class="title" x="20" y="38">Historical queue throughput</text>',
            '  <text class="subtitle" x="20" y="65">Apple M1 · Apple Clang 21.0.0 · Release · commit 5d73d0eb · setup included</text>',
            *ticks,
            *bars,
            f'  <text class="subtitle" x="{PLOT_LEFT}" y="{plot_bottom + 70}">items per second</text>',
            "</svg>",
            "",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--evidence", type=Path, default=root / "docs/evidence/benchmarks/legacy-5d73d0eb.json")
    parser.add_argument("--output", type=Path, default=root / "docs/BENCHMARKS.svg")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    generated = render(read_measurements(args.evidence))
    if args.check:
        current = args.output.read_text(encoding="utf-8") if args.output.exists() else ""
        if current != generated:
            raise SystemExit(f"generated figure is stale: {args.output}")
    else:
        args.output.write_text(generated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
