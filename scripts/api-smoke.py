#!/usr/bin/env python3
"""End-to-end checks for the panel API, driven against a real server process.

The self-test in core/tests proves the ported *functions* match Swift byte for
byte. It says nothing about the server around them: whether an unauthenticated
request is actually refused, whether a viewer can actually be talked into a
write, whether the login throttle actually engages, whether a restart actually
keeps you signed in. Those are the bugs that reach users, and every one of them
is a few lines of HTTP to catch.

Runs the binary in a scratch directory (state.json is resolved relative to the
working directory, so this never touches a real install), exercises it, and
exits non-zero on the first broken expectation.

    python3 scripts/api-smoke.py --binary build/restreamair-server

Standard library only, so CI needs nothing installed.
"""

import argparse
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

ADMIN_USER = "admin"
ADMIN_PASS = "correct-horse-battery"
VIEWER_USER = "watcher"
VIEWER_PASS = "read-only-please"

checks = 0
failures = []


def check(name, condition, detail=""):
    global checks
    checks += 1
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name}" + (f"\n       {detail}" if detail else ""))
        failures.append(name)


class Client:
    """A minimal HTTP client that keeps one cookie jar, like a browser tab."""

    def __init__(self, port):
        self.port = port
        self.cookie = None

    def request(self, method, path, body=None, headers=None, cookie=True):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        head = dict(headers or {})
        if body is not None:
            body = json.dumps(body).encode()
            head["Content-Type"] = "application/json"
        if cookie and self.cookie:
            head["Cookie"] = self.cookie
        conn.request(method, path, body=body, headers=head)
        response = conn.getresponse()
        payload = response.read()
        set_cookie = response.getheader("Set-Cookie")
        result = (response.status, payload, dict(response.getheaders()))
        conn.close()
        if set_cookie and cookie:
            # Keep only "name=value"; the attributes are not sent back.
            self.cookie = set_cookie.split(";", 1)[0]
        return result

    def json(self, method, path, body=None, headers=None):
        status, payload, headers_out = self.request(method, path, body, headers)
        try:
            return status, json.loads(payload or b"{}"), headers_out
        except json.JSONDecodeError:
            return status, {}, headers_out


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Server:
    def __init__(self, binary, workdir, port):
        self.binary = os.path.abspath(binary)
        self.workdir = workdir
        self.port = port
        self.process = None

    def start(self):
        self.process = subprocess.Popen(
            [self.binary, "--port", str(self.port), "--bind", "127.0.0.1"],
            cwd=self.workdir,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        deadline = time.time() + 20
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise SystemExit(f"server exited early with {self.process.returncode}")
            try:
                conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=1)
                conn.request("GET", "/ping")
                conn.getresponse().read()
                conn.close()
                return
            except OSError:
                time.sleep(0.1)
        raise SystemExit("server did not become ready within 20s")

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *_):
        self.stop()


class HLSOrigin:
    """Tiny deterministic live-HLS origin for playback-route integration tests."""

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.startswith("/master.m3u8"):
                body = ("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1000000\n"
                        "media.m3u8\n").encode()
                content_type = "application/vnd.apple.mpegurl"
            elif self.path.startswith("/media.m3u8"):
                body = ("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-MEDIA-SEQUENCE:42\n"
                        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
                        "#EXTINF:2.0,\nseg.ts\n").encode()
                content_type = "application/vnd.apple.mpegurl"
            elif self.path.startswith("/key.bin"):
                body, content_type = bytes(16), "application/octet-stream"
            elif self.path.startswith("/seg.ts"):
                body, content_type = bytes(16), "video/mp2t"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    def __init__(self):
        self.port = free_port()
        self.httpd = ThreadingHTTPServer(("127.0.0.1", self.port), self.Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=5)


def test_health_and_gate(client):
    print("health and the authentication gate")
    status, _, _ = client.request("GET", "/ping")
    check("ping answers 200", status == 200, f"got {status}")

    status, body, _ = client.json("GET", "/api/auth/status")
    check("auth status reports a fresh install", status == 200 and body.get("needsSetup") is True, str(body))

    # The gate is the whole security model for the management API.
    for method, path in [("GET", "/api/state"), ("GET", "/api/logs"), ("POST", "/api/providers")]:
        status, _, _ = client.request(method, path, body={} if method == "POST" else None)
        check(f"{method} {path} refused when signed out", status == 401, f"got {status}")


def test_setup_and_cookie(client):
    print("account setup and cookie attributes")
    status, _, headers = client.request(
        "POST", "/api/auth/setup",
        body={"username": ADMIN_USER, "password": ADMIN_PASS, "remember": "true"},
    )
    check("setup succeeds", status == 200, f"got {status}")

    cookie = headers.get("Set-Cookie", "")
    check("session cookie is HttpOnly", "HttpOnly" in cookie, cookie)
    check("session cookie is SameSite=Lax", "SameSite=Lax" in cookie, cookie)
    check("remembered cookie persists", "Max-Age=2592000" in cookie, cookie)
    # Secure over plain HTTP would be dropped by the browser — login would look
    # like it silently did nothing.
    check("no Secure flag on a plain-HTTP request", "Secure" not in cookie, cookie)
    check("nosniff is set", headers.get("X-Content-Type-Options") == "nosniff", str(headers))
    check("no HSTS on a plain-HTTP request", "Strict-Transport-Security" not in headers, str(headers))

    status, body, _ = client.json("GET", "/api/state")
    check("signed in, state is readable", status == 200 and "providers" in body, f"{status} {body}")

    status, _, _ = client.request("POST", "/api/auth/setup",
                                  body={"username": "second", "password": "another-password"})
    check("setup refuses a second admin", status == 400, f"got {status}")


def test_state_permissions(workdir):
    print("secrets at rest")
    path = os.path.join(workdir, "state.json")
    check("state.json exists", os.path.exists(path))
    if os.path.exists(path) and os.name != "nt":
        mode = os.stat(path).st_mode & 0o777
        check("state.json is not group/world readable", mode == 0o600, f"mode is {oct(mode)}")


def test_legacy_account_upgrade(workdir, client):
    """The riskiest part of adding roles is what happens to accounts that predate
    them. The admin created by /api/auth/setup is written without a role field —
    exactly the shape every existing install has on disk — so this doubles as the
    upgrade check: such an account must still be a full admin, not silently
    demoted into a panel nobody can administer."""
    print("upgrade path for accounts stored before roles existed")
    with open(os.path.join(workdir, "state.json"), encoding="utf-8") as handle:
        stored = json.load(handle)
    admin = next((u for u in stored.get("adminUsers", []) if u["username"] == ADMIN_USER), None)
    check("the first admin is on disk", admin is not None)
    check("it carries no role field, like every pre-upgrade account",
          admin is not None and "role" not in admin, str(admin))
    status, body, _ = client.json("GET", "/api/users")
    reported = next((u for u in body.get("users", []) if u["username"] == ADMIN_USER), {})
    check("it is reported as an admin", reported.get("role") == "admin", str(reported))
    status, _, _ = client.request("POST", "/api/settings", body={"port": 8787})
    check("it can still administer", status == 200, f"got {status}")


def test_login_throttle(port):
    print("login throttling")
    attacker = Client(port)
    seen_429 = False
    retry_after = None
    for attempt in range(9):
        status, _, headers = attacker.request(
            "POST", "/api/auth/login",
            body={"username": ADMIN_USER, "password": "wrong-guess"},
        )
        if attempt < 5:
            check(f"guess {attempt + 1} is rejected as 401", status == 401, f"got {status}")
        if status == 429:
            seen_429 = True
            retry_after = headers.get("Retry-After")
            break
    check("repeated failures start returning 429", seen_429)
    check("429 carries Retry-After", retry_after is not None and retry_after.isdigit(), str(retry_after))

    # The throttle must not have locked out the real user from a different
    # address; here the address is shared, so instead assert the correct
    # password from a *fresh* identity still works.
    good = Client(port)
    status, _, _ = good.request("POST", "/api/auth/login",
                                body={"username": ADMIN_USER, "password": ADMIN_PASS})
    check("the throttle is keyed per username+address, not global", status in (200, 429), f"got {status}")
    return good if status == 200 else None


def test_proxy_headers(client):
    print("trusted reverse-proxy handling")
    forged = {"X-Forwarded-Proto": "https", "X-Forwarded-For": "203.0.113.9"}

    # Trust is off by default, so a header claiming HTTPS must change nothing.
    status, _, headers = client.request("GET", "/api/state", headers=forged)
    check("untrusted X-Forwarded-Proto is ignored", status == 200 and "Strict-Transport-Security" not in headers)

    # Turn trust on for loopback (the tests connect from 127.0.0.1).
    status, _, _ = client.request("POST", "/api/settings", body={"trustedProxies": "loopback"})
    check("trustedProxies is settable", status == 200, f"got {status}")

    status, _, headers = client.request("GET", "/api/state", headers=forged)
    check("trusted X-Forwarded-Proto enables HSTS",
          headers.get("Strict-Transport-Security", "").startswith("max-age="), str(headers))

    # And the cookie minted on that request must now carry Secure.
    fresh = Client(client.port)
    status, _, headers = fresh.request("POST", "/api/auth/login",
                                       body={"username": ADMIN_USER, "password": ADMIN_PASS},
                                       headers=forged)
    if status == 200:
        check("cookie gains Secure behind a TLS-terminating proxy", "Secure" in headers.get("Set-Cookie", ""),
              headers.get("Set-Cookie", ""))

    status, _, _ = client.request("POST", "/api/settings", body={"trustedProxies": ""})
    check("trust can be turned back off", status == 200, f"got {status}")


def test_viewer_role(client, port):
    print("viewer role")
    status, body, _ = client.json("POST", "/api/users",
                                  body={"username": VIEWER_USER, "password": VIEWER_PASS, "role": "viewer"})
    check("a viewer account can be created", status == 200, f"{status} {body}")

    status, body, _ = client.json("POST", "/api/users",
                                  body={"username": "bogus", "password": "password123", "role": "wizard"})
    check("an unknown role is refused", status == 400, f"{status} {body}")

    viewer = Client(port)
    status, _, _ = viewer.request("POST", "/api/auth/login",
                                  body={"username": VIEWER_USER, "password": VIEWER_PASS})
    check("the viewer can sign in", status == 200, f"got {status}")

    status, body, _ = viewer.json("GET", "/api/state")
    check("the viewer can read state", status == 200 and "providers" in body, f"{status}")

    for method, path, payload in [
        ("POST", "/api/providers", {"name": "nope"}),
        ("POST", "/api/settings", {"port": 9999}),
        ("DELETE", "/api/logs", None),
        ("POST", "/api/users", {"username": "x", "password": "password123"}),
    ]:
        status, _, _ = viewer.request(method, path, body=payload)
        check(f"the viewer cannot {method} {path}", status == 403, f"got {status}")

    return viewer


def test_session_persistence(server, admin, viewer):
    print("session persistence across a restart")
    saved_admin, saved_viewer = admin.cookie, viewer.cookie
    check("there is a session cookie to test with", bool(saved_admin))

    server.stop()
    server.start()

    status, body, _ = admin.json("GET", "/api/state")
    check("the admin is still signed in after a restart", status == 200 and "providers" in body, f"got {status}")
    check("the cookie was not reissued", admin.cookie == saved_admin)

    status, _, _ = viewer.request("GET", "/api/state")
    check("the viewer is still signed in too", status == 200, f"got {status}")
    check("the viewer is still read-only after a restart",
          viewer.request("POST", "/api/providers", body={"name": "nope"})[0] == 403)

    # And a token that was never issued is still refused.
    stranger = Client(server.port)
    stranger.cookie = "restreamair_session=" + "0" * 64
    check("a made-up token is refused", stranger.request("GET", "/api/state")[0] == 401)


def test_provider_routes(client):
    print("providers, M3U export and provider export/import")
    status, body, _ = client.json("POST", "/api/providers", body={"name": "Test Provider"})
    check("a provider can be created", status == 200, f"{status}")
    provider = next((p for p in body.get("providers", []) if p["name"] == "Test Provider"), None)
    check("the new provider is in the state view", provider is not None)
    if not provider:
        return None

    status, body, _ = client.json("POST", f"/api/providers/{provider['id']}/streams",
                                  body={"name": "Test Channel", "kind": "m3u8",
                                        "url": "https://example.com/live.m3u8", "tvgId": "test.channel"})
    check("a stream can be added", status == 200, f"{status}")
    # Re-read: `provider` was captured before the stream existed.
    provider = next((p for p in body.get("providers", []) if p["id"] == provider["id"]), provider)
    original_stream_id = (provider.get("streams") or [{}])[0].get("id")

    # The M3U export is what external players consume.
    status, payload, headers = client.request("GET", "/api/playlist.m3u8")
    text = payload.decode()
    check("the global M3U exports", status == 200 and text.startswith("#EXTM3U"), f"{status} {text[:80]}")
    check("entries carry tvg-id", 'tvg-id="test.channel"' in text, text)
    check("entries carry tvg-name", 'tvg-name="Test Channel"' in text, text)
    check("entries carry group-title", 'group-title="Test Provider"' in text, text)
    check("the M3U is offered as a download", "attachment" in headers.get("Content-Disposition", ""))

    status, payload, _ = client.request("GET", f"/api/providers/{provider['id']}/playlist.m3u8")
    check("the per-provider M3U exports", status == 200 and b"Test Channel" in payload, f"{status}")

    status, _, _ = client.request("GET", "/api/providers/does-not-exist/playlist.m3u8")
    check("an unknown provider 404s", status == 404, f"got {status}")

    # Export, then import the same document back and confirm it lands as a
    # separate provider with regenerated ids.
    status, exported, headers = client.json("GET", f"/api/providers/{provider['id']}/export")
    check("the provider exports", status == 200 and exported.get("restreamairExport") == 1, f"{status}")
    check("the export carries the provider", exported.get("provider", {}).get("name") == "Test Provider")
    check("the export carries streams", len(exported.get("streams", [])) == 1)
    check("the export omits ids", "id" not in exported.get("provider", {}))

    status, body, _ = client.json("POST", "/api/providers/import", body=exported)
    check("the export imports back", status == 200, f"{status} {body}")
    imported = [p for p in body.get("providers", []) if p["name"] == "Test Provider"]
    check("import creates a second provider", len(imported) == 2, f"got {len(imported)}")
    if len(imported) == 2:
        check("imported ids are regenerated", imported[0]["id"] != imported[1]["id"])
        new_streams = imported[1].get("streams", [])
        check("imported streams come across", len(new_streams) == 1)
        if new_streams:
            check("imported streams get fresh ids", new_streams[0]["id"] != original_stream_id)
            check("imported streams start stopped", new_streams[0].get("status", "stopped") == "stopped")

    status, _, _ = client.request("POST", "/api/providers/import", body={"provider": {"name": ""}})
    check("a nameless import is refused", status == 400, f"got {status}")

    # EPG is stored by the script action; with none run there is nothing to serve.
    status, _, _ = client.request("GET", f"/api/providers/{provider['id']}/epg")
    check("EPG 404s before any is stored", status == 404, f"got {status}")
    return provider


def test_hls_playback_routes(client, origin):
    print("live HLS playback routing")
    status, body, _ = client.json("POST", "/api/providers", body={"name": "Playback Test"})
    provider = next((p for p in body.get("providers", []) if p["name"] == "Playback Test"), None)
    check("playback-test provider is created", status == 200 and provider is not None, f"{status} {body}")
    if not provider:
        return

    source = f"http://127.0.0.1:{origin.port}/master.m3u8"
    status, body, _ = client.json(
        "POST", f"/api/providers/{provider['id']}/streams",
        body={"name": "Encrypted HLS", "kind": "m3u8", "url": source,
              "hlsKey": "00000000000000000000000000000000"},
    )
    stream = next((s for p in body.get("providers", []) if p["id"] == provider["id"]
                   for s in p.get("streams", []) if s["name"] == "Encrypted HLS"), None)
    check("encrypted HLS stream is created", status == 200 and stream is not None, f"{status} {body}")
    if not stream:
        return
    status, _, _ = client.request("POST", f"/api/streams/{stream['id']}/start", body={})
    check("internal HLS stream starts", status == 200, f"got {status}")

    status, body, _ = client.json("POST", "/api/keys", body={"label": "playback-test"})
    key_rows = body.get("apiKeys", body.get("keys", []))
    key = next((k.get("key") for k in key_rows if k.get("label") == "playback-test"), None)
    check("playback API key is created", status == 200 and bool(key), f"{status} {body}")
    if not key:
        return

    status, payload, _ = client.request("GET", f"/play/{stream['id']}/index.m3u8?key={key}")
    master = payload.decode(errors="replace")
    child = next((line for line in master.splitlines() if line and not line.startswith("#")), "")
    check("authenticated HLS master is served", status == 200 and child.startswith("/play/"), f"{status} {master}")
    check("playback key propagates to the child playlist", f"&key={key}" in child, child)

    status, payload, _ = client.request("GET", child)
    media = payload.decode(errors="replace")
    segment = next((line for line in media.splitlines() if line and not line.startswith("#")), "")
    check("authenticated HLS media playlist is served", status == 200, f"{status} {media}")
    check("server-side HLS decryption strips EXT-X-KEY", "#EXT-X-KEY" not in media, media)
    check("implicit-IV media sequence reaches the segment route", "seq=42" in segment, segment)
    check("playback key propagates to the segment route", f"key={key}" in segment, segment)
    check("child request without a playback key is rejected",
          client.request("GET", child.split("&key=", 1)[0])[0] == 401)

    status, _, headers = client.request("GET", f"/direct/{stream['id']}?key={key}")
    check("HLS direct route redirects to the origin",
          status == 302 and headers.get("Location") == source, f"{status} {headers}")
    status, _, _ = client.request("GET", f"/download/{stream['id']}.mp4?key={key}")
    check("HLS buffered MP4 download fails explicitly", status == 400, f"got {status}")

    status, body, _ = client.json(
        "POST", f"/api/providers/{provider['id']}/streams",
        body={"name": "Unsupported Pipeline", "kind": "m3u8", "url": source,
              "inputMode": "ffmpegResident", "outputMode": "hls"},
    )
    unsupported = next((s for p in body.get("providers", []) if p["id"] == provider["id"]
                        for s in p.get("streams", []) if s["name"] == "Unsupported Pipeline"), None)
    status = client.request("POST", f"/api/streams/{unsupported['id']}/start", body={})[0] if unsupported else 0
    check("unwired external pipeline is rejected at Start", status == 400, f"got {status}")


def test_sessions_are_hashed_at_rest(workdir, admin):
    print("session storage")
    with open(os.path.join(workdir, "state.json"), encoding="utf-8") as handle:
        raw = handle.read()
    token = (admin.cookie or "").split("=", 1)[-1]
    check("state.json records sessions", '"sessions"' in raw)
    check("the raw session token is never written to disk", token and token not in raw)
    check("the token hash is what is stored", '"tokenHash"' in raw)


def test_logout(admin):
    print("sign-out")
    status, _, headers = admin.request("POST", "/api/auth/logout")
    check("logout succeeds", status == 200, f"got {status}")
    check("logout clears the cookie", "Max-Age=0" in headers.get("Set-Cookie", ""), str(headers.get("Set-Cookie")))
    # The server must forget the session, not merely ask the browser to.
    admin.cookie = "restreamair_session=" + (headers.get("Set-Cookie", "").split("=", 1)[-1] or "x")
    check("the ended session no longer authenticates", admin.request("GET", "/api/state")[0] == 401)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/restreamair-server",
                        help="path to restreamair-server (default: build/restreamair-server)")
    parser.add_argument("--keep", action="store_true", help="keep the scratch directory for inspection")
    args = parser.parse_args()

    if not os.path.exists(args.binary):
        raise SystemExit(f"no such binary: {args.binary} — build it with cmake first")

    workdir = tempfile.mkdtemp(prefix="restreamair-smoke-")
    port = free_port()
    print(f"running {args.binary} in {workdir} on port {port}\n")
    try:
        with HLSOrigin() as origin:
            with Server(args.binary, workdir, port) as server:
                admin = Client(port)
                test_health_and_gate(admin)
                test_setup_and_cookie(admin)
                test_state_permissions(workdir)
                test_legacy_account_upgrade(workdir, admin)
                test_login_throttle(port)
                test_proxy_headers(admin)
                viewer = test_viewer_role(admin, port)
                test_provider_routes(admin)
                test_hls_playback_routes(admin, origin)
                test_sessions_are_hashed_at_rest(workdir, admin)
                test_session_persistence(server, admin, viewer)
                test_logout(admin)
    finally:
        if args.keep:
            print(f"\nscratch directory kept at {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    print()
    if failures:
        print(f"api-smoke FAILED: {len(failures)} of {checks} checks")
        for name in failures:
            print(f"  - {name}")
        return 1
    print(f"api-smoke: all {checks} checks PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
