#!/usr/bin/env python3
"""How many connections does an HTTP proxy let you open at once?

Opens CONNECT tunnels one after another and keeps every one open until the
proxy refuses (Proxy-Seller style plans answer `429 PROXY_MAX_CONNS`). The
number it reached is the limit, as long as nothing else is using the proxy:
other clients' connections count against the same cap, so it can come out
lower than the plan says. That is the point — run it when a proxy starts
refusing and compare it with what the plan promises.

    scripts/proxy-conn-limit.py http://user:pass@host:50100
    scripts/proxy-conn-limit.py PROXY1 PROXY2 --rounds 3 --max 300
    echo http://user:pass@host:50100 | scripts/proxy-conn-limit.py -

Pass `-` to read proxies from stdin (one per line) so the password stays out
of the process list and shell history. Passwords are never printed.
Exit status: 0 = measured every proxy, 1 = a proxy could not be measured
(unreachable, or it rejected the login).
"""
import argparse
import base64
import re
import resource
import socket
import sys
import time
from urllib.parse import urlsplit


def open_tunnel(host, port, auth, target, timeout):
    """One CONNECT through the proxy: (socket or None, status line, proxy error code)."""
    try:
        sock = socket.create_connection((host, port), timeout=timeout)
    except OSError as err:
        return None, "cannot connect: %s" % err, ""
    request = "CONNECT %s HTTP/1.1\r\nHost: %s\r\n" % (target, target)
    if auth:
        request += "Proxy-Authorization: Basic %s\r\n" % auth
    try:
        sock.sendall((request + "\r\n").encode())
        reply = b""
        while b"\r\n\r\n" not in reply:
            chunk = sock.recv(4096)
            if not chunk:
                break
            reply += chunk
    except OSError as err:
        sock.close()
        return None, "no answer: %s" % err, ""
    head = reply.decode("latin1", "ignore")
    status = head.split("\r\n", 1)[0] or "closed without a reply"
    code = re.search(r"X-Proxy-Error-Message: (\S+)", head)
    if " 200" not in status:
        sock.close()
        sock = None
    return sock, status, code.group(1) if code else ""


def measure(host, port, auth, target, limit, rounds, rest, timeout):
    """Returns the connections opened per round, or None if the proxy cannot be used."""
    results = []
    for number in range(1, rounds + 1):
        held, why = [], ""
        for _ in range(limit):
            sock, status, code = open_tunnel(host, port, auth, target, timeout)
            if sock is None:
                why = ("%s %s" % (status, code)).strip()
                break
            held.append(sock)
            time.sleep(0.03)  # gentle: some proxies rate-limit bursts of new connections
        if not held and ("407" in why or "cannot connect" in why or "no answer" in why):
            print("  round %d: %s" % (number, why))
            return None
        print("  round %d: %d open at once -> %s" % (
            number, len(held), why or "no refusal up to --max (%d)" % limit))
        results.append(len(held))
        for sock in held:
            sock.close()
        if number < rounds:
            time.sleep(rest)  # the proxy needs a moment to notice they closed
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("proxy", nargs="+", help="http://user:pass@host:port, or - to read them from stdin")
    parser.add_argument("--max", type=int, default=200, help="stop after this many (default 200)")
    parser.add_argument("--rounds", type=int, default=2, help="repeat to check it is stable (default 2)")
    parser.add_argument("--rest", type=int, default=15, help="seconds between rounds (default 15)")
    parser.add_argument("--target", default="ipinfo.io:443", help="host:port to tunnel to (default ipinfo.io:443)")
    parser.add_argument("--timeout", type=int, default=8, help="seconds to wait per connection (default 8)")
    args = parser.parse_args()

    proxies = []
    for item in args.proxy:
        proxies += [l.strip() for l in sys.stdin if l.strip()] if item == "-" else [item]
    # Hundreds of sockets need a higher descriptor limit than the usual 1024 shell default.
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, max(soft, args.max + 64)), hard))

    failed = False
    for url in proxies:
        parts = urlsplit(url if "://" in url else "http://" + url)
        if not parts.hostname or not parts.port:
            print("%s: expected http://[user:pass@]host:port" % url.split("@")[-1])
            failed = True
            continue
        auth = ""
        if parts.username:
            auth = base64.b64encode(("%s:%s" % (parts.username, parts.password or "")).encode()).decode()
        print("%s:%d" % (parts.hostname, parts.port))
        found = measure(parts.hostname, parts.port, auth, args.target, args.max, args.rounds, args.rest, args.timeout)
        if found is None:
            failed = True
        elif min(found) == max(found):
            print("  => limit %d connections at once" % found[0])
        else:
            print("  => between %d and %d connections (rounds disagree: something else is using the proxy)"
                  % (min(found), max(found)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
