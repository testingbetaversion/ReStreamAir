#!/usr/bin/env python3
"""One integration scenario: idle HLS -> shared local buffer -> idle again.
Requires ffmpeg; generates a tiny local audio/video source. No public streams.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

spec = importlib.util.spec_from_file_location("api", Path(__file__).with_name("api-smoke.py"))
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)


def run(binary, multi=False, skip_idle=False):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("ffmpeg is required for the buffered HLS check")
    with tempfile.TemporaryDirectory(prefix="restream-buffer-") as scratch:
        root = Path(scratch)
        media = root / "media"
        media.mkdir()
        generate = [ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=10",
                        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "60", "-c:v", "libx264",
                        "-preset", "ultrafast", "-g", "10", "-c:a", "aac", "-f", "hls", "-hls_time", "1",
                        "-hls_list_size", "0"]
        if multi:
            generate += ["-map", "0:v", "-map", "0:v", "-map", "1:a", "-map", "1:a",
                "-var_stream_map", "v:0,agroup:audio,name:high v:1,agroup:audio,name:low a:0,agroup:audio,name:english,default:yes a:1,agroup:audio,name:spanish",
                "-master_pl_name", "source.m3u8", str(media / "%v.m3u8")]
        else:
            generate += [str(media / "source.m3u8")]
        subprocess.run(generate, check=True)
        requests = []
        origin_started = time.monotonic()

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                name = Path(urlsplit(self.path).path).name
                path = media / (name[:-4] + ".ts" if name.endswith(".pdf") else name)
                if not path.is_file():
                    self.send_error(404)
                    return
                data = path.read_bytes()
                if name.endswith(".m3u8"):
                    data = data.replace(b"#EXT-X-ENDLIST\n", b"").replace(b".ts\n", b".pdf\n")
                    if b"#EXTINF:" in data:
                        # Advance a real live window. A frozen final window can
                        # strand FFmpeg while it is still analyzing all tracks.
                        header, *segments = data.split(b"#EXTINF:")
                        end = min(len(segments), 6 + int(time.monotonic() - origin_started))
                        start = max(0, end - 6)
                        header = re.sub(rb"#EXT-X-MEDIA-SEQUENCE:\d+", f"#EXT-X-MEDIA-SEQUENCE:{start}".encode(), header)
                        data = header + b"".join(b"#EXTINF:" + segment for segment in segments[start:end])
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Content-Type", "application/vnd.apple.mpegurl" if name.endswith(".m3u8") else "video/mp2t")
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *_):
                pass

        origin = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=origin.serve_forever, daemon=True)
        thread.start()
        try:
            with api.Server(binary, scratch, api.free_port()) as server:
                client = api.Client(server.port)
                client.json("POST", "/api/auth/setup", {"username": api.ADMIN_USER, "password": api.ADMIN_PASS})
                _, doc, _ = client.json("POST", "/api/providers", {"name": "Buffer fixture"})
                pid = doc["providers"][0]["id"]
                options = {}
                if multi:
                    options = {"representations": ["v0", "v1", "a0", "a1"],
                        "representationOrder": ["v0", "v1", "a0", "a1"],
                        "representationMeta": {"v0": {"type": "video"}, "v1": {"type": "video"},
                            "a0": {"type": "audio"}, "a1": {"type": "audio"}}}
                _, doc, _ = client.json("POST", f"/api/providers/{pid}/streams", {
                    "name": "Buffered", "kind": "m3u8", "url": f"http://127.0.0.1:{origin.server_port}/source.m3u8",
                    "inputMode": "hlsBuffered", "directSource": True, "outputMode": "hls",
                    "hlsSegmentSeconds": 1, "playlistSegments": 3, **options})
                stream = doc["providers"][0]["streams"][0]
                sid = stream["id"]
                status, doc, _ = client.json("POST", f"/api/streams/{sid}/start", {})
                assert status == 200, doc
                time.sleep(0.4)
                assert requests == [], "Start downloaded media before any viewer"
                print("ok: armed stream does not download without viewers", flush=True)
                _, doc, _ = client.json("POST", "/api/keys", {"label": "fixture"})
                key = doc["keys"][0]["key"]
                path = f"/play/{sid}/index.m3u8?key={key}"
                assert client.request("GET", f"/play/{sid}/index.m3u8")[0] == 401
                assert requests == [], "Unauthorized viewer started the downloader"
                status, payload, _ = client.request("GET", path)
                if status != 200:
                    _, logs, _ = client.json("GET", f"/api/logs?streamId={sid}&limit=30")
                    raise AssertionError(logs)
                playlist = payload.decode()
                if multi:
                    assert playlist.count("#EXT-X-STREAM-INF:") == 2, playlist
                    assert playlist.count("#EXT-X-MEDIA:TYPE=AUDIO") == 2, playlist
                def verify_local(text):
                    children = [x for x in text.splitlines() if x and not x.startswith("#")]
                    children += re.findall(r'URI="([^"]+)"', text)
                    for child in children:
                        assert child.startswith(f"/play/{sid}/") and "u=" not in child and f"key={key}" in child, text
                        status, payload, _ = client.request("GET", child)
                        assert status == 200, (child, status, payload)
                        if urlsplit(child).path.endswith(".m3u8"):
                            verify_local(payload.decode())
                verify_local(playlist)
                second = api.Client(server.port)
                assert second.request("GET", path)[0] == 200
                _, logs, _ = client.json("GET", f"/api/logs?streamId={sid}&limit=100")
                assert sum(e["event"] == "hlsBufferStart" for e in logs["entries"]) == 1
                assert requests and all("key=" not in req for req in requests)
                print("ok: viewers share downloaded local segments; playback keys stay local", flush=True)
                if not skip_idle:
                    # One bounded wait verifies the actual idle policy. Reading logs
                    # does not count as a viewer and cannot keep the buffer alive.
                    deadline = time.monotonic() + 34
                    while time.monotonic() < deadline:
                        _, logs, _ = client.json("GET", f"/api/logs?streamId={sid}&limit=100")
                        if any(e["event"] == "hlsBufferIdle" for e in logs["entries"]):
                            break
                        time.sleep(0.5)
                    else:
                        raise AssertionError("Downloader did not stop after the last viewer")
                    before = len(requests)
                    time.sleep(1.2)
                    assert len(requests) == before, "Origin fetches continued after idle shutdown"
                    print("ok: downloader stops after viewers leave", flush=True)
                client.request("POST", f"/api/streams/{sid}/stop", {})
        finally:
            origin.shutdown()
            origin.server_close()
            thread.join(5)
    print("hls-buffer-smoke: PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/restreamair-server")
    parser.add_argument("--multi", action="store_true", help="Exercise two video qualities and two audio tracks")
    parser.add_argument("--skip-idle", action="store_true", help="Skip the 30-second idle check when only testing startup/output")
    args = parser.parse_args()
    run(args.binary, args.multi, args.skip_idle)
