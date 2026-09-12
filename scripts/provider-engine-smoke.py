#!/usr/bin/env python3
"""DASH lifecycle, selection, fallback and script session integration tests."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
from pathlib import Path
import sys
import tempfile
import time
import threading
import json
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("api_smoke", Path(__file__).with_name("api-smoke.py"))
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)


def run(binary):
    with tempfile.TemporaryDirectory(prefix="rs-provider-engine-") as workdir:
        with api.Server(binary, workdir, api.free_port()) as server, api.HLSOrigin() as origin:
            client = api.Client(server.port)
            assert client.request("POST", "/api/auth/setup", {"username": api.ADMIN_USER, "password": api.ADMIN_PASS})[0] == 200
            lock = threading.Lock()
            hits = {}
            mode = {"static": False, "freeze": False, "track": "low", "malformed": False, "delay": 0}
            class DashOrigin(origin.httpd.RequestHandlerClass):
                def do_GET(self):
                    with lock:
                        hits.setdefault(self.path, []).append(time.monotonic())
                        tick = 0 if mode["freeze"] or mode["static"] else int(time.monotonic())
                        values = dict(mode)
                    if self.path.startswith("/fail.mpd"):
                        self.send_error(503)
                        return
                    if self.path.endswith(".mpd"):
                        # Tiny clear media payloads verify engine policy and
                        # playlist publication; decoding is covered elsewhere.
                        template = f'<SegmentTemplate timescale="1" media="$RepresentationID$-$Time$.m4s"><SegmentTimeline><S t="{tick}" d="1" r="2"/></SegmentTimeline></SegmentTemplate>'
                        body = (f'<MPD type="{"static" if values["static"] else "dynamic"}" minimumUpdatePeriod="PT0.2S" suggestedPresentationDelay="PT{values["delay"]}S"><Period>'
                            f'<AdaptationSet contentType="video">{template}<Representation id="high" bandwidth="2000000" height="1080" codecs="avc1"/><Representation id="{values["track"]}" bandwidth="500000" height="360" codecs="avc1"/></AdaptationSet>'
                            f'<AdaptationSet contentType="audio" lang="en">{template}<Representation id="eng" bandwidth="64000" codecs="mp4a"/></AdaptationSet>'
                            f'<AdaptationSet contentType="audio" lang="ur">{template}<Representation id="urd" bandwidth="64000" codecs="mp4a"/></AdaptationSet>'
                            '</Period></MPD>').encode()
                        if values["malformed"]:
                            body = body[:-6]  # recoverable missing MPD closing tag
                    else:
                        body = b'\x00\x00\x00\x10mdat' + bytes(8)
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
            origin.httpd.RequestHandlerClass = DashOrigin
            base = f"http://127.0.0.1:{origin.port}"
            _, state, _ = client.json("POST", "/api/providers", {"name": "Engine"})
            provider = next(p for p in state["providers"] if p["name"] == "Engine")
            route = f"/api/providers/{provider['id']}"
            options = {"httpGetAttempts": 1, "httpGetTimeoutSeconds": 1, "retryNewManifestCount": 0,
                       "noRestartOnError": True, "restartDelaySeconds": 1, "dontWaitForFullPlaylist": True,
                       "hlsFragmentDurationSeconds": 1, "stalledStreamTimeoutSeconds": 20}
            def configure(**patch):
                options.update(patch)
                assert client.request("PUT", route, {"name": "Engine", "options": options})[0] == 200
            configure()
            _, state, _ = client.json("POST", route + "/streams", {"name": "Policy", "kind": "mpd", "url": base + "/fail.mpd", "parallelDownloads": 2})
            sid = next(s["id"] for p in state["providers"] if p["id"] == provider["id"] for s in p["streams"])
            _, keys, _ = client.json("POST", "/api/keys", {"label": "engine"})
            key = next(k["key"] for k in keys["keys"] if k["label"] == "engine")
            playback = f"/play/{sid}/index.m3u8?key={key}"
            def status():
                _, state, _ = client.json("GET", "/api/state")
                return next(s["status"] for p in state["providers"] if p["id"] == provider["id"] for s in p["streams"] if s["id"] == sid)
            def logs(event):
                _, result, _ = client.json("GET", f"/api/logs?streamId={sid}&limit=300")
                return [entry for entry in result["entries"] if entry["event"] == event]
            def wait_for(fn, seconds=8):
                until = time.monotonic() + seconds
                while time.monotonic() < until:
                    result = fn()
                    if result:
                        return result
                    time.sleep(0.1)
                raise AssertionError(f"Timed out; status={status()}, logs={logs('providerRestart') + logs('manifest')[:3] + logs('renditions')[:2]}")
            def start(path="/live.mpd"):
                client.request("POST", f"/api/streams/{sid}/stop", {})
                assert client.request("PUT", f"/api/streams/{sid}", {"name": "Policy", "kind": "mpd", "url": base + path, "parallelDownloads": 2})[0] == 200
                assert client.request("POST", f"/api/streams/{sid}/start", {})[0] == 200
            start("/fail.mpd")
            wait_for(lambda: status() == "stopped")
            assert len(hits["/fail.mpd"]) == 1
            configure(retryNewManifestCount=2)
            start("/fail.mpd")
            wait_for(lambda: status() == "stopped")
            assert len(hits["/fail.mpd"]) == 4
            configure(noRestartOnError=False, retryNewManifestCount=0, restartDelaySeconds=2, coolDownAutoRestart=True)
            start("/fail.mpd")
            wait_for(lambda: len(hits["/fail.mpd"]) >= 7, seconds=13)
            recent = hits["/fail.mpd"][-3:]
            assert recent[1] - recent[0] >= 1.8 and recent[2] - recent[1] >= 3.8, recent
            client.request("POST", f"/api/streams/{sid}/stop", {})
            print("PASS: DASH exhausted-request stop, fresh-manifest retry count, restart delay and cooldown", flush=True)

            configure(noRestartOnError=True, coolDownAutoRestart=False, defaultVideo="height<=720", defaultAudio="lang=ur", noRestartOnTrackChange=True)
            start()
            master = wait_for(lambda: (r[1].decode() if (r := client.request("GET", playback))[0] == 200 else None))
            assert "rep=low" in master and "rep=urd" in master and "rep=high" not in master and "rep=eng" not in master, master
            mode["track"] = "changed"
            wait_for(lambda: b"rep=changed" in client.request("GET", playback)[1])
            assert not logs("trackChanged")
            configure(noRestartOnTrackChange=False)
            start()
            wait_for(lambda: client.request("GET", playback)[0] == 200)
            mode["track"] = "changed-again"
            wait_for(lambda: logs("trackChanged"))
            client.request("POST", f"/api/streams/{sid}/stop", {})
            print("PASS: video/audio preferences and both track-change policies", flush=True)

            mode.update(static=True, delay=0)
            configure(noRestartOnTrackChange=True, ignoreDashStaticFlag=False, restartFinishedBroadcast=False)
            start()
            wait_for(lambda: client.request("GET", playback)[0] == 200)
            media = playback + "&rep=changed-again&mtype=video"
            wait_for(lambda: b"#EXT-X-ENDLIST" in client.request("GET", media)[1])
            polls = len(hits["/live.mpd"])
            time.sleep(0.6)
            assert len(hits["/live.mpd"]) == polls
            configure(ignoreDashStaticFlag=True)
            start()
            wait_for(lambda: client.request("GET", playback)[0] == 200)
            polls = len(hits["/live.mpd"])
            wait_for(lambda: len(hits["/live.mpd"]) > polls + 2)
            assert b"#EXT-X-ENDLIST" not in client.request("GET", media)[1]
            configure(ignoreDashStaticFlag=False, restartFinishedBroadcast=True, restartDelaySeconds=0)
            before = len(logs("liveStart"))
            start()
            wait_for(lambda: len(logs("liveStart")) >= before + 2, seconds=12)
            print("PASS: static MPD completion, ignore-static polling and finished-broadcast restart", flush=True)

            mode.update(static=False, freeze=True)
            configure(restartFinishedBroadcast=False, stalledStreamTimeoutSeconds=2)
            start()
            wait_for(lambda: status() == "stopped")
            mode.update(freeze=False, delay=3)
            configure(stalledStreamTimeoutSeconds=20, useDashDelay=True)
            before = time.monotonic()
            start()
            wait_for(lambda: client.request("GET", playback)[0] == 200)
            assert time.monotonic() - before >= 2.8
            print("PASS: stalled-stream timeout and MPD presentation delay", flush=True)

            mode.update(delay=0, malformed=True)
            configure(useDashDelay=False, legacyDashParser=False)
            start()
            wait_for(lambda: status() == "stopped")
            configure(legacyDashParser=True)
            start()
            wait_for(lambda: client.request("GET", playback)[0] == 200)
            mode["malformed"] = False
            configure(offAirFallback=True, offAirFallbackUrl=base + "/fallback.mpd")
            start("/fail.mpd")
            wait_for(lambda: "/fallback.mpd" in hits)
            wait_for(lambda: client.request("GET", playback)[0] == 200)
            client.request("POST", f"/api/streams/{sid}/stop", {})
            print("PASS: strict/tolerant DASH parsing and off-air fallback publication", flush=True)

            # Script-backed starts choose the named CDN and clear the idle
            # provider session before preparing a fresh session path.
            sessiondir = Path(workdir) / "runtime" / "sessions" / provider["id"]
            sessiondir.mkdir(parents=True, exist_ok=True)
            marker = sessiondir / "old-session"
            marker.write_text("stale")
            script = Path(workdir) / "manifest.py"
            script.write_text("import json\\n".replace("\\n", "\n") + "print(" + repr(json.dumps({"ManifestUrl": base + "/fail.mpd", "Cdn": [{"Name": "chosen", "ManifestUrl": base + "/chosen.mpd"}]})) + ")\n")
            options.update(alwaysResetSession=True, defaultCdn="chosen", offAirFallback=False)
            assert client.request("PUT", route, {"name": "Engine", "options": options, "scriptPath": str(script), "scriptActions": ["manifest"]})[0] == 200
            assert client.request("PUT", f"/api/streams/{sid}", {"name": "Policy", "kind": "mpd", "url": base + "/fail.mpd", "sessionManifest": True})[0] == 200
            assert client.request("POST", f"/api/streams/{sid}/start", {})[0] == 200
            wait_for(lambda: "/chosen.mpd" in hits)
            assert not marker.exists()
            client.request("POST", f"/api/streams/{sid}/stop", {})
            print("PASS: named CDN selection and idle-provider session reset", flush=True)
            script.write_text("import time\ntime.sleep(1.2)\n" + script.read_text())
            before = len(logs("scriptManifest"))
            with ThreadPoolExecutor(max_workers=1) as pool:
                future = pool.submit(client.request, "POST", f"/api/streams/{sid}/start", {})
                wait_for(lambda: len(logs("scriptManifest")) > before)
                assert client.request("POST", f"/api/streams/{sid}/stop", {})[0] == 200
                assert future.result()[0] == 409
            assert status() == "stopped"
            print("PASS: Stop supersedes an in-flight scripted start", flush=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/restreamair-server")
    run(parser.parse_args().binary)
