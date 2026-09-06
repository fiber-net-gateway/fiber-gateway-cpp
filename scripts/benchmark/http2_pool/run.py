#!/usr/bin/env python3
"""Run the native pool client against an isolated, repository-pinned Nginx instance."""

import argparse
import datetime
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import signal
import socket
import subprocess
import time


ROOT = Path(__file__).resolve().parents[3]
NGINX = ROOT / "temp/nginx-install/sbin/nginx"
TEMPLATE = ROOT / "scripts/benchmark/http2_pool/configs/nginx_backend.conf"


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def resources(pid):
    root = Path(f"/proc/{pid}")
    try:
        status = dict(line.split(":", 1) for line in (root / "status").read_text().splitlines() if ":" in line)
        stat = (root / "stat").read_text().rsplit(")", 1)[1].split()
        result = {"pid": pid, "rss_kib": int(status.get("VmRSS", "0 kB").split()[0]),
                  "fds": len(list((root / "fd").iterdir())), "threads": int(status.get("Threads", "0")),
                  "cpu_ticks": int(stat[11]) + int(stat[12])}
        for line in (root / "smaps_rollup").read_text().splitlines():
            if line.startswith("Pss:"):
                result["pss_kib"] = int(line.split()[1])
        return result
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return {"pid": pid, "exited": True}


def children(pid):
    try:
        return [int(p) for p in Path(f"/proc/{pid}/task/{pid}/children").read_text().split()]
    except FileNotFoundError:
        return []


class Backend:
    def __init__(self, directory, fixtures, cert, key, args, keepalive):
        self.directory = directory
        self.process = None
        self.stopped_workers = set()
        self.cpu_prefix = ["taskset", "-c", args.backend_cpus] if args.backend_cpus else []
        self.log = (directory / "nginx-process.log").open("w")
        self.config = directory / "nginx.conf"
        text = TEMPLATE.read_text().replace("build/http2-pool-bench/www", "@FIXTURES@")
        text = text.replace("build/http2-pool-bench", str(directory))
        text = text.replace("@FIXTURES@", str(fixtures))
        text = text.replace("127.0.0.1:18082", f"127.0.0.1:{args.port}")
        text = text.replace("127.0.0.1:18083", f"127.0.0.1:{args.port + 1}")
        text = text.replace("keepalive_requests 1000000000;", f"keepalive_requests {keepalive};")
        # Keep full logs by default; the option is explicit in the manifest.
        if args.no_access_log:
            text = text.replace(f"access_log {directory}/access.jsonl pool_bench buffer=64k flush=1s;", "access_log off;")
        tls = f"""
    server {{
        listen 127.0.0.1:{args.port + 2} ssl;
        server_name localhost;
        ssl_certificate {cert};
        ssl_certificate_key {key};
        ssl_protocols TLSv1.2 TLSv1.3;
        location / {{ try_files $uri =404; }}
        location = /slow.bin {{ limit_rate 64k; try_files /large.bin =404; }}
    }}
"""
        self.config.write_text(text.rsplit("}", 1)[0] + tls + "}\n")
        self.command = [str(NGINX), "-p", str(ROOT) + "/", "-c", str(self.config)]

    def start(self):
        subprocess.run(self.command + ["-t"], check=True, stdout=self.log, stderr=self.log)
        self.process = subprocess.Popen(self.cpu_prefix + self.command + ["-g", "daemon off;"],
                                        stdout=self.log, stderr=self.log, start_new_session=True)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"Nginx exited; see {self.log.name}")
            if children(self.process.pid):
                return
            time.sleep(0.02)
        raise RuntimeError("Nginx workers did not start")

    def signal_workers(self, sig):
        targets = children(self.process.pid)
        if not targets:
            raise RuntimeError("No owned Nginx workers to signal")
        for pid in targets:
            try:
                os.kill(pid, sig)
                if sig == signal.SIGSTOP:
                    self.stopped_workers.add(pid)
            except ProcessLookupError:
                pass
        return targets

    def resume(self):
        for pid in self.stopped_workers:
            # Only signal children of our still-running master; do not risk PID reuse.
            if self.process and self.process.poll() is None and pid in children(self.process.pid):
                os.kill(pid, signal.SIGCONT)
        self.stopped_workers.clear()

    def stop(self):
        self.resume()
        if self.process and self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=5)

    def close(self):
        self.stop()
        self.log.close()


def cases(args, cert):
    duration = args.duration_ms or (10000 if args.suite == "smoke" else 30000)
    base = ["--port", str(args.port), "--duration-ms", str(duration), "--warmup-ms", "1000"]
    rounds = 20 if args.suite == "smoke" else 1000
    result = []

    def add(name, extra=(), fault=None, keepalive=1000000000):
        result.append({"name": name, "args": base + list(extra), "fault": fault, "keepalive": keepalive,
                       "duration_ms": duration})

    add("single_connection", ["--loops", "1", "--concurrency", "128", "--connections", "1", "--streams", "128"])
    add("four_loops", ["--loops", "4", "--concurrency", "128"])
    add("acquire_only", ["--loops", "4", "--concurrency", "128", "--acquire-only"])
    add("peer_one", ["--port", str(args.port + 1), "--pre-settings", "0", "--concurrency", "32"])
    add("pre_settings_zero", ["--pre-settings", "0", "--lifetime", "17"])
    add("parallel_dials", ["--concurrency", "128", "--streams", "1", "--dials", "4", "--dial-delay-ms", "10"])
    add("parallel_dials_with_budget", ["--concurrency", "128", "--streams", "1", "--dials", "4",
                                      "--dial-delay-ms", "10", "--acquire-ms", "5000", "--timeout-ms", "6000"])
    add("cancel_50", ["--cancel-percent", "50", "--path", "/large.bin"])
    add("cancel_pair", ["--loops", "1", "--concurrency", "2", "--connections", "1",
                        "--cancel-percent", "50", "--path", "/large.bin"])
    add("large_pair_control", ["--loops", "1", "--concurrency", "2", "--connections", "1", "--path", "/large.bin"])
    add("cancel_recovery", ["--cancel-percent", "50", "--cancel-until-ms", "5000", "--path", "/large.bin"])
    add("flow_control", ["--path", "/large.bin", "--concurrency", "32", "--read-delay-ms", "100"])
    add("idle_zero", ["--idle", "0", "--concurrency", "32"])
    add("idle_expiry", ["--idle-ms", "100", "--rate", "4", "--concurrency", "1"])
    add("lifetime_one", ["--lifetime", "1"])
    add("multi_key", ["--keys", "1024", "--idle", "1", "--total-connections", "8", "--allow-errors"])
    add("clear_under_load", ["--clear-ms", "300", "--concurrency", "128"])
    add("goaway", ["--concurrency", "32", "--allow-errors"], keepalive=17)
    add("reload", ["--allow-errors"], "reload")
    add("worker_kill", ["--allow-errors"], "kill")
    add("connection_refused", ["--allow-errors"], "stop")
    add("unresponsive_backend", ["--allow-errors"], "pause")
    add("tls", ["--port", str(args.port + 2), "--tls", "--ca-file", str(cert)])
    add("tls_churn", ["--port", str(args.port + 2), "--tls", "--ca-file", str(cert), "--lifetime", "17"])
    add("tls_bad_trust", ["--port", str(args.port + 2), "--tls", "--bad-trust", "--allow-errors"])
    add("fifo", ["--scenario", "fifo", "--loops", "4", "--rounds", str(rounds)])
    add("lifecycle", ["--scenario", "lifecycle", "--loops", "4", "--rounds", str(rounds)])
    add("bounded_overload", ["--loops", "2", "--concurrency", "64", "--streams", "1", "--connections", "1",
                             "--rate", "10000", "--hold-ms", "10", "--acquire-ms", "20", "--allow-errors"])
    if args.suite == "soak":
        result = []
        duration = args.soak_seconds * 1000
        base = ["--port", str(args.port), "--duration-ms", str(duration), "--warmup-ms", "10000"]
        add("soak", ["--loops", "4", "--concurrency", "128", "--keys", "64", "--cancel-percent", "10",
                     "--lifetime", "1000", "--clear-ms", "60000", "--allow-errors"])
    if args.case:
        selected = set(args.case.split(","))
        result = [case for case in result if case["name"] in selected]
        if {case["name"] for case in result} != selected:
            raise ValueError("Unknown case name")
    if any(case["fault"] and case["duration_ms"] < 10000 for case in result):
        raise ValueError("Fault cases require at least 10000 ms to observe recovery")
    return result


def run_case(args, case, directory, fixtures, cert, key):
    directory.mkdir()
    backend = Backend(directory, fixtures, cert, key, args, case["keepalive"])
    command = (["taskset", "-c", args.client_cpus] if args.client_cpus else []) + [str(args.binary)] + case["args"]
    save_json(directory / "command.json", command)
    client = None
    events = []
    began = time.monotonic()
    fault_at = 2.0 + min(3.0, case["duration_ms"] / 1000 * 0.2)
    injected = restored = False
    summary = None
    try:
        backend.start()
        # Readiness only; the native client's header checks verify HTTP/2 end to end.
        probe = http.client.HTTPConnection("127.0.0.1", args.port, timeout=3)
        probe.request("GET", "/small.bin")
        response = probe.getresponse()
        if response.status != 200 or len(response.read()) != 1024:
            raise RuntimeError("Nginx fixture readiness failed")
        probe.close()
        with (directory / "client.jsonl").open("w") as out, (directory / "client.stderr").open("w") as err, \
                (directory / "resources.jsonl").open("w") as samples:
            client = subprocess.Popen(command, stdout=out, stderr=err, cwd=ROOT)
            began = time.monotonic()
            next_sample = 0
            watchdog = 60 + case["duration_ms"] / 1000
            if case["name"] in ("fifo", "lifecycle"):
                watchdog = 60 + (20 if args.suite == "smoke" else 1000) * 2
            while client.poll() is None:
                elapsed = time.monotonic() - began
                if elapsed > watchdog:
                    client.send_signal(signal.SIGABRT)
                    events.append({"at": elapsed, "event": "watchdog"})
                    try:
                        client.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        client.kill()
                    break
                if elapsed >= next_sample:
                    samples.write(json.dumps({"at": elapsed, "client": resources(client.pid),
                                              "backend": [resources(pid) for pid in children(backend.process.pid)]}) + "\n")
                    samples.flush()
                    next_sample += 1
                if case["fault"] and not injected and elapsed >= fault_at:
                    event = {"at": elapsed, "event": case["fault"], "workers_before": children(backend.process.pid)}
                    if case["fault"] == "reload":
                        backend.process.send_signal(signal.SIGHUP)
                    elif case["fault"] == "kill":
                        backend.signal_workers(signal.SIGKILL)
                    elif case["fault"] == "pause":
                        backend.signal_workers(signal.SIGSTOP)
                    elif case["fault"] == "stop":
                        backend.stop()
                    events.append(event)
                    injected = True
                if injected and not restored and elapsed >= fault_at + 3:
                    if case["fault"] == "pause":
                        backend.resume()
                    elif case["fault"] == "stop":
                        backend.start()
                    events.append({"at": elapsed, "event": "restored", "workers_after": children(backend.process.pid)})
                    restored = True
                time.sleep(0.05)
            client.wait(timeout=5)
        client_samples = []
        malformed_output = False
        for line in (directory / "client.jsonl").read_text().splitlines():
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                malformed_output = True
                continue
            if value["type"] == "summary":
                summary = value
            elif value["type"] == "sample":
                client_samples.append(value)
        passed = client.returncode == 0 and summary is not None and summary["pass"] and not malformed_output
        if case["fault"]:
            passed = passed and injected and restored
            # Compare cumulative counters in a post-recovery window for every loop.
            # This does not hide fault-window errors behind --allow-errors.
            recovery = []
            for loop in range((summary or {}).get("loops", 0)):
                after = [s for s in client_samples if s["loop"] == loop and
                         s["elapsed_ms"] >= (fault_at + 4) * 1000]
                clean = len(after) >= 2 and after[-1]["success"] > after[0]["success"] and \
                    after[-1]["errors"] == after[0]["errors"]
                recovery.append({"loop": loop, "clean": clean,
                                 "window_ms": after[-1]["elapsed_ms"] - after[0]["elapsed_ms"] if after else 0})
            passed = passed and bool(recovery) and all(item["clean"] for item in recovery)
            events.append({"event": "recovery_check", "loops": recovery})
        if case["name"] == "tls_bad_trust" and summary:
            passed = passed and summary["success"] == 0 and summary["dial_error"] > 0
        if case["name"] == "goaway" and summary:
            passed = passed and summary["goaway"] > 0
        result = {"name": case["name"], "pass": bool(passed), "returncode": client.returncode,
                  "elapsed_s": time.monotonic() - began, "summary": summary, "events": events}
    finally:
        if client and client.poll() is None:
            client.kill()
            client.wait()
        backend.close()
    save_json(directory / "result.json", result)
    print(json.dumps({"case": case["name"], "pass": result["pass"], "seconds": round(result["elapsed_s"], 2),
                      "rps": (summary or {}).get("measured_rps"), "errors": (summary or {}).get("errors"),
                      "violations": (summary or {}).get("violations")}), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build-http2-pool-release/example/http2_pool_benchmark")
    parser.add_argument("--suite", choices=("smoke", "standard", "soak"), default="smoke")
    parser.add_argument("--case", help="comma-separated case names")
    parser.add_argument("--duration-ms", type=int, default=0)
    parser.add_argument("--soak-seconds", type=int, default=28800)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--port", type=int, default=18082)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--no-access-log", action="store_true")
    parser.add_argument("--client-cpus", help="taskset CPU list, e.g. 0-3")
    parser.add_argument("--backend-cpus", help="disjoint taskset CPU list, e.g. 4-5")
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    if not args.binary.is_file() or args.repeat < 1 or not 1024 <= args.port <= 65533:
        parser.error("build the benchmark first; repeat must be positive and port in 1024..65533")
    if args.duration_ms < 0 or args.soak_seconds < 1:
        parser.error("durations must be positive")
    for port in range(args.port, args.port + 3):
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("127.0.0.1", port))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = (args.output or ROOT / "build/http2-pool-bench/results" / stamp).resolve()
    output.mkdir(parents=True, exist_ok=False)
    fixtures = output / "www"
    fixtures.mkdir()
    hashes = {}
    for name, size in (("small.bin", 1024), ("medium.bin", 65536), ("large.bin", 1048576)):
        body = bytes(range(256)) * (size // 256)
        (fixtures / name).write_bytes(body)
        hashes[name] = hashlib.sha256(body).hexdigest()
    cert, key = output / "cert.pem", output / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=localhost",
                    "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1", "-keyout", str(key), "-out", str(cert)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    version = subprocess.run([str(NGINX), "-V"], capture_output=True, text=True, check=True).stderr
    manifest = {"created_utc": stamp, "suite": args.suite, "args": {k: str(v) if isinstance(v, Path) else v
                for k, v in vars(args).items()}, "nginx": version, "platform": platform.platform(),
                "cpu_affinity": sorted(os.sched_getaffinity(0)), "clock_ticks": os.sysconf("SC_CLK_TCK"),
                "git_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "git_status": subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True),
                "fixtures_sha256": hashes, "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest()}
    cache = args.binary.parent.parent / "CMakeCache.txt"
    if cache.exists():
        manifest["build_settings"] = [line for line in cache.read_text().splitlines() if line.startswith(
            ("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS:", "CMAKE_EXE_LINKER_FLAGS:",
             "FIBER_ENABLE_LTO:", "FIBER_USE_JEMALLOC:"))]
    save_json(output / "manifest.json", manifest)
    results = []
    print(f"Results: {output}", flush=True)
    for repetition in range(args.repeat):
        for case in cases(args, cert):
            result = run_case(args, case, output / f"{repetition + 1:02d}-{case['name']}", fixtures, cert, key)
            results.append(result)
            save_json(output / "results.json", results)
    print(f"Passed {sum(result['pass'] for result in results)}/{len(results)} cases", flush=True)
    return 0 if all(result["pass"] for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
