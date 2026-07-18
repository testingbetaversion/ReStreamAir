# ReStreamAir

A local DASH → HLS restreaming toolkit written in Swift, with a web control panel. One binary (plus an optional macOS menu bar companion), built with plain `swiftc` (no Package.swift/Xcode project):

- `restreamair` — everything: the web panel/server (default), and the DASH-inspection/live-restream CLI tools as `dash`/`live` subcommands.
- `restreamair-menubar` — optional macOS-only menu bar companion that launches/monitors `restreamair serve`.

## Install

The quickest path is a prebuilt binary — no toolchain needed. Grab the one for your platform from the [latest release](https://github.com/testingbetaversion/ReStreamAir/releases/latest):

| Platform | Asset |
|----------|-------|
| macOS (Apple Silicon) | `restreamair` |
| Linux x86-64 | `restreamair-linux` |
| Linux arm64 | `restreamair-linux-arm64` |

```sh
chmod +x restreamair          # (or restreamair-linux / restreamair-linux-arm64)
./restreamair                 # starts the web panel — same as ./restreamair serve
```

The Swift runtime is statically linked in, so no toolchain is needed. The Linux binaries do need two common system libraries that Foundation uses — `libcurl4` (HTTP) and `libxml2` (XML). They're already present on most systems; if you hit `error while loading shared libraries: libcurl.so.4`, install them:

```sh
sudo apt-get update && sudo apt-get install -y libcurl4 libxml2   # Debian/Ubuntu
```

## Building from source

**Prerequisites**: a Swift 5.9+ toolchain.

- macOS: Xcode or the Xcode Command Line Tools (`xcode-select --install`) already provide `swiftc`.
- Linux: install a Swift toolchain from [swift.org/install](https://www.swift.org/install/) (or via `swiftly`). No other dependencies — this project has zero external packages, so there's nothing to fetch.

Compile every `.swift` source into one binary — a single `swiftc` invocation, no Package.swift/Xcode project (`MenuBarApp.swift` is the separate, macOS-only menu-bar target, so it's excluded here):

```sh
git clone <this repo> ReStreamAir
cd ReStreamAir
swiftc $(ls *.swift | grep -v MenuBarApp.swift) -o restreamair
./restreamair
```

(The `swift` interpreter can't run this project directly — it ignores the `@main` entry point in immediate mode, and the panel re-execs *itself* to spawn its `dash`/`live` worker subprocesses, which needs a real compiled executable. So it must be built with `swiftc`, not run with `swift`.)

`dash`/`live` are the panel's other two subcommands — the underlying CLI tools it spawns for you; you don't normally run them directly.

Open `http://127.0.0.1:8787` once it's running. The very first launch (nothing in `data/state.json` yet) serves a **Create admin account** screen instead of the normal UI — pick a username/password there, then sign in with them. Nothing else works until that account exists. Re-running later reuses whatever's already in `data/state.json` (providers, streams, that admin account, ...) — it's not reset on every run.

Common overrides, all optional (fuller reference in [Provider defaults & per-stream network overrides](#provider-defaults--per-stream-network-overrides) and the sections after it):

```sh
./restreamair serve --port 9000                              # different port
./restreamair serve --bind 127.0.0.1                          # only accept local connections
./restreamair serve --admin-username admin --admin-password "a-new-password"  # reset/recover the admin account
```

To run it in the background instead of tying up a terminal: `nohup ./restreamair > restreamair.log 2>&1 & disown`, or see [Launching without a terminal window](#launching-without-a-terminal-window) below for a proper macOS `.app`/menu-bar setup.

### Build for macOS

The command above targets macOS fine as-is (`restreamair` itself is AppKit-free and portable). The optional menu bar companion is a separate, macOS-only target:

```sh
swiftc HTTPClient.swift MenuBarApp.swift -o restreamair-menubar -framework AppKit
```

### Build for Linux

Same single command as "Running from source" above — `restreamair` builds and runs fully on Linux, with **no external dependencies and identical behaviour to the macOS build**. Where macOS uses Apple frameworks, Linux uses portable equivalents that are compiled in automatically:

- HTTP server: a POSIX-socket server instead of Apple's `Network` framework.
- Crypto: pure-Swift SHA-256/PBKDF2 (admin login) and AES (internal CENC clearkey + HLS AES-128 decryption) instead of CommonCrypto — verified byte-for-byte against it, and against NIST/FIPS test vectors (`restreamair selftest`).

So the internal remuxer stays fully self-contained on Linux — clearkey/AES decryption does **not** require ffmpeg, same as on macOS. `restreamair-menubar` is the only thing not built on Linux (it needs AppKit).

Prebuilt binaries are attached to each [GitHub release](https://github.com/testingbetaversion/ReStreamAir/releases): macOS (arm64), Linux x86-64 (`restreamair-linux`), and Linux arm64 (`restreamair-linux-arm64`).

### Already running?

Starting a second instance on a port that's already bound (by a previous `restreamair`, or anything else) fails fast with a clear `error: Port <n> is already in use …` message and a suggested `lsof -ti:<port> | xargs kill` instead of silently sitting there doing nothing.

## Usage

`restreamair` dispatches on its first argument. No argument, or `serve`, starts the web panel — that's the only thing most people need:

```sh
./restreamair                    # same as: ./restreamair serve
./restreamair serve --port 9000  # override the port for this run
```

The other two modes are the DASH-inspection and live-restream CLI tools, reached as subcommands of the one binary:

```sh
./restreamair dash action=describeFunctions
./restreamair live action=describeFunctions
```

### Port and bind address

Port is resolved in this order: `--port <n>` on the command line, then the `PORT` environment variable, then the **Settings** panel's saved port (`data/state.json`, editable from the web UI), then `8787`.

Bind address works the same way: `--bind <address>`, then `BIND_ADDRESS`, then the Settings panel's saved value, then "all interfaces" (the historical default). Set it to `127.0.0.1` to only accept connections from the same machine, or a specific LAN IP to bind just that interface:

```sh
./restreamair serve --port 9000 --bind 127.0.0.1
```

Changing either takes effect on the next restart (a running listener can't rebind).

### Default admin credentials

`--admin-username`/`--admin-password` (or the `RESTREAMAIR_ADMIN_USERNAME`/`RESTREAMAIR_ADMIN_PASSWORD` env vars) overwrite whatever admin accounts already exist with exactly one, using those credentials — a deliberate reset, not a merge. Useful for scripted deployments or recovering from a lost password:

```sh
./restreamair serve --admin-username admin --admin-password "a-new-password"
```

Any existing session cookies for the old admin(s) stop working immediately, since their username no longer exists in the account list.

## `restreamair dash` (DASH inspection CLI)

Print help:

```sh
./restreamair dash
```

List segment URLs. Initialization URLs are included by default:

```sh
./restreamair dash --list --count 5 https://livesim2.dashif.org/livesim2/testpic_2s/Manifest.mpd
```

Show MPD representation information:

```sh
./restreamair dash --info https://livesim2.dashif.org/livesim2/testpic_2s/Manifest.mpd
./restreamair dash --info --video-only https://livesim2.dashif.org/livesim2/testpic_2s/Manifest.mpd
```

Filter output:

```sh
./restreamair dash --list --representation V300 --count 10 <MPD_URL>
./restreamair dash --list --audio-only --period 0 <MPD_URL>
./restreamair dash --list --video-only --no-init <MPD_URL>
```

Request controls:

```sh
./restreamair dash --list -H 'Authorization: Bearer TOKEN' --timeout 10 <MPD_URL>
./restreamair dash --list --proxy http://127.0.0.1:8888 <MPD_URL>
./restreamair dash --list --base-url https://cdn.example.com/path/ <MPD_URL>
```

JSON output:

```sh
./restreamair dash --info --json <MPD_URL>
./restreamair dash --list --json --count 3 <MPD_URL>
```

Live MPD polling:

```sh
./restreamair dash --list --representation V300 --sleep-requests 2 <LIVE_MPD_URL>
```

### Action mode

Every action prints JSON to stdout: `{ "ok": true, "action": "function_name", "result": {} }`, or on failure `{ "ok": false, "error": "message" }`.

```sh
./restreamair dash action=<function_name> key=value key2=value2
```

Common arguments shared by MPD actions:

| Argument | Required | Purpose |
| --- | --- | --- |
| `mpd` | yes | Absolute MPD URL. |
| `timeout` | no | HTTP timeout in seconds. Default `30`. |
| `header` | no | HTTP header string. Use `Name: value`, or separate multiple with `\|`. |
| `proxy` | no | HTTP or HTTPS proxy URL. |

Boolean values accept `true`, `false`, `1`, `0`, `yes`, `no`.

**`describeFunctions`** — returns the exported actions, arguments, and output contract. No arguments.

**`info`** — fetches and parses an MPD, returns periods/adaptation sets/representations.

```sh
./restreamair dash action=info mpd=https://example.com/index.mpd representation=2
```

**`readMPD`** — flexible MPD read: parsed model, raw XML tree, selected elements, expanded segments, or raw XML text, in any combination. See `elements`/`attributes`/`includeTree`/`includeSegments`/`includeRawXML` etc.

```sh
./restreamair dash action=readMPD mpd=https://example.com/index.mpd includeModel=false elements=Representation,SegmentTemplate attributes=id,bandwidth,media,initialization
```

**`listSegments`** — fetches/parses an MPD, expands matching `SegmentTemplate` entries, returns segment URLs.

```sh
./restreamair dash action=listSegments mpd=https://example.com/index.mpd representation=2 count=5
```

**`parseDuration`** — parses an ISO 8601 DASH duration into seconds, e.g. `PT1M30S`.

**`fillTemplate`** — replaces `SegmentTemplate` tokens (`$RepresentationID$`, `$Bandwidth$`, `$Number$`, `$Time$`, `$SubNumber$`) with supplied values. `$SubNumber$` is DASH-IF low-latency chunked addressing (e.g. `chunkdurssr` test assets): when a `SegmentTimeline` `<S>` entry carries a `k` attribute and the media template references `$SubNumber$`, each segment position is expanded into `k` separately fetchable CMAF chunks instead of one whole segment.

## `restreamair live` (live MPD → M3U8 CLI)

**`createLiveM3U8`** — polls a live DASH MPD, downloads new init/media segments into a temp folder, keeps a bounded queue, removes old segment files, and writes a live HLS M3U8 playlist. Static/offline MPDs are rejected unless `forceOffline=true`. A single segment/chunk fetch failure (routine at the live edge, especially for low-latency chunked sources where sub-second chunks can be requested slightly before the origin has produced them, or where an origin's last chunk of a segment is never reliably produced at all) doesn't kill the worker — it's retried a couple of times, then logged and skipped, and the next poll picks up where the timeline left off. For chunked (`$SubNumber$`) segments specifically, chunks are always requested in order, so anything already downloaded before a later chunk fails is a clean, gapless prefix and is kept (a slightly truncated segment); nothing past the failure point for that segment is ever served, since a hole in the *middle* of a GOP breaks decoding in a way a whole missing segment doesn't. `EXT-X-MEDIA-SEQUENCE` is tracked as its own counter, always incrementing by exactly 1 per segment added to the playlist regardless of how that segment's underlying DASH/chunk numbering looks — per RFC 8216 §4.3.3.2 consecutive segments' sequence numbers must differ by exactly 1, and using a chunked source's own (much larger, unevenly-spaced) segment numbers directly made spec-literal HLS clients like `ffplay` read a jump of a few real segments as "thousands of segments behind" and loop on a full playlist reload/reseek instead of ever settling into steady playback.

| Argument | Required | Purpose |
| --- | --- | --- |
| `mpd` | yes | Live MPD URL. |
| `representation` | no | Representation id to convert. Recommended. |
| `period` | no | Period id or zero-based index. |
| `output` / `tempDir` | no | Playlist path / download directory. |
| `playlistSegments` / `keepSegments` / `downloadAhead` | no | Windowing controls. |
| `pollInterval` | no | Seconds between polls. Defaults to MPD `minimumUpdatePeriod` or `2`. |
| `forceOffline` / `maxPolls` | no | Testing/offline controls. |
| `decryptionKeys` | no | `KID:KEY` hex pairs for CENC clearkey decryption. See below. |
| `segmentUrlParams` | no | Raw query-string fragment appended to every segment/init URL before download. |
| `manifestHeader` | no | HTTP header string used only for the MPD fetch (falls back to `header`). |
| `timeout` / `header` / `proxy` | no | See common arguments above. |

```sh
./restreamair live action=createLiveM3U8 mpd=https://example.com/index.mpd representation=2 playlistSegments=8 keepSegments=12
```

Output fields: `playlist`, `tempDir`, `downloaded`, `kept`, `playlistSegments`, `live`.

Every fetch/download this worker performs also prints a compact JSON log line to stdout (`{"event":"downloadSegment","url":"...","status":"success","bytes":1234}`) — the panel captures and shows these in its **Logs** view (see below); running the worker standalone, they're just visible on the terminal.

## Web panel

```sh
./restreamair
```

Open `http://127.0.0.1:8787` (or whatever port you configured). Providers hold one or more streams; each stream is either a live DASH restream (`kind=mpd`, run through `restreamair live` as a self-spawned subprocess) or an HLS proxy (`kind=m3u8`, remote `.m3u8` fetched and rewritten on the fly).

### Admin accounts

On first run the panel serves a **Create admin account** screen instead of the normal UI — nothing works until one exists. After that, every `/api/*` endpoint (except `/api/auth/*`) requires either a signed-in session (cookie, from the login form) or an `Authorization: Basic base64(username:password)` header (for scripts/`curl`). Add more admins from the **Users** panel once signed in.

**Remember me** (checked by default on the sign-in form) controls how long that session sticks around: checked gets a persistent cookie good for 30 days; unchecked gets a session cookie that disappears as soon as the browser closes (the server-side session backing it is capped at 24h either way as a safety net).

Password hashing (PBKDF2-HMAC-SHA256 via CommonCrypto) is macOS-only, matching the rest of the app's decryption features — account creation/login will report a clear "unsupported on this platform" error on Linux rather than storing anything unsafe.

### Stream editor: automatic detect & probe

There's no manual Detect button or Type dropdown — just paste a source URL. As soon as you paste, tab away, or pause typing, the panel fetches it (through whatever proxy/headers you've set, **including the Manifest headers field** — a manifest that needs an auth header to be readable gets probed with that same header, not an anonymous request) and sniffs DASH vs. HLS, and lists every available video quality and audio track — reusing the same MPD parser as `restreamair dash` (`DashSegmentsCLI.probe`) or the HLS master-playlist parser (`M3U8Rewriter.probeMaster`), not a separate implementation. The detected type (MPD/M3U8) is shown in the status line under the field. For DASH sources it also fetches each video representation's init segment and reports any CENC KID it finds, pre-filling **Decryption keys** with `KID:00000000000000000000000000000000` placeholder lines for every KID detected — regardless of which representations you've selected — so you only have to paste in the real key. The **Decryption keys** field itself stays hidden until protection is actually detected (or a stream already has saved keys).

Every detected video quality and audio track is selected by default — uncheck whichever ones you don't want. Select multiple video qualities and/or audio tracks and the stream serves an HLS **master playlist** (`#EXT-X-STREAM-INF` per quality, `#EXT-X-MEDIA:TYPE=AUDIO` per track) instead of a single-quality one — one restream worker is spawned per selected representation. Each variant always gets a distinct `BANDWIDTH` value, real (from the detected manifest) when available, otherwise a distinct placeholder — identical-bandwidth, no-`RESOLUTION` entries are legal but some HLS clients (hls.js included) collapse them into a single selectable rendition, which silently hid every quality but one for representations added by typing an id manually instead of via detect.

**Playlist segments** has a floor of 3 — anything fewer starves players of buffer and causes stalls/rebuffering, so values below 3 are clamped up.

### Audio delay (lip-sync offset)

The **Audio delay (ms)** field on a stream shifts that stream's audio track relative to its video, for sources where the two are already out of sync at the origin. It can be negative (audio earlier) or positive (audio later). Applied only to the representation(s) flagged as audio — implemented by rewriting each downloaded audio segment's `moof > traf > tfdt` baseMediaDecodeTime (converted through that track's `mdhd` timescale) before it's added to the output playlist, so it's a standards-correct timestamp shift rather than a re-encode.

### Provider / stream logo

Leave the **Logo** field blank when creating a provider or stream (or importing channels/events from a script — see below) and the server tries to auto-assign one by searching [Clearbit's](https://clearbit.com) keyless company-autocomplete endpoint for a name match and using its real PNG logo. Best effort — if nothing matches confidently, no logo is assigned and the UI falls back to showing the first letter of the name instead. Requires outbound network access to `autocomplete.clearbit.com` the first time a given name is looked up (cached afterwards, per-name, to `logo-cache.json`, so the same name never triggers a second lookup). This does mean the name being looked up (a provider or channel name) is sent to Clearbit's servers to search — fine for something like "ESPN" or "HBO Max", worth knowing if a name itself is sensitive. Both the provider grid and stream cards (provider list and All Streams) show the assigned logo.

Click **Find logo** next to either Logo field to look one up for whatever's currently in the Name field without waiting for save — same lookup, exposed directly as `GET /api/logo-lookup?name=<name>` (admin auth required, like the rest of `/api/*`) returning `{"logo": "<url>"}` or `{"logo": null}`.

### Provider defaults & per-stream network overrides

A provider can set a default **Proxy** and generic **Headers** (applied to every request its streams make). Each stream can override the proxy (**Network override** — blank inherits the provider's), and has three separate header buckets that layer on top of the provider's generic headers: **Manifest headers** (MPD/playlist fetch), **Media headers** (segment/init fetch), **HLS key headers** (the AES-128 key fetch, `kind=m3u8` only).

For CDNs that put an auth token in the *source URL's* query string (e.g. `?token=abc&region=us`) but expect that same token on every segment/init request too, check **Append each stream's own Source URL query params to its segments** in Provider settings — each stream then automatically reuses its own URL's query string for every media/init fetch, no need to duplicate the token anywhere or keep it in sync by hand. Leave it off and use the **Segment URL params** fallback field instead for a fixed query string that isn't part of any stream's URL (shared across every stream in the provider).

### Script providers

Instead of a fixed manifest URL, a provider can delegate to an external script — any Python/`sh`/executable program — that owns login, session/cookie persistence, manifest resolution, and (optionally) DRM key acquisition. This exists so that scripts already written against this protocol (e.g. one built on a shared session/DNS-over-HTTPS/worker helper library) work against ReStreamAir unmodified, without ReStreamAir having to implement any of that logic itself — it only spawns the script and passes/consumes plain text. **[SCRIPTING.md](SCRIPTING.md)** is the practical "how do I write one" guide with worked examples; this section is the protocol reference.

**Currently implemented**: setting a provider's **Script path** (plus optional **Bind**/**DoH URL**/**Worker**, forwarded to the script verbatim and otherwise unused by ReStreamAir), and managing one or more **Accounts** — each is just a **Username**/**Password** (sent as `user=`/`password=` when set — either can be left blank if your script doesn't need it, e.g. a token-only login or a pairing-only flow; the Username also is the account's label everywhere in the UI, there's no separate name), with an **Enabled** toggle (unchecking keeps the account configured but skips it for Login/Pair/Load channels/Load events — a kill switch on using its credentials, not just a display preference). An **Account selection** setting controls which account each call uses: **Active account** (a radio marks one account as always-used, the default), **Rotate** (cycles through every enabled account in turn, one per call — spreads load/rate limits across several credentialed accounts), or **Random** (a different enabled account each call). ReStreamAir never reads or writes a script's session file — the script manages that entirely on its own. Saving is automatic: clicking Login/Pair/Load channels/Load events first saves whatever's in the dialog (including the provider's **Name** — this doubles as the rename UI), so there's no separate save step to remember. Live output shows inline (polls the same log store the Logs tab uses, under a synthetic `script:<providerId>` id). **Load channels**/**Load events** run the `channels`/`events` action and create/update one stream per entry under that provider (matched by name, so re-running an import updates in place instead of duplicating). The **All Streams** view has a Channels/Events/Manually added filter to see just one kind.

**Planned, not yet wired up**: the `manifest`/`cdm`/`heartbeat` actions and the session-manifest live-playback pipeline that would call them (fetch a manifest from the script, try each returned CDN in order until one plays, fall back to a fresh script call if all of them fail, refresh on expiry, periodic heartbeat). Streams imported via channels/events already store every field the script reports (`SessionManifest`, `ScriptParams`, `CdmType`, `Autostart`, `Start`/`End`, ...) so nothing is lost — they just aren't acted on for playback yet. The rest of this section documents the full target protocol so scripts can be written against it now even though ReStreamAir doesn't call every action yet.

#### Script invocation

Every call gets a flat `key=value` argv (no `--` flags) — the same convention ReStreamAir's own internal worker process already uses for itself. `.py` scripts run under `python3`, `.sh`/`.bash` under `/bin/sh`, anything else is executed directly (must be `chmod +x` with its own shebang). Common to every action, always sent first:

```
action=[login|pair|manifest|cdm|events|channels|heartbeat] bind=[bind] proxy=[proxy] doh=[doh] worker=[worker]
```

then `user=[username] password=[password]` — the active account's Username/Password, **only appended if that field is non-empty** (so `login`/`pair` for a token-only or pairing-only account gets no `user=`/`password=` at all, and a blank Password field means no `password=` even if `user=` is present). An account's Username doubles as its label everywhere in the UI (the account list, the active-account dropdown, export) — there's no separate account name to keep in sync with it.

For every action except `login`/`pair`, that's followed by the active account's own free-text `params` (a legacy field with no UI to edit it anymore — set on import/export or via the API — kept for scripts that relied on it), then any action-specific params below.

- **`login`** / **`pair`** — no params beyond `user=`/`password=` above. The script authenticates (or walks a device-pairing flow, printing a code for the user and blocking until it's entered elsewhere) and persists whatever session/cookie state it needs to a file only it knows about.
- **`manifest`** — `action=manifest`, plus that stream's `ScriptParams`. Expected stdout (JSON):
  ```json
  {
    "ManifestUrl": "https://...",
    "Cdn": [{ "Name": "akamai", "ManifestUrl": "https://..." }],
    "Headers": {
      "manifest": { "user-agent": "...", "custom-header": "..." },
      "media": { "user-agent": "..." }
    },
    "Heartbeat": { "Url": "https://...", "Params": ["param1", "param2"], "PeriodMs": 300000 }
  }
  ```
- **`cdm`** — `action=cdm drm=[widevine|playready] challenge=[...] pssh=[...]`, plus the stream's `ScriptParams`. ReStreamAir has no embedded CDM of its own (no Widevine/PlayReady challenge generation) — `challenge=`/`pssh=` are passed through empty unless a future version adds one, so a script needing them generates its own challenge and performs its own license exchange (as a script already can, entirely independently). Expected stdout: one `kid:key` hex pair per line — the exact format `Decryption keys` already accepts by hand, so this drops straight into the same clearkey decryption path described below.
- **`events`** / **`channels`** — `action=events` / `action=channels`, no extra params. Expected stdout (JSON), each entry becoming one stream under the provider once wired up:
  ```json
  {
    "Channels": [
      {
        "Name": "Channel name",
        "Mode": "live",
        "SessionManifest": true,
        "ScriptParams": "id=123 country=fr",
        "CdmType": "widevine",
        "UseCdm": true,
        "Video": "best",
        "Audio": "desc:en",
        "OnDemand": false,
        "SpeedUp": true,
        "Autostart": true,
        "Start": 1682631577,
        "End": 1682632577,
        "RecordEvent": true
      }
    ]
  }
  ```
  (`"Events"` for the events action, same shape.)
- **`heartbeat`** — `action=heartbeat heartbeaturl=[Heartbeat.Url from the manifest script] heartbeatparams=[Heartbeat.Params from the manifest script]`, fired on the `Heartbeat.PeriodMs` interval a `manifest` call returned.

#### Provider export / import

The download icon on a provider card (`GET /api/providers/<id>/export`) downloads that provider as one JSON file: every setting, script account (usernames/passwords included — this file is exactly as sensitive as `state.json` itself, handle it accordingly), every stream, and — if a script is configured — the script's own source, base64-embedded directly rather than referenced by path. **Import provider** on the Providers tab (`POST /api/providers/import`) reverses it on any install: every id (provider/streams/accounts) is regenerated fresh since the originals mean nothing on a different install, the active account is re-resolved by name instead, and an embedded script file gets written out to a `scripts/` folder (created if needed, existing files with the same name are never overwritten — a numeric suffix is added instead) and `chmod +x`'d. End to end, moving a fully working provider to another machine is: click the download icon, click Import provider there, done — no separate copying of the script file by hand.

#### M3U playlist export

For pointing an external player (VLC, Kodi, TiviMate, ...) at your streams instead of using the panel: the list icon on a provider card (`GET /api/providers/<id>/playlist.m3u8`) downloads an M3U8 playlist of just that provider's streams, and **Export playlist (all)** on the Providers tab (`GET /api/playlist.m3u8`) covers every provider in one file. Each entry points at the same universal `/play/<streamId>/index.m3u8` URL the in-panel player and "copy direct link" already use, tagged with `group-title` (the provider's name, for players that group channels) and `tvg-logo` (the stream's own logo, falling back to the provider's) — standard IPTV M3U attributes most players already understand. The playlist lists every stream regardless of state, but opening an entry doesn't start it: `kind=m3u8` streams (proxied live, no local worker) play immediately either way, while `kind=mpd` streams need to already be **Start**ed from the panel first, same as opening their direct link would — the URL 404s ("Runtime file is not ready") until then.

### CENC decryption (DASH clearkey)

For `kind=mpd` streams whose segments are CENC-encrypted (`cenc` scheme, AES-CTR), paste one or more `KID:KEY` hex pairs into **Decryption keys** (one pair per line — auto-detect pre-fills the KID(s) it found with a zeroed placeholder key; just replace the zeros). The live worker decrypts each downloaded init/media segment before it's added to the output playlist.

- This decrypts CENC `cenc` (AES-CTR) sample data directly using a key you already have — it works **regardless of which DRM systems the manifest declares** (Widevine, PlayReady, FairPlay, clearkey are all just different license-delivery labels wrapped around the same CENC-encrypted bytes; the KID/key pair for the content is the same one every one of those systems would eventually hand a certified player). Multi-DRM test assets that advertise Widevine+PlayReady+ClearKey side by side decrypt fine here as long as you supply their clearkey KID:KEY.
- **What's actually not possible**: automatically *acquiring* a key from a real Widevine/PlayReady/FairPlay license server when you don't already have it. That requires a certified, proprietary CDM (device certificates, secure key storage, a licensed request/response protocol) that can't be legally or technically reimplemented here. Also not supported: `cbcs` pattern encryption (only full-sample `cenc`/CTR).
- If you supply exactly one `KID:KEY` pair it's used as the default regardless of the segment's actual KID.
- macOS-only (`CommonCrypto`) — fails clearly on Linux instead of silently serving encrypted output.

### HLS AES-128 decryption

For `kind=m3u8` streams protected with `#EXT-X-KEY:METHOD=AES-128`, fill in **HLS key** (hex) and optionally **HLS IV** (hex). The panel decrypts each segment server-side before proxying it and strips `#EXT-X-KEY` from the playlist it serves. Blank IV defaults to the segment's media sequence number, per RFC 8216 §5.2. Also macOS-only.

### Live DASH MPD alongside HLS

Every running `kind=mpd` stream also serves `GET /play/<streamId>/index.mpd` — a live DASH manifest generated from the exact same downloaded segments as `index.m3u8`, not a second copy fetched or stored separately. Built with `<SegmentList>` (literal per-segment `<SegmentURL media="...">` entries) rather than `<SegmentTemplate>`'s `$Number$` token substitution, since on-disk filenames aren't a fixed-width formula a template could reconstruct — this also sidesteps needing exact per-segment timing, at the cost of a nominal (not frame-exact) declared segment duration. One `<AdaptationSet>` for video, one for audio, one `<Representation>` per selected quality/track, mirroring the HLS master playlist's structure.

### Direct stream (no playlist)

For players/pipelines that would rather consume raw fMP4 bytes than poll a playlist, every running `kind=mpd` stream also exposes a **direct link** per representation: `GET /direct/<streamId>/<repId>` (single-representation streams can omit `/<repId>`). The connection stays open — the server sends the init segment once, then appends each new media segment to the same response as it's downloaded, so the client just reads a continuously growing byte stream and never has to re-request anything. No `m3u8`, no polling, no reload storm. The stream editor's player panel lists a real, clickable link per representation (not a read-only field you have to select and copy out of) — a **Copy** button next to the list copies all of them at once. Same auth rules as `/play/*`/`/restream/*` — pass `?key=<key>` if the provider has API keys configured.

### Download (no ffmpeg)

`GET /download/<streamId>/<repId>` (`.mp4` suffix optional, e.g. `/download/<streamId>/<repId>.mp4`) is a one-shot download instead of a live tail: it concatenates whatever's currently buffered on disk (init + every kept segment — the same window backing the live playlist) and serves it as-is with `Content-Disposition: attachment`. Same clickable-link-list treatment as the direct link, plus a `download` attribute so clicking one starts a save-as instead of opening in the browser. No ffmpeg, no subprocess — every downloaded segment is already a self-contained fragmented-MP4 chunk (its own `moof`+`mdat`, decodable once the init/`moov` has been seen, the same property that makes it tailable live at `/direct/`), so concatenating the bytes we already have on disk *is* a valid, directly-playable `.mp4` file. (An earlier version of this endpoint remuxed to MPEG-TS via `ffmpeg -c copy`; that was dropped after finding it depended on a real ffmpeg bug — some DASH-IF low-latency chunked sources with very large absolute segment timestamps make ffmpeg's muxer silently produce zero packets while still reporting success. Nothing in this project shells out to ffmpeg by default anymore — the one remaining optional touchpoint, `ffprobe` as a last-resort segment-duration fallback, defaults off and has to be explicitly opted into via `probeDuration=true` on the `live` CLI.)

### Big player

Every card in **All Streams** has an expand button that jumps straight into the stream editor's Player tab (skipping Config) for a larger view than the floating Picture-in-Picture quick-play, without leaving the grid.

### Monitoring (clients, bandwidth, server specs, connections)

Live **Server** view (the default landing view after sign-in), pushed over Server-Sent Events at `/api/events` roughly every second:

- **Input / Output Bandwidth** — Input is bytes pulled from each stream's source (segment downloads for `kind=mpd`, upstream fetches for `kind=m3u8`); Output is bytes served back out to viewers. Both are tracked globally and per stream, current throughput and all-time total, and persist across restarts.
- **CPU Load** — 1-minute load average alongside core count (`x.xx / N`), plus a separate detailed CPU%/model/peak tile below.
- **Active clients per stream** — distinct clients (by API key, else IP) seen in the last ~30-60 seconds.
- **Server specs** — CPU model/core count, RAM, OS version, disk capacity, live CPU%/memory/disk/uptime, all-time peak CPU%/memory.
- **Active connections table** — one row per current playback connection: stream, provider, type (mpd/hls), user (API key label or "Open access"), client IP, user agent, a per-connection UID, connection uptime, error count, and live per-connection bandwidth. Filterable with the search box above the table.

### Logs

The **Logs** view shows structured events forwarded from each running stream's `restreamair live` worker: manifest fetches, segment downloads (with proxy used, byte count, success/error), one line per event. Polled from `/api/logs?streamId=&limit=&date=` every 2 seconds, but only while that tab is open — the moment you switch away the polling stops, so it adds no overhead otherwise.

- **Normal / Verbose toggle** — Normal shows the condensed one-line summary; Verbose shows the full raw JSON line the worker emitted for that event.
- **Previous runs** — every entry is also appended to `logs/<yyyy-MM-dd>.jsonl`, so history survives a server restart. The run picker next to the stream filter lists available dates; "Current run" reads the fast in-memory buffer (last 500 events across all streams), older dates read straight from that day's file.
- **Rotation** — each day's log file is capped at 8MB; once a write would exceed that it's rotated to `<yyyy-MM-dd>.<epoch>.jsonl` and a fresh file is opened, keeping the newest 5 rotations per day. The run picker only lists the current day's active file, not its rotated overflow parts.
- **Level filter** — an "All levels / Errors only" dropdown next to the stream and run filters, for jumping straight to failures on a busy stream.
- **Deep link from a stream card** — every stream card (in a provider's list and in All Streams) has a small logs icon that opens the Logs view pre-filtered to that stream.

### API keys

Generate/revoke from the **API Keys** panel. Keys gate playback only — `/play/*`, `/restream/*`, `/proxy/*` require `?key=<key>` or `Authorization: Bearer <key>` once at least one key exists. With zero keys, playback stays open. (This is separate from admin auth above, which gates the `/api/*` config surface instead.)

### Web UI

Single horizontal bar up top (brand, view switcher, sign-out) — no sidebar, and no manual refresh button since everything already polls/streams live. Providers are a horizontal chip switcher (with its own quick-search box) instead of a list; clicking one shows its streams below, with their own search box. **New provider** and **Provider settings** open the same kind of in-page modal dialog as the stream editor, not an inline form. Clicking a stream card (or **New Stream**) opens the stream editor dialog — no split view, no separate browser window — with a combined Start/Stop toggle and a small icon-only Delete tucked to the side so the primary actions don't get lost in a row of same-sized buttons. Providers and streams are both deletable (provider deletion also stops and removes all of its streams). A provider's **Streams** panel also has **Start all** / **Stop all** / **Delete all** buttons for bulk control over every stream it owns, alongside the per-stream controls. An **All Streams** view shows every stream from every provider as a card grid, with a provider filter and a search box; click a card to edit it, or use the big-player expand, quick-play (pops the stream into a floating Picture-in-Picture window without leaving the grid), logs, Start/Stop, or Delete controls without opening the editor. The player tab plays automatically as soon as you open it, no separate Load click, and always plays the HLS (`m3u8`) rendition — the direct fMP4 link and the one-shot .mp4 download are separate, explicit opt-ins next to it (each shown one per line, not squashed onto a single row, when a stream has more than one representation). When a stream has more than one video quality and/or audio track, **Quality** / **Audio** dropdowns appear above the player, backed by hls.js's `levels`/`audioTracks` — hidden entirely when there's nothing to pick. hls.js runs in debug mode, so its full internal event log (fragment loads, level switches, buffering state, errors) is available in the browser console for troubleshooting playback without needing server-side logs. Dark, floating-glass panels with a red/orange/purple gradient accent by default (a manual light-theme toggle is still available), and a single consistent line-icon set throughout (24×24, uniform stroke weight) instead of a mix of ad-hoc shapes. Installable as an app (manifest + service worker), lightweight transitions instead of jarring full reloads. No build step — plain HTML/CSS/JS in `public/` (hls.js is vendored locally rather than pulled from a CDN, so playback isn't at the mercy of tracking-prevention/ad-blockers or being offline).

## macOS menu bar

`restreamair-menubar` is an optional, macOS-only companion app. It shows a status item reflecting whether the panel is running, with a single toggle menu item that reads "Start" or "Stop" depending on current state (not two separate always-visible items), "Open Panel", and a live running-stream count pulled from `/api/state`.

```sh
./restreamair-menubar
```

By default it looks for `restreamair` next to its own binary; override with `RESTREAMAIR_PATH=/path/to/restreamair`. Override the port with `PORT`.

### Launching without a terminal window

Run from a terminal like above and it behaves like any other CLI tool — reasonable for testing, but `restreamair-menubar` is a bare Unix executable, not a macOS app bundle. Double-click it in Finder and macOS falls back to its default handler for unbundled executables, which is to open Terminal.app and run it there — the menu bar item still works, it's just not headless.

Wrap it in a minimal `.app` bundle to fix that — LaunchServices runs a bundled executable directly, no terminal involved. Point `RESTREAMAIR_PATH` at the real `restreamair` binary via `LSEnvironment` so the bundle can live anywhere (e.g. `/Applications`) while `restreamair`/`state.json`/`public/`/etc. stay right here in the project directory; the working directory follows `RESTREAMAIR_PATH`, not wherever the bundle itself is:

```sh
APP="ReStreamAir.app"
mkdir -p "$APP/Contents/MacOS"
cp restreamair-menubar "$APP/Contents/MacOS/"
cat > "$APP/Contents/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>restreamair-menubar</string>
    <key>CFBundleIdentifier</key>
    <string>com.restreamair.menubar</string>
    <key>CFBundleName</key>
    <string>ReStreamAir</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>LSUIElement</key>
    <true/>
    <key>LSEnvironment</key>
    <dict>
        <key>RESTREAMAIR_PATH</key>
        <string>$(pwd)/restreamair</string>
    </dict>
</dict>
</plist>
EOF
open "$APP"
```

`LSUIElement` is what keeps it out of the Dock (menu-bar-only, same as running it from a terminal already does via `setActivationPolicy(.accessory)`) — the bundle itself is what removes the Terminal window. Move `ReStreamAir.app` to `/Applications` afterward if you want it there; the `RESTREAMAIR_PATH` baked into `Info.plist` keeps pointing at this project directory regardless of where the bundle itself ends up. Re-run this snippet (or edit `Info.plist` directly) if the project directory moves.

## Data layout

Everything lives directly in the working directory — no `data/` subfolder to go looking in:

- `state.json` — providers, streams, admin accounts, API keys, settings, all-time bandwidth/system-peak totals.
- `runtime/<streamId>/` — downloaded segments + playlist(s) for running DASH restreams (one subfolder per representation when multi-quality). Gated: only ever reachable through the API-key-checked `/restream/`, `/play/`, `/direct/`, and `/download/` routes, never served directly as a static file.
- `logo-cache.json` — per-name logo URL cache for auto-logo lookup (name → Clearbit logo URL, or empty string for a confirmed miss).
- `logs/<yyyy-MM-dd>.jsonl` — persisted log history, one JSON object per line, rotated daily. Powers the Logs view's "previous runs" picker.

An install that still has the old `data/` + `public/runtime/` layout from an earlier version gets migrated automatically on the next `restreamair serve` startup — nothing to do by hand, and nothing is deleted if a new-layout file somehow already exists at the destination.
