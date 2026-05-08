#!/usr/bin/env python3
"""Run compressor_benchmark across bitstream folders without clobbering results."""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BENCHMARK = ROOT / "codec_benchmark" / "build" / "compressor_benchmark"
DEFAULT_ALGORITHMS = ("zlib", "heatshrink", "g5")
RESULT_FILES = ("compression.jsonl", "summary.csv", "stdout.log", "stderr.log", "commands.tsv", "metadata.jsonl")


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run compression benchmarks and append results.")
    parser.add_argument(
        "--results-dir",
        type=Path,
        required=True,
        help="result directory to append to, for example results/run-YYYYMMDD-HHMMSS",
    )
    parser.add_argument("--runs", type=int, default=1, help="runs per compression row; default: 1")
    parser.add_argument(
        "--algorithm",
        "--algorithms",
        dest="algorithms",
        action="append",
        help="algorithm to run; repeat for multiple. Defaults to all current algorithms.",
    )
    parser.add_argument(
        "--folder",
        dest="folders",
        action="append",
        type=Path,
        help="bitstream folder to run; repeat for multiple. Defaults to all folders under image_sources.",
    )
    parser.add_argument(
        "--replace",
        action="store_true",
        help="delete existing result files in the result directory before running",
    )
    return parser.parse_args()


def folders_with_bitstreams() -> list[Path]:
    folders = {path.parent for path in (ROOT / "image_sources").rglob("*.bs-od")}
    return sorted(folders)


def normalize_folder(folder: Path) -> Path:
    folder = folder if folder.is_absolute() else ROOT / folder
    return folder.resolve()


def summarize(jsonl: Path, csv_path: Path) -> None:
    fields = [
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

    rows = []
    with jsonl.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{jsonl}:{line_number}: invalid JSON: {exc}") from exc
            rows.append({field: record.get(field, "") for field in fields})

    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    if args.runs <= 0:
        raise SystemExit("--runs must be positive")

    results_dir = args.results_dir if args.results_dir.is_absolute() else ROOT / args.results_dir
    results_dir.mkdir(parents=True, exist_ok=True)
    if args.replace:
        for name in RESULT_FILES:
            (results_dir / name).unlink(missing_ok=True)

    algorithms = tuple(args.algorithms or DEFAULT_ALGORITHMS)
    folders = [normalize_folder(folder) for folder in args.folders] if args.folders else folders_with_bitstreams()
    total = len(algorithms) * len(folders)
    if total == 0:
        raise SystemExit("no benchmark commands to run")

    jsonl = results_dir / "compression.jsonl"
    summary_csv = results_dir / "summary.csv"
    stdout_log = results_dir / "stdout.log"
    stderr_log = results_dir / "stderr.log"
    commands_tsv = results_dir / "commands.tsv"
    metadata_jsonl = results_dir / "metadata.jsonl"

    run_id = datetime.now().strftime("%Y%m%d-%H%M%S")
    started = time.monotonic()
    metadata = {
        "event": "start",
        "run_id": run_id,
        "started_at": datetime.now().isoformat(timespec="seconds"),
        "runs": args.runs,
        "algorithms": list(algorithms),
        "folders": [str(folder.relative_to(ROOT)) for folder in folders],
        "command_count": total,
        "replace": args.replace,
    }
    with metadata_jsonl.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(metadata, separators=(",", ":")) + "\n")

    write_header = not commands_tsv.exists() or commands_tsv.stat().st_size == 0
    with stdout_log.open("a", encoding="utf-8") as stdout_handle, \
            stderr_log.open("a", encoding="utf-8") as stderr_handle, \
            commands_tsv.open("a", encoding="utf-8") as command_handle:
        if write_header:
            command_handle.write("run_id\tindex\ttotal\truns\talgorithm\tfolder\treturncode\telapsed_s\n")

        index = 0
        for folder in folders:
            rel_folder = folder.relative_to(ROOT)
            for algorithm in algorithms:
                index += 1
                command = [
                    str(BENCHMARK),
                    "--runs",
                    str(args.runs),
                    "--jsonl",
                    str(jsonl),
                    algorithm,
                    str(folder),
                ]

                print(f"[{index:03d}/{total:03d}] {algorithm} {rel_folder}", flush=True)
                stdout_handle.write(f"\n===== [{run_id} {index}/{total}] {algorithm} {rel_folder} =====\n")
                stderr_handle.write(f"\n===== [{run_id} {index}/{total}] {algorithm} {rel_folder} =====\n")
                stdout_handle.flush()
                stderr_handle.flush()

                command_started = time.monotonic()
                completed = subprocess.run(
                    command,
                    cwd=ROOT,
                    stdout=stdout_handle,
                    stderr=stderr_handle,
                    check=False,
                )
                elapsed = time.monotonic() - command_started
                command_handle.write(
                    f"{run_id}\t{index}\t{total}\t{args.runs}\t{algorithm}\t{rel_folder}\t"
                    f"{completed.returncode}\t{elapsed:.3f}\n"
                )
                command_handle.flush()

                if completed.returncode != 0:
                    print(f"failed: {algorithm} {rel_folder} returned {completed.returncode}", file=sys.stderr)
                    return completed.returncode

                overall_elapsed = time.monotonic() - started
                avg_per_command = overall_elapsed / index
                remaining = avg_per_command * (total - index)
                print(
                    f"    done in {elapsed:.1f}s; elapsed {overall_elapsed / 60:.1f}m; "
                    f"eta {remaining / 60:.1f}m",
                    flush=True,
                )

    summarize(jsonl, summary_csv)
    with metadata_jsonl.open("a", encoding="utf-8") as handle:
        handle.write(
            json.dumps(
                {
                    "event": "finish",
                    "run_id": run_id,
                    "finished_at": datetime.now().isoformat(timespec="seconds"),
                    "elapsed_s": time.monotonic() - started,
                },
                separators=(",", ":"),
            )
            + "\n"
        )

    print(f"finished {total} commands in {(time.monotonic() - started) / 60:.1f}m", flush=True)
    print(f"updated {display_path(summary_csv)}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
