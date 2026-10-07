# Provider scripts

A provider script is an optional program that handles source-specific work ReStreamAir cannot do from a fixed URL alone.

Use one when a provider needs custom login, device pairing, channel discovery, expiring manifest URLs, heartbeats, or an external key workflow.

If you only want to start, stop, create, or inspect streams from another application, skip to [Control ReStreamAir over HTTP](#control-restreamair-over-http). That uses the management API and does not require a provider script.

For a complete starting point, copy [`scripts/providers/example/provider.py`](scripts/providers/example/provider.py). It implements every action against a pretend provider, and the [worked examples](#worked-examples) below show what each action receives and prints.

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

| File | Linux / macOS | Windows |
|---|---|---|
| `script.py` | `python3 -u script.py ...` | `python -u script.py ...` |
| `script.sh` or `script.bash` | `/bin/sh script.sh ...` | `sh script.sh ...` (Git for Windows or WSL) |
| `script.bat` or `script.cmd` | — | `cmd.exe /c script.bat ...` |
| `script.ps1` | — | `powershell.exe -NoProfile -ExecutionPolicy Bypass -File script.ps1 ...` |
| Any other file | Executed directly; it needs a shebang and executable permission. | Executed directly. |

A `.sh` script is run by `/bin/sh` even when its shebang names `bash`, so avoid bash-only syntax or rename it and make it executable.

Each action is limited by the provider's **Script timeout** option (default 30 seconds, up to 3600). A script that runs longer is killed and the action fails. Whatever it printed before then is still shown.

A stream can use a different script from its provider via its **Script override** setting. Playback and the stream-start actions run the override.

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
| `user`, `password` | The provider's active account, each only when non-empty. |
| `bind`, `doh`, `worker` | The provider's script network settings, present only when configured. |
| `proxy` | The first proxy in effect. For a stream action it is present only when the stream's **Use proxy for: Script** box is ticked. |

Any action run for a stream (every playback step, and a panel or API test with a `streamId`) also gets:

| Argument | Meaning |
|---|---|
| `id` | The ReStreamAir stream ID, e.g. `stream_...`. |
| `url` | The stream's source URL. May be empty for a session-manifest stream. |
| script params | The stream's stored `ScriptParams` tokens, appended last. |

Action-specific arguments such as `pssh=` or `kid=` follow.

Do not depend on argument order. Parse by key. Script params come after the built-in arguments, so avoid giving them the names `id` or `url`: a script that takes the first match would read the built-in value. The examples here use `channel=` for that reason.

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

Enable only the actions your script implements. **An action that isn't ticked is never invoked** — not by playback, and not by the panel's run buttons, which answer `400` and say which box to tick. A stream inherits its provider's selection and can override it; an empty override keeps the script away from that one stream entirely. `downloadinit` and `downloadmedia` can be ticked but are never called yet.

### Account actions

| Action | Use | Output |
|---|---|---|
| `login` | Sign in and save a session. With the provider option **Always reset session** on, the session directory is cleared first, but only while none of the provider's streams are running. | Progress text. Exit 0 on success. |
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
            "ScriptParams": "channel=101",
            "SessionManifest": True,
            "UseCdm": False,
            "Autostart": False
        }
    ]
}))
```

`ManifestScript` (or `manifestScript`) is accepted as a legacy alias for
`ScriptParams`. Its space-separated `key=value` tokens are saved on the imported
stream and passed to stream actions such as `manifest` and `cdm`. A value that
needs it is `b64:`-encoded on the way through, the same as any other argument.

Re-running an import matches entries by name and updates them instead of creating duplicates. Imported entries normally need either a source URL entered later or `SessionManifest: true` with a working `manifest` action.

Events use the same basic shape under `Events` and may include `Start`, `End`, and `RecordEvent`. Start/End are Unix epoch seconds. Matching uses name plus source type within the provider; event and channel entries of the same name remain distinct. These are imported metadata, not an automatic start/stop or recording schedule. See [EVENTS.md](EVENTS.md#scheduled-provider-events) for every field and an external scheduler workflow.

### Session and lifecycle actions

| Action | Use | Output |
|---|---|---|
| `manifest` | Return a fresh source URL, CDN mirrors, headers, and heartbeat settings. | JSON shown below. |
| `start` | Claim or prepare a source-side stream session. | Exit 0. |
| `stop` | Release the source-side session. | Exit 0. |
| `heartbeat` | Keep the source-side session alive. | Exit 0. |

Example `manifest` response:

```python
channel_id = arg("channel")   # from ScriptParams; arg("id") is the stream ID

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

`manifest` runs on start of a stream that has **Session manifest** ticked, before anything reads the stream's source URL — unless the stream already has a saved session URL that still answers with a manifest, in which case that session is reused and `manifest` is skipped (the provider option **Always refresh session manifest** turns the reuse off). Its `ManifestUrl`, `Cdn` mirrors, `Headers` and `Heartbeat.PeriodMs` are stored on the stream, so the panel shows what the stream is really playing and every pipeline reads the live session URL. A start whose `manifest` action fails is refused rather than begun against a stale URL.

When `ManifestUrl` is empty but `Cdn` contains usable URLs, the first CDN URL
becomes the primary source and the remaining entries stay available as mirrors.

`manifest` is also the recovery hook. When a playlist fetch or source probe fails on the primary URL and on every CDN mirror, a stream with **Session manifest** ticked runs `manifest` once and retries the fresh sources. The fresh session is saved even if the player that triggered it has disconnected. There is a 60-second cooldown per stream, and the logs show `cdnFallback` and `manifestRefresh` events. A `.mpd` stream restarted by the provider's restart policy runs `manifest` again as part of that start.

The server stores the heartbeat interval but does not run a periodic heartbeat scheduler. Playback never calls `start`, `stop` or `heartbeat` either. Call declared `start`, `stop`, or `heartbeat` actions explicitly through the script API when your integration needs them. These are independent of the monitoring SSE keepalive.

### Pipeline and key actions

| Action | Important inputs | Expected output |
|---|---|---|
| `url` | `url` | Replacement URL or `{"Url":"..."}`. Empty output keeps the original. Run from the panel or API only; playback does not call it yet. |
| `downloadmanifest` | `url` | Raw manifest text or `{"ManifestContent":"..."}`. Run from the panel or API only; playback does not call it yet. |
| `pssh` | `pssh`, `url` | Replacement PSSH or `{"ProcessedPssh":"..."}`. Empty output keeps the original. |
| `initparse` | `url`, `init` (plain base64 of the init segment, no `b64:` prefix) | JSON with any of `kid`/`kids` and `pssh`/`psshAll`/`psshWidevine`/`psshPlayReady`, each a string or an array. |
| `cdm` | KIDs, PSSH values, key URI, CDM type | Clear keys as `KID:KEY` lines or JSON. |

DRM is detected automatically, so there is nothing to tick per stream. On every start of a stream whose script declares `cdm`, ReStreamAir first runs `manifest` (if the stream uses a session manifest), then searches the fresh manifest, its first HLS media playlist and the init segment for every KID, PSSH box and HLS key URI. If the stored clear keys already cover every discovered KID, they are reused and `cdm` is skipped. A missing or changed KID—or DRM input with no identifiable KID—runs `cdm` and passes `kid=`, `pssh=`, `psshAll=`, `psshWidevine=`, `psshPlayReady=` and `keyUri=`, along with `cdm=external`, the stream's `cdmType=` and its script params. The returned pairs replace the active decryption keys.

If `cdm` exits non-zero or returns no usable pairs, the start keeps trying before giving up: the configured `cdmType=`, then each other DRM system the manifest carries a PSSH for (`widevine`, `playready`), first against the session URL and then against every `Cdn` mirror in turn (a mirror whose manifest cannot be fetched is skipped). The first attempt that yields keys wins. Only when every attempt fails is the start refused, with the number of `cdm` attempts and CDNs in the error. A `manifest` result that succeeded is kept on the stream even then, so the panel shows the session URL and its mirrors. If no manifest could be fetched at all, nothing was licensed and the start proceeds on the stored keys.

For Widevine/PlayReady HLS, the rewritten playlist removes the DRM key tag and routes its fMP4 init and media fragments through server-side CENC decryption.

PSSH is looked for in three places, in order:

1. the manifest — `cenc:pssh` in an MPD, a Widevine `EXT-X-KEY` in a playlist;
2. the initialization segment, fetched when the manifest carried no box, and scanned for `pssh` boxes and `tenc` default KIDs;
3. built from the KIDs, when the source only ever names one — a version 0 Widevine box whose `WidevinePsshData` carries the key ids, the same box `pywidevine`'s `PSSH.new(key_ids=…)` round-trips.

If none of those places has any DRM (no PSSH, KID or key URI), the source is clear: `cdm` is not run and the stream starts normally. The `UseCdm` field in channel/event output is accepted but no longer needed.

No `pywidevine` (or any other Python) is involved: the box is assembled in C. ReStreamAir still has no CDM of its own — the script performs the licence exchange and returns clear keys.

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

Optional `pssh` and `initparse` hooks fail soft and fall back to the built-in parser. A failed `manifest` or `cdm` action stops the start, because an expired source URL or missing clear key cannot produce playable output.

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
  id=stream_test \
  url= \
  channel=101
```

To test DRM parsing and a `cdm` action with real stream context, add the stream in the panel and use the script action buttons in its **Scripting & DRM** section, or call the `script/run` route described below. The server records the exact command line it ran, under the `scriptCommand` log event, so you can copy it into a terminal.

## Worked examples

Each example below runs [`scripts/providers/example/provider.py`](scripts/providers/example/provider.py) from a terminal with the arguments ReStreamAir would pass, followed by what the script prints. Lines ReStreamAir parses go to stdout. Progress lines go to stderr, and the panel shows both.

Set up a scratch session directory first:

```bash
cd scripts/providers/example
S=/tmp/rs-example
```

### Reading arguments

The example parses every argument once into a dictionary:

```python
ARGS = {}
for token in sys.argv[1:]:
    key, _, raw = token.partition("=")
    # Keep the first value: built-in arguments (id, url) come before the
    # stream's script params, so a param can never shadow them.
    ARGS.setdefault(key, decode(raw))

def arg(name, default=""):
    return ARGS.get(name, default) or default
```

It prints results with `print(json.dumps(...))`, progress with `print(..., file=sys.stderr)`, and exits nonzero on failure.

### `login`

The panel passes the active account. The password always arrives `b64:`-encoded.

```bash
python3 provider.py action=login sessiondir=$S cookies=$S/cookies.txt \
  user=me@example.com password=b64:aHVudGVyMg==
```

```text
Signing in as me@example.com...
Login saved
```

The script writes `$S/session.json` and exits 0. Later actions read the token from there. Without a login, they fail:

```bash
python3 provider.py action=manifest sessiondir=/tmp/empty channel=101; echo "exit=$?"
```

```text
Not logged in - run the Login action first
exit=1
```

### `pair`

Print the code the user has to enter, then wait for approval. Flush as you go, so the code shows in the panel while the script is still running. In the example, pairing replaces the session saved by `login`.

```bash
python3 provider.py action=pair sessiondir=$S
```

```text
Open https://example.com/activate and enter code ABCD-1234
Device paired
```

### `channels`

```bash
python3 provider.py action=channels sessiondir=$S
```

```json
{"Channels": [
  {"Name": "Example News", "ScriptParams": "channel=101", "SessionManifest": true, "UseCdm": false},
  {"Name": "Example Sport", "ScriptParams": "channel=202", "SessionManifest": true, "UseCdm": true, "CdmType": "widevine"}
]}
```

This imports two streams, or updates them if they already exist. Each one keeps `channel=...` as its script params, and gets its URL from `manifest` at start. **Example Sport** also gets its keys from `cdm`.

Channel and event entries accept these fields: `Name` (required), `Mode`, `ScriptParams`, `SessionManifest`, `UseCdm`, `CdmType`, `OnDemand`, `SpeedUp`, `Autostart` and `RecordEvent`. Events also accept `Start` and `End`. Lower-camel-case spellings such as `sessionManifest` are accepted too. An import never sets a source URL.

### `events`

```bash
python3 provider.py action=events sessiondir=$S
```

```json
{"Events": [
  {"Name": "Cup Final", "ScriptParams": "event=9001", "SessionManifest": true,
   "Start": 1790439091, "End": 1790446291, "RecordEvent": false}
]}
```

`Start` and `End` are Unix epoch seconds. They are stored for display and for your own scheduler, and they don't start anything by themselves.

### `epg`

Print XMLTV (or JSON). ReStreamAir stores it verbatim.

```bash
python3 provider.py action=epg sessiondir=$S
```

```xml
<?xml version="1.0" encoding="UTF-8"?>
<tv><channel id="101"><display-name>Example News</display-name></channel></tv>
```

### `manifest`

On start, ReStreamAir passes the stream ID, its current URL (empty for an imported stream) and its script params:

```bash
python3 provider.py action=manifest sessiondir=$S cookies=$S/cookies.txt \
  id=stream_abc url= channel=101
```

```json
{
  "ManifestUrl": "https://cdn1.example.com/live/101/index.mpd?token=token-for-me@example.com",
  "Cdn": [{"Name": "backup", "ManifestUrl": "https://cdn2.example.com/live/101/index.mpd?token=token-for-me@example.com"}],
  "Headers": {
    "manifest": {"Authorization": "Bearer token-for-me@example.com"},
    "media": {"Referer": "https://example.com/"}
  },
  "Heartbeat": {"PeriodMs": 300000}
}
```

`Headers.manifest` is sent with manifest requests and `Headers.media` with segment requests. A `Cdn` entry may carry its own `Headers` block in the same shape.

A stream with no `channel=` param cannot be resolved, so the script fails and the start is refused:

```text
This stream has no channel= or event= script param
```

### `start`, `stop`, `heartbeat`

These only need an exit code. Anything printed is just shown in the log.

```bash
python3 provider.py action=start sessiondir=$S id=stream_abc channel=101
python3 provider.py action=heartbeat sessiondir=$S
python3 provider.py action=stop sessiondir=$S id=stream_abc channel=101
```

```text
Claimed a playback slot for 101
Session still alive
Released the playback slot for 101
```

### `url`

Print a replacement URL, or nothing to keep the original:

```bash
python3 provider.py action=url sessiondir=$S url=https://cdn1.example.com/live/101/index.mpd
```

```json
{"Url": "https://cdn1.example.com/live/101/index.mpd?token=token-for-me@example.com"}
```

### `downloadmanifest`

```bash
python3 provider.py action=downloadmanifest sessiondir=$S url=https://cdn1.example.com/live/101/index.m3u8
```

```json
{"ManifestContent": "#EXTM3U\n#EXT-X-TARGETDURATION:6\n"}
```

Printing the raw manifest text instead of JSON works too.

### `pssh`

ReStreamAir passes the PSSH box it found as plain base64. Print nothing to keep it, or print a replacement box:

```bash
python3 provider.py action=pssh sessiondir=$S pssh=AAAAW3Bzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAADsIARIQ... url=https://...
```

No output means the original box is kept. A replacement is used only if it is a complete, valid `pssh` box. Otherwise the original is kept.

### `initparse`

`init` is the whole init segment as plain base64. Return any KIDs or PSSH boxes you find:

```python
def action_initparse():
    init = base64.b64decode(arg("init"))
    kids = []
    i = init.find(b"tenc")
    if i >= 0 and len(init) >= i + 28:
        kids.append(init[i + 12:i + 28].hex())   # tenc default KID
    reply({"kids": kids})
```

```json
{"kids": ["c3d43de9ff5b5a45cdc9f4e7f177a1a5"]}
```

These are added to what the built-in parser already found.

### `cdm`

At start, a `cdm` call for a Widevine stream looks like this (the long values are shortened here):

```bash
python3 provider.py action=cdm sessiondir=$S cookies=$S/cookies.txt \
  id=stream_abc url=https://cdn1.example.com/live/202/index.mpd channel=202 \
  cdm=external challenge= cdmType=widevine \
  kid=c3d43de9ff5b5a45cdc9f4e7f177a1a5 \
  pssh=AAAAW3Bzc2gAAAAA7e+L... psshAll=AAAAW3Bzc2gAAAAA7e+L... \
  psshWidevine=AAAAW3Bzc2gAAAAA7e+L...
```

```text
cdmType=widevine kids=['c3d43de9ff5b5a45cdc9f4e7f177a1a5'] pssh=yes
{"keys": [{"kid": "c3d43de9ff5b5a45cdc9f4e7f177a1a5", "key": "00112233445566778899aabbccddeeff"}]}
```

`kid`, `psshAll` and `keyUri` are comma-separated lists when there is more than one. The progress line goes to stderr, so only the JSON is parsed. Printing `c3d43de9ff5b5a45cdc9f4e7f177a1a5:00112233445566778899aabbccddeeff`, one pair per line, works as well.

### A shell script

A script doesn't have to be Python. This `provider.sh` implements `login` and `manifest` with `curl` and `jq`:

```sh
#!/bin/sh
# Collect key=value arguments into shell variables prefixed arg_.
for token in "$@"; do
  key=${token%%=*}; value=${token#*=}
  case $value in
    b64:*) value=$(printf %s "${value#b64:}" | base64 -d) ;;
  esac
  case $key in
    action|sessiondir|user|password|proxy|channel) eval "arg_$key=\$value" ;;
  esac
done

session="$arg_sessiondir/session.json"
proxy_opt=${arg_proxy:+--proxy "$arg_proxy"}

case $arg_action in
  login)
    mkdir -p "$arg_sessiondir"
    curl -fsS $proxy_opt -d "user=$arg_user" --data-urlencode "password=$arg_password" \
      https://api.example.com/login > "$session" || { echo "Login failed" >&2; exit 1; }
    echo "Login saved" >&2
    ;;
  manifest)
    token=$(jq -r .token "$session") || { echo "Not logged in" >&2; exit 1; }
    curl -fsS $proxy_opt -H "Authorization: Bearer $token" \
      "https://api.example.com/channels/$arg_channel/play" |
      jq --arg auth "Bearer $token" '{ManifestUrl: .url, Headers: {manifest: {Authorization: $auth}}}'
    ;;
  *)
    echo "Unsupported action: $arg_action" >&2; exit 1 ;;
esac
```

It runs under `/bin/sh`, so it sticks to POSIX shell. `eval` is safe here because it only ever assigns to names from a fixed list.

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

The completed response includes recent log `entries`, combined script `output`, and `exitCode`. Check `exitCode` as well as the HTTP status, because a failed script still returns `200`:

```bash
curl -s --user "$auth" -X POST "$base/api/providers/$provider_id/script/login" | jq '{exitCode, output}'
```

```json
{"exitCode": 0, "output": "Signing in as me@example.com...\nLogin saved\n"}
```

Import the provider's channels, then list the streams it created:

```bash
curl --fail-with-body --user "$auth" -X POST "$base/api/providers/$provider_id/script/channels"
curl -s --user "$auth" "$base/api/state" |
  jq --arg p "$provider_id" '.providers[] | select(.id == $p) | .streams[] | {id, name, scriptParams}'
```

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

Or ask one stream for a fresh session manifest, with the same arguments a start would pass:

```bash
curl --fail-with-body --user "$auth" \
  -H 'Content-Type: application/json' \
  -d "{\"action\":\"manifest\",\"streamId\":\"$stream_id\"}" \
  "$base/api/providers/$provider_id/script/run"
```

This direct test supplies the stream's ID, source URL and script params (plus `cdm=external` and an empty `challenge=` for `cdm`). It does not start the stream or recreate the full live pipeline context.

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
