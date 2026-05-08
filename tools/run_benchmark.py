#!/usr/bin/env python3
"""Run compressor_benchmark across bitstream folders without clobbering results."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import shutil
import subprocess
import sys
import time
import threading
from datetime import datetime
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BENCHMARK = ROOT / "codec_benchmark" / "build" / "compressor_benchmark"
DEFAULT_ALGORITHMS = ("zlib", "heatshrink", "g5", "brotli", "zstd")
RESULT_FILES = ("compression.jsonl", "summary.csv", "stdout.log", "stderr.log", "commands.tsv", "metadata.jsonl")


@dataclass(frozen=True)
class BenchmarkTask:
    index: int
    total: int
    run_id: str
    runs: int
    algorithm: str
    folder: Path
    rel_folder: Path
    command: list[str]


@dataclass(frozen=True)
class BenchmarkResult:
    task: BenchmarkTask
    returncode: int
    elapsed_s: float
    jsonl_path: Path
    stdout_path: Path
    stderr_path: Path


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
    parser.add_argument("--jobs", type=int, default=1, help="parallel benchmark commands; default: 1")
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
        "--variant",
        dest="variants",
        action="append",
        help="variant to run; repeat for multiple. Passed through to compressor_benchmark.",
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


def append_file(target: Path, source: Path) -> None:
    if not source.exists() or source.stat().st_size == 0:
        return
    with target.open("a", encoding="utf-8") as out, source.open("r", encoding="utf-8") as src:
        for line in src:
            out.write(line)


def append_command_result(
    result: BenchmarkResult,
    stdout_log: Path,
    stderr_log: Path,
    jsonl: Path,
    commands_tsv: Path,
) -> None:
    task = result.task
    stdout_header = f"\n===== [{task.run_id} {task.index}/{task.total}] {task.algorithm} {task.rel_folder} =====\n"
    stderr_header = stdout_header

    with stdout_log.open("a", encoding="utf-8") as handle:
        handle.write(stdout_header)
    append_file(stdout_log, result.stdout_path)

    with stderr_log.open("a", encoding="utf-8") as handle:
        handle.write(stderr_header)
    append_file(stderr_log, result.stderr_path)

    append_file(jsonl, result.jsonl_path)

    with commands_tsv.open("a", encoding="utf-8") as handle:
        handle.write(
            f"{task.run_id}\t{task.index}\t{task.total}\t{task.runs}\t{task.algorithm}\t{task.rel_folder}\t"
            f"{result.returncode}\t{result.elapsed_s:.3f}\n"
        )


def run_task(task: BenchmarkTask, temp_dir: Path) -> BenchmarkResult:
    jsonl_path = temp_dir / f"{task.index:04d}.jsonl"
    stdout_path = temp_dir / f"{task.index:04d}.stdout.log"
    stderr_path = temp_dir / f"{task.index:04d}.stderr.log"

    command = list(task.command)
    jsonl_index = command.index("--jsonl") + 1
    command[jsonl_index] = str(jsonl_path)

    command_started = time.monotonic()
    with stdout_path.open("w", encoding="utf-8") as stdout_handle, \
            stderr_path.open("w", encoding="utf-8") as stderr_handle:
        completed = subprocess.run(
            command,
            cwd=ROOT,
            stdout=stdout_handle,
            stderr=stderr_handle,
            check=False,
        )
    elapsed = time.monotonic() - command_started
    return BenchmarkResult(task, completed.returncode, elapsed, jsonl_path, stdout_path, stderr_path)


def main() -> int:
    args = parse_args()
    if args.runs <= 0:
        raise SystemExit("--runs must be positive")
    if args.jobs <= 0:
        raise SystemExit("--jobs must be positive")

    results_dir = args.results_dir if args.results_dir.is_absolute() else ROOT / args.results_dir
    results_dir.mkdir(parents=True, exist_ok=True)
    if args.replace:
        for name in RESULT_FILES:
            (results_dir / name).unlink(missing_ok=True)
        for temp_dir in results_dir.glob(".tmp-*"):
            if temp_dir.is_dir():
                shutil.rmtree(temp_dir)

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
        "jobs": args.jobs,
        "algorithms": list(algorithms),
        "variants": list(args.variants or []),
        "folders": [str(folder.relative_to(ROOT)) for folder in folders],
        "command_count": total,
        "replace": args.replace,
    }
    with metadata_jsonl.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(metadata, separators=(",", ":")) + "\n")

    write_header = not commands_tsv.exists() or commands_tsv.stat().st_size == 0
    with commands_tsv.open("a", encoding="utf-8") as command_handle:
        if write_header:
            command_handle.write("run_id\tindex\ttotal\truns\talgorithm\tfolder\treturncode\telapsed_s\n")

    tasks: list[BenchmarkTask] = []
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
            ]
            for variant in args.variants or []:
                command.extend(["--variant", variant])
            command.extend([algorithm, str(folder)])
            tasks.append(BenchmarkTask(index, total, run_id, args.runs, algorithm, folder, rel_folder, command))

    temp_dir = results_dir / f".tmp-{run_id}"
    temp_dir.mkdir(parents=True, exist_ok=True)

    completed_count = 0
    first_failure = 0
    output_lock = threading.Lock()
    max_workers = min(args.jobs, total)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
        future_to_task = {}
        for task in tasks:
            print(f"[{task.index:03d}/{task.total:03d}] {task.algorithm} {task.rel_folder}", flush=True)
            future_to_task[executor.submit(run_task, task, temp_dir)] = task

        for future in concurrent.futures.as_completed(future_to_task):
            task = future_to_task[future]
            try:
                result = future.result()
            except Exception as exc:
                with output_lock:
                    completed_count += 1
                    first_failure = first_failure or 1
                    print(f"failed: {task.algorithm} {task.rel_folder}: {exc}", file=sys.stderr, flush=True)
                continue

            with output_lock:
                append_command_result(result, stdout_log, stderr_log, jsonl, commands_tsv)
                completed_count += 1
                if result.returncode != 0:
                    first_failure = first_failure or result.returncode
                    print(
                        f"failed: {task.algorithm} {task.rel_folder} returned {result.returncode}",
                        file=sys.stderr,
                        flush=True,
                    )

                overall_elapsed = time.monotonic() - started
                avg_per_command = overall_elapsed / completed_count
                remaining = avg_per_command * (total - completed_count)
                print(
                    f"    [{completed_count:03d}/{total:03d}] {task.algorithm} {task.rel_folder} "
                    f"done in {result.elapsed_s:.1f}s; elapsed {overall_elapsed / 60:.1f}m; "
                    f"eta {remaining / 60:.1f}m",
                    flush=True,
                )

    if first_failure:
        return first_failure

    summarize(jsonl, summary_csv)
    shutil.rmtree(temp_dir)
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
