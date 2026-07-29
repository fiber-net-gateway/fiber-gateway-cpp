#!/usr/bin/env python3

import csv
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def parse_counters(path):
    result = {}
    if not path.exists():
        return result
    for line in path.read_text(errors="replace").splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1].isdigit():
            result[fields[0]] = int(fields[1])
    return result


def delta(before, after, name):
    if name not in before or name not in after:
        return math.nan
    return max(0, after[name] - before[name])


def median(rows, name):
    values = [row[name] for row in rows if math.isfinite(row[name])]
    return statistics.median(values) if values else math.nan


def coefficient_of_variation(values):
    if len(values) < 2:
        return math.nan
    mean = statistics.mean(values)
    return statistics.stdev(values) / mean if mean else math.nan


def main():
    if len(sys.argv) != 2:
        print("usage: summarize_http3.py <result-directory>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    rows = []
    for json_path in sorted(root.glob("runs/*/rep-*/*/client.json")):
        run = json_path.parent
        data = json.loads(json_path.read_text())
        before = parse_counters(run / "sut-before.cpu.stat")
        after = parse_counters(run / "sut-after.cpu.stat")
        nstat_before = parse_counters(run / "nstat-before.txt")
        nstat_after = parse_counters(run / "nstat-after.txt")
        status = int((run / "client.status").read_text().strip())
        cpu_seconds = delta(before, after, "usage_usec") / 1_000_000
        duration_seconds = data["measurement_elapsed_ms"] / 1000
        udp_in_errors = delta(nstat_before, nstat_after, "UdpInErrors")
        udp_rcvbuf_errors = delta(nstat_before, nstat_after, "UdpRcvbufErrors")
        udp_sndbuf_errors = delta(nstat_before, nstat_after, "UdpSndbufErrors")
        phase_errors = sum(data["phase_errors"].values())
        valid = all(
            (
                status == 0,
                data["failed"] == 0,
                phase_errors == 0,
                data["endpoint"]["dropped_datagrams"] == 0,
                data["endpoint"]["recv_storage_rejected"] == 0,
                udp_in_errors == 0,
                udp_rcvbuf_errors == 0,
                udp_sndbuf_errors == 0,
            )
        )
        rows.append(
            {
                "case": json_path.parts[-4],
                "implementation": json_path.parts[-2],
                "repetition": int(json_path.parts[-3].removeprefix("rep-")),
                "valid": int(valid),
                "rps": data["requests_per_second"],
                "mib_per_second": data["mib_per_second"],
                "p50_us": data["latency"]["total"]["p50_us"],
                "p99_us": data["latency"]["total"]["p99_us"],
                "p999_us": data["latency"]["total"]["p999_us"],
                "succeeded": data["succeeded"],
                "failed": data["failed"],
                "phase_errors": phase_errors,
                "endpoint_drops": data["endpoint"]["dropped_datagrams"],
                "recv_storage_rejected": data["endpoint"]["recv_storage_rejected"],
                "udp_in_errors": udp_in_errors,
                "udp_rcvbuf_errors": udp_rcvbuf_errors,
                "udp_sndbuf_errors": udp_sndbuf_errors,
                "pto_count": data["quic"]["pto_count"],
                "sut_cpu_seconds": cpu_seconds,
                "sut_cpu_percent": cpu_seconds / duration_seconds * 100,
                "requests_per_sut_cpu_second": (
                    data["succeeded"] / cpu_seconds if cpu_seconds else math.nan
                ),
            }
        )
    if not rows:
        print(f"no client JSON files under {root}", file=sys.stderr)
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
        valid_group = [row for row in group if row["valid"]]
        measured = valid_group if valid_group else group
        rps_values = [row["rps"] for row in measured]
        summaries.append(
            {
                "case": case_name,
                "implementation": implementation,
                "runs": len(group),
                "valid_runs": len(valid_group),
                "rps_median": median(measured, "rps"),
                "rps_cv": coefficient_of_variation(rps_values),
                "mib_per_second_median": median(measured, "mib_per_second"),
                "p50_us_median": median(measured, "p50_us"),
                "p99_us_median": median(measured, "p99_us"),
                "p999_us_median": median(measured, "p999_us"),
                "sut_cpu_percent_median": median(measured, "sut_cpu_percent"),
                "cpu_efficiency_median": median(
                    measured, "requests_per_sut_cpu_second"
                ),
                "failed_total": sum(row["failed"] for row in group),
                "endpoint_drops_total": sum(
                    row["endpoint_drops"] for row in group
                ),
                "udp_errors_total": sum(
                    row["udp_in_errors"]
                    + row["udp_rcvbuf_errors"]
                    + row["udp_sndbuf_errors"]
                    for row in group
                ),
                "pto_total": sum(row["pto_count"] for row in group),
            }
        )
    with (root / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summaries[0]))
        writer.writeheader()
        writer.writerows(summaries)

    by_pair = defaultdict(dict)
    for row in rows:
        by_pair[(row["case"], row["repetition"])][row["implementation"]] = row
    paired = []
    for (case_name, repetition), pair in sorted(by_pair.items()):
        if "lite" not in pair or "openresty" not in pair:
            continue
        lite = pair["lite"]
        openresty = pair["openresty"]
        paired.append(
            {
                "case": case_name,
                "repetition": repetition,
                "valid": int(lite["valid"] and openresty["valid"]),
                "rps_lite_over_openresty": lite["rps"] / openresty["rps"],
                "p99_lite_over_openresty": lite["p99_us"]
                / openresty["p99_us"],
                "cpu_efficiency_lite_over_openresty": lite[
                    "requests_per_sut_cpu_second"
                ]
                / openresty["requests_per_sut_cpu_second"],
            }
        )
    with (root / "paired.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(paired[0]))
        writer.writeheader()
        writer.writerows(paired)

    with (root / "summary.md").open("w") as stream:
        stream.write(
            "| Case | Implementation | Valid | RPS median | RPS CV | MiB/s | "
            "p50 ms | p99 ms | p99.9 ms | SUT CPU % | req/SUT CPU-s | "
            "Errors | PTO |\n"
        )
        stream.write("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for row in summaries:
            errors = row["failed_total"] + row["endpoint_drops_total"] + row[
                "udp_errors_total"
            ]
            stream.write(
                f"| {row['case']} | {row['implementation']} | "
                f"{row['valid_runs']}/{row['runs']} | {row['rps_median']:.0f} | "
                f"{row['rps_cv']:.3f} | {row['mib_per_second_median']:.1f} | "
                f"{row['p50_us_median'] / 1000:.2f} | "
                f"{row['p99_us_median'] / 1000:.2f} | "
                f"{row['p999_us_median'] / 1000:.2f} | "
                f"{row['sut_cpu_percent_median']:.1f} | "
                f"{row['cpu_efficiency_median']:.0f} | {errors} | "
                f"{row['pto_total']} |\n"
            )
    print(root / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
