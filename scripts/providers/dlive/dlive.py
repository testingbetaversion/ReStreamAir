
import json
from pathlib import Path
import re
import sys
from urllib.parse import parse_qs, urljoin, urlsplit

import requests
from bs4 import BeautifulSoup

from get_all_players import (BASE_URL, TIMEOUT, PLAYER_TIMEOUT, TOTAL_TIMEOUT, USER_AGENT,
                             get_all_players, positive_seconds, run_with_timeout)

CHANNELS_URL = urljoin(BASE_URL, "24-7-channels.php")


def parse_params(arguments):
    """Preserve embedded '=' characters in O11 argument values."""
    return dict(arg.split("=", 1) for arg in arguments if "=" in arg)


def session_factory_for(params):
    """Provide a separate configured HTTP session for each resolver worker."""
    network = {name: params.get(name, "") for name in ("bind", "proxy", "worker")}
    doh = params.get("doh", "")
    if not (doh or any(network.values())):
        return requests.Session
    try:
        import o11
    except ModuleNotFoundError as exc:
        if exc.name != "o11":
            raise
        if doh or network["bind"] or network["worker"]:
            raise ValueError("bind, doh and worker require the O11-provided o11 module.") from exc

        def new_session():
            session = requests.Session()
            session.proxies.update({"http": network["proxy"], "https": network["proxy"]})
            return session

        return new_session
    if doh:
        o11.dns(doh)
    return lambda: o11.session(**network).get_session()


def parse_channels(document):
    """Read only channel cards; ignore navigation, ads and other links."""
    soup = BeautifulSoup(document, "html.parser")
    channels = []
    seen = set()
    for card in soup.select("a.card[href]"):
        target = urlsplit(urljoin(CHANNELS_URL, card["href"]))
        if target.netloc != urlsplit(BASE_URL).netloc or target.path != "/watch.php":
            continue
        raw_id = parse_qs(target.query).get("id", [""])[0]
        title = card.select_one(".card__title")
        if not raw_id.isascii() or not raw_id.isdigit() or int(raw_id) < 1 or title is None:
            continue
        channel_id = int(raw_id)
        name = title.get_text(" ", strip=True)
        if not name or channel_id in seen:
            continue
        seen.add(channel_id)
        channels.append({
            # "Id": channel_id,
            "Name": name,
            "Mode": "live",
            "SessionManifest": True,
            "ManifestScript": f"id={channel_id}",
            "ScriptParams": f"id={channel_id}",
            "UseCdm": False,
            "Video": "best",
            "OnDemand": True,
            "SpeedUp": True,
        })
    if not channels:
        raise ValueError("No channel cards were found in the DLive channel directory.")
    return {"Channels": channels}


def get_channels(session_factory=requests.Session, *, timeout=TOTAL_TIMEOUT,
                 connect_timeout=TIMEOUT[0], read_timeout=TIMEOUT[1]):
    def load():
        with session_factory() as session:
            response = session.get(
                CHANNELS_URL,
                headers={"User-Agent": USER_AGENT, "Referer": BASE_URL},
                timeout=(connect_timeout, read_timeout),
            )
            response.raise_for_status()
            # The directory is UTF-8; some data-first attributes contain truncated
            # characters. Decode those safely while preserving full channel titles.
            return parse_channels(response.content.decode("utf-8", errors="replace"))
    return run_with_timeout(load, timeout, "DLive channel directory")


def manifest_from_players(resolved, preferred_player=None, diagnostics=None):
    diagnostics = sys.stderr if diagnostics is None else diagnostics
    cdns = []
    candidates = []
    for player in resolved["players"]:
        if player.get("error"):
            print(f"{player['name']}: {player['error']}", file=diagnostics)
        for index, stream in enumerate(player.get("streams", []), start=1):
            url = stream.get("url", "")
            if urlsplit(url).scheme not in ("https", "http"):
                continue
            kind = stream.get("type", "video")
            name = player["name"]
            if index > 1:
                name += f" / {index}"
            if kind != "hls":
                name += f" ({kind.upper()})"
            headers = {"User-Agent": USER_AGENT, **stream.get("headers", {})}
            cdn = {
                "Name": name,
                "ManifestUrl": url,
                "Headers": {"Manifest": dict(headers), "Media": dict(headers)},
            }
            cdns.append(cdn)
            candidates.append((player["name"], kind, cdn))
    if not cdns:
        raise ValueError("No stream URL was resolved for this channel.")

    eligible = candidates
    if preferred_player:
        selected_name = preferred_player.strip()
        if selected_name.isdigit():
            selected_name = f"Player {int(selected_name)}"
        eligible = [item for item in candidates if item[0].casefold() == selected_name.casefold()]
        if not eligible:
            raise ValueError(f"No stream was resolved for {selected_name}.")
    selected = next((item[2] for item in eligible if item[1] == "hls"), eligible[0][2])
    return {
        "Cdn": cdns,
        "ManifestUrl": selected["ManifestUrl"],
        "Headers": selected["Headers"],
        "Heartbeat": {"Url": "", "Params": "", "PeriodMs": 300000},
    }


def run_action(params):
    action = params.get("action", "channels")
    if action == "heartbeat":
        return None
    if action not in ("channels", "manifest"):
        raise ValueError(f"Unsupported action: {action}. Use channels, manifest or heartbeat.")
    channel_id = None
    if action == "manifest":
        raw_id = params.get("id", "")
        if not re.fullmatch(r"[0-9]+", raw_id) or int(raw_id) < 1:
            raise ValueError("Manifest requests require a positive channel ID, for example id=51.")
        channel_id = int(raw_id)
    limits = {name: positive_seconds(params.get(name) or default, name)
              for name, default in (("timeout", TOTAL_TIMEOUT), ("player_timeout", PLAYER_TIMEOUT),
                                    ("connect_timeout", TIMEOUT[0]), ("read_timeout", TIMEOUT[1]))}
    factory = session_factory_for(params)
    if action == "channels":
        return get_channels(factory, **{key: value for key, value in limits.items() if key != "player_timeout"})
    fast = str(params.get("fast", "1")).lower() not in ("0", "false", "no", "off")
    resolved = get_all_players(channel_id, session_factory=factory, **limits,
                               fast=fast, grace_seconds=params.get("cdn_grace", 1.0),
                               preferred_player=params.get("player"))
    return manifest_from_players(resolved, params.get("player"))


def main(arguments=None):
    arguments = sys.argv[1:] if arguments is None else arguments
    if "--help" in arguments or "-h" in arguments:
        print(__doc__)
        return 0
    params = parse_params(arguments)
    try:
        output = run_action(params)
        if output is not None:
            document = json.dumps(output, indent=2, ensure_ascii=False) + "\n"
            if params.get("output"):
                Path(params["output"]).write_text(document, encoding="utf-8")
            sys.stdout.write(document)
        return 0
    except (requests.RequestException, ValueError, OSError) as exc:
        print(f"DLive error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
