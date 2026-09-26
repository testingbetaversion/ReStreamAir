#!/usr/bin/env python3
"""Example ReStreamAir provider script.

Implements every action ReStreamAir can call, against a pretend provider at
example.com. Copy it, then replace the bodies of the handlers with real
requests. See SCRIPTING.md for the full contract.

Try it from a terminal:

    python3 provider.py action=login sessiondir=/tmp/rs user=me password=b64:aHVudGVyMg==
    python3 provider.py action=channels sessiondir=/tmp/rs
    python3 provider.py action=manifest sessiondir=/tmp/rs id=stream_x url= channel=101
"""
import base64
import json
import os
import sys
import time

# --- arguments ----------------------------------------------------------------

def decode(raw):
    if raw.startswith("b64:"):
        return base64.b64decode(raw[4:]).decode("utf-8")
    return raw

ARGS = {}
for token in sys.argv[1:]:
    key, _, raw = token.partition("=")
    # Keep the first value: built-in arguments (id, url) come before the
    # stream's script params, so a param can never shadow them.
    ARGS.setdefault(key, decode(raw))

def arg(name, default=""):
    return ARGS.get(name, default) or default

def log(message):
    """Progress for the panel. stderr, so stdout stays machine-readable."""
    print(message, file=sys.stderr, flush=True)

def fail(message):
    log(message)
    raise SystemExit(1)

def reply(document):
    """The one JSON document ReStreamAir reads from stdout."""
    print(json.dumps(document), flush=True)

# --- session ------------------------------------------------------------------

SESSION_DIR = arg("sessiondir", ".")
SESSION_FILE = os.path.join(SESSION_DIR, "session.json")

def load_session():
    try:
        with open(SESSION_FILE) as file:
            return json.load(file)
    except (OSError, ValueError):
        return {}

def save_session(session):
    os.makedirs(SESSION_DIR, exist_ok=True)
    with open(SESSION_FILE, "w") as file:
        json.dump(session, file)

def require_token():
    token = load_session().get("token")
    if not token:
        fail("Not logged in - run the Login action first")
    return token

# --- account actions ----------------------------------------------------------

def action_login():
    user, password = arg("user"), arg("password")
    if not user or not password:
        fail("Select an account with a username and password in Provider settings")
    log(f"Signing in as {user}...")
    # Real script: POST the credentials, going through arg("proxy") if set.
    save_session({"user": user, "token": "token-for-" + user, "at": int(time.time())})
    log("Login saved")

def action_pair():
    log("Open https://example.com/activate and enter code ABCD-1234")
    # Real script: poll the provider until the code is approved.
    save_session({"token": "paired-token", "at": int(time.time())})
    log("Device paired")

# --- catalogue actions --------------------------------------------------------

def action_channels():
    require_token()
    reply({"Channels": [
        {"Name": "Example News", "ScriptParams": "channel=101",
         "SessionManifest": True, "UseCdm": False},
        {"Name": "Example Sport", "ScriptParams": "channel=202",
         "SessionManifest": True, "UseCdm": True, "CdmType": "widevine"},
    ]})

def action_events():
    require_token()
    start = int(time.time()) + 3600
    reply({"Events": [
        {"Name": "Cup Final", "ScriptParams": "event=9001",
         "SessionManifest": True, "Start": start, "End": start + 7200,
         "RecordEvent": False},
    ]})

def action_epg():
    require_token()
    print('<?xml version="1.0" encoding="UTF-8"?>\n'
          '<tv><channel id="101"><display-name>Example News</display-name></channel></tv>')

# --- session and lifecycle actions --------------------------------------------

def action_manifest():
    token = require_token()
    channel = arg("channel") or arg("event")
    if not channel:
        fail("This stream has no channel= or event= script param")
    reply({
        "ManifestUrl": f"https://cdn1.example.com/live/{channel}/index.mpd?token={token}",
        "Cdn": [{"Name": "backup",
                 "ManifestUrl": f"https://cdn2.example.com/live/{channel}/index.mpd?token={token}"}],
        "Headers": {
            "manifest": {"Authorization": f"Bearer {token}"},
            "media": {"Referer": "https://example.com/"},
        },
        "Heartbeat": {"PeriodMs": 300000},
    })

def action_start():
    require_token()
    log(f"Claimed a playback slot for {arg('channel') or arg('id')}")

def action_stop():
    log(f"Released the playback slot for {arg('channel') or arg('id')}")

def action_heartbeat():
    require_token()
    log("Session still alive")

# --- pipeline and key actions -------------------------------------------------

def action_url():
    url = arg("url")
    if "token=" in url:
        print("")  # empty output keeps the original URL
    else:
        reply({"Url": url + ("&" if "?" in url else "?") + "token=" + require_token()})

def action_downloadmanifest():
    # Real script: fetch arg("url") with whatever the provider insists on.
    reply({"ManifestContent": "#EXTM3U\n#EXT-X-TARGETDURATION:6\n"})

def action_pssh():
    # Return nothing to keep the box ReStreamAir found, or a replacement box
    # (base64 of a complete `pssh` box) to license against instead.
    print("")

def action_initparse():
    init = base64.b64decode(arg("init"))   # plain base64 of the init segment
    kids = []
    i = init.find(b"tenc")
    if i >= 0 and len(init) >= i + 28:
        # tenc: 4 bytes version/flags, 4 bytes pattern/isProtected/IV size, then the KID
        kids.append(init[i + 12:i + 28].hex())
    reply({"kids": kids})

def action_cdm():
    kids = [k for k in arg("kid").split(",") if k]
    pssh = arg("pssh")
    log(f"cdmType={arg('cdmType')} kids={kids} pssh={'yes' if pssh else 'no'}")
    if not kids and not pssh:
        fail("No KID or PSSH to license")
    # Real script: send a licence request through your own authorised CDM
    # workflow and collect the clear keys it returns.
    keys = [{"kid": kid, "key": "00112233445566778899aabbccddeeff"} for kid in kids]
    reply({"keys": keys})

# --- dispatch -----------------------------------------------------------------

ACTIONS = {name[len("action_"):]: fn for name, fn in globals().items()
           if name.startswith("action_")}

if __name__ == "__main__":
    handler = ACTIONS.get(arg("action"))
    if not handler:
        fail(f"Unsupported action: {arg('action')!r}")
    handler()
