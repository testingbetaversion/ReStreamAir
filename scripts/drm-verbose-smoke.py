#!/usr/bin/env python3
"""Automatic DRM detection on start, and verbose (debug) logging.

python3 scripts/drm-verbose-smoke.py --binary build/restreamair-server

A provider script declares `cdm` but no stream ticks the legacy useCdm flag:
  * a protected MPD (cenc:default_KID) must run `cdm` and store its keys;
  * a clear MPD must start without running `cdm` and without an error;
  * /api/logs?verbose=1 must turn on debug entries (HTTP attempts with timings,
    playback requests) that the normal view never returns.
No public sources or real credentials are used.
"""
import argparse
import importlib.util
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, urlsplit

spec = importlib.util.spec_from_file_location("api_smoke", Path(__file__).with_name("api-smoke.py"))
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)
check = api.check

KID = "c3d43de9ff5b5a45cdc9f4e7f177a1a5"
KEY = "00112233445566778899aabbccddeeff"


def mpd(protected):
    protection = (
        '<ContentProtection schemeIdUri="urn:mpeg:dash:mp4protection:2011" value="cenc" '
        f'cenc:default_KID="{KID[:8]}-{KID[8:12]}-{KID[12:16]}-{KID[16:20]}-{KID[20:]}"/>'
        if protected else "")
    return f'''<?xml version="1.0"?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" xmlns:cenc="urn:mpeg:cenc:2013" type="dynamic"
     availabilityStartTime="2026-01-01T00:00:00Z" minimumUpdatePeriod="PT2S" profiles="urn:mpeg:dash:profile:isoff-live:2011">
  <Period id="1" start="PT0S">
    <AdaptationSet mimeType="video/mp4" contentType="video">
      {protection}
      <SegmentTemplate timescale="1000" duration="2000" initialization="init.mp4" media="seg-$Number$.m4s" startNumber="1"/>
      <Representation id="v1" bandwidth="500000" codecs="avc1.4d401e" width="640" height="360"/>
    </AdaptationSet>
  </Period>
</MPD>'''.encode()


class Origin:
    def __enter__(self):
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                path = urlsplit(self.path).path
                if path.endswith(".mpd"):
                    body = mpd("protected" in path)
                    self.send_response(200)
                    self.send_header("Content-Type", "application/dash+xml")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                else:
                    self.send_error(404)

            def log_message(self, *_):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base = f"http://127.0.0.1:{self.server.server_port}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(5)


def run(binary):
    with tempfile.TemporaryDirectory(prefix="restream-drm-") as scratch, Origin() as origin:
        root = Path(scratch)
        script = root / "provider.py"
        script.write_text(f'''import pathlib, sys
args = dict(a.split("=", 1) for a in sys.argv[1:] if "=" in a)
if args.get("action") == "cdm":
    (pathlib.Path(__file__).parent / "cdm.calls").open("a").write(args.get("kid", "") + "\\n")
    print("{KID}:{KEY}")
''')
        calls = root / "cdm.calls"
        with api.Server(binary, scratch, api.free_port()) as server:
            client = api.Client(server.port)
            status, _, _ = client.json("POST", "/api/auth/setup", {
                "username": api.ADMIN_USER, "password": api.ADMIN_PASS})
            assert status == 200
            status, doc, _ = client.json("POST", "/api/providers", {"name": "DRM"})
            provider = doc["providers"][0]
            pid = provider["id"]
            client.json("PUT", f"/api/providers/{pid}", {
                **provider, "scriptPath": str(script), "scriptActions": ["cdm"]})
            status, doc, _ = client.json("POST", "/api/keys", {"label": "viewer"})
            key = next(k["key"] for k in doc.get("apiKeys", doc.get("keys", [])) if k["label"] == "viewer")

            def create(name, path):
                status, doc, _ = client.json("POST", f"/api/providers/{pid}/streams", {
                    "name": name, "kind": "mpd", "url": origin.base + path})
                assert status == 200, doc
                return next(s for p in doc["providers"] for s in p["streams"] if s["name"] == name)

            def logs(sid, verbose=False):
                q = f"/api/logs?streamId={quote(sid)}&limit=2000" + ("&verbose=1" if verbose else "")
                status, doc, _ = client.json("GET", q)
                assert status == 200, doc
                return doc

            # Turn verbose on before anything happens, as the panel's view does.
            doc = logs("__panel__", verbose=True)
            check("verbose: /api/logs?verbose=1 reports debug capture active",
                  doc.get("verbose", {}).get("active") is True)

            protected = create("Protected", "/protected.mpd")
            status, doc, _ = client.json("POST", f"/api/streams/{protected['id']}/start", {})
            check("drm: protected stream starts without useCdm", status == 200, doc)
            check("drm: cdm ran once with the manifest's KID",
                  calls.exists() and calls.read_text().split() == [KID],
                  calls.read_text() if calls.exists() else "no calls")
            stored = next(s for p in client.json("GET", "/api/state")[1]["providers"]
                          for s in p["streams"] if s["id"] == protected["id"])
            check("drm: the returned key was stored", KID in (stored.get("decryptionKeys") or ""))

            status, doc, _ = client.json("POST", f"/api/streams/{protected['id']}/stop", {})
            status, doc, _ = client.json("POST", f"/api/streams/{protected['id']}/start", {})
            check("drm: restart reuses stored keys instead of re-running cdm",
                  status == 200 and calls.read_text().split() == [KID], calls.read_text())

            clear = create("Clear", "/clear.mpd")
            status, doc, _ = client.json("POST", f"/api/streams/{clear['id']}/start", {})
            check("drm: clear stream starts", status == 200, doc)
            check("drm: clear stream did not run cdm", calls.read_text().split() == [KID])
            messages = [e.get("message", "") for e in logs(clear["id"])["entries"]]
            check("drm: clear stream logs 'playing without the CDM'",
                  any("playing without the CDM" in m for m in messages), messages[:10])

            client.request("GET", f"/play/{protected['id']}/index.m3u8?key={quote(key)}", cookie=False)
            time.sleep(0.5)

            normal = logs(protected["id"])["entries"]
            check("verbose: normal view has no debug entries",
                  not any(e["level"] == "debug" for e in normal))
            verbose = logs(protected["id"], verbose=True)["entries"]
            fetches = [e for e in verbose if e["level"] == "debug" and e["event"] == "httpFetch"]
            check("verbose: HTTP attempts are logged under the stream", len(fetches) > 0)
            check("verbose: HTTP lines carry status, headers and timings",
                  any(e.get("status") == 200 and "timing (ms)" in e.get("message", "")
                      and "response headers:" in e.get("message", "") for e in fetches),
                  fetches[:1])
            plays = [e for e in verbose if e["event"] == "playRequest"]
            check("verbose: playback request logged with its auth method and no key",
                  plays and "?key=" in plays[0]["message"] and key not in plays[0]["message"]
                  and "playback key in ?key= → allowed" in plays[0]["message"], plays[:1])
            check("verbose: DRM detail logged without key values",
                  any(e["event"] == "cdm" and e["level"] == "debug" and KID in e.get("message", "")
                      for e in verbose)
                  # The script's own stdout (scriptOutput) is logged verbatim as
                  # always; only the server's debug lines must mask key values.
                  and not any(KEY in e.get("message", "") for e in verbose if e["level"] == "debug"))
    if api.failures:
        print(f"drm-verbose-smoke: {len(api.failures)} check(s) FAILED")
        raise SystemExit(1)
    print("drm-verbose-smoke: PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    run(parser.parse_args().binary)
