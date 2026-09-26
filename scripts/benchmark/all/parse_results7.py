#!/usr/bin/env python3
"""#7 post-fix full-matrix verification parser.

Reads temp/bench/results7/s*/  (bench7.sh output: every h3 scenario .txt ends
with a MEASURED "TIER_WORKERS: n" line — per-sample tier is mandatory, see
all_benchmark_6 §7.5 errata). Reports medians + Δ vs #5 (09-14) and #6 (09-26)
embedded baselines. H3 additionally reports per-worker (rps/tier) per sample
and its median — the only valid cross-run comparison basis.
"""
import json, math, re, sys
from pathlib import Path

BASE = Path("/software/Code/fiber-gateway-cpp/temp/bench/results7")
SAMPLES = sorted(int(d.name[1:]) for d in BASE.iterdir() if d.is_dir() and d.name.startswith("s"))

SCEN = ["get1k", "get64k", "get1m", "post1m"]
PROXIES = {"h1": ["lite", "openresty", "nginx"], "h2": ["lite", "openresty", "nginx"], "h3": ["lite", "nginx"]}

# ---- baselines: medians (raw rps) ----
SEP14 = {  # #5 report (true values; h1/h2 raw files were overwritten 09-23, report is authoritative)
    "h1": {"lite": [130367, 69183, 9758, 5380], "openresty": [214076, 83354, 10151, 2107], "nginx": [209025, 84005, 10362, 2142]},
    "h2": {"lite": [178596, 22149, 1771, 852], "openresty": [121177, 8582, 580, 272], "nginx": [109046, 11269, 805, 374]},
    "h3": {"lite": [93463, 13203, 1473, 751], "nginx": [61552, 8756, 616, 229]},
}
SEP26 = {  # #6 report medians (pre-fix, AES preference + libcrypto restored mid-run; see §7.4 6b rerun)
    "h1": {"lite": [144464, 77948, 10072, 5873], "openresty": [225147, 95188, 10239, 2151], "nginx": [218852, 92873, 9866, 2160]},
    "h2": {"lite": [159840, 10366, 733, 356], "openresty": [129228, 8721, 592, 274], "nginx": [114660, 11667, 820, 380]},
    "h3": {"lite": [95415, 6837, 475, 284], "nginx": [82813, 9153, 630, 279]},
}
# ---- baselines: H3 per-worker (~, from tier-listed sample tables in the reports) ----
SEP14W = {"lite": [36000, 6500, 505, 250], "nginx": [25000, 2900, 205, 92]}
SEP26W = {"lite": [32000, 2700, 190, 130], "nginx": [27000, 3000, 210, 94]}

def median(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2

def load(proto, proxy, scen):
    """-> list of dicts per sample: rps + tier (h3) + raw txt (first error lines)."""
    out = []
    for s in SAMPLES:
        txt_p = BASE / f"s{s}" / f"{proto}_{proxy}_{scen}.txt"
        if not txt_p.exists():
            continue
        txt = txt_p.read_text(errors="replace")
        if proto == "h3":
            jp = BASE / f"s{s}" / f"{proto}_{proxy}_{scen}.json"
            if not jp.exists():
                continue
            j = json.loads(jp.read_text())
            m = re.search(r"TIER_WORKERS:\s*(\d+)", txt)
            tier = int(m.group(1)) if m else None
            out.append({"rps": j["requests_per_second"], "tier": tier, "failed": j.get("failed", 0),
                        "drop": j["endpoint"].get("dropped_datagrams", 0)})
        else:
            rps = None
            if proto == "h1":
                m = re.search(r"Requests/sec:\s*([\d.]+)", txt)
                if m:
                    rps = float(m.group(1))
                non2xx = int((re.search(r"Non-2xx-or-3xx responses:\s*(\d+)", txt) or [None, "0"])[1]) if "Non-2xx" in txt else 0
                if "Socket errors" in txt and re.search(r"Socket errors: connect \d+, read \d+, write \d+, timeout \d+", txt):
                    se = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", txt)
                    if sum(map(int, se.groups())) > 0:
                        rps = None
                if non2xx:
                    rps = None
            else:  # h2
                m = re.search(r"finished in [\d.]+[a-z]+, ([\d.]+) req/s", txt)
                if m:
                    rps = float(m.group(1))
                if re.search(r"failed: (\d+)", txt) and re.search(r"failed: ([1-9]\d*)", txt):
                    rps = None
            if rps is not None:
                out.append({"rps": rps})
    return out

def fmt_delta(pct):
    return f"{pct:+.0f}%" if pct is not None else "n/a"

def pct(cur, base):
    return (cur / base - 1) * 100 if base else None

def report_h(proto):
    print(f"\n=== {proto.upper()} ===")
    hdr = f"| {'scenario':8} | {'proxy':10} | {'RPS med':>10} | {'Δ#5':>6} | {'Δ#6':>6} | samples |"
    print(hdr)
    for si, scen in enumerate(SCEN):
        for proxy in PROXIES[proto]:
            ss = load(proto, proxy, scen)
            if not ss:
                print(f"| {scen:8} | {proxy:10} | (no data) | | | |")
                continue
            rpss = [x["rps"] for x in ss]
            med = median(rpss)
            d5 = pct(med, SEP14[proto][proxy][si])
            d6 = pct(med, SEP26[proto][proxy][si])
            detail = ",".join(f"{x['rps']/1000:.1f}k" for x in ss)
            print(f"| {scen:8} | {proxy:10} | {med:>10,.0f} | {fmt_delta(d5):>6} | {fmt_delta(d6):>6} | {detail} |")

def report_h3():
    print("\n=== H3 (tier-measured per sample) ===")
    print("| scen | proxy | RPS med | Δ#5 | Δ#6 | per-sample rps(tier) | /w med | Δw#5 | Δw#6 |")
    for si, scen in enumerate(SCEN):
        for proxy in PROXIES["h3"]:
            ss = load("h3", proxy, scen)
            if not ss:
                print(f"| {scen} | {proxy} | (no data) | | | | | | |")
                continue
            rpss = [x["rps"] for x in ss]
            med = median(rpss)
            d5 = pct(med, SEP14["h3"][proxy][si])
            d6 = pct(med, SEP26["h3"][proxy][si])
            detail = ",".join(f"{x['rps']/1000:.2f}k({x['tier']}w)" if x["tier"] else f"{x['rps']/1000:.2f}k(?)" for x in ss)
            perw = [x["rps"] / x["tier"] for x in ss if x["tier"]]
            if perw:
                medw = median(perw)
                dw5 = pct(medw, SEP14W[proxy][si])
                dw6 = pct(medw, SEP26W[proxy][si])
                wtxt = f"{medw:,.0f} | {fmt_delta(dw5):>6} | {fmt_delta(dw6):>6}"
            else:
                wtxt = "n/a | n/a | n/a"
            print(f"| {scen} | {proxy} | {med:,.0f} | {fmt_delta(d5):>6} | {fmt_delta(d6):>6} | {detail} | {wtxt} |")

def integrity():
    bad = []
    for d in BASE.iterdir():
        if not d.is_dir():
            continue
        for f in d.glob("*.txt"):
            txt = f.read_text(errors="replace")
            if f.name.startswith("h3_") and "TIER_WORKERS" not in txt:
                bad.append(f"{f.name}: no TIER line")
            if "non-2xx or 3xx" in txt and re.search(r"[1-9]\d* non-2xx", txt):
                bad.append(f"{f.name}: non-2xx")
    if bad:
        print("INTEGRITY WARNINGS:")
        for b in bad:
            print(" ", b)
    else:
        print("integrity: all h3 samples tier-measured, no non-2xx flagged")

if __name__ == "__main__":
    print(f"samples: {SAMPLES}")
    integrity()
    for p in ("h1", "h2"):
        report_h(p)
    if "noh3" not in sys.argv:
        report_h3()
