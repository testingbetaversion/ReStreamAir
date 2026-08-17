#!/usr/bin/env python3
"""Benchmark an HTTP/HTTPS proxy for live DASH restreaming.

Answers one question: can this proxy carry N live streams at a given quality?

Three things decide that, and a proxy can fail on any one of them
independently — which is why all three are measured separately:

  1. Manifest reliability.  Every rendition re-reads the MPD every couple of
     seconds.  A proxy that randomly refuses even a third of those will stall
     the engine in a backoff loop while looking perfectly fast on a throughput
     test.  This is measured first because it is the cheapest to check and the
     most common way a proxy that "has enough bandwidth" still fails.
  2. Aggregate throughput.  N streams share one proxy; the ceiling is what the
     proxy gives with many connections open, not what one connection gets.
  3. Per-stream realtime factor.  A live stream must be fetched *faster* than
     it plays or it can never recover from a hiccup.  Below 1.0x it is falling
     behind permanently; 1.2x or better is the target.

Run it on the machine that will do the restreaming, never a laptop — the route
from your own machine to the proxy is not the route the server takes, and the
difference has been a factor of four in practice.

Usage:
    ./proxy-bench.py <proxy-url> [options]

    ./proxy-bench.py http://user:pass@1.2.3.4:8080
    ./proxy-bench.py https://user:pass@br160.proxy.nordvpn.com:89 --quality 1080
    ./proxy-bench.py http://1.2.3.4:8080 --quality both --streams 7

Options:
    --quality 720|1080|both   target height (default 720)
    --streams N               how many concurrent streams to prove (default 7)
    --parallel N              concurrent segment fetches per stream (default 6,
                              matching the engine's parallelDownloads)
    --segments N              segments pulled per stream (default 12)
    --probes N                manifest reliability probes (default 12)
    --channels FILE           newline-separated MPD URLs, one per channel
    --timeout N               per-request timeout in seconds (default 60)

Exit status is 0 if the proxy meets the target, 1 if it does not, so this can
gate a deploy.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

# The channels this was built against. Override with --channels for any other
# source; nothing below is claro-specific beyond this list.
DEFAULT_CHANNELS = [
    "https://getcdn.clarocdn.com.br/Content/Channel/SPOGLHD/dsc4/manifest.mpd",
    "https://getcdn.clarocdn.com.br/Content/Channel/SPOCBQ4HD/dsc3/manifest.mpd",
    "https://getcdn.clarocdn.com.br/Content/Channel/RJOGLHD/dsc4/manifest.mpd",
    "https://getcdn.clarocdn.com.br/Content/Channel/BHZGLHD/dsc4/manifest.mpd",
    "https://getcdn.clarocdn.com.br/Content/Channel/BSBGLHD/dsc4/manifest.mpd",
    "https://getcdn.clarocdn.com.br/Content/Channel/RECGLHD/dsc4/manifest.mpd",
]

# A live stream must be pulled faster than it plays. At exactly 1.0x any jitter
# is unrecoverable, so this is the bar for "comfortable".
TARGET_REALTIME = 1.20

BOLD, DIM, RED, GRN, YEL, RST = "\033[1m", "\033[2m", "\033[31m", "\033[32m", "\033[33m", "\033[0m"
if not sys.stdout.isatty():
    BOLD = DIM = RED = GRN = YEL = RST = ""


def curl(proxy, url, out=None, timeout=60, fmt="%{http_code}"):
    """One curl through the proxy. Returns (write-out string, ok)."""
    cmd = ["curl", "-s", "-m", str(timeout), "-L", "--proxy", proxy,
           "-o", out or os.devnull, "-w", fmt, url]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout + 15)
        return r.stdout.strip(), r.returncode == 0
    except subprocess.TimeoutExpired:
        return "", False


def fetch_text(proxy, url, timeout=60):
    """Fetches a URL and returns (body, effective_url) or (None, None)."""
    with tempfile.NamedTemporaryFile(delete=False) as f:
        path = f.name
    try:
        wo, _ = curl(proxy, url, out=path, timeout=timeout, fmt="%{http_code}|%{url_effective}")
        parts = wo.split("|", 1)
        if len(parts) != 2 or parts[0] != "200":
            return None, None
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace"), parts[1]
    finally:
        os.unlink(path)


def parse_mpd(body, base, height):
    """Picks the video rendition nearest `height` and expands its timeline.

    Representation ids are not comparable across channels — the same id can be
    1080p video on one and audio on another — so the rendition is chosen by its
    declared height, which is the only stable way to say "give me 720p".
    """
    media = re.search(r'media="([^"]+)"', body)
    if not media:
        return None
    media = media.group(1)
    try:
        vid = body.split('mimeType="video/mp4"')[1].split("</AdaptationSet>")[0]
    except IndexError:
        return None

    best, best_gap, best_bw = None, None, 0
    for m in re.finditer(r'<Representation([^>]*)id="(stream_\d+|[^"]+)"([^>]*)/?>', vid):
        attrs = m.group(1) + m.group(3)
        h = re.search(r'height="(\d+)"', attrs)
        bw = re.search(r'bandwidth="(\d+)"', attrs)
        if not h:
            continue
        gap = abs(int(h.group(1)) - height)
        if best_gap is None or gap < best_gap:
            best, best_gap, best_bw = m.group(2), gap, int(bw.group(1)) if bw else 0

    times, t = [], None
    for m in re.finditer(r'<S(?: t="(\d+)")? d="(\d+)"(?: r="(-?\d+)")?', vid):
        if m.group(1):
            t = int(m.group(1))
        if t is None:
            continue
        d, rep = int(m.group(2)), int(m.group(3) or 0)
        for _ in range(max(rep, 0) + 1):
            times.append(t)
            t += d
    ts = re.search(r'timescale="(\d+)"', vid)
    dur = re.search(r'<S[^>]*d="(\d+)"', vid)
    seg_secs = (int(dur.group(1)) / int(ts.group(1))) if (ts and dur) else 2.0
    if not best or len(times) < 4:
        return None
    return {"rep": best, "bw": best_bw, "media": media, "base": base,
            "times": times, "seg": seg_secs}


def probe_reliability(proxy, url, n, timeout):
    """Manifest success rate — the check that catches a flaky rotating proxy."""
    ok = codes = 0
    seen = {}
    for _ in range(n):
        wo, _ = curl(proxy, url, timeout=timeout)
        code = wo.strip() or "000"
        seen[code] = seen.get(code, 0) + 1
        codes += 1
        if code == "200":
            ok += 1
        time.sleep(0.4)
    return ok, codes, seen


def egress_ips(proxy, n, timeout):
    """Distinct exit IPs. More than one means requests are not from one host,
    which is a common cause of intermittent 403s from entitlement-checking CDNs."""
    ips = []
    for _ in range(n):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            path = f.name
        try:
            curl(proxy, "https://api.ipify.org", out=path, timeout=timeout)
            with open(path) as fh:
                v = fh.read().strip()
            if v and len(v) < 46:
                ips.append(v)
        finally:
            os.unlink(path)
    return ips


def pull_stream(proxy, plan, count, parallel, timeout, tmpdir, idx):
    """One stream's worth of segments, `parallel` at a time — the engine's shape."""
    urls = []
    for t in plan["times"][-(count + 2):-2]:
        u = plan["base"] + "/" + plan["media"].replace("$RepresentationID$", plan["rep"]).replace("$Time$", str(t))
        urls.append(u)
    t0 = time.time()
    total = ok = 0
    procs = []
    for j, u in enumerate(urls):
        out = os.path.join(tmpdir, f"s{idx}_{j}")
        procs.append((out, subprocess.Popen(
            ["curl", "-s", "-m", str(timeout), "-L", "--proxy", proxy, "-o", out, u],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)))
        while sum(1 for _, p in procs if p.poll() is None) >= parallel:
            time.sleep(0.05)
    for _, p in procs:
        p.wait()
    for out, _ in procs:
        if os.path.exists(out):
            s = os.path.getsize(out)
            total += s
            if s > 10000:
                ok += 1
            os.remove(out)
    el = max(time.time() - t0, 0.001)
    return {"bytes": total, "ok": ok, "n": len(urls), "elapsed": el,
            "media": len(urls) * plan["seg"], "realtime": len(urls) * plan["seg"] / el}


def run_quality(proxy, plans, height, streams, count, parallel, timeout):
    print(f"\n{BOLD}== {height}p : {streams} concurrent streams =={RST}")
    use = [plans[i % len(plans)] for i in range(streams)]
    nominal = sum(p["bw"] for p in use) / 1e6
    print(f"{DIM}   nominal media rate: {nominal:.1f} Mbps "
          f"({', '.join(p['rep'] for p in use[:len(plans)])}){RST}")
    with tempfile.TemporaryDirectory() as tmp:
        t0 = time.time()
        with ThreadPoolExecutor(max_workers=streams) as ex:
            res = list(ex.map(
                lambda a: pull_stream(proxy, a[1], count, parallel, timeout, tmp, a[0]),
                enumerate(use)))
        el = max(time.time() - t0, 0.001)
    tot = sum(r["bytes"] for r in res)
    okall = sum(r["ok"] for r in res)
    nall = sum(r["n"] for r in res)
    agg = tot * 8 / 1e6 / el
    worst = min(r["realtime"] for r in res)
    for i, r in enumerate(res):
        c = GRN if r["realtime"] >= TARGET_REALTIME else (YEL if r["realtime"] >= 1.0 else RED)
        print(f"   stream {i+1}: {r['ok']:2d}/{r['n']:2d} segs  {r['bytes']/1e6:6.1f} MB  "
              f"{c}{r['realtime']:5.2f}x realtime{RST}")
    print(f"   {DIM}{'-'*52}{RST}")
    print(f"   aggregate: {okall}/{nall} segments, {tot/1e6:.1f} MB in {el:.1f}s = "
          f"{BOLD}{agg:.2f} Mbps{RST}")
    delivered = okall / nall if nall else 0
    ok = worst >= 1.0 and delivered >= 0.98
    good = worst >= TARGET_REALTIME and delivered >= 0.99
    if good:
        print(f"   {GRN}{BOLD}PASS{RST} — every stream ≥ {TARGET_REALTIME:.2f}x with headroom")
    elif ok:
        print(f"   {YEL}{BOLD}MARGINAL{RST} — all above 1.0x but the slowest is {worst:.2f}x; "
              f"no room for a hiccup")
    else:
        why = []
        if worst < 1.0:
            why.append(f"slowest stream {worst:.2f}x (needs ≥1.0)")
        if delivered < 0.98:
            why.append(f"only {delivered*100:.0f}% of segments delivered")
        print(f"   {RED}{BOLD}FAIL{RST} — " + "; ".join(why))
    return good, ok, agg


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("proxy")
    ap.add_argument("--quality", default="720", choices=["720", "1080", "both"])
    ap.add_argument("--streams", type=int, default=7)
    ap.add_argument("--parallel", type=int, default=6)
    ap.add_argument("--segments", type=int, default=12)
    ap.add_argument("--probes", type=int, default=12)
    ap.add_argument("--timeout", type=int, default=60)
    ap.add_argument("--channels")
    a = ap.parse_args()

    channels = DEFAULT_CHANNELS
    if a.channels:
        channels = [l.strip() for l in open(a.channels) if l.strip() and not l.startswith("#")]

    redacted = re.sub(r"//[^@]*@", "//***:***@", a.proxy)
    print(f"{BOLD}proxy-bench{RST}  {redacted}")
    print(f"{DIM}  host: {os.uname().nodename}   target: {a.streams} streams @ {a.quality}p{RST}")

    print(f"\n{BOLD}== connectivity =={RST}")
    wo, _ = curl(a.proxy, "https://api.ipify.org", timeout=a.timeout)
    if wo.strip() != "200":
        print(f"   {RED}proxy unreachable (http={wo.strip() or 'none'}){RST}")
        sys.exit(1)
    ips = egress_ips(a.proxy, 6, a.timeout)
    uniq = sorted(set(ips))
    print(f"   egress IP(s): {', '.join(uniq) if uniq else 'unknown'}")
    if len(uniq) > 1:
        print(f"   {YEL}rotating exits ({len(uniq)} IPs) — a CDN that checks entitlement per IP")
        print(f"   will refuse some requests at random; watch the manifest rate below{RST}")

    print(f"\n{BOLD}== manifest reliability =={RST}  {DIM}(every rendition re-reads this every ~2s){RST}")
    ok, n, codes = probe_reliability(a.proxy, channels[0], a.probes, a.timeout)
    rate = ok / n if n else 0
    detail = ", ".join(f"{k}x{v}" for k, v in sorted(codes.items()))
    c = GRN if rate >= 0.98 else (YEL if rate >= 0.9 else RED)
    print(f"   {ok}/{n} succeeded ({c}{rate*100:.0f}%{RST})   [{detail}]")
    if rate < 0.9:
        print(f"   {RED}A poll fails whenever the manifest does. At {(1-rate)*100:.0f}% failure the")
        print(f"   engine spends most of its time in backoff regardless of bandwidth.{RST}")

    print(f"\n{BOLD}== reading channel manifests =={RST}")
    heights = [720, 1080] if a.quality == "both" else [int(a.quality)]
    plans = {}
    for h in heights:
        got = []
        for url in channels:
            body, eff = fetch_text(a.proxy, url, a.timeout)
            if not body:
                continue
            p = parse_mpd(body, eff.rsplit("/", 1)[0], h)
            if p:
                got.append(p)
        if not got:
            print(f"   {RED}no channel manifests readable at {h}p — cannot test{RST}")
            sys.exit(1)
        picked = ", ".join("{} {:.1f}M".format(p["rep"], p["bw"] / 1e6) for p in got[:3])
        print(f"   {h}p: {len(got)}/{len(channels)} channels readable "
              f"({DIM}{picked}…{RST})")
        plans[h] = got

    results = {}
    for h in heights:
        results[h] = run_quality(a.proxy, plans[h], h, a.streams,
                                 a.segments, a.parallel, a.timeout)

    print(f"\n{BOLD}== verdict =={RST}")
    allgood = True
    for h in heights:
        good, ok_, agg = results[h]
        verdict = f"{GRN}yes{RST}" if good else (f"{YEL}marginal{RST}" if ok_ else f"{RED}no{RST}")
        print(f"   {a.streams} streams @ {h}p : {verdict}   ({agg:.1f} Mbps aggregate)")
        allgood = allgood and good
    if rate < 0.9:
        print(f"   {YEL}note: manifest reliability {rate*100:.0f}% will stall the engine even where "
              f"throughput passes{RST}")
        allgood = False
    sys.exit(0 if allgood else 1)


if __name__ == "__main__":
    main()
