# Writing a provider script

A provider script is any program — Python, shell, or a compiled executable — that owns login, session persistence, and optionally manifest/key resolution for a source ReStreamAir can't just hit with a fixed URL. ReStreamAir spawns the script with a flat argument list and reads whatever it prints back. It never parses the script's session state.

This is the practical guide; [README.md](README.md#script-providers) has the summary. Start here if you're building a script from scratch.

## Three things to know first

**1. ReStreamAir only calls the actions you declare.** Provider settings has a **Script actions** grid — tick what your script implements. Anything unticked is never invoked, so a script that only does `channels` never has to handle a `start` it doesn't understand. Each stream inherits its provider's set and can override it (**Override provider script actions** in the stream editor), which is how you keep one script's `cdm` hook off a single channel that doesn't need it.

**2. Values may arrive base64-encoded.** Passing secrets and free text as literal argv is unsafe — a password ends up visible in `ps`, and a value containing a space or a quote gets mangled. So ReStreamAir encodes a value when it needs to and marks it `b64:`. **Always decode**; see below.

**3. You get a durable place to keep a session.** Every call carries `sessiondir=` and `cookies=` pointing into `runtime/scripts/<providerId>/`. ReStreamAir creates the directory, never reads it, and deletes it with the provider.

## How ReStreamAir runs your script

The script path comes from **Provider settings → Script path**. The interpreter is picked from the extension:

| Extension | Runs as |
|---|---|
| `.py` | `python3 <path> <args...>` |
| `.sh` / `.bash` | `/bin/sh <path> <args...>` |
| anything else | `<path> <args...>` directly — needs `chmod +x` and its own shebang |

Every invocation gets `sys.argv`-style `key=value` tokens (no `--` flags), in this order:

1. `action=<name>` — see the [action reference](#action-reference).
2. `bind=` `proxy=` `doh=` `worker=` — the provider's fields, forwarded verbatim. ReStreamAir implements none of source-interface binding, DNS-over-HTTPS, or worker/reverse-proxy rewriting; if your script needs one, it does it. Any may be empty.
3. `sessiondir=` `cookies=` — your durable session directory, and a suggested cookie file inside it.
4. `user=` `password=` — the active account's credentials, **each only present when that field is non-empty**. They're independent: a token-only login might get just `user=`, a pairing flow neither. The Username also labels the account throughout the UI.
5. For every action except `login`/`pair`: the account's `params` string, split on whitespace (e.g. `region=us device=abc`). No UI field sets this any more — it survives for imported providers.
6. Action-specific params.

Order never matters for parsing, but that is the order sent.

### Reading args, with decoding

```python
import base64, sys

def decode(value):
    """Values ReStreamAir couldn't pass literally arrive as b64:<base64>."""
    if value.startswith("b64:"):
        return base64.b64decode(value[4:]).decode("utf-8")
    return value

def get_param(name, default=""):
    for arg in sys.argv[1:]:
        key, _, value = arg.partition("=")
        if key == name:
            return decode(value)
    return default

action = get_param("action")
user = get_param("user")
password = get_param("password")     # very likely b64: — always decode
```

A value is encoded when it contains **whitespace, a control character, a quote, a backslash, or any non-ASCII byte** — plus **always** for `password`, so a credential never appears in a process listing.

Everything else is sent as-is, deliberately: ReStreamAir runs the interpreter directly with no shell in between, so `?`, `&`, `;`, `|` and `*` are never interpreted and encoding them would only turn every ordinary URL into an unreadable blob. So `action=manifest`, `id=123`, a plain username and `url=https://cdn.example.com/a.mpd?token=x&region=eu` all arrive exactly as written — but a password, a value with a space, and anything non-ASCII arrive `b64:`-prefixed.

Going the other way, anything your script prints may be plain text or base64. ReStreamAir tries `b64:` first, then bare base64 — but accepts the bare form only when it decodes to valid UTF-8 *and* re-encodes to exactly what you sent, so an ordinary word like `test` is never mistaken for base64. When in doubt, prefix it:

```python
def emit(text):
    print("b64:" + base64.b64encode(text.encode()).decode())
```

### The session directory

```python
import os

session_dir = get_param("sessiondir") or os.path.dirname(os.path.abspath(__file__))
cookie_file = get_param("cookies") or os.path.join(session_dir, "cookies.txt")
session_file = os.path.join(session_dir, "session.json")
```

Falling back to the script's own directory keeps it runnable by hand from a terminal, where neither arg is set. The path is stable across restarts, so a login done once survives — use **Clear session** in Provider settings to force a fresh one.

## A minimal login script

Bare on purpose — enough to see the shape.

```python
#!/usr/bin/env python3
import base64, json, os, sys

def decode(value):
    return base64.b64decode(value[4:]).decode("utf-8") if value.startswith("b64:") else value

def get_param(name, default=""):
    for arg in sys.argv[1:]:
        key, _, value = arg.partition("=")
        if key == name:
            return decode(value)
    return default

SESSION_DIR = get_param("sessiondir") or os.path.dirname(os.path.abspath(__file__))
SESSION_FILE = os.path.join(SESSION_DIR, "session.json")

def login():
    user, password = get_param("user"), get_param("password")
    if not user or not password:
        print("missing user/password", file=sys.stderr)
        sys.exit(1)
    print(f"logging in as {user}...", flush=True)     # streams to the panel live
    json.dump({"user": user, "token": "example-token"}, open(SESSION_FILE, "w"))
    print("login ok", flush=True)

action = get_param("action")
if action == "login":
    login()
else:
    print(f"unhandled action: {action}", file=sys.stderr)
    sys.exit(1)
```

Set it as the Script path, tick **Login** in Script actions, add an account, then click **Login** — the output appears live in the panel.

### Pairing

Pairing is for device-code flows: request a code, print it, block until the user enters it elsewhere. Output streams line by line as it's printed, so the code appears immediately rather than after the script exits.

```python
def pair():
    print("go to https://example.com/pair and enter code AB12-CD34", flush=True)
    for _ in range(60):
        time.sleep(2)
        if check_if_paired():                     # your own polling
            json.dump({"paired": True}, open(SESSION_FILE, "w"))
            print("paired", flush=True)
            return
    print("pairing timed out", file=sys.stderr)
    sys.exit(1)
```

An account needs no credentials for pairing — leave Username and Password blank and don't read them.

## Bulk-listing channels or events

`channels` / `events` print one JSON object and exit. ReStreamAir creates or updates one stream per entry, matched by name, so re-running updates in place instead of duplicating.

```python
def channels():
    session = json.load(open(SESSION_FILE))
    raw = [{"id": "123", "name": "Example Channel 1"}, {"id": "456", "name": "Example Channel 2"}]
    print(json.dumps({"Channels": [
        {
            "Name": c["name"],
            "Mode": "live",
            "SessionManifest": True,
            "ScriptParams": f"id={c['id']}",
            "CdmType": "widevine",
            "UseCdm": True,
            "Video": "best",
            "OnDemand": False,
            "SpeedUp": True,
        }
        for c in raw
    ]}))
```

`events()` is the same shape with `{"Events": [...]}`, plus `Start`/`End` (unix seconds) and `Autostart`/`RecordEvent`.

**`ScriptParams` is the one to get right** — it's what your `manifest`, `cdm` and `heartbeat` handlers receive for *that* channel, so put whatever identifies the stream in it (`id=123`, `slug=stream-name`). Note it is split on whitespace, so a value containing a space must be written pre-encoded: `title=b64:TXkgU2hvdw==`.

## Action reference

Every action gets the common prefix above. **Wired** means ReStreamAir calls it today. The two unwired ones are configurable now so that enabling them later needs no reconfiguration, but nothing invokes them yet — routing every segment through a spawned subprocess needs a persistent worker, which is a separate change.

| Action | Wired | Extra inputs | Expected stdout |
|---|---|---|---|
| `login` | yes | — | Progress text, streamed live. Persist your own session. |
| `pair` | yes | — | Progress text, streamed live. |
| `channels` | yes | — | `{"Channels": [...]}` |
| `events` | yes | — | `{"Events": [...]}` |
| `epg` | yes | — | XMLTV, or `{"EPG": [{"ChannelId": "101", "Title": "...", "Start": 1700000000, "End": 1700003600}]}`. Stored verbatim; read it back from `GET /api/providers/<id>/epg`. |
| `start` | yes | stream's `ScriptParams` | Ignored — exit 0. Called when a stream starts. |
| `stop` | yes | stream's `ScriptParams` | Ignored — exit 0. Called when a stream stops. |
| `manifest` | yes | stream's `ScriptParams` | `{"ManifestUrl": "...", "Cdn": [{"Name": "c1", "ManifestUrl": "..."}], "Headers": {"manifest": {...}, "media": {...}}, "Heartbeat": {"Url": "...", "Params": [...], "PeriodMs": 300000}}` |
| `url` | yes | `url=` | The replacement URL, or `{"Url": "..."}`. Print nothing to leave it alone. |
| `downloadmanifest` | yes | `url=` | The raw manifest text, or `{"ManifestContent": "..."}`. |
| `pssh` | yes | `pssh=` `url=` | The processed PSSH, or `{"ProcessedPssh": "..."}`. Print nothing to keep the original. |
| `initparse` | yes | `url=` `init=` (base64 init segment) | `{"Kid": "...", "Pssh": "...", "PsshWidevine": "...", "PsshPlayReady": "..."}` — any subset; each is folded into the following `cdm` call. |
| `cdm` | yes | `cdm=external` `cdmType=` `pssh=` `psshAll=` `psshWidevine=` `psshPlayReady=` `kid=` `keyUri=` + `ScriptParams` | `KID:KEY` lines, `--> KID:KEY` lines, or `{"keys": [{"kid": "...", "key": "..."}]}` |
| `heartbeat` | yes | `heartbeaturl=` `heartbeatparams=` + `ScriptParams` | Ignored — exit 0 for success. |
| `downloadinit` | **no** | `url=` | Raw init segment bytes. |
| `downloadmedia` | **no** | `url=` | Raw segment bytes. |

ReStreamAir has no embedded CDM, so `challenge=` is always empty — a script needing one generates it and performs its own licence exchange. It forwards every DRM detail it parsed and lets the script pick what it needs; nothing has to be used.

## Testing without the panel

It's just a subprocess, so run it the way ReStreamAir would:

```sh
python3 provider.py action=login bind= proxy= doh= worker= sessiondir=/tmp/rsa-session cookies=/tmp/rsa-session/cookies.txt user=you@example.com password=b64:aHVudGVyMg==
```

```sh
python3 provider.py action=channels bind= proxy= doh= worker= sessiondir=/tmp/rsa-session user=you@example.com
```

If that prints what you expect, it behaves the same inside the panel — nothing happens to the args beyond building this same list and streaming stdout/stderr back.

## Things worth knowing

- **Nothing is sandboxed.** The script runs as the same OS user as ReStreamAir, with all of that user's filesystem and network access. Don't point a script path at something you don't trust.
- **stderr is captured too**, interleaved with stdout in the output panel — fine for debug logging without polluting the JSON that `channels`/`events`/`cdm`/`manifest` need to be parseable. Those parsers tolerate log lines printed before the JSON blob.
- **Timeouts**: `channels`/`events` get 60s, `epg` 120s, `manifest` 45s, `cdm` 60s, the pipeline hooks (`url`, `pssh`, `initparse`) 15–20s, and `start`/`stop`/`heartbeat` 20s. `login`/`pair` run in the background with no timeout, since pairing legitimately takes a while — but nothing kills a script that hangs forever either.
- **Pipeline hooks fail soft.** If `url`, `downloadmanifest`, `pssh` or `initparse` errors, times out, or prints nothing, ReStreamAir logs it and carries on with its own built-in behaviour. A broken hook degrades; it doesn't take the stream down.
- **Passwords live in `state.json`**, not just handed to the script and forgotten — treat that file like any credentials file. The same goes for an exported provider, which embeds accounts *and* the script's source.
