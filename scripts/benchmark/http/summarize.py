#!/usr/bin/env python3

import csv
import math
import re
import statistics
import sys
import random
from collections import defaultdict
from pathlib import Path


def percentile(values, fraction):
    if not values:
        return math.nan
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return float(ordered[lower])
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def parse_properties(path):
    properties = {}
    if not path.exists():
        return properties
    for line in path.read_text(errors="replace").splitlines():
        if "=" not in line:
            continue
        name, value = line.split("=", 1)
        properties[name] = value
    return properties


def parse_counters(path):
    counters = {}
    if not path.exists():
        return counters
    for line in path.read_text(errors="replace").splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[1].isdigit():
            counters[fields[0]] = int(fields[1])
    return counters


def counter_delta(before, after, name):
    if name not in before or name not in after:
        return None
    return max(0, after[name] - before[name])


def integer_property(properties, name):
    value = properties.get(name, "")
    return int(value) if value.isdigit() else None


def parse_h2load(path):
    result = {}
    if not path.exists():
        return result
    text = path.read_text(errors="replace")
    finished = re.search(r"finished in\s+([^,]+),\s+([0-9.]+) req/s", text)
    if finished:
        result["reported_duration"] = finished.group(1)
        result["requests_per_second"] = float(finished.group(2))
    requests = re.search(
        r"requests:\s+(\d+) total,\s+(\d+) started,\s+(\d+) done,\s+"
        r"(\d+) succeeded,\s+(\d+) failed,\s+(\d+) errored,\s+(\d+) timeout",
        text,
    )
    if requests:
        names = (
            "total",
            "started",
            "done",
            "succeeded",
            "failed",
            "errored",
            "timeout",
        )
        for name, value in zip(names, requests.groups()):
            result[name] = int(value)
    traffic = re.search(r"traffic:\s+([^ ]+) \(([^)]+)\)", text)
    if traffic:
        result["traffic_total"] = traffic.group(1)
        result["traffic_rate"] = traffic.group(2)
    return result


def parse_requests(path):
    durations = []
    status_counts = defaultdict(int)
    if not path.exists():
        return durations, status_counts
    with path.open(errors="replace") as stream:
        for line in stream:
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 3:
                continue
            try:
                status_counts[fields[1]] += 1
                durations.append(int(fields[2]))
            except ValueError:
                continue
    return durations, status_counts


def load_meta(path):
    values = {}
    for line in path.read_text(errors="replace").splitlines():
        if "=" in line:
            name, value = line.split("=", 1)
            values[name] = value
    return values


def median_or_nan(values):
    finite = [value for value in values if value is not None and math.isfinite(value)]
    return statistics.median(finite) if finite else math.nan


def bootstrap_median_interval(values, iterations=20000):
    values = [value for value in values if math.isfinite(value)]
    if not values:
        return math.nan, math.nan
    generator = random.Random(20260719)
    medians = []
    for _ in range(iterations):
        sample = [generator.choice(values) for _ in values]
        medians.append(statistics.median(sample))
    medians.sort()
    return percentile(medians, 0.025), percentile(medians, 0.975)


def format_number(value, digits=2):
    if value is None or not math.isfinite(value):
        return "n/a"
    return f"{value:.{digits}f}"


def main():
    if len(sys.argv) != 2:
        print("usage: summarize.py <result-directory>", file=sys.stderr)
        return 2

    result_dir = Path(sys.argv[1]).resolve()
    rows = []
    for meta_path in sorted(result_dir.glob("runs/*/rep-*/*/meta.env")):
        run_dir = meta_path.parent
        meta = load_meta(meta_path)
        output = parse_h2load(run_dir / "h2load.out")
        durations, statuses = parse_requests(run_dir / "requests.tsv")
        sut_before = parse_properties(run_dir / "sut-before.unit")
        sut_after = parse_properties(run_dir / "sut-after.unit")
        sut_settled = parse_properties(run_dir / "sut-settled.unit")
        backend_before = parse_properties(run_dir / "backend-before.unit")
        backend_after = parse_properties(run_dir / "backend-after.unit")
        sut_cpu_stat_before = parse_counters(run_dir / "sut-before.cpu.stat")
        sut_cpu_stat_after = parse_counters(run_dir / "sut-after.cpu.stat")
        backend_cpu_stat_before = parse_counters(run_dir / "backend-before.cpu.stat")
        backend_cpu_stat_after = parse_counters(run_dir / "backend-after.cpu.stat")
        sut_cpu_before = integer_property(sut_before, "CPUUsageNSec")
        sut_cpu_after = integer_property(sut_after, "CPUUsageNSec")
        backend_cpu_before = integer_property(backend_before, "CPUUsageNSec")
        backend_cpu_after = integer_property(backend_after, "CPUUsageNSec")
        sut_cpu = None
        backend_cpu = None
        if sut_cpu_before is not None and sut_cpu_after is not None:
            sut_cpu = max(0.0, (sut_cpu_after - sut_cpu_before) / 1_000_000_000)
        if backend_cpu_before is not None and backend_cpu_after is not None:
            backend_cpu = max(0.0, (backend_cpu_after - backend_cpu_before) / 1_000_000_000)
        sut_usage_usec = counter_delta(
            sut_cpu_stat_before, sut_cpu_stat_after, "usage_usec"
        )
        backend_usage_usec = counter_delta(
            backend_cpu_stat_before, backend_cpu_stat_after, "usage_usec"
        )
        if sut_usage_usec is not None:
            sut_cpu = sut_usage_usec / 1_000_000
        if backend_usage_usec is not None:
            backend_cpu = backend_usage_usec / 1_000_000
        succeeded = output.get("succeeded", sum(statuses.values()))
        row = {
            "case": meta.get("case", ""),
            "implementation": meta.get("implementation", ""),
            "repetition": int(meta.get("repetition", 0)),
            "requests_per_second": output.get("requests_per_second", math.nan),
            "total": output.get("total", 0),
            "succeeded": succeeded,
            "failed": output.get("failed", 0),
            "errored": output.get("errored", 0),
            "timeout": output.get("timeout", 0),
            "logged_requests": len(durations),
            "p50_us": percentile(durations, 0.50),
            "p95_us": percentile(durations, 0.95),
            "p95_us": percentile(durations, 0.95),
            "p99_us": percentile(durations, 0.99),
            "p999_us": percentile(durations, 0.999),
            "max_us": max(durations) if durations else math.nan,
            "sut_cpu_seconds": sut_cpu,
            "sut_user_cpu_seconds": (
                counter_delta(sut_cpu_stat_before, sut_cpu_stat_after, "user_usec")
                or 0
            )
            / 1_000_000,
            "sut_system_cpu_seconds": (
                counter_delta(sut_cpu_stat_before, sut_cpu_stat_after, "system_usec")
                or 0
            )
            / 1_000_000,
            "sut_nr_throttled": counter_delta(
                sut_cpu_stat_before, sut_cpu_stat_after, "nr_throttled"
            ),
            "sut_throttled_seconds": (
                counter_delta(
                    sut_cpu_stat_before, sut_cpu_stat_after, "throttled_usec"
                )
                or 0
            )
            / 1_000_000,
            "backend_cpu_seconds": backend_cpu,
            "requests_per_sut_cpu_second": (
                succeeded / sut_cpu if sut_cpu is not None and sut_cpu > 0 else math.nan
            ),
            "sut_memory_peak_bytes": integer_property(sut_after, "MemoryPeak"),
            "sut_result": sut_settled.get("Result", sut_after.get("Result", "")),
            "sut_active_state": sut_settled.get(
                "ActiveState", sut_after.get("ActiveState", "")
            ),
            "http_2xx": statuses.get("200", 0) + statuses.get("204", 0),
        }
        rows.append(row)

    if not rows:
        print(f"no runs found under {result_dir}", file=sys.stderr)
        return 1

    fieldnames = list(rows[0])
    with (result_dir / "runs.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    groups = defaultdict(list)
    for row in rows:
        groups[(row["case"], row["implementation"])].append(row)

    summary_rows = []
    for (case_name, implementation), group in sorted(groups.items()):
        summary_rows.append(
            {
                "case": case_name,
                "implementation": implementation,
                "runs": len(group),
                "rps_median": median_or_nan(
                    [row["requests_per_second"] for row in group]
                ),
                "rps_min": min(row["requests_per_second"] for row in group),
                "rps_max": max(row["requests_per_second"] for row in group),
                "p50_us_median": median_or_nan([row["p50_us"] for row in group]),
                "p99_us_median": median_or_nan([row["p99_us"] for row in group]),
                "p999_us_median": median_or_nan([row["p999_us"] for row in group]),
                "sut_cpu_seconds_median": median_or_nan(
                    [row["sut_cpu_seconds"] for row in group]
                ),
                "requests_per_sut_cpu_second_median": median_or_nan(
                    [row["requests_per_sut_cpu_second"] for row in group]
                ),
                "memory_peak_mib_median": median_or_nan(
                    [
                        row["sut_memory_peak_bytes"] / 1024 / 1024
                        for row in group
                        if row["sut_memory_peak_bytes"] is not None
                    ]
                ),
                "throttled_seconds_median": median_or_nan(
                    [row["sut_throttled_seconds"] for row in group]
                ),
                "failed_total": sum(row["failed"] for row in group),
                "errored_total": sum(row["errored"] for row in group),
                "timeout_total": sum(row["timeout"] for row in group),
                "unexpected_exits": sum(
                    row["sut_active_state"] != "active" for row in group
                ),
            }
        )

    with (result_dir / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary_rows[0]))
        writer.writeheader()
        writer.writerows(summary_rows)

    paired = defaultdict(dict)
    for row in rows:
        paired[(row["case"], row["repetition"])][row["implementation"]] = row
    paired_rows = []
    for (case_name, repetition), implementations in sorted(paired.items()):
        if "lite" not in implementations or "nginx" not in implementations:
            continue
        lite = implementations["lite"]
        nginx = implementations["nginx"]
        paired_rows.append(
            {
                "case": case_name,
                "repetition": repetition,
                "rps_lite_over_nginx": (
                    lite["requests_per_second"] / nginx["requests_per_second"]
                ),
                "p99_lite_over_nginx": lite["p99_us"] / nginx["p99_us"],
                "cpu_efficiency_lite_over_nginx": (
                    lite["requests_per_sut_cpu_second"]
                    / nginx["requests_per_sut_cpu_second"]
                ),
            }
        )
    if paired_rows:
        with (result_dir / "paired.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(paired_rows[0]))
            writer.writeheader()
            writer.writerows(paired_rows)

        paired_groups = defaultdict(list)
        for row in paired_rows:
            paired_groups[row["case"]].append(row)
        paired_summary_rows = []
        for case_name, group in sorted(paired_groups.items()):
            output = {"case": case_name, "pairs": len(group)}
            for name in (
                "rps_lite_over_nginx",
                "p99_lite_over_nginx",
                "cpu_efficiency_lite_over_nginx",
            ):
                values = [row[name] for row in group]
                low, high = bootstrap_median_interval(values)
                median = statistics.median(values)
                deviations = [abs(value - median) for value in values]
                output[f"{name}_median"] = median
                output[f"{name}_min"] = min(values)
                output[f"{name}_max"] = max(values)
                output[f"{name}_mad"] = statistics.median(deviations)
                output[f"{name}_ci95_low"] = low
                output[f"{name}_ci95_high"] = high
                output[f"{name}_gt_one"] = sum(value > 1 for value in values)
            paired_summary_rows.append(output)
        with (result_dir / "paired-summary.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(paired_summary_rows[0]))
            writer.writeheader()
            writer.writerows(paired_summary_rows)

    with (result_dir / "summary.md").open("w") as stream:
        stream.write("| Case | Implementation | Runs | RPS median | p50 ms | p99 ms | p99.9 ms | req/SUT CPU-s | Throttled s | Peak MiB | Errors | Exits |\n")
        stream.write("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for row in summary_rows:
            # h2load reports errored and timeout as subsets of failed. Do not add
            # them again when showing the total failed request count.
            errors = row["failed_total"]
            stream.write(
                f"| {row['case']} | {row['implementation']} | {row['runs']} | "
                f"{format_number(row['rps_median'], 0)} | "
                f"{format_number(row['p50_us_median'] / 1000)} | "
                f"{format_number(row['p99_us_median'] / 1000)} | "
                f"{format_number(row['p999_us_median'] / 1000)} | "
                f"{format_number(row['requests_per_sut_cpu_second_median'], 0)} | "
                f"{format_number(row['throttled_seconds_median'], 3)} | "
                f"{format_number(row['memory_peak_mib_median'], 1)} | "
                f"{errors} | {row['unexpected_exits']} |\n"
            )

    print(result_dir / "summary.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
