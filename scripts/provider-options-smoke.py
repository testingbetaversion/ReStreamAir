#!/usr/bin/env python3
"""Provider options integration checks using a temporary server and local origin."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import threading
import importlib.util
import os
import shutil
from pathlib import Path
import sys
import tempfile
import time

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location("api_smoke", Path(__file__).with_name("api-smoke.py"))
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)


def run(binary):
    with tempfile.TemporaryDirectory(prefix="rs-provider-options-") as workdir:
        with api.Server(binary, workdir, api.free_port()) as server, api.HLSOrigin() as origin:
            client = api.Client(server.port)
            assert client.request("POST", "/api/auth/setup", {"username": api.ADMIN_USER, "password": api.ADMIN_PASS})[0] == 200
            options = {"userAgent": "ProviderTest/1", "xForwardedFor": "192.0.2.7", "scriptTimeoutSeconds": 1,
                       "outputFragmentsCount": 7, "dontWaitForFullPlaylist": True, "autoRefreshEvents": True,
                       "maxStreamsConcurrency": 1}
            status, state, _ = client.json("POST", "/api/providers", {"name": "Options", "options": options})
            assert status == 200
            provider = next(p for p in state["providers"] if p["name"] == "Options")
            route = f"/api/providers/{provider['id']}"
            assert all(provider["options"][key] == value for key, value in options.items())
            assert len(state["providerOptionFields"]) == 41
            assert all(not f["inactive"] for f in state["providerOptionFields"])

            for invalid in ([], None, {"scriptTimeoutSeconds": 0}, {"outputFragmentsCount": 2},
                            {"hlsFragmentDurationSeconds": 31}, {"userAgent": "agent\r\nInjected: yes"},
                            {"noRestartOnError": "true"}, {"playbackDelaySeconds": 1.5}):
                assert client.request("PUT", route, {"name": "Must not save", "options": invalid})[0] == 400
            status, state, _ = client.json("PUT", route, {"name": "Options", "options": {"playbackDelaySeconds": 12}})
            restored = next(p for p in state["providers"] if p["id"] == provider["id"])
            assert status == 200 and all(restored["options"][key] == value for key, value in options.items())
            assert restored["options"]["playbackDelaySeconds"] == 12
            print("PASS: schema, support labels, validation and partial saves")

            server.stop()
            server.start()
            _, state, _ = client.json("GET", "/api/state")
            assert next(p for p in state["providers"] if p["id"] == provider["id"])["options"] == restored["options"]
            print("PASS: provider options survive restart")

            # Require the configured headers at a real HTTP origin. Every HLS
            # master and media request must pass through the provider headers.
            original = origin.httpd.RequestHandlerClass

            class RequireHeaders(original):
                def do_GET(self):
                    if self.headers.get("User-Agent") != "ProviderTest/1" or self.headers.get("X-Forwarded-For") != "192.0.2.7":
                        self.send_error(403)
                        return
                    super().do_GET()

            origin.httpd.RequestHandlerClass = RequireHeaders
            streams = []
            for name in ("First", "Second"):
                _, state, _ = client.json("POST", route + "/streams", {
                    "name": name, "kind": "m3u8", "url": f"http://127.0.0.1:{origin.port}/master.m3u8"})
                streams.append(next(s for p in state["providers"] if p["id"] == provider["id"] for s in p["streams"] if s["name"] == name))
            assert client.request("POST", f"/api/streams/{streams[0]['id']}/start", {})[0] == 200
            assert client.request("POST", f"/api/streams/{streams[1]['id']}/start", {})[0] == 409
            _, keys, _ = client.json("POST", "/api/keys", {"label": "options"})
            key = next(k["key"] for k in keys["keys"] if k["label"] == "options")
            status, payload, _ = client.request("GET", f"/play/{streams[0]['id']}/index.m3u8?key={key}")
            assert status == 200, (status, payload)
            child = next(line for line in payload.decode().splitlines() if line and not line.startswith("#"))
            assert client.request("GET", child)[0] == 200
            print("PASS: provider headers reach master and media origin requests; stream limit enforced")

            # Exercise real request policy, including a separate provider to
            # detect cookie leakage across worker threads and provider IDs.
            lock = threading.Lock()
            counts = {"retry": 0, "active": 0, "peak": 0}
            class PolicyOrigin(original):
                def do_GET(self):
                    if self.path == "/retry.m3u8":
                        with lock:
                            counts["retry"] += 1
                            attempt = counts["retry"]
                        if attempt % 2:
                            self.send_error(503)
                            return
                    if self.path == "/redirect.json":
                        body = b'{"data":{"ManifestUrl":"/media.m3u8"}}'
                    else:
                        body = b'#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\nseg.ts\n'
                    if self.path == "/cookie.m3u8" and "session=provider-one" not in self.headers.get("Cookie", ""):
                        self.send_error(403)
                        return
                    if self.path == "/clean.m3u8" and "session=" in self.headers.get("Cookie", ""):
                        self.send_error(403)
                        return
                    delayed = self.path == "/concurrent.m3u8"
                    if delayed:
                        with lock:
                            counts["active"] += 1
                            counts["peak"] = max(counts["peak"], counts["active"])
                        time.sleep(0.25)
                    if self.path == "/slow.m3u8":
                        time.sleep(2)
                    self.send_response(200)
                    if self.path == "/set.m3u8":
                        self.send_header("Set-Cookie", "session=provider-one; Path=/")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    try:
                        self.wfile.write(body)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    finally:
                        if delayed:
                            with lock:
                                counts["active"] -= 1

            origin.httpd.RequestHandlerClass = PolicyOrigin
            def probe(path, pid=provider["id"]):
                return client.json("POST", "/api/probe", {"providerId": pid, "url": f"http://127.0.0.1:{origin.port}{path}"})
            assert client.request("PUT", route, {"name": "Options", "options": {"httpGetAttempts": 2, "httpGetTimeoutSeconds": 1,
                                                            "maxDownloadConcurrency": 1, "detectJsonRedirect": True}})[0] == 200
            assert probe("/retry.m3u8")[0] == 200 and counts["retry"] == 2
            assert probe("/redirect.json")[0] == 200
            assert client.request("PUT", route, {"name": "Options", "options": {"httpGetAttempts": 1}})[0] == 200
            assert probe("/retry.m3u8")[0] == 400 and counts["retry"] == 3
            before = time.monotonic()
            assert probe("/slow.m3u8")[0] == 400 and time.monotonic() - before < 1.8
            with ThreadPoolExecutor(max_workers=3) as pool:
                results = list(pool.map(lambda _: probe("/concurrent.m3u8")[0], range(3)))
            assert results == [200] * 3 and counts["peak"] == 1, (results, counts)
            assert client.request("PUT", route, {"name": "Options", "options": {"useSessionCookies": True}})[0] == 200
            assert probe("/set.m3u8")[0] == 200
            assert probe("/cookie.m3u8")[0] == 200
            _, other_state, _ = client.json("POST", "/api/providers", {"name": "Isolated", "options": {"useSessionCookies": True}})
            other = next(p for p in other_state["providers"] if p["name"] == "Isolated")
            assert probe("/clean.m3u8", other["id"])[0] == 200
            assert probe("/cookie.m3u8", other["id"])[0] == 400
            assert client.request("PUT", route, {"name": "Options", "options": {"useSessionCookies": False}})[0] == 200
            assert probe("/clean.m3u8")[0] == 200
            print("PASS: HTTP attempt/timeout limits, JSON redirects, provider download cap and isolated cookie jars")

            script = os.path.join(workdir, "slow-provider.py")
            Path(script).write_text("import time\ntime.sleep(4)\nprint('too late')\n")
            assert client.request("PUT", route, {"name": "Options", "scriptPath": script, "scriptActions": ["login"]})[0] == 200
            before = time.monotonic()
            status, result, _ = client.json("POST", route + "/script/run", {"action": "login"})
            assert status == 200 and result["exitCode"] != 0 and time.monotonic() - before < 3
            assert "timed out" in result["output"], result
            print("PASS: configured script timeout terminates the action")

            # Catalogue updates run on the maintenance clock, without a client
            # waiting for the script, and only delete imported entries.
            catalogue = Path(workdir) / "catalogue.json"
            catalogue.write_text('{"events":[{"name":"Scheduled event","end":4102444800}]}')
            script = Path(workdir) / "catalogue.py"
            script.write_text("from pathlib import Path\nprint(Path(" + repr(str(catalogue)) + ").read_text())\n")
            _, state, _ = client.json("POST", "/api/providers", {"name": "Scheduled"})
            scheduled = next(p for p in state["providers"] if p["name"] == "Scheduled")
            scheduled_route = f"/api/providers/{scheduled['id']}"
            def configure_scheduled(options):
                assert client.request("PUT", scheduled_route, {"name": "Scheduled", "scriptPath": str(script),
                    "scriptActions": ["events", "channels"], "options": options})[0] == 200
            def scheduled_streams():
                _, state, _ = client.json("GET", "/api/state")
                return next(p["streams"] for p in state["providers"] if p["id"] == scheduled["id"])
            def wait_for(predicate, seconds=8):
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    value = predicate()
                    if value:
                        return value
                    time.sleep(0.15)
                raise AssertionError("Timed out waiting for provider maintenance")
            def applied_script_runs():
                _, result, _ = client.json("GET", f"/api/logs?streamId=script:{scheduled['id']}&limit=300")
                return sum(1 for entry in result["entries"] if entry["event"] == "scriptEnd")
            def settle_scripts():
                # Turning auto-refresh off stops new runs being SCHEDULED, but a run
                # the timer already dispatched still lands, and a background run is
                # applied on the maintenance tick rather than when the script exits
                # — so it can overtake the manual runs below and import whatever the
                # catalogue said when it started. Every applied run logs scriptEnd,
                # so wait for that count to stop moving before the catalogue is
                # rewritten to mean something else. Without this the test is a race
                # against a one-second timer, and losing it plants an event ending in
                # 2100 that autoRemoveFinishedEvents will never clean up.
                last, since = applied_script_runs(), time.monotonic()
                while time.monotonic() - since < 1.5:
                    time.sleep(0.1)
                    count = applied_script_runs()
                    if count != last:
                        last, since = count, time.monotonic()
            configure_scheduled({"autoRefreshEvents": True, "eventsRefreshSeconds": 1})
            first = wait_for(lambda: next((s for s in scheduled_streams() if s["name"] == "Scheduled event"), None))
            configure_scheduled({"autoRefreshEvents": False})
            settle_scripts()
            catalogue.write_text('{"events":[{"name":"Scheduled event","end":1}]}')
            assert client.request("POST", scheduled_route + "/script/events", {})[0] == 200
            configure_scheduled({"reuseEventIndex": True})
            catalogue.write_text('{"events":[{"name":"Replacement event","end":4102444800}]}')
            assert client.request("POST", scheduled_route + "/script/events", {})[0] == 200
            replacement = next(s for s in scheduled_streams() if s["name"] == "Replacement event")
            assert replacement["id"] == first["id"]
            catalogue.write_text('{"events":[{"name":"Replacement event","end":1}]}')
            assert client.request("POST", scheduled_route + "/script/events", {})[0] == 200
            configure_scheduled({"autoRemoveFinishedEvents": True})
            wait_for(lambda: not scheduled_streams())
            catalogue.write_text('{"channels":[{"name":"Imported channel"}]}')
            assert client.request("POST", scheduled_route + "/script/channels", {})[0] == 200
            assert client.request("POST", scheduled_route + "/streams", {"name": "Manual channel", "kind": "m3u8", "url": f"http://127.0.0.1:{origin.port}/media.m3u8"})[0] == 200
            configure_scheduled({"autoRemoveMissingChannels": True})
            catalogue.write_text('{"channels":[]}')
            assert client.request("POST", scheduled_route + "/script/channels", {})[0] == 200
            assert [s["name"] for s in scheduled_streams()] == ["Manual channel"]
            print("PASS: background event refresh, finished-event cleanup, event ID reuse and missing-channel cleanup")

            for name in ("Auto one", "Auto two"):
                assert client.request("POST", scheduled_route + "/streams", {"name": name, "kind": "m3u8", "url": f"http://127.0.0.1:{origin.port}/media.m3u8", "autostart": True})[0] == 200
            configure_scheduled({"sequentialAutostartPeriodSeconds": 1, "maxStreamsConcurrency": 1})
            started = wait_for(lambda: next((s for s in scheduled_streams() if s["status"] == "running"), None))
            assert started["name"] == "Auto one"
            configure_scheduled({"sequentialAutostartPeriodSeconds": 0, "autoRestartPeriodSeconds": 1, "restartDelaySeconds": 2})
            wait_for(lambda: all(s["status"] == "stopped" for s in scheduled_streams()))
            wait_for(lambda: any(s["status"] == "running" for s in scheduled_streams()))
            configure_scheduled({"autoRestartPeriodSeconds": 0, "randomAutostartPeriodSeconds": 1})
            assert client.request("POST", f"/api/streams/{started['id']}/stop", {})[0] == 200
            wait_for(lambda: any(s["status"] == "running" for s in scheduled_streams()))
            assert sum(s["status"] == "running" for s in scheduled_streams()) == 1
            print("PASS: sequential/random autostart, concurrency cap and timed restart with delay")
            if shutil.which("ffmpeg"):
                _, state, _ = client.json("POST", "/api/providers", {"name": "FFmpeg policy", "options": {"noRestartOnError": True, "httpGetTimeoutSeconds": 1, "maxStreamsConcurrency": 1}})
                ffprovider = next(p for p in state["providers"] if p["name"] == "FFmpeg policy")
                _, state, _ = client.json("POST", f"/api/providers/{ffprovider['id']}/streams", {"name": "Failing FFmpeg", "kind": "m3u8", "inputMode": "ffmpegTsHls", "url": f"http://127.0.0.1:{origin.port}/cookie.m3u8"})
                ffsid = next(s["id"] for p in state["providers"] if p["id"] == ffprovider["id"] for s in p["streams"])
                assert client.request("POST", f"/api/streams/{ffsid}/start", {})[0] == 200
                def ff_stopped():
                    _, state, _ = client.json("GET", "/api/state")
                    return next(s["status"] for p in state["providers"] if p["id"] == ffprovider["id"] for s in p["streams"]) == "stopped"
                wait_for(ff_stopped)
                print("PASS: real FFmpeg failure updates status and releases provider concurrency")



if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/restreamair-server")
    run(parser.parse_args().binary)
