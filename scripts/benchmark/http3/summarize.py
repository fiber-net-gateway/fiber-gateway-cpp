#!/usr/bin/env python3

import csv
import math
import random
import re
import statistics
import sys
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


def parse_env(path):
    result = {}
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                result[key] = value
    return result


def parse_counters(path):
    result = {}
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[1].isdigit():
                result[fields[0]] = int(fields[1])
    return result


def parse_integer(path):
    if not path.exists():
        return math.nan
    try:
        return int(path.read_text().strip())
    except ValueError:
        return math.nan


def parse_client_cpu_percent(path):
    text = path.read_text(errors="replace") if path.exists() else ""
    match = re.search(r"Percent of CPU this job got:\s*([0-9.]+)%", text)
    return float(match.group(1)) if match else math.nan


def delta(before, after, key):
    if key not in before or key not in after:
        return math.nan
    return max(0, after[key] - before[key])


def parse_output(path):
    result = {}
    text = path.read_text(errors="replace") if path.exists() else ""
    match = re.search(r"finished in\s+[^,]+,\s+([0-9.]+) req/s", text)
    result["rps"] = float(match.group(1)) if match else math.nan
    match = re.search(
        r"requests:\s+(\d+) total,\s+(\d+) started,\s+(\d+) done,\s+"
        r"(\d+) succeeded,\s+(\d+) failed,\s+(\d+) errored,\s+(\d+) timeout",
        text,
    )
    names = ("total", "started", "done", "succeeded", "failed", "errored", "timeout")
    for name, value in zip(names, match.groups() if match else (0,) * len(names)):
        result[name] = int(value)
    match = re.search(r"UDP datagram:\s+(\d+) sent,\s+(\d+) received", text)
    result["udp_sent"] = int(match.group(1)) if match else 0
    result["udp_received"] = int(match.group(2)) if match else 0
    packet_line = re.search(r"packets lost:\s+(\d+)", text)
    result["packets_lost_min"] = int(packet_line.group(1)) if packet_line else 0
    statuses = {}
    match = re.search(r"status codes:\s+(\d+) 2xx,\s+(\d+) 3xx,\s+(\d+) 4xx,\s+(\d+) 5xx", text)
    if match:
        statuses = dict(zip(("2xx", "3xx", "4xx", "5xx"), map(int, match.groups())))
    result.update(statuses)
    return result


def parse_requests(path):
    durations = []
    status = defaultdict(int)
    if not path.exists():
        return durations, status
    with path.open(errors="replace") as stream:
        for line in stream:
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 3:
                continue
            try:
                code = fields[1]
                status[code] += 1
                if code.startswith("2"):
                    durations.append(int(fields[2]))
            except ValueError:
                pass
    return durations, status


def median(values):
    finite = [value for value in values if math.isfinite(value)]
    return statistics.median(finite) if finite else math.nan


def bootstrap_median_interval(values, iterations=20000):
    finite = [value for value in values if math.isfinite(value)]
    if not finite:
        return math.nan, math.nan
    generator = random.Random(20260719)
    samples = []
    for _ in range(iterations):
        samples.append(statistics.median(generator.choice(finite) for _ in finite))
    return percentile(samples, 0.025), percentile(samples, 0.975)


def main():
    if len(sys.argv) != 2:
        print("usage: summarize.py <result-directory>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    rows = []
    for meta_path in sorted(root.glob("runs/*/rep-*/*/meta.env")):
        run = meta_path.parent
        meta = parse_env(meta_path)
        output = parse_output(run / "h2load.out")
        durations, statuses = parse_requests(run / "requests.tsv")
        before = parse_counters(run / "sut-before.cpu.stat")
        after = parse_counters(run / "sut-after.cpu.stat")
        backend_before = parse_counters(run / "backend-before.cpu.stat")
        backend_after = parse_counters(run / "backend-after.cpu.stat")
        nstat_before = parse_counters(run / "nstat-before.txt")
        nstat_after = parse_counters(run / "nstat-after.txt")
        cpu_usec = delta(before, after, "usage_usec")
        cpu_seconds = cpu_usec / 1_000_000 if math.isfinite(cpu_usec) else math.nan
        backend_cpu_usec = delta(backend_before, backend_after, "usage_usec")
        backend_cpu_seconds = backend_cpu_usec / 1_000_000 if math.isfinite(backend_cpu_usec) else math.nan
        duration_seconds = float(meta.get("duration_seconds", "0"))
        good_requests = sum(value for key, value in statuses.items() if key.startswith("2"))
        good_rps = good_requests / duration_seconds if duration_seconds > 0 else math.nan
        row = {
            "case": meta.get("case", ""),
            "implementation": meta.get("implementation", ""),
            "repetition": int(meta.get("repetition", 0)),
            "rps": good_rps,
            "reported_rps": output["rps"],
            "total": output["total"],
            "succeeded": output["succeeded"],
            "failed": output["failed"],
            "errored": output["errored"],
            "timeout": output["timeout"],
            "http_2xx": good_requests,
            "http_5xx": sum(value for key, value in statuses.items() if key.startswith("5")),
            "status_zero": statuses.get("0", 0) + statuses.get("-1", 0),
            "logged": len(durations),
            "p50_us": percentile(durations, 0.50),
            "p95_us": percentile(durations, 0.95),
            "p99_us": percentile(durations, 0.99),
            "p999_us": percentile(durations, 0.999),
            "max_us": max(durations) if durations else math.nan,
            "sut_cpu_seconds": cpu_seconds,
            "requests_per_sut_cpu_second": good_requests / cpu_seconds if cpu_seconds > 0 else math.nan,
            "sut_cpu_percent": cpu_seconds / duration_seconds * 100 if duration_seconds > 0 else math.nan,
            "sut_throttled_seconds": delta(before, after, "throttled_usec") / 1_000_000,
            "sut_memory_current_mib": parse_integer(run / "sut-after.memory.current") / (1024 * 1024),
            "sut_memory_peak_mib": parse_integer(run / "sut-after.memory.peak") / (1024 * 1024),
            "backend_cpu_percent": backend_cpu_seconds / duration_seconds * 100 if duration_seconds > 0 else math.nan,
            "client_cpu_percent": parse_client_cpu_percent(run / "h2load.time"),
            "udp_in_errors": delta(nstat_before, nstat_after, "UdpInErrors"),
            "udp_rcvbuf_errors": delta(nstat_before, nstat_after, "UdpRcvbufErrors"),
            "udp_sndbuf_errors": delta(nstat_before, nstat_after, "UdpSndbufErrors"),
            "udp_sent": output["udp_sent"],
            "udp_received": output["udp_received"],
            "gate_status": int((run / "gate.status").read_text().strip()) if (run / "gate.status").exists() else -1,
            "load_status": int((run / "h2load.status").read_text().strip()) if (run / "h2load.status").exists() else -1,
        }
        rows.append(row)
    if not rows:
        print(f"no runs under {root}", file=sys.stderr)
        return 1

    with (root / "runs.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    groups = defaultdict(list)
    for row in rows:
        groups[(row["case"], row["implementation"])].append(row)
    summaries = []
    for (case_name, implementation), group in sorted(groups.items()):
        summaries.append({
            "case": case_name,
            "implementation": implementation,
            "runs": len(group),
            "rps_median": median([row["rps"] for row in group]),
            "rps_min": min(row["rps"] for row in group),
            "rps_max": max(row["rps"] for row in group),
            "p50_us_median": median([row["p50_us"] for row in group]),
            "p99_us_median": median([row["p99_us"] for row in group]),
            "p999_us_median": median([row["p999_us"] for row in group]),
            "cpu_efficiency_median": median([row["requests_per_sut_cpu_second"] for row in group]),
            "cpu_seconds_per_million_median": median([
                1_000_000 / row["requests_per_sut_cpu_second"]
                for row in group if row["requests_per_sut_cpu_second"] > 0
            ]),
            "sut_cpu_percent_median": median([row["sut_cpu_percent"] for row in group]),
            "sut_memory_peak_mib_median": median([row["sut_memory_peak_mib"] for row in group]),
            "client_cpu_percent_median": median([row["client_cpu_percent"] for row in group]),
            "backend_cpu_percent_median": median([row["backend_cpu_percent"] for row in group]),
            "udp_in_errors_total": sum(row["udp_in_errors"] for row in group),
            "udp_rcvbuf_errors_total": sum(row["udp_rcvbuf_errors"] for row in group),
            "udp_sndbuf_errors_total": sum(row["udp_sndbuf_errors"] for row in group),
            "failed_total": sum(row["failed"] for row in group),
            "http_5xx_total": sum(row["http_5xx"] for row in group),
            "status_zero_total": sum(row["status_zero"] for row in group),
            "gate_failures": sum(row["gate_status"] != 0 for row in group),
            "load_failures": sum(row["load_status"] != 0 for row in group),
        })
    with (root / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summaries[0]))
        writer.writeheader()
        writer.writerows(summaries)

    paired = []
    by_pair = defaultdict(dict)
    for row in rows:
        by_pair[(row["case"], row["repetition"])][row["implementation"]] = row
    for (case_name, repetition), implementations in sorted(by_pair.items()):
        if "lite" not in implementations or "nginx" not in implementations:
            continue
        lite = implementations["lite"]
        nginx = implementations["nginx"]
        paired.append({
            "case": case_name,
            "repetition": repetition,
            "rps_lite_over_nginx": lite["rps"] / nginx["rps"],
            "p99_lite_over_nginx": lite["p99_us"] / nginx["p99_us"],
            "cpu_efficiency_lite_over_nginx": lite["requests_per_sut_cpu_second"] / nginx["requests_per_sut_cpu_second"],
        })
    if paired:
        with (root / "paired.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(paired[0]))
            writer.writeheader()
            writer.writerows(paired)
        paired_groups = defaultdict(list)
        for row in paired:
            paired_groups[row["case"]].append(row)
        paired_summary = []
        for case_name, group in sorted(paired_groups.items()):
            rps_values = [row["rps_lite_over_nginx"] for row in group]
            p99_values = [row["p99_lite_over_nginx"] for row in group]
            cpu_values = [row["cpu_efficiency_lite_over_nginx"] for row in group]
            rps_low, rps_high = bootstrap_median_interval(rps_values)
            p99_low, p99_high = bootstrap_median_interval(p99_values)
            cpu_low, cpu_high = bootstrap_median_interval(cpu_values)
            paired_summary.append({
                "case": case_name,
                "pairs": len(group),
                "rps_ratio_median": median(rps_values),
                "rps_ratio_ci95_low": rps_low,
                "rps_ratio_ci95_high": rps_high,
                "p99_ratio_median": median(p99_values),
                "p99_ratio_ci95_low": p99_low,
                "p99_ratio_ci95_high": p99_high,
                "cpu_efficiency_ratio_median": median(cpu_values),
                "cpu_efficiency_ratio_ci95_low": cpu_low,
                "cpu_efficiency_ratio_ci95_high": cpu_high,
            })
        with (root / "paired-summary.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(paired_summary[0]))
            writer.writeheader()
            writer.writerows(paired_summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
