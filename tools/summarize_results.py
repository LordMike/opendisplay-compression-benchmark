#!/usr/bin/env python3
"""Convert compressor benchmark JSONL output into a flat CSV table."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any


FIELDS = [
    "status",
    "algorithm",
    "variant",
    "input_file",
    "output_file",
    "width",
    "height",
    "input_bytes",
    "compressed_bytes",
    "ratio",
    "avg_ms",
    "runs",
    "error",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Summarize compressor benchmark JSONL as CSV.")
    parser.add_argument("jsonl", type=Path, help="compression.jsonl from compressor_benchmark --jsonl")
    parser.add_argument(
        "csv",
        type=Path,
        nargs="?",
        help="output CSV path; defaults to summary.csv beside the JSONL file",
    )
    return parser.parse_args()


def clean_value(value: Any) -> Any:
    if isinstance(value, float):
        return f"{value:.8g}"
    return "" if value is None else value


def main() -> int:
    args = parse_args()
    output = args.csv or args.jsonl.with_name("summary.csv")
    rows = []

    with args.jsonl.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{args.jsonl}:{line_number}: invalid JSON: {exc}") from exc
            rows.append({field: clean_value(record.get(field, "")) for field in FIELDS})

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)

    print(f"wrote {len(rows)} rows to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

