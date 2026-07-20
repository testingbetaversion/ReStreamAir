# Writing a provider script

A provider script is any program — Python, shell, or a compiled executable — that owns login, session persistence, and (optionally) manifest/key resolution for a source ReStreamAir can't just hit with a fixed URL. ReStreamAir never reads or writes the script's session state itself; it only spawns the script with a flat argument list and reads whatever the script prints back.

This doc is the practical "how do I write one" companion to the reference in [README.md](README.md#script-providers), which has the full protocol tables. Start here if you're building a script from scratch.

## What's actually implemented right now

Read this before writing a script so you know what ReStreamAir will call:

- **Implemented**: `login`, `pair`, `channels`, `events`. You can configure a script provider, add accounts, click Login/Pair and watch the output live, and click "Load channels"/"Load events" to bulk-import streams from your script's output.
- **Not wired up yet**: `manifest`, `cdm`, `heartbeat`, and the session-manifest live-playback pipeline that would call them (try each CDN a `manifest` call returns, fall back to a fresh script call if all fail, refresh on expiry, periodic heartbeat). Streams imported via `channels`/`events` store everything the script reported (`SessionManifest`, `ScriptParams`, `CdmType`, etc.) so nothing is lost — playback for those streams just doesn't act on it yet. Worth writing your script's `manifest`/`cdm` handlers now anyway if you're building this out; they'll start getting called once that lands.

## How ReStreamAir runs your script

The script path is whatever you set in **Provider settings → Script path**. The interpreter is picked from the extension:

| Extension | Runs as |
|---|---|
| `.py` | `python3 <path> <args...>` |
| `.sh` / `.bash` | `/bin/sh <path> <args...>` |
| anything else | `<path> <args...>` directly — must be executable (`chmod +x`) with its own shebang |

Every invocation gets `sys.argv`-style `key=value` tokens (no `--` flags), always in this order:

1. `action=<login|pair|channels|events|manifest|cdm|heartbeat>`
2. `bind=<...> proxy=<...> doh=<...> worker=<...>` — the provider's Bind/Proxy/DoH URL/Worker fields, forwarded verbatim. ReStreamAir doesn't implement any of source-interface binding, DNS-over-HTTPS, or worker/reverse-proxy header rewriting itself — if your script needs one of these, it has to do it. Any of the four can be an empty string (`bind=`) if unset.
3. `user=<...> password=<...>` — the active account's **Username**/**Password**, each **only included if that specific field is non-empty** — `user=` and `password=` are independent, so you can get one without the other (a token-only login might only need `user=`; a pairing flow usually needs neither). An account's Username is also what identifies it everywhere in the UI (the dropdown, logs, export) — there's no separate display name to keep in sync with it.
4. For every action except `login`/`pair`: the active account's own `params` string, split on whitespace (e.g. `region=us device=abc123`). There's no UI field for this anymore (accounts created in the dialog can't set it) — it only carries a value if set another way, e.g. imported from an exported provider. Kept for backward compatibility, not something to rely on for new scripts.
5. Any action-specific params (see below).

A minimal script just needs to read `sys.argv` for `action=` and branch on it. Nothing about the argument *order* matters for parsing — `key=value` tokens can come in any order — but ReStreamAir always sends them in the order above.

### Reading `key=value` args (Python)

```python
import sys

def get_param(name, default=""):
    for arg in sys.argv[1:]:
        key, _, value = arg.partition("=")
        if key == name:
            return value
    return default

action = get_param("action")
user = get_param("user")
password = get_param("password")
```

## A minimal login script

This is deliberately bare — enough to see the shape, not a real integration. It reads `user=`/`password=`, "logs in", and writes a session file next to itself (mirroring the sessionfile-lives-next-to-the-script convention).

```python
#!/usr/bin/env python3
import sys, os, json

def get_param(name, default=""):
    for arg in sys.argv[1:]:
        key, _, value = arg.partition("=")
        if key == name:
            return value
    return default

SESSION_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "session.json")

def login():
    user = get_param("user")
    password = get_param("password")
    if not user or not password:
        print("missing user/password", file=sys.stderr)
        sys.exit(1)

    # Replace with a real request to whatever you're authenticating against.
    print(f"logging in as {user}...", flush=True)
    session = {"user": user, "token": "example-token"}
    json.dump(session, open(SESSION_FILE, "w"))
    print("login ok", flush=True)

action = get_param("action")
if action == "login":
    login()
else:
    print(f"unhandled action: {action}", file=sys.stderr)
    sys.exit(1)
```

Set this as a provider's Script path, add an account with a username and password, click **Login**, and you'll see `logging in as ...` / `login ok` appear live in the output panel.

### Pairing instead of (or alongside) login

Pairing is for device-code flows: your script requests a code, prints instructions, then blocks polling until the user has entered that code somewhere else. Because ReStreamAir streams output live line-by-line as it's printed (not just once the process exits), this works the way you'd expect — the code shows up in the panel immediately, before the script has finished:

```python
import time

def pair():
    print("go to https://example.com/pair and enter code AB12-CD34", flush=True)
    for _ in range(60):
        time.sleep(2)
        if check_if_paired():  # your own polling logic
            json.dump({"paired": True}, open(SESSION_FILE, "w"))
            print("paired", flush=True)
            return
    print("pairing timed out", file=sys.stderr)
    sys.exit(1)
```

Pairing gets `user=`/`password=` the same way login does (whichever of the two fields are non-empty on the active account) — but an account doesn't need either set for pairing to work, since a device-code flow authenticates without them. If your pairing script doesn't need credentials, just leave Username/Password blank on the account and don't read those params.

## Bulk-listing channels or events

`action=channels` / `action=events` should print one JSON object to stdout and exit — no waiting on anything. ReStreamAir parses it and creates/updates one stream per entry under that provider (matched by name, so re-running "Load channels" updates existing entries instead of duplicating them).

```python
import json

def channels():
    session = json.load(open(SESSION_FILE))
    # Replace with a real call using session["token"] etc.
    raw_channels = [{"id": "123", "name": "Example Channel 1"}, {"id": "456", "name": "Example Channel 2"}]

    output = {"Channels": []}
    for c in raw_channels:
        output["Channels"].append({
            "Name": c["name"],
            "Mode": "live",
            "SessionManifest": True,
            "ScriptParams": f"id={c['id']}",
            "CdmType": "widevine",
            "UseCdm": True,
            "Video": "best",
            "OnDemand": False,
            "SpeedUp": True,
        })
    print(json.dumps(output))

action = get_param("action")
if action == "channels":
    channels()
```

`events()` is the same shape, printing `{"Events": [...]}"` instead, with `Start`/`End` (unix seconds) and `Autostart`/`RecordEvent` set as needed. See the [README's field table](README.md#script-providers) for what every key means.

`ScriptParams` is the important one to get right — it's what your `manifest`/`cdm`/`heartbeat` handlers will receive for *that specific channel/event*, so put whatever your handler needs to look up the stream (e.g. `id=123` or `slug=stream-name`).

## Complete Action Reference

Every action receives the common argument prefix (`action=<name> user=... password=... bind=... proxy=... doh=... worker=...`). Here are all supported actions, their extra inputs, and expected stdout outputs:

| Action | Extra Inputs Passed | Expected Output Format (stdout) |
|---|---|---|
| `login` / `pair` | Common args | Live progress text to stdout/stderr. Script saves session token locally. |
| `channels` | Common args | JSON: `{"Channels": [{"Name": "...", "Mode": "live", "SessionManifest": true, "UseCdm": true, "CdmType": "widevine", "ScriptParams": "id=101"}]}` |
| `events` | Common args | JSON: `{"Events": [{"Name": "...", "Mode": "live", "SessionManifest": true, "Autostart": true, "Start": 1700000000, "End": 1700007200, "ScriptParams": "eventId=50"}]}` |
| `epg` | Common args | XMLTV XML string or JSON: `{"EPG": [{"ChannelId": "101", "Title": "Show Name", "Start": 1700000000, "End": 1700003600}]}` |
| `manifest` | Common args + `ScriptParams` | JSON: `{"ManifestUrl": "https://...", "Cdn": [{"Name": "c1", "ManifestUrl": "..."}], "Headers": {"Manifest": {...}, "Media": {...}}, "Heartbeat": {"PeriodMs": 300000}}` |
| `downloadmanifest` | Common args + `url=<manifestUrl>` | Raw manifest text string (MPD or M3U8) or JSON: `{"ManifestContent": "..."}` |
| `downloadmedia` | Common args + `url=<segmentUrl>` | Raw binary segment payload streamed to stdout. |
| `pssh` | Common args + `pssh=<pssh> url=<url>` | JSON: `{"ProcessedPssh": "AAAA..."}` |
| `cdm` | Common args + `cdm=external\|internal cdmType=widevine\|playready pssh=<pssh> kid=<kid>` + `ScriptParams` | Clear keys as `KID:KEY` lines, `--> KID:KEY` lines, or JSON: `{"keys": [{"kid": "...", "key": "..."}]}` |
| `heartbeat` | Common args + `ScriptParams` | Ping session. Exit code 0 = success, non-zero = error. |

## Testing a script without the panel

Since it's just a subprocess with plain args, run it exactly the way ReStreamAir would, straight from a terminal:

```sh
python3 provider.py action=login bind= proxy= doh= worker= user=you@example.com password=hunter2
python3 provider.py action=channels bind= proxy= doh= worker= user=you@example.com password=hunter2
```

If that prints what you expect, it'll behave the same way inside the panel — the panel isn't doing anything to the args beyond building this same list and streaming stdout/stderr back.

## A couple of things worth knowing

- **Nothing here is sandboxed.** The script runs as the same OS user as ReStreamAir, with whatever filesystem/network access that user has. Don't point a script path at something you don't trust.
- **stderr is captured too**, interleaved with stdout in the output panel — fine to use it for your own debug logging without polluting the JSON stdout that `channels`/`events`/`cdm`/`manifest` need to be parseable.
- **Timeouts**: `channels`/`events` are given 60 seconds to finish before ReStreamAir gives up on them. `login`/`pair` run in the background with no timeout (pairing can legitimately take a while) — but that also means a script that hangs forever will just sit there; nothing currently kills it for you.
- **Passwords are stored in ReStreamAir's own `state.json`** (in the account you configure), not just handed to the script and forgotten — treat that file with the same care you'd give any credentials file.
