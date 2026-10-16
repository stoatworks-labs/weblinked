#!/usr/bin/env python3
"""control_stress — leans on a running WebLinked the way issue #12's rig did.

Four clients poll GET /api/state every 50 ms with a 300 ms timeout (Daniel's
supervisor polled every 200 ms with the same timeout; this is twenty times the
traffic) while one of these runs:

  toggle  disable and re-enable one output over POST /api/output
  stop    SIGSTOP the process, wait, SIGCONT it — as a stall longer than any
          output's queue (needs --pid)
  cap     hold more idle connections open than the server allows, then ask
          for /api/state on one more

It reports how every poll went, how slow /api/state got, the process's thread
and connection counts (from ps and lsof, once a second), and the output's own
counters before and after — for a DeckLink that includes frames_late,
frames_dropped, reanchors and ring_misses. Python 3, standard library only.

  tools/control_stress.py --port 7654 --output decklink0 toggle --cycles 20 --interval 3
  tools/control_stress.py --port 7654 --output decklink0 --pid 1234 stop --cycles 11 --pause 3 --interval 60
  tools/control_stress.py --port 7654 --output decklink0 cap --connections 80

See docs/04-verification.md section 40.
"""
import argparse
import http.client
import json
import os
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time

COUNTERS = ("frames", "frames_late", "frames_dropped", "reanchors",
            "reanchored_frames", "ring_misses", "buffered_frames")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7654)
    parser.add_argument("--token", default="")
    parser.add_argument("--output", required=True, help="output name, as in /api/state")
    parser.add_argument("--pid", type=int, default=0, help="WebLinked's pid (stop; sampling)")
    parser.add_argument("--pollers", type=int, default=4)
    sub = parser.add_subparsers(dest="phase", required=True)
    toggle = sub.add_parser("toggle")
    toggle.add_argument("--cycles", type=int, default=20)
    toggle.add_argument("--interval", type=float, default=3.0, help="seconds between changes")
    stop = sub.add_parser("stop")
    stop.add_argument("--cycles", type=int, default=6)
    stop.add_argument("--pause", type=float, default=3.0, help="seconds stopped")
    stop.add_argument("--interval", type=float, default=10.0, help="seconds running between stops")
    cap = sub.add_parser("cap")
    cap.add_argument("--connections", type=int, default=80)
    args = parser.parse_args()
    if args.phase == "stop" and not args.pid:
        parser.error("stop needs --pid")

    headers = {"Connection": "close"}
    if args.token:
        headers["Authorization"] = "Bearer " + args.token

    def request(method, path, body=None, timeout=10.0):
        conn = http.client.HTTPConnection(args.host, args.port, timeout=timeout)
        try:
            sent = dict(headers)
            data = None
            if body is not None:
                data = json.dumps(body)
                sent["Content-Type"] = "application/json"
            conn.request(method, path, body=data, headers=sent)
            response = conn.getresponse()
            return response.status, response.read()
        finally:
            conn.close()

    def state():
        status, body = request("GET", "/api/state", timeout=5)
        if status != 200:
            raise RuntimeError(f"/api/state answered {status}: {body[:200]!r}")
        return json.loads(body)

    def output_of(s):
        for o in s.get("outputs", []):
            if o.get("name") == args.output:
                return o
        raise SystemExit(f"no output called {args.output!r}; have "
                         f"{[o.get('name') for o in s.get('outputs', [])]}")

    def describe(label, s):
        o = output_of(s)
        p = s.get("pacing", {})
        counters = " ".join(f"{k}={o[k]}" for k in COUNTERS if k in o)
        print(f"{label}: ticks={p.get('ticks')} dropped_ticks={p.get('dropped_ticks')} "
              f"last_tick_age_ms={p.get('last_tick_age_ms')} output_skips={p.get('output_skips')} | "
              f"{args.output}: enabled={o.get('enabled')} running={o.get('running')} "
              f"stale={o.get('stale')} {counters}", flush=True)

    stopping = threading.Event()
    lock = threading.Lock()
    tally = {"ok": 0, "timeout": 0, "refused": 0, "error": 0, "stale": 0}
    latencies = []

    def poller():
        while not stopping.is_set():
            start = time.monotonic()
            try:
                status, body = request("GET", "/api/state", timeout=0.3)
                took = (time.monotonic() - start) * 1000
                with lock:
                    if status == 200:
                        tally["ok"] += 1
                        latencies.append(took)
                        s = json.loads(body)
                        if s.get("stale") or any(o.get("stale") for o in s.get("outputs", [])):
                            tally["stale"] += 1
                    elif status == 503:
                        tally["refused"] += 1
                    else:
                        tally["error"] += 1
            except socket.timeout:
                with lock:
                    tally["timeout"] += 1
            except Exception:
                with lock:
                    tally["error"] += 1
            time.sleep(0.05)

    samples = []

    def sampler():
        while not stopping.is_set() and args.pid:
            try:
                # One line per thread: -M on macOS, -L on Linux.
                flag = "-M" if sys.platform == "darwin" else "-L"
                ps = subprocess.run(["ps", flag, "-p", str(args.pid)],
                                    capture_output=True, text=True).stdout.splitlines()
                lsof = subprocess.run(["lsof", "-nP", "-a", "-p", str(args.pid),
                                       f"-iTCP:{args.port}", "-sTCP:ESTABLISHED"],
                                      capture_output=True, text=True).stdout.splitlines()
                samples.append((max(0, len(ps) - 1), max(0, len(lsof) - 1)))
            except Exception:
                pass
            time.sleep(1.0)

    threads = [threading.Thread(target=poller, daemon=True) for _ in range(args.pollers)]
    threads.append(threading.Thread(target=sampler, daemon=True))
    for t in threads:
        t.start()

    describe("before", state())

    if args.phase == "toggle":
        failures, slowest = 0, 0.0
        for cycle in range(args.cycles):
            for enabled in (False, True):
                start = time.monotonic()
                status, body = request("POST", "/api/output",
                                       {"name": args.output, "enabled": enabled})
                slowest = max(slowest, time.monotonic() - start)
                if status != 200:
                    failures += 1
                    print(f"cycle {cycle + 1} enabled={enabled}: {status} {body[:200]!r}", flush=True)
                time.sleep(args.interval)
            describe(f"cycle {cycle + 1}", state())
        print(f"toggle: {args.cycles} off/on cycles, {failures} failed, "
              f"slowest POST {slowest * 1000:.0f} ms")
    elif args.phase == "stop":
        for cycle in range(args.cycles):
            os.kill(args.pid, signal.SIGSTOP)
            time.sleep(args.pause)
            os.kill(args.pid, signal.SIGCONT)
            resumed = time.monotonic()
            while True:
                try:
                    state()
                    break
                except Exception:
                    time.sleep(0.01)
            answered = (time.monotonic() - resumed) * 1000
            time.sleep(args.interval)
            describe(f"stop {cycle + 1} (answered {answered:.0f} ms after SIGCONT)", state())
    else:
        held = [socket.create_connection((args.host, args.port), timeout=2)
                for _ in range(args.connections)]
        time.sleep(0.5)
        start = time.monotonic()
        try:
            status, body = request("GET", "/api/state", timeout=2)
            print(f"with {len(held)} idle connections held: GET /api/state -> {status} in "
                  f"{(time.monotonic() - start) * 1000:.0f} ms: {body[:100]!r}")
        except Exception as error:
            print(f"with {len(held)} idle connections held: GET /api/state -> {error!r}")
        time.sleep(2.0)  # long enough for the once-a-second sampler to see them
        for connection in held:
            connection.close()
        time.sleep(1.0)
        status, _ = request("GET", "/api/state", timeout=2)
        print(f"after closing them: GET /api/state -> {status}")

    time.sleep(1.0)
    stopping.set()
    describe("after", state())
    with lock:
        lat = sorted(latencies)
        counts = dict(tally)
    print(f"polls: ok={counts['ok']} timed out={counts['timeout']} 503={counts['refused']} "
          f"other errors={counts['error']} with a stale output={counts['stale']}")
    if lat:
        print(f"/api/state latency ms: median={statistics.median(lat):.1f} "
              f"p99={lat[min(len(lat) - 1, int(0.99 * len(lat)))]:.1f} max={lat[-1]:.1f}")
    if samples:
        print(f"process threads: min={min(s[0] for s in samples)} max={max(s[0] for s in samples)}; "
              f"established control connections: max={max(s[1] for s in samples)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
