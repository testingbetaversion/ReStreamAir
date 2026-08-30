# Provider scripts

A provider script is an optional program that handles source-specific work ReStreamAir cannot do from a fixed URL alone.

Use one when a provider needs custom login, device pairing, channel discovery, expiring manifest URLs, heartbeats, or an external key workflow.

If you only want to start, stop, create, or inspect streams from another application, skip to [Control ReStreamAir over HTTP](#control-restreamair-over-http). That uses the management API and does not require a provider script.

## Five-minute example

This script implements `login`. It reads the arguments ReStreamAir supplies and saves a pretend token in the provider's private session directory.

Create `provider.py`:

```python
#!/usr/bin/env python3
import base64
import json
import os
import sys

def arg(name, default=""):
    for item in sys.argv[1:]:
        key, _, raw = item.partition("=")
        if key != name:
            continue
        if raw.startswith("b64:"):
            return base64.b64decode(raw[4:]).decode("utf-8")
        return raw
    return default

action = arg("action")
session_dir = arg("sessiondir", ".")

if action == "login":
    os.makedirs(session_dir, exist_ok=True)
    with open(os.path.join(session_dir, "session.json"), "w") as file:
        json.dump({"user": arg("user"), "token": "example-token"}, file)
    print("Login saved")
else:
    print(f"Unsupported action: {action}", file=sys.stderr)
    raise SystemExit(1)
```

Configure it:

1. Open **Provider settings**.
2. Set **Script path** to the full path of `provider.py`.
3. Add an account.
4. Enable the **Login** script action.
5. Save and press **Login**.

The panel displays anything written to stdout or stderr.

## The basic contract

ReStreamAir starts the script as a child process:

| File | Command used |
|---|---|
| `script.py` | `python3 script.py ...` |
| `script.sh` or `script.bash` | `/bin/sh script.sh ...` |
| Any other file | Executed directly; it needs a shebang and executable permission. |

Arguments are flat `key=value` tokens, not `--flags`:

```text
action=login
sessiondir=runtime/sessions/provider_...
cookies=runtime/sessions/provider_.../cookies.txt
user=someone@example.com
password=b64:...
```

The common arguments are:

| Argument | Meaning |
|---|---|
| `action` | What ReStreamAir wants the script to do. |
| `sessiondir` | Durable directory for this provider's cookies and tokens. |
| `cookies` | Suggested cookie-jar path inside `sessiondir`. |
| `user`, `password` | Selected provider account, when non-empty. |
| `bind`, `proxy`, `doh`, `worker` | Optional provider settings, present only when configured. |

Action-specific arguments may follow. A stream test adds the stream's `id` and source `url`.

Do not depend on argument order. Parse by key.

## Decoding values

Passwords and values containing whitespace, quotes, backslashes, control characters, or non-ASCII text are encoded as `b64:<base64>`.

Always decode marked values:

```python
import base64

def decode(raw):
    if raw.startswith("b64:"):
        return base64.b64decode(raw[4:]).decode("utf-8")
    return raw
```

Ordinary URLs remain plain because ReStreamAir runs the process directly, without a shell. Characters such as `?`, `&`, `;`, `|`, and `*` are not interpreted.

## Session storage

Each provider gets:

```text
runtime/sessions/<provider-id>/
```

Put cookies, access tokens, pairing results, or other reusable state there. The directory survives server restarts.

ReStreamAir does not inspect its contents. **Clear session** recursively deletes the entire directory, and deleting the provider does the same. The next script invocation recreates it.

Typical setup:

```python
session_dir = arg("sessiondir", ".")
cookie_file = arg("cookies", os.path.join(session_dir, "cookies.txt"))
session_file = os.path.join(session_dir, "session.json")
```

## Actions

Enable only the actions your script implements. A stream inherits its provider's selection and can override it.

### Account actions

| Action | Use | Output |
|---|---|---|
| `login` | Sign in and save a session. | Progress text. Exit 0 on success. |
| `pair` | Complete a device-code or pairing flow. | Print the code and progress. Exit 0 on success. |

### Catalogue actions

| Action | Use | Output |
|---|---|---|
| `channels` | List channels to import. | `{"Channels":[...]}` |
| `events` | List scheduled events to import. | `{"Events":[...]}` |
| `epg` | Fetch guide data. | XMLTV or JSON stored verbatim. |

A small channel response:

```python
print(json.dumps({
    "Channels": [
        {
            "Name": "News",
            "Mode": "live",
            "ScriptParams": "id=101",
            "SessionManifest": True,
            "UseCdm": False,
            "Autostart": False
        }
    ]
}))
```

Re-running an import matches entries by name and updates them instead of creating duplicates. Imported entries normally need either a source URL entered later or `SessionManifest: true` with a working `manifest` action.

Events use the same basic shape under `Events` and may include `Start`, `End`, and `RecordEvent`.

### Session and lifecycle actions

| Action | Use | Output |
|---|---|---|
| `manifest` | Return a fresh source URL, CDN mirrors, headers, and heartbeat settings. | JSON shown below. |
| `start` | Claim or prepare a source-side stream session. | Exit 0. |
| `stop` | Release the source-side session. | Exit 0. |
| `heartbeat` | Keep the source-side session alive. | Exit 0. |

Example `manifest` response:

```python
channel_id = arg("id")

print(json.dumps({
    "ManifestUrl": f"https://cdn.example/live/{channel_id}.mpd",
    "Cdn": [
        {"Name": "backup", "ManifestUrl": f"https://backup.example/live/{channel_id}.mpd"}
    ],
    "Headers": {
        "manifest": {"Authorization": "Bearer example"},
        "media": {"User-Agent": "ReStreamAir provider script"}
    },
    "Heartbeat": {
        "PeriodMs": 300000
    }
}))
```

`manifest` is also the recovery hook for an expired source URL. When the current URL and its mirrors fail, ReStreamAir can ask the script for a fresh one and retry.

### Pipeline and key actions

| Action | Important inputs | Expected output |
|---|---|---|
| `url` | `url` | Replacement URL or `{"Url":"..."}`. Empty output keeps the original. |
| `downloadmanifest` | `url` | Raw manifest text or `{"ManifestContent":"..."}`. |
| `pssh` | `pssh`, `url` | Replacement PSSH or `{"ProcessedPssh":"..."}`. |
| `initparse` | `url`, base64 `init` | JSON containing any discovered KID/PSSH values. |
| `cdm` | KIDs, PSSH values, key URI, CDM type | Clear keys as `KID:KEY` lines or JSON. |

Example key output:

```text
c3d43de9ff5b5a45cdc9f4e7f177a1a5:11223344556677889900aabbccddeeff
```

or:

```json
{"keys":[{"kid":"c3d43de9ff5b5a45cdc9f4e7f177a1a5","key":"11223344556677889900aabbccddeeff"}]}
```

ReStreamAir has no embedded Widevine, PlayReady, or FairPlay CDM. The script must perform its own authorized external workflow and return clear keys. `challenge=` is empty.

`downloadinit` and `downloadmedia` are reserved/configurable actions but are not part of the normal per-segment pipeline today.

## Output and errors

- Put machine-readable JSON or keys on stdout.
- Put debugging and errors on stderr.
- Exit `0` for success and nonzero for failure.
- Flush progress lines when the user needs to see them immediately.

```python
print("Waiting for device pairing...", flush=True)
print("Provider rejected the token", file=sys.stderr, flush=True)
```

The panel combines stdout and stderr for display while structured logs retain their severity.

Pipeline hooks fail soft: an empty, failed, or timed-out hook is logged and ReStreamAir falls back to its built-in behavior where possible.

## Test from a terminal

Run the script with the same style of arguments ReStreamAir uses:

```bash
python3 provider.py \
  action=login \
  sessiondir=/tmp/restreamair-session \
  cookies=/tmp/restreamair-session/cookies.txt \
  user=you@example.com \
  password=b64:aHVudGVyMg==
```

Test a manifest action:

```bash
python3 provider.py \
  action=manifest \
  sessiondir=/tmp/restreamair-session \
  cookies=/tmp/restreamair-session/cookies.txt \
  id=101
```

For DRM parsing and an optional `cdm` script:

```bash
./restreamair cdmprobe mpd=<url> script=<path>
```

## Control ReStreamAir over HTTP

The panel's server-side controls use the same HTTP API available to automation.

Use a panel admin account:

```bash
base=http://127.0.0.1:8787
auth='admin:your-panel-password'
```

List IDs:

```bash
curl --fail-with-body --user "$auth" "$base/api/state"
```

Start and stop a stream:

```bash
stream_id=stream_...
curl --fail-with-body --user "$auth" -X POST "$base/api/streams/$stream_id/start"
curl --fail-with-body --user "$auth" -X POST "$base/api/streams/$stream_id/stop"
```

Run a provider action:

```bash
provider_id=provider_...
curl --fail-with-body --user "$auth" -X POST \
  "$base/api/providers/$provider_id/script/login"
```

The completed response includes recent log `entries`, combined script `output`, and `exitCode`.

Follow live script logs:

```bash
curl --fail-with-body --user "$auth" \
  "$base/api/logs?streamId=script:$provider_id&limit=500"
```

Test a hook against one configured stream:

```bash
curl --fail-with-body --user "$auth" \
  -H 'Content-Type: application/json' \
  -d "{\"action\":\"pssh\",\"streamId\":\"$stream_id\"}" \
  "$base/api/providers/$provider_id/script/run"
```

This direct test supplies the stream's ID and source URL. It does not start the stream or recreate the full live pipeline context.

Clear the provider's saved script session:

```bash
curl --fail-with-body --user "$auth" -X POST \
  "$base/api/providers/$provider_id/script/clear-session"
```

Playback API keys cannot call management routes. See [API.md](API.md) for the complete HTTP API.

## Safety and practical notes

- Provider scripts are not sandboxed. They run as the same operating-system user as ReStreamAir.
- Account passwords are stored in `state.json` because the real value must be passed to the script. Protect that file and provider exports.
- Avoid shelling out with secrets in command text. ReStreamAir already launches the configured script directly.
- Keep action handlers idempotent where possible. Login, channel import, manifest refresh, and heartbeat may be retried.
- Store only provider-owned state in `sessiondir`; **Clear session** deletes everything below it.
