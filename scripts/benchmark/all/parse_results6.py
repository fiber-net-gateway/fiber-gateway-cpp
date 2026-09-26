#!/usr/bin/env python3
"""Parse multi-sample benchmark results (temp/bench/results6/s*/) into median
tables, compared against the 2026-09-14 baseline #5 (feature/all_benchmark_5.md,
git a0263f3 — last BoringSSL-TLS build before the in-house TLS engine swap)."""
import glob, json, os, re, statistics, sys

BASE = "/software/Code/fiber-gateway-cpp/temp/bench/results6"
SCENS = [("get1k", "GET 1 KiB"), ("get64k", "GET 64 KiB"), ("get1m", "GET 1 MiB"), ("post1m", "POST echo 1 MiB")]
PROX = {"h1": ["lite", "openresty", "nginx"], "h2": ["lite", "openresty", "nginx"], "h3": ["lite", "nginx"]}
PN = {"lite": "lite-nginx", "openresty": "OpenResty", "nginx": "nginx(quic)"}

# 2026-09-14 baseline #5 median RPS (feature/all_benchmark_5.md, git a0263f3)
SEP14 = {
    ("h1", "get1k"): {"lite": 130367, "openresty": 214076, "nginx": 209025},
    ("h1", "get64k"): {"lite": 69183, "openresty": 83354, "nginx": 84005},
    ("h1", "get1m"): {"lite": 9758, "openresty": 10151, "nginx": 10362},
    ("h1", "post1m"): {"lite": 5380, "openresty": 2107, "nginx": 2142},
    ("h2", "get1k"): {"lite": 178596, "openresty": 121177, "nginx": 109046},
    ("h2", "get64k"): {"lite": 22149, "openresty": 8582, "nginx": 11269},
    ("h2", "get1m"): {"lite": 1771, "openresty": 580, "nginx": 805},
    ("h2", "post1m"): {"lite": 852, "openresty": 272, "nginx": 374},
    ("h3", "get1k"): {"lite": 93463, "nginx": 61552},
    ("h3", "get64k"): {"lite": 13203, "nginx": 8756},
    ("h3", "get1m"): {"lite": 1473, "nginx": 616},
    ("h3", "post1m"): {"lite": 751, "nginx": 229},
}


def to_ms(v):
    if v is None: return None
    s = str(v).strip()
    m = re.match(r'^([\d.]+)\s*(us|ms|s|ns)?$', s)
    if not m:
        try: return float(s) / 1000.0
        except Exception: return None
    val, unit = float(m.group(1)), m.group(2)
    return {"us": val / 1000, "ms": val, "s": val * 1000, "ns": val / 1e6, None: val / 1000}.get(unit)


def to_mbs(v):
    if v is None: return None
    m = re.match(r'^([\d.]+)\s*([KMGT]?i?B)', str(v).strip())
    if not m: return None
    mult = {"B": 1, "KB": 1e3, "MB": 1e6, "GB": 1e9, "TB": 1e12,
            "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3, "TiB": 1024**4}
    return float(m.group(1)) * mult.get(m.group(2), 1e6) / 1e6


def parse_h1(txt):
    d = {}
    m = re.search(r'Requests/sec:\s*([\d.]+)', txt); d['rps'] = float(m.group(1)) if m else None
    m = re.search(r'Transfer/sec:\s*([\d.]+)([KMGT]?B)', txt); d['bw'] = to_mbs(m.group(1) + m.group(2)) if m else None
    m = re.search(r'Non-2xx or 3xx responses:\s*(\d+)', txt); d['fail'] = int(m.group(1)) if m else 0
    m = re.search(r'Socket errors:\s*connect (\d+), read (\d+), write (\d+), timeout (\d+)', txt)
    if m: d['fail'] += sum(int(g) for g in m.groups())
    for p in ('50', '90', '99'):
        m = re.search(r'^\s*' + p + r'%\s+(\S+)', txt, re.M); d['p' + p] = to_ms(m.group(1)) if m else None
    return d


def parse_h2(txt):
    d = {}
    m = re.search(r'finished in [\d.]+s,\s*([\d.]+) req/s,\s*([\d.]+)([KMGT]?B)/s', txt)
    if m: d['rps'], d['bw'] = float(m.group(1)), to_mbs(m.group(2) + m.group(3))
    m = re.search(r'(\d+) succeeded.*?(\d+) failed', txt)
    if m: d['fail'] = int(m.group(2))
    m = re.search(r'time for request:\s*\S+\s+(\S+)\s+(\S+)', txt, re.M)
    if m: d['max'], d['mean'] = to_ms(m.group(1)), to_ms(m.group(2))
    return d


def parse_h3(txt, js):
    if js is not None:
        return {"rps": js["requests_per_second"], "bw": js["mib_per_second"] * 1.048576,
                "p50": js["latency"]["total"]["p50_us"] / 1000,
                "p90": js["latency"]["total"]["p90_us"] / 1000,
                "p99": js["latency"]["total"]["p99_us"] / 1000,
                "fail": js["failed"], "drops": js["endpoint"]["dropped_datagrams"]}
    d = {}
    m = re.search(r'throughput=([\d.]+) req/s,\s*([\d.]+) MiB/s', txt)
    if m: d['rps'], d['bw'] = float(m.group(1)), float(m.group(2)) * 1.048576
    m = re.search(r'failed=(\d+)', txt); d['fail'] = int(m.group(1)) if m else None
    m = re.search(r'dropped_datagrams=(\d+)', txt); d['drops'] = int(m.group(1)) if m else None
    return d


PARS = {"h1": parse_h1, "h2": parse_h2, "h3": parse_h3}


def median_key(dicts, key):
    vals = [d[key] for d in dicts if d.get(key) is not None]
    return statistics.median(vals) if vals else None


def spread(dicts, key):
    vals = [d[key] for d in dicts if d.get(key) is not None]
    if len(vals) < 2: return ""
    lo, hi = min(vals), max(vals)
    return f"±{(hi - lo) / 2 / statistics.median(vals) * 100:.0f}%"


def load(proto, px, skey):
    """All samples for one cell: list of parsed dicts."""
    out = []
    for f in sorted(glob.glob(os.path.join(BASE, "s*", f"{proto}_{px}_{skey}.txt"))):
        txt = open(f).read()
        js = None
        jf = f[:-4] + ".json"
        if os.path.exists(jf):
            try: js = json.load(open(jf))
            except Exception: pass
        d = PARS[proto](txt, js) if proto == "h3" else PARS[proto](txt)
        if d.get("rps") is not None: out.append(d)
    return out


def fmt(x, prec=0):
    return "-" if x is None else (f"{x:,.{prec}f}" if isinstance(x, float) else str(x))


def delta(proto, skey, px, rps):
    base = SEP14.get((proto, skey), {}).get(px)
    if base is None or rps is None: return "-"
    return f"{(rps / base - 1) * 100:+.0f}%"


only_proto = sys.argv[1] if len(sys.argv) > 1 else None
for proto in ["h1", "h2", "h3"]:
    if only_proto and proto != only_proto: continue
    print(f"\n## {proto.upper()}  (median across samples; Δ vs 2026-09-14 baseline #5)")
    cols = {"h1": ["RPS", "Δ", "spread", "BW MB/s", "p50", "p90", "p99", "err"],
            "h2": ["RPS", "Δ", "spread", "BW MB/s", "mean", "max", "err"],
            "h3": ["RPS", "Δ", "spread", "BW MB/s", "p50", "p90", "p99", "drops"]}[proto]
    units = {"p50": "ms", "p90": "ms", "p99": "ms", "mean": "ms", "max": "ms"}
    print("| scenario | proxy | " + " | ".join(c + (" " + units[c] if c in units else "") for c in cols) + " |")
    print("|---|---|" + "---:|" * len(cols))
    for skey, sname in SCENS:
        for px in PROX[proto]:
            ds = load(proto, px, skey)
            if not ds:
                print(f"| {sname} | {PN[px]} | (missing) |" + " |" * (len(cols) - 1)); continue
            rps = median_key(ds, "rps")
            row = [sname, PN[px]]
            for c in cols:
                if c == "RPS": row.append(fmt(rps))
                elif c == "Δ": row.append(delta(proto, skey, px, rps))
                elif c == "spread": row.append(spread(ds, "rps"))
                elif c == "err": row.append(str(sum(d.get("fail", 0) or 0 for d in ds)))
                elif c == "drops": row.append(str(sum(d.get("drops", 0) or 0 for d in ds)))
                else: row.append(fmt(median_key(ds, c.split()[0].lower()), 2 if c in units else 0))
            print("| " + " | ".join(row) + " |")
    print(f"(samples per cell: {len(glob.glob(os.path.join(BASE, 's*', 'h1_*')))} dirs)")
