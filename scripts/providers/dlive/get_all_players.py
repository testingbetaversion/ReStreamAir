

import argparse
import base64
import html
import json
import math
import queue
import re
import sys
import threading
import time
from urllib.parse import parse_qs, quote, urlencode, urljoin, urlsplit

import requests
from bs4 import BeautifulSoup

BASE_URL = "https://dlive.sx/"
USER_AGENT = (
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/140.0.0.0 Safari/537.36"
)
TIMEOUT = (3.0, 5.0)
PLAYER_TIMEOUT = 12.0
TOTAL_TIMEOUT = 20.0
JS_STRING = r'''(?:"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')'''


def positive_seconds(value, name):
    try:
        seconds = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be a positive number of seconds.") from exc
    if not math.isfinite(seconds) or seconds <= 0:
        raise ValueError(f"{name} must be a positive number of seconds.")
    return seconds


def remaining(deadline):
    seconds = deadline - time.monotonic()
    if seconds <= 0:
        raise TimeoutError("Lookup deadline exceeded.")
    return seconds


def run_with_timeout(callback, seconds, description):
    """Bound even DNS/proxy calls that do not honor Requests socket timeouts.

    Daemon workers are deliberate: a timed-out ThreadPoolExecutor worker would
    still be joined at interpreter exit, preventing this CLI from finishing.
    A timed-out daemon may finish later when this module is used as a library.
    """
    seconds = positive_seconds(seconds, "timeout")
    result_queue = queue.Queue(maxsize=1)

    def run():
        try:
            result_queue.put((True, callback()))
        except Exception as exc:
            result_queue.put((False, exc))

    threading.Thread(target=run, daemon=True, name="dlive-request").start()
    try:
        success, result = result_queue.get(timeout=seconds)
    except queue.Empty as exc:
        raise TimeoutError(f"{description} timed out after {seconds:g}s.") from exc
    if not success:
        raise result
    return result


def origin(url):
    parts = urlsplit(url)
    return f"{parts.scheme}://{parts.netloc}"


def b64(value):
    return base64.b64decode(value, validate=True).decode("utf-8")


def decode_config(encoded):
    scrambled = b64(encoded)
    size = (len(scrambled) + 3) // 4
    chunks = [""] * 4
    for index, destination in enumerate((2, 0, 3, 1)):
        chunk = scrambled[index * size : (index + 1) * size]
        chunks[destination] = b64(chunk[:3] + chunk[4:])
    return json.loads(b64("".join(chunks)))


def js_string(literal):
    # Decode only string escapes, never JavaScript expressions.
    value = literal[1:-1]
    value = re.sub(r"\\u([0-9a-fA-F]{4})", lambda m: chr(int(m[1], 16)), value)
    value = re.sub(r"\\x([0-9a-fA-F]{2})", lambda m: chr(int(m[1], 16)), value)
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "r": "\r", "t": "\t"}.get(m[1], m[1]), value)


def variable(script, name):
    match = re.search(rf"\b{re.escape(name)}\s*=\s*({JS_STRING})", script)
    return js_string(match[1]) if match else None


def hls_urls(script):
    found = []
    for match in re.finditer(JS_STRING, script):
        value = html.unescape(js_string(match[0])).strip()
        if value.startswith(("https://", "http://", "//")) and ".m3u8" in urlsplit(value).path.lower():
            found.append(value)
    return found


def extract_settings(soup, page_url=""):
    """Return URL/format pairs from the currently supported player formats."""
    script = "\n".join(tag.get_text() for tag in soup.find_all("script", src=False))
    match = re.search(r"window\._econfig\s*=\s*['\"]([A-Za-z0-9+/=]+)['\"]", script)
    if match:
        config = decode_config(match[1])
        return [(config[key], "hls") for key in ("stream_url", "stream_url_nop2p") if config.get(key)]

    # VideoCDN uses a repeating XOR over two hex strings for Clappr's source.
    # Prefer that source over literal URLs used by background connection checks.
    for source in re.finditer(r"\bsource\s*:\s*([\w$]+)\s*[,}]", script):
        match = re.search(
            rf"\b{re.escape(source[1])}\s*=\s*xorDecrypt\(\s*([\w$]+)\s*,\s*([\w$]+)\s*\)",
            script,
        )
        if not match:
            continue
        values = [variable(script, name) for name in match.groups()]
        if not all(value and re.fullmatch(r"(?:[0-9a-fA-F]{2})+", value) for value in values):
            raise ValueError("Invalid hex-encoded player source.")
        encrypted, key = (bytes.fromhex(value) for value in values)
        url = bytes(value ^ key[index % len(key)] for index, value in enumerate(encrypted)).decode("utf-8").strip()
        if urlsplit(url).scheme not in ("http", "https") or not urlsplit(url).path.lower().endswith(".m3u8"):
            raise ValueError("Decoded player source is not an HLS URL.")
        return [(url, "hls")]

    # Clappr settings with source: window.atob('...').
    urls = []
    for match in re.finditer(r"\b(?:source|file)\s*:\s*(?:window\.)?atob\(\s*['\"]([A-Za-z0-9+/=]+)['\"]\s*\)", script):
        urls.append(b64(match[1]).strip())

    # XYZStreams builds a URL template from the current page's query parameters.
    query = parse_qs(urlsplit(page_url).query)
    bindings = {}
    for match in re.finditer(rf"\b(?:const|let|var)\s+([\w$]+)\s*=\s*[\w$]+\.get\(\s*({JS_STRING})\s*\)", script):
        values = query.get(js_string(match[2]))
        if values:
            bindings[match[1]] = values[0]
    for match in re.finditer(r"\b(?:const|let|var)\s+([\w$]+)\s*=\s*encodeURIComponent\(\s*([\w$]+)\s*\)", script):
        if match[2] in bindings:
            bindings[match[1]] = quote(bindings[match[2]], safe="-_.!~*'()")
    for match in re.finditer(r"`(https?://[^`]*\.m3u8[^`]*)`", script):
        template = match[1]
        names = re.findall(r"\$\{([\w$]+)\}", template)
        if all(name in bindings for name in names):
            value = re.sub(r"\$\{([\w$]+)\}", lambda m: bindings[m[1]], template)
            if "${" not in value:
                urls.append(value)

    # Epiembeds: byte array, XOR key, then subtraction, with changing names.
    pattern = (
        r"\bvar\s+([_$\w]+)\s*=\s*(\[[\d,\s]+\])\s*,\s*"
        r"([_$\w]+)\s*=\s*(\d+)\s*,\s*([_$\w]+)\s*=\s*(\d+)\s*,"
    )
    for match in re.finditer(pattern, script):
        array_name, numbers, key_name, key, offset_name, offset = match.groups()
        formula = rf"{re.escape(array_name)}\s*\[[^]]+\]\s*\^\s*{re.escape(key_name)}\s*\)\s*-\s*{re.escape(offset_name)}"
        if re.search(formula, script):
            decoded = "".join(chr(((n ^ int(key)) - int(offset)) & 255) for n in json.loads(numbers))
            urls.extend(hls_urls(decoded))

    # Wiki provider: source returned as a character array + array + DOM suffix.
    pattern = (
        r'return\s*\(\s*(\[(?:\s*"(?:\\.|[^"\\])*"\s*,?)+\])'
        r'\.join\(""\)\s*\+\s*([\w$]+)\.join\(""\)\s*\+'
        r'\s*document\.getElementById\("([^"]+)"\)\.innerHTML\s*\)'
    )
    for match in re.finditer(pattern, script):
        array, suffix_name, element_id = match.groups()
        suffix = re.search(rf"\b{re.escape(suffix_name)}\s*=\s*(\[[^;]*?\])\s*;", script)
        element = soup.find(id=element_id)
        if suffix and element is not None:
            urls.append("".join(json.loads(array)) + "".join(json.loads(suffix[1])) + element.decode_contents())

    urls.extend(hls_urls(script))
    if urls:
        return [(url, "hls") for url in dict.fromkeys(urls)]

    media = []
    for source in soup.select("video[src], video source[src], source#source[src]"):
        url = source.get("src", "").strip()
        if not url or "/retry/" in url:
            continue
        mime = source.get("type", "").lower()
        kind = "hls" if ".m3u8" in url or "mpegurl" in mime else "webm" if "webm" in mime else "video"
        media.append((url, kind))
    return media


def next_embed(soup, page_url, session, deadline=None, request_timeout=TIMEOUT, state=None):
    frames = soup.find_all("iframe")
    preferred = [f for f in frames if f.get("id") in ("thatframe", "player", "MyFrame") or f.get("name") == "iframe_a"]
    candidates = preferred or (frames if len(frames) == 1 else [])
    for frame in candidates:
        value = b64(frame["data-encoded"]).strip() if frame.get("data-encoded") else frame.get("src", "").strip()
        if value and value != "about:blank":
            return urljoin(page_url, value)

    scripts = "\n".join(tag.get_text() for tag in soup.find_all("script", src=False))
    # Literal src inside document.write('<ifr'+'ame ...'). No JS execution.
    for match in re.finditer(r"document\.write\((.*?)\);", scripts, re.S):
        markup = re.sub(r"'\s*\+\s*'|\"\s*\+\s*\"", "", match[1])
        frame = re.search(r'''<iframe\b[^>]*\bsrc=["'](https?://[^"']+)["']''', markup)
        if frame:
            return html.unescape(frame[1])

    # Wiki provider constructs an iframe using fid and wiki.js.
    wiki_script = next((t.get("src") for t in soup.find_all("script", src=True) if urlsplit(t["src"]).path.endswith("/wiki.js")), None)
    fid = variable(scripts, "fid")
    if wiki_script and fid:
        response = fetch(session, urljoin(page_url, wiki_script), page_url,
                         deadline=deadline, request_timeout=request_timeout, state=state)
        match = re.search(r'''src=["'](https?://[^"']+\?player=)''', response.text)
        if match and "&live=" in response.text:
            return match[1] + "desktop&" + urlencode({"live": fid})
    return None


def fetch(session, url, parent, deadline=None, request_timeout=TIMEOUT, state=None):
    if urlsplit(url).scheme not in ("http", "https"):
        raise ValueError("Unsupported embed URL scheme.")
    referrer = parent if origin(parent) == origin(url) else origin(parent) + "/"
    if state is not None:
        state["host"] = urlsplit(url).hostname or url
    if deadline is not None:
        left = remaining(deadline)
        request_timeout = tuple(min(value, left) for value in request_timeout)
    response = session.get(url, headers={"Referer": referrer}, timeout=request_timeout)
    response.raise_for_status()
    if deadline is not None:
        remaining(deadline)
    return response


def resolve_player(player, watch_url, session_factory=requests.Session,
                   deadline=None, request_timeout=TIMEOUT, state=None):
    result = {**player, "status": "failed", "streams": [], "chain": []}
    current, parent = player["player_url"], watch_url
    seen = set()
    try:
        with session_factory() as session:
            session.headers["User-Agent"] = USER_AGENT
            for _ in range(10):
                if current in seen:
                    raise ValueError("Embed redirect loop detected.")
                seen.add(current)
                response = fetch(session, current, parent, deadline=deadline,
                                 request_timeout=request_timeout, state=state)
                current = response.url
                result["chain"].append(current)
                soup = BeautifulSoup(response.text, "html.parser")
                media = extract_settings(soup, current)

                # LiveLive24 may initially serve a placeholder, then fetch a token.
                if not media and soup.select_one("video#player source#source"):
                    scripts = "\n".join(t.get_text() for t in soup.find_all("script", src=False))
                    api, base, stream_id = (variable(scripts, n) for n in ("apiUrl", "baseUrl", "streamId"))
                    if api and base and stream_id and "'/webm/?t='" in scripts:
                        data = fetch(session, api, current, deadline=deadline,
                                     request_timeout=request_timeout, state=state).json().get("parsed_data", {})
                        if data.get("token") and data.get("code"):
                            media = [(f"{base}/{data['token']}/{data['code']}/{stream_id}/webm/?t={int(time.time()*1000)}", "webm")]

                if media:
                    unique = set()
                    for url, kind in media:
                        url = urljoin(current, url)
                        if url in unique or urlsplit(url).scheme not in ("http", "https"):
                            continue
                        unique.add(url)
                        result["streams"].append({
                            "url": url, "type": kind,
                            "headers": {"User-Agent": USER_AGENT, "Referer": origin(current) + "/", "Origin": origin(current)},
                        })
                    if result["streams"]:
                        if deadline is not None:
                            remaining(deadline)
                        result["status"] = "resolved"
                        result["embed_url"] = current
                        return result
                following = next_embed(soup, current, session, deadline=deadline,
                                       request_timeout=request_timeout, state=state)
                if not following:
                    raise ValueError("No stream setting or supported embedded player found.")
                parent, current = current, following
            raise ValueError("Exceeded 10 nested player pages.")
    except (requests.RequestException, ValueError, KeyError, TypeError, TimeoutError) as exc:
        result["error"] = str(exc)
    return result


def collect_players(players, watch_url, session_factory, deadline,
                    player_timeout, request_timeout, fast=False, grace_seconds=1.0):
    """Keep completed results, mark expired players, and never join stuck I/O."""
    completed = queue.Queue()
    results = {}
    active = {}
    pending = iter(enumerate(players))
    success_deadline = None

    def resolve(index, player, expires, state):
        try:
            result = resolve_player(player, watch_url, session_factory,
                                    deadline=expires, request_timeout=request_timeout, state=state)
        except Exception as exc:
            result = {**player, "status": "failed", "streams": [], "chain": [], "error": str(exc)}
        completed.put((index, time.monotonic(), result))

    def start_available():
        while len(active) < 7 and time.monotonic() < deadline:
            item = next(pending, None)
            if item is None:
                return
            index, player = item
            started = time.monotonic()
            expires = min(deadline, started + player_timeout)
            state = {"host": urlsplit(player["player_url"]).hostname}
            active[index] = (player, started, expires, state)
            threading.Thread(target=resolve, args=(index, player, expires, state),
                             daemon=True, name=f"dlive-player-{index+1}").start()

    def accept(item):
        nonlocal success_deadline
        index, finished, result = item
        job = active.get(index)
        if job is not None and finished <= job[2]:
            results[index] = result
            del active[index]
            if fast and success_deadline is None and any(
                stream.get("type") == "hls" and urlsplit(stream.get("url", "")).scheme in ("http", "https")
                for stream in result.get("streams", [])
            ):
                success_deadline = min(deadline, finished + grace_seconds)

    start_available()
    while active:
        # Drain ready results before expiring jobs at the same deadline.
        while True:
            try:
                accept(completed.get_nowait())
            except queue.Empty:
                break
        now = time.monotonic()
        if success_deadline is not None and now >= success_deadline:
            break
        for index, (player, started, expires, state) in list(active.items()):
            if now >= expires:
                budget = expires - started
                results[index] = {**player, "status": "failed", "streams": [], "chain": [],
                                  "error": f"Timed out after {budget:.1f}s at {state['host']}; skipped."}
                del active[index]
        start_available()
        if active:
            next_deadline = min(job[2] for job in active.values())
            if success_deadline is not None:
                next_deadline = min(next_deadline, success_deadline)
            wait = max(0.0, next_deadline - time.monotonic())
            try:
                accept(completed.get(timeout=wait))
            except queue.Empty:
                pass
    if success_deadline is not None:
        # Unfinished daemon requests do not delay the CLI's exit. Return every
        # completed alternative without claiming skipped players failed.
        for index, player in enumerate(players):
            results.setdefault(index, {**player, "status": "skipped", "streams": [], "chain": []})
        return [results[index] for index in range(len(players))]
    for index, player in pending:
        results[index] = {**player, "status": "failed", "streams": [], "chain": [],
                          "error": "Overall lookup timeout reached before this player started."}
    return [results[index] for index in range(len(players))]


def get_all_players(channel_id, session_factory=requests.Session, *,
                    timeout=TOTAL_TIMEOUT, player_timeout=PLAYER_TIMEOUT,
                    connect_timeout=TIMEOUT[0], read_timeout=TIMEOUT[1],
                    fast=False, grace_seconds=1.0, preferred_player=None):
    channel_id = int(channel_id)
    if channel_id < 1:
        raise ValueError("Channel ID must be a positive integer.")
    timeout = positive_seconds(timeout, "timeout")
    player_timeout = positive_seconds(player_timeout, "player_timeout")
    request_timeout = (positive_seconds(connect_timeout, "connect_timeout"),
                       positive_seconds(read_timeout, "read_timeout"))
    deadline = time.monotonic() + timeout
    watch_url = urljoin(BASE_URL, "watch.php") + "?" + urlencode({"id": channel_id})

    def load_players():
        with session_factory() as session:
            session.headers["User-Agent"] = USER_AGENT
            response = fetch(session, watch_url, BASE_URL, deadline=deadline,
                             request_timeout=request_timeout)
        soup = BeautifulSoup(response.text, "html.parser")
        return [{"name": b.get_text(strip=True), "player_url": urljoin(response.url, b["data-url"])}
                for b in soup.select("button.player-btn[data-url]")]

    players = run_with_timeout(load_players, remaining(deadline), "DLive channel page")
    if not players:
        raise ValueError("No player buttons found for this channel ID.")
    if preferred_player:
        name = str(preferred_player).strip()
        if name.isdigit():
            name = f"Player {int(name)}"
        players = [player for player in players if player["name"].casefold() == name.casefold()]
        if not players:
            raise ValueError(f"No player button found for {name}.")
    results = collect_players(players, watch_url, session_factory, deadline,
                              player_timeout, request_timeout, fast,
                              positive_seconds(grace_seconds, "grace_seconds"))
    return {"channel_id": channel_id, "watch_url": watch_url, "players": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("channel_id", type=int)
    parser.add_argument("--json", action="store_true", help="Include stream types, playback headers, and embed chains as JSON")
    parser.add_argument("--timeout", type=float, default=TOTAL_TIMEOUT, help="Overall lookup deadline in seconds (default: 20)")
    parser.add_argument("--player-timeout", type=float, default=PLAYER_TIMEOUT, help="Deadline per player in seconds (default: 12)")
    parser.add_argument("--connect-timeout", type=float, default=TIMEOUT[0], help="Connection timeout in seconds (default: 3)")
    parser.add_argument("--read-timeout", type=float, default=TIMEOUT[1], help="Socket read timeout in seconds (default: 5)")
    args = parser.parse_args()
    try:
        result = get_all_players(args.channel_id, timeout=args.timeout, player_timeout=args.player_timeout,
                                 connect_timeout=args.connect_timeout, read_timeout=args.read_timeout)
    except (requests.RequestException, ValueError, TimeoutError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for player in result["players"]:
            print(f"{player['name']}:")
            for stream in player["streams"]:
                print(f"  {stream['type'].upper()}: {stream['url']}")
            if player.get("error"):
                print(f"  Error: {player['error']}")
    return 0 if any(p["streams"] for p in result["players"]) else 1


if __name__ == "__main__":
    sys.exit(main())
