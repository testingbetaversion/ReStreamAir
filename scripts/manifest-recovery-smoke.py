#!/usr/bin/env python3
"""Exercise CDN fallback and session recovery against a local deterministic origin.

python3 scripts/manifest-recovery-smoke.py --binary build/restreamair-server
No public stream sources or real provider credentials are used.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import socket
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote, urlsplit, parse_qs

spec = importlib.util.spec_from_file_location("api_smoke", Path(__file__).with_name("api-smoke.py"))
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
check = api.check


class Origin:
    def __enter__(self):
        fixture = self
        self.requests = []
        self.blocked = threading.Event()
        self.release = threading.Event()

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                fixture.requests.append((self.path, dict(self.headers)))
                path = urlsplit(self.path).path
                if path.startswith("/blocked/"):
                    fixture.blocked.set()
                    fixture.release.wait(8)
                if path.startswith(("/bad/", "/bad2/", "/blocked/")):
                    self.send_error(403, "fixture source expired")
                    return
                expected_header = "fresh-media" if path.endswith(".ts") else "fresh-cdn"
                if path.startswith("/fresh/") and self.headers.get("X-Session") != expected_header:
                    self.send_error(403, "missing fresh session header")
                    return
                if path.endswith("/master.m3u8"):
                    body = b'#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1000000\nmedia.m3u8\n'
                elif path.endswith("/media.m3u8"):
                    body = b'#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:7\n#EXTINF:2,\nseg.ts\n'
                elif path.endswith("/seg.ts"):
                    body = b"fixture segment"
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/vnd.apple.mpegurl")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base = f"http://127.0.0.1:{self.server.server_port}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(5)

    def paths(self):
        return [urlsplit(p).path for p, _ in self.requests]


def run(binary):
    with tempfile.TemporaryDirectory(prefix="restream-recovery-") as scratch, Origin() as origin:
        root = Path(scratch)
        script = root / "manifest.py"
        script.write_text('''import json, pathlib, sys, time
args = dict(arg.split("=", 1) for arg in sys.argv[1:] if "=" in arg)
root = pathlib.Path(__file__).parent
count = root / (args["id"] + ".runs")
count.write_text(str(int(count.read_text()) + 1 if count.exists() else 1))
reply = json.loads((root / "reply.json").read_text())
if reply.pop("fail", False):
    print("fixture provider unavailable", file=sys.stderr)
    sys.exit(1)
time.sleep(reply.pop("delay", 0))
print(json.dumps(reply))
''')
        with api.Server(binary, scratch, api.free_port()) as server:
            client = api.Client(server.port)
            status, _, _ = client.json("POST", "/api/auth/setup", {
                "username": api.ADMIN_USER, "password": api.ADMIN_PASS})
            assert status == 200
            status, doc, _ = client.json("POST", "/api/providers", {"name": "Recovery"})
            provider = doc["providers"][0]
            pid = provider["id"]
            client.json("PUT", f"/api/providers/{pid}", {
                **provider, "scriptPath": str(script), "scriptActions": ["manifest"]})
            status, doc, _ = client.json("POST", "/api/keys", {"label": "fixture"})
            key = next(k["key"] for k in doc.get("apiKeys", doc.get("keys", [])) if k["label"] == "fixture")

            def create(name, session=False, path="/bad/master.m3u8", mirrors=None):
                status, doc, _ = client.json("POST", f"/api/providers/{pid}/streams", {
                    "name": name, "kind": "m3u8", "url": origin.base + path,
                    "cdnUrls": mirrors if mirrors is not None else [origin.base + "/bad2/master.m3u8"],
                    "sessionManifest": session,
                })
                assert status == 200, doc
                return next(s for p in doc["providers"] for s in p["streams"] if s["name"] == name)

            def arm_playback(stream):
                # Start creates the initial session; playback must recover it
                # later. Reset only the fixture counter after this setup call.
                status, doc, _ = client.json("PUT", f"/api/streams/{stream['id']}", {
                    **stream, "sessionManifest": True})
                assert status == 200, doc
                response = root / "reply.json"
                recovery_reply = response.read_text()
                response.write_text(json.dumps({"ManifestUrl": stream["url"],
                    "Cdn": [{"ManifestUrl": url} for url in stream["cdnUrls"]]}))
                status, doc, _ = client.json("POST", f"/api/streams/{stream['id']}/start", {})
                assert status == 200, doc
                response.write_text(recovery_reply)
                (root / (stream["id"] + ".runs")).unlink()

            def runs(stream):
                count = root / (stream["id"] + ".runs")
                return int(count.read_text()) if count.exists() else 0

            def reply(**extra):
                (root / "reply.json").write_text(json.dumps({
                    "ManifestUrl": origin.base + "/bad/master.m3u8?fresh=1",
                    "Cdn": [{"Name": "fresh", "ManifestUrl": origin.base + "/fresh/master.m3u8",
                        "Headers": {"Manifest": {"X-Session": "fresh-cdn"}, "Media": {"X-Session": "fresh-media"}}}],
                    "Headers": {"Manifest": {"X-Session": "fresh"}, "Media": {"X-Session": "fresh"}},
                    **extra,
                }))

            def stored(stream):
                _, state, _ = client.json("GET", "/api/state")
                return next(s for p in state["providers"] for s in p["streams"] if s["id"] == stream["id"])

            def play(stream, variant=None):
                path = f"/play/{stream['id']}/index.m3u8?key={key}"
                if variant:
                    path += "&variant=" + quote(variant, safe="")
                return client.request("GET", path)

            def wait_for(predicate, timeout=6):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if predicate():
                        return True
                    time.sleep(0.05)
                return False

            def abandon_refresh(stream):
                sock = socket.create_connection(("127.0.0.1", server.port))
                sock.sendall((f"GET /play/{stream['id']}/index.m3u8?key={key} HTTP/1.1\r\n"
                              "Host: localhost\r\n\r\n").encode())
                try:
                    assert wait_for(lambda: runs(stream) == 1), "refresh never started"
                finally:
                    sock.close()

            origin.requests.clear()
            status, doc, _ = client.json("POST", "/api/probe", {
                "url": origin.base + "/bad/master.m3u8",
                "cdnUrls": [origin.base + "/bad2/master.m3u8", origin.base + "/good/master.m3u8"]})
            check("standalone probe tries every CDN in order", status == 200 and origin.paths() == [
                "/bad/master.m3u8", "/bad2/master.m3u8", "/good/master.m3u8"], str((status, doc, origin.paths())))
            check("probe reports the successful CDN", doc.get("sourceUrl") == origin.base + "/good/master.m3u8")

            stream = create("Fallback", mirrors=[origin.base + "/bad2/master.m3u8", origin.base + "/good/master.m3u8"])
            client.request("POST", f"/api/streams/{stream['id']}/start", {})
            origin.requests.clear()
            status, body, _ = play(stream)
            check("playback falls back after 403", status == 200 and origin.paths() == [
                "/bad/master.m3u8", "/bad2/master.m3u8", "/good/master.m3u8"])
            child = next((x for x in body.decode().splitlines() if x and not x.startswith("#")), "")
            check("fallback rewrites child URLs against the successful CDN", parse_qs(urlsplit(child).query).get("variant") == [origin.base + "/good/media.m3u8"])
            check("playback key propagates to fallback child URL", parse_qs(urlsplit(child).query).get("key") == [key])
            check("playback still rejects missing keys", client.request("GET", f"/play/{stream['id']}/index.m3u8")[0] == 401)
            origin.requests.clear()
            status, body, _ = play(stream, origin.base + "/bad/media.m3u8")
            check("variant fallback keeps the media rendition", status == 200 and b"#EXT-X-MEDIA-SEQUENCE:7" in body and origin.paths() == [
                "/bad/media.m3u8", "/bad2/media.m3u8", "/good/media.m3u8"])

            reply()
            stream = create("Probe refresh", session=True)
            origin.requests.clear()
            status, doc, _ = client.json("POST", "/api/probe", {"streamId": stream["id"]})
            check("stream probe refreshes only after all old CDNs fail", status == 200 and runs(stream) == 1 and origin.paths() == [
                "/bad/master.m3u8", "/bad2/master.m3u8", "/bad/master.m3u8", "/fresh/master.m3u8"], str((status, doc, origin.paths())))
            check("probe retries with fresh session headers", doc.get("sourceUrl") == origin.base + "/fresh/master.m3u8")
            saved = stored(stream)
            check("probe saves refreshed URL, CDN list and headers", saved["url"].endswith("?fresh=1") and saved["cdnUrls"] == [origin.base + "/fresh/master.m3u8"] and "X-Session: fresh" in saved["manifestHeaders"])
            check("probe does not start the stream", saved["status"] == "stopped")
            status, _, _ = client.json("POST", "/api/probe", {"streamId": stream["id"], "url": origin.base + "/bad/edited.m3u8"})
            check("edited URL does not refresh saved stream", status == 400 and runs(stream) == 1 and stored(stream)["url"] == saved["url"])

            stream = create("Playback refresh")
            arm_playback(stream)
            origin.requests.clear()
            status, body, _ = play(stream)
            check("playback refreshes and retries the new CDN list", status == 200 and runs(stream) == 1 and origin.paths() == [
                "/bad/master.m3u8", "/bad2/master.m3u8", "/bad/master.m3u8", "/fresh/master.m3u8"], str((status, body, origin.paths())))
            check("playback keeps the stream running after recovery", stored(stream)["running"])
            child = next(x for x in body.decode().splitlines() if x and not x.startswith("#"))
            status, media, _ = client.request("GET", child)
            check("CDN-specific manifest headers survive child playlist requests", status == 200 and b"#EXTINF" in media)
            segment = next(x for x in media.decode().splitlines() if x and not x.startswith("#"))
            status, payload, _ = client.request("GET", segment)
            check("CDN-specific media headers reach segments", status == 200 and payload == b"fixture segment")
            check("playback key is never forwarded to the origin", all("key=" not in path and "Authorization" not in headers for path, headers in origin.requests))

            reply(delay=1)
            stream = create("Disconnected recovery")
            arm_playback(stream)
            abandon_refresh(stream)
            check("refresh survives the triggering player disconnecting",
                  wait_for(lambda: stored(stream)["url"].endswith("?fresh=1")))
            origin.requests.clear()
            status, _, _ = play(stream)
            check("next viewer uses saved fresh sources during cooldown",
                  status == 200 and runs(stream) == 1 and "/bad2/master.m3u8" not in origin.paths())

            reply(delay=1)
            stream = create("Disconnected recovery edited")
            arm_playback(stream)
            abandon_refresh(stream)
            edited_url = origin.base + "/good/master.m3u8"
            client.json("PUT", f"/api/streams/{stream['id']}", {**stored(stream), "url": edited_url})
            time.sleep(2)
            check("detached recovery cannot overwrite a newer source edit", stored(stream)["url"] == edited_url)

            reply(ManifestUrl=origin.base + "/bad/new.m3u8", Cdn=[])
            stream = create("Exhausted refresh")
            arm_playback(stream)
            status, body, _ = play(stream)
            check("fresh list failure returns a bounded upstream error", status == 502 and b"403" in body and runs(stream) == 1)
            play(stream)
            check("repeated failures respect refresh cooldown", runs(stream) == 1)
            _, logs, _ = client.json("GET", f"/api/logs?streamId={stream['id']}&limit=100")
            check("logs explain CDN fallback and refresh", {"manifestFetch", "cdnFallback", "manifestRefresh"}.issubset({e["event"] for e in logs["entries"]}))

            reply(fail=True)
            stream = create("Script failure", session=True)
            status, doc, _ = client.json("POST", "/api/probe", {"streamId": stream["id"]})
            check("provider script failure is surfaced", status == 400 and "manifest action exited 1" in doc.get("error", "") and runs(stream) == 1)
            check("failed refresh preserves saved source", stored(stream)["url"] == stream["url"])

            reply()
            stream = create("Concurrent edit", session=True, path="/blocked/master.m3u8")
            result = []
            def blocked_probe():
                c = api.Client(server.port)
                c.cookie = client.cookie
                result.append(c.json("POST", "/api/probe", {"streamId": stream["id"]}))
            worker = threading.Thread(target=blocked_probe)
            worker.start()
            assert origin.blocked.wait(5)
            client.json("PUT", f"/api/streams/{stream['id']}", {**stream, "url": origin.base + "/good/master.m3u8"})
            origin.release.set()
            worker.join(10)
            check("concurrent edit prevents stale manifest recovery", bool(result) and result[0][0] == 400 and runs(stream) == 0 and stored(stream)["url"] == origin.base + "/good/master.m3u8")

    if api.failures:
        raise SystemExit(f"FAILED {len(api.failures)} of {api.checks}: {api.failures}")
    print(f"manifest-recovery-smoke: all {api.checks} checks PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/restreamair-server")
    run(parser.parse_args().binary)
