# ReStreamAir

A local DASH → HLS restreaming toolkit with a web control panel. Point it at a live DASH or HLS source, and it serves that stream back out as HLS to any player on your network — decrypting CENC clearkey or HLS AES-128 along the way, without shelling out to ffmpeg.

Written in Swift with a C core (`core/`, see [The C core](#the-c-core)). Two binaries:

- **`restreamair`** — everything: the web panel (default), plus `dash` / `live` CLI subcommands it spawns for itself.
- **`restreamair-menubar`** — optional macOS-only menu bar companion that launches and monitors the panel.

## Install

Grab a prebuilt binary from the [latest release](https://github.com/testingbetaversion/ReStreamAir/releases/latest), or the [nightly](https://github.com/testingbetaversion/ReStreamAir/releases/tag/nightly) for the newest features. No toolchain needed — the Swift runtime is linked in.

| Platform | Asset |
|---|---|
| macOS (Apple Silicon / Intel) | `restreamair-macos.tar.gz` |
| Linux x86-64 (dynamic) | `restreamair-linux-x86_64.tar.gz` |
| Linux x86-64 (static) | `restreamair-linux-static` |
| Windows x64 | `restreamair-windows-x64.zip` |

```sh
chmod +x restreamair
./restreamair          # starts the web panel — same as ./restreamair serve
```

The dynamic Linux build needs two libraries Foundation uses, present on most systems. If you hit `error while loading shared libraries: libcurl.so.4`:

```sh
sudo apt-get update && sudo apt-get install -y libcurl4 libxml2   # Debian/Ubuntu
```

## Quick start

Open `http://127.0.0.1:8787`. The first launch serves a **Create admin account** screen — nothing else works until that account exists. Everything you set up afterwards persists in `state.json`; it is not reset between runs.

```sh
./restreamair serve --port 9000                 # different port
./restreamair serve --bind 127.0.0.1            # local connections only
./restreamair serve --admin-username admin --admin-password "new-password"
```

**Port** resolves in this order: `--port`, `$PORT`, the Settings panel's saved value, then `8787`. **Bind address** works the same way (`--bind`, `$BIND_ADDRESS`, Settings, then all interfaces). Both take effect on the next restart — a live listener can't rebind.

The admin flags (or `RESTREAMAIR_ADMIN_USERNAME` / `RESTREAMAIR_ADMIN_PASSWORD`) **replace** every existing admin with exactly one — a deliberate reset for scripted deploys or a lost password, not a merge. Existing sessions stop working immediately.

Starting a second instance on a taken port fails fast with the port number and a suggested `lsof -ti:<port> | xargs kill`, rather than hanging silently.

To run headless: `nohup ./restreamair > restreamair.log 2>&1 & disown`, or see [Launching without a terminal](#launching-without-a-terminal) for a proper macOS `.app`.

## Building from source

**Prerequisites**: a Swift 5.9+ toolchain — Xcode Command Line Tools on macOS (`xcode-select --install`), or [swift.org/install](https://www.swift.org/install/) on Linux. There are no external packages to fetch.

```sh
git clone <this repo> ReStreamAir
cd ReStreamAir
swift build -c release
.build/release/restreamair
```

The same command builds on macOS and Linux; where macOS uses Apple frameworks, Linux compiles in portable equivalents automatically (a POSIX-socket HTTP server instead of `Network`). Behaviour is identical, including decryption — the internal remuxer never needs ffmpeg on either platform.

> Use `swift build`, not `swift file.swift` or a bare `swiftc` invocation: the panel re-execs *itself* to spawn worker subprocesses, so it needs a real compiled executable, and the Swift sources now link the C core.

The menu bar companion is a separate macOS-only target:

```sh
swiftc HTTPClient.swift MenuBarApp.swift -o restreamair-menubar -framework AppKit
```

### The C core

`core/` holds a C library the app's logic is migrating into, so Linux and Windows can eventually run the panel from a C++ executable with no Swift toolchain at all. **That migration is in progress — the C server does not serve the panel yet**, and the shipping binary on every platform is still the Swift one.

Ported so far: SHA-256/HMAC/PBKDF2, AES (block, CTR, CBC), RFC 3986 URL resolution, the HLS playlist rewriter, the ffmpeg argument builder, and IP/CIDR matching for trusted proxies. Four call sites already run on it — admin password hashing, HLS AES-128 decryption, CENC subsample decryption, and reverse-proxy trust. The first three used to fork on `#if canImport(CommonCrypto)`; each is now one implementation everywhere, and the app no longer links CommonCrypto at all. Trusted-proxy matching was written in C first precisely so both servers agree, byte for byte, on what `10.0.0.0/8` covers — getting that wrong is a security bug in either direction.

Ports are not merely tested, they are **proven equal** to the Swift code they replace: `restreamair selftest` runs both implementations over the same inputs and fails on any difference. That is what keeps a password hashed by an older build still verifying today, so `Crypto.swift` and `AES.swift` stay as the reference.

The C server links **libcurl** (outbound HTTPS) and **libxml2** (MPD parsing) for source auto-detect. Install the dev headers first:

```sh
sudo apt-get install -y cmake build-essential libcurl4-openssl-dev libxml2-dev   # Debian/Ubuntu
```

```sh
cmake -S . -B build && cmake --build build
./build/restream_selftest                 # NIST/FIPS vectors + goldens captured from Swift
python3 scripts/api-smoke.py --binary build/restreamair-server   # end-to-end HTTP checks
./build/restreamair-server --port 8080    # serves the panel + the ported API
```

Two test suites, and they cover different things. `restream_selftest` proves the ported *functions* produce the same bytes as the Swift ones. `scripts/api-smoke.py` runs the real server in a scratch directory and exercises the layer around them over HTTP — the auth gate, the viewer role, the login throttle, cookie attributes, session persistence across a restart, the mode of `state.json`, the M3U export and the provider export/import round trip. CI runs both, on Linux and macOS, plus a third pass under AddressSanitizer and UBSan. `-DRS_BUILD_SERVER=OFF` builds the libraries and self-test alone, which is how the Windows CI job verifies the core without needing libcurl and libxml2.

`restreamair-server` accepts an optional `serve` subcommand (so its argv matches the Swift binary), `-p`/`--port`, `-b`/`--bind`, and `--root`. It auto-detects `public/` and serves the panel's static files. The control API is being ported to C incrementally. Working in C today:

- **Admin auth** — create an account, sign in, sign out, with the same roles, session persistence and login throttling as the Swift panel (and the same `state.json`, so a session started on one server is honoured by the other).
- **Full management** — create, edit and delete providers, streams, users and API keys, all through the real panel UI.
- **Live monitoring** — the Server view's `/api/events` SSE stream with real host stats (CPU, memory, disk, load, uptime, OS).
- **Source auto-detect** — `/api/probe` fetches a DASH/HLS source and lists its qualities, audio tracks and any DRM KIDs, so the stream editor's paste-to-detect works.
- **Playback** — direct-source redirects (`/source/<id>`), HLS passthrough, and live DASH→HLS through a threaded engine that downloads, decrypts and windows segments per representation, including the multi-quality master playlist and the audio-delay `tfdt` shift.
- **Script providers** — provider script actions run and their output reaches the panel's script pane and the Logs tab.
- **Logs** — the `/api/logs` ring buffer behind the Logs view.
- **Export and import** — `/api/playlist.m3u8`, per-provider M3U, provider export and import, and the stored EPG.

Not ported yet: `/direct/` and `/download/`, because the C live engine hands each cached segment out exactly once and has no API to replay the whole buffer — adding one is a change to the engine's ownership model rather than a route. Those return an honest `501`; the Swift binary remains the fully working one for them. Password hashes and `state.json` are shared, so an account or provider created by either server is seen by the other, and the C server never drops fields it doesn't model.

This produces `librestream_base.a` (portable logic, no sockets), `librestream_core.a` (that plus the [mongoose](https://github.com/cesanta/mongoose) HTTP server), and both executables, under GCC, Clang and MSVC. `Package.swift` compiles the same C sources into the Swift app, so the two build systems must be kept in step. Test fixtures and goldens are generated — rerun `scripts/gen-parity-fixtures.py`, and see the header comment in `scripts/gen-goldens.py`.

## Web panel

Providers hold streams. Each stream is either a **live DASH restream** (`kind=mpd` — run through a `restreamair live` subprocess) or an **HLS proxy** (`kind=m3u8` — a remote playlist fetched and rewritten on the fly).

### Accounts and access

Every `/api/*` endpoint except `/api/auth/*` needs a signed-in session cookie or an `Authorization: Basic` header. Add more accounts from the **Settings → Accounts** panel. **Remember me** gives a 30-day persistent cookie; unchecked gives a browser-session cookie. Server-side sessions are capped at 24h either way.

**Roles.** An account is either an **Admin** (full control) or a **Viewer** (read-only: it can see the panel, the monitoring view and the logs, but every write is refused with a `403`). Enforcement is on the HTTP method rather than a list of routes, so a route added later is read-only for viewers by default. Provider **export** is admin-only too, since it embeds script-account passwords. Accounts created before roles existed are admins, and the last admin can't be deleted.

**Sessions survive a restart.** They're persisted in `state.json` and reloaded on boot, so "remember me for 30 days" means 30 days rather than "until the next restart". What's stored is the SHA-256 of each token, never the token: `state.json` is a file you back up and copy around, and a live session token in it would be a spare key to the panel. Deleting an account ends its sessions immediately.

**Failed sign-ins are throttled** per username + client address: five free attempts, then a doubling delay (2s, 4s, 8s …) capped at 15 minutes, answered with `429` and a `Retry-After`. An hour of quiet clears the record. The counter is deliberately in memory only — persisting it would let anyone who can reach the login form lock an account out across restarts. Keying on the address as well as the username means one attacker can't lock a real operator out by guessing at their username from somewhere else.

Passwords are hashed with PBKDF2-HMAC-SHA256 (100k iterations) from the C core, identically on every platform.

**API keys** (generated in the **API Keys** panel) are separate, and gate *playback* only: once at least one key exists, `/play/*`, `/restream/*` and `/proxy/*` require `?key=<key>` or `Authorization: Bearer <key>`. With no keys, playback is open.

### Stream editor

No Detect button and no type dropdown — paste a source URL and the panel probes it as soon as you pause typing. It sniffs DASH vs HLS (using the same parsers as the `dash` CLI, not a second implementation), lists every video quality and audio track, and fetches DASH init segments to report any CENC KID it finds — pre-filling **Decryption keys** with `KID:0000…` placeholders so you only paste the real key. Probing uses your configured proxy and **Manifest headers**, so a manifest that needs auth is probed with it.

- **Multi-quality** — every track is selected by default. Select more than one and the stream serves an HLS master playlist (one worker per representation). Each variant gets a distinct `BANDWIDTH`, real when detected, otherwise a placeholder, since some clients collapse identical-bandwidth entries into one.
- **Playlist segments** — floor of 3; fewer starves player buffers and causes stalls.
- **Audio delay (ms)** — shifts audio against video for sources that arrive out of sync. Positive or negative. Applied by rewriting each audio segment's `tfdt` baseMediaDecodeTime through its `mdhd` timescale — a standards-correct timestamp shift, not a re-encode.
- **Direct source** — makes `/play/<id>/index.m3u8` answer with a `302` to the source URL (or the CDN mirror a resident ffmpeg last rotated to) instead of proxying. Lets you flip an existing stream to direct delivery without re-pointing clients. The API-key gate still applies to the redirect.
- **Logo** — leave blank and the server looks one up by name via [Clearbit's](https://clearbit.com) keyless autocomplete endpoint, caching results in `logo-cache.json`. Best effort; falls back to the name's first letter. Note this sends the provider or channel name to Clearbit. **Find logo** does the same lookup on demand (`GET /api/logo-lookup?name=`).

### Network settings

A provider sets a default **Proxy** and generic **Headers** for every request its streams make. Each stream can override the proxy and adds three header buckets on top: **Manifest headers**, **Media headers**, and **HLS key headers** (`kind=m3u8` only).

For CDNs that carry an auth token in the source URL's query string and expect it on every segment too, enable **Append each stream's own Source URL query params to its segments**. For a fixed query string that isn't part of any stream's URL, use the provider-wide **Segment URL params** instead.

### Decryption

**CENC (DASH clearkey)** — paste `KID:KEY` hex pairs into **Decryption keys**, one per line. The worker decrypts each init/media segment before it enters the output playlist. Supply exactly one pair and it's used regardless of the segment's actual KID.

This works **whatever DRM the manifest advertises** — Widevine, PlayReady, FairPlay and clearkey are just different license-delivery wrappers around the same CENC bytes, so a multi-DRM asset decrypts fine given its clearkey pair. What is *not* possible is **acquiring** a key from a real license server without already having it; that needs a certified proprietary CDM. `cbcs` pattern encryption is also unsupported (full-sample `cenc`/CTR only).

**HLS AES-128** — fill in **HLS key** (hex) and optionally **HLS IV**. Segments are decrypted server-side and `#EXT-X-KEY` is stripped from the playlist served. A blank IV defaults to the segment's media sequence number, per RFC 8216 §5.2.

Both run on every platform.

### Playback endpoints

| Route | Purpose |
|---|---|
| `/play/<id>/index.m3u8` | The universal HLS link — what the panel player, copy-link and M3U export all use. |
| `/play/<id>/index.mpd` | Live DASH manifest for the same `kind=mpd` stream, generated from the same on-disk segments. Uses `<SegmentList>` rather than `<SegmentTemplate>`, since filenames aren't a fixed-width formula; segment durations are nominal, not frame-exact. |
| `/direct/<id>/<repId>` | Raw fMP4 tail. The connection stays open: init segment once, then each new segment appended to the same response. No playlist, no polling. `<repId>` is optional for single-representation streams. |
| `/download/<id>/<repId>` | One-shot download of whatever is currently buffered (`.mp4` suffix optional). Every segment is a self-contained fragmented-MP4 chunk, so concatenating them *is* a playable file — no ffmpeg, no subprocess. |
| `/source/<id>` | Redirect to the origin (or current CDN mirror). |
| `/ping` | Liveness probe: `{"status":"ok",...}`. Deliberately unauthenticated so a supervisor, container healthcheck or uptime monitor can use it without holding a credential, and deliberately says nothing about the install. |

Streams also export as M3U for external players: the list icon on a provider card (`GET /api/providers/<id>/playlist.m3u8`), or **Export playlist (all)** (`GET /api/playlist.m3u8`). Entries carry `tvg-id`, `tvg-name`, `group-title` and `tvg-logo`. `tvg-id` is what binds guide data to a channel in TiviMate, Kodi or Jellyfin — set it per stream with **EPG channel id** in the editor, or leave it blank to use the stream id. The playlist lists every stream, but opening one doesn't start it — `kind=mpd` streams must be **Start**ed from the panel first, or the URL 404s.

### Script providers

Instead of a fixed manifest URL, a provider can delegate to an external script — Python, `sh`, or any executable — that owns login, session persistence, manifest resolution and optionally DRM key acquisition. ReStreamAir only spawns it and exchanges text on stdin/stdout. **[SCRIPTING.md](SCRIPTING.md)** is the how-to guide; this is the summary.

Configure a **Script path** (plus optional **Bind** / **DoH URL** / **Worker**, forwarded verbatim) and one or more **Accounts** — a Username/Password pair, either of which may be blank for token-only or pairing-only flows, with an **Enabled** toggle. **Account selection** picks between *Active* (one marked account), *Rotate* (each call uses the next enabled account) and *Random*.

**Script actions.** A provider declares which actions its script implements, as a checkbox grid in Provider settings; ReStreamAir never calls anything unticked, so a script that only does `channels` is never handed a `start` it can't answer. Each stream inherits that set and can override it — including overriding to nothing, to keep the script away from one particular stream.

| | Actions |
|---|---|
| **Account** | `login`, `pair` |
| **Catalogue** | `channels`, `events`, `epg` |
| **Lifecycle** | `start`, `stop`, `heartbeat` |
| **Pipeline** | `manifest` (session manifest), `url` (rewrite a URL before fetching), `downloadmanifest`, `pssh`, `initparse`, `cdm` |
| **Not wired yet** | `downloadinit`, `downloadmedia` — configurable now, but nothing calls them: a subprocess per segment needs a persistent worker rather than one spawn per fetch |

The pipeline hooks fail soft — a hook that errors, times out or prints nothing is logged and ReStreamAir carries on with its built-in behaviour, so a broken script degrades rather than taking the stream down. They run in the panel process, which means they cover manifest and DRM resolution; segment fetching for a running `kind=mpd` stream happens in the `live` worker subprocess and is not hooked.

**Session store.** Each provider gets `runtime/scripts/<providerId>/`, passed on every call as `sessiondir=` and `cookies=`. The script keeps its login there and it survives restarts; ReStreamAir creates the directory, never reads it, and deletes it with the provider. **Clear session** in Provider settings wipes it.

**Load channels** / **Load events** create or update one stream per entry, matched by name so re-running updates in place. **Load EPG** stores whatever the script returns (XMLTV or JSON), served back from `GET /api/providers/<id>/epg`. The **All Streams** view can filter by Channels / Events / Manually added. Clicking any script action saves the dialog first, and live output shows inline.

Every call gets a flat `key=value` argv (no `--` flags). `.py` runs under `python3`, `.sh`/`.bash` under `/bin/sh`, anything else directly (needs `chmod +x` and a shebang):

```
action=<name> bind= proxy= doh= worker= sessiondir= cookies= [user=] [password=]
```

**Values may be base64.** Passing a secret or free text as literal argv is unsafe — a password shows up in `ps`, and a value with a space gets torn apart. So a value containing whitespace, a control character, a quote, a backslash or non-ASCII is base64-encoded and marked `b64:`, and a password always is. Everything else stays readable: there's no shell between ReStreamAir and the interpreter, so `?`, `&` and `;` are never interpreted and a URL arrives as written. Scripts must decode — it's three lines, in SCRIPTING.md. Output coming back is auto-detected the same way, accepting `b64:` or unambiguous bare base64.

`challenge=` is always empty: ReStreamAir has no embedded CDM, so a script needing one generates its own challenge and performs its own license exchange.

**`manifest` also serves as failure recovery.** When a manifest fetch fails on its primary URL *and* every CDN mirror, a script-backed provider is asked for a fresh `ManifestUrl`/`Cdn`/`Headers`, which is persisted and retried. This fires inline for m3u8 passthrough, and in the background once a `kind=mpd` worker has needed two consecutive restarts. Throttled to one refresh per stream per minute, never concurrent for the same stream.

**Export / import** — the download icon on a provider card (`GET /api/providers/<id>/export`) produces one JSON file with every setting, stream, script account (**passwords included — treat it as sensitively as `state.json`**) and the script's own source, base64-embedded. **Import provider** (`POST /api/providers/import`) reverses it on any install: ids are regenerated, the active account is re-resolved by name, and the embedded script is written into `scripts/` and `chmod +x`'d, never overwriting an existing file.

### Monitoring and logs

The **Server** view is the landing page, pushed over Server-Sent Events at `/api/events` about once a second:

- **Input / output bandwidth** — bytes pulled from sources vs served to viewers, global and per stream, current and all-time, persisted across restarts.
- **Active clients per stream** — distinct clients (by API key, else IP) in the last ~30–60s.
- **Server specs** — CPU model and cores, load average, live CPU/memory/disk/uptime, and all-time peaks.
- **Active connections** — one row per playback connection: stream, provider, type, user, IP, user agent, uptime, errors and live bandwidth. Searchable.

The **Logs** view shows structured events from each running worker — manifest fetches, segment downloads with proxy, byte count and outcome. It polls `/api/logs` every 2s **only while open**. Toggle Normal/Verbose for the raw JSON, filter by stream, level, or run date, and jump straight to a stream's logs from the icon on its card. History is appended to `logs/<date>.jsonl`; each day is capped at 8MB, then rotated to `<date>.<epoch>.jsonl` keeping the newest 5.

### Interface

A single top bar — brand, view switcher, sign-out. No sidebar and no refresh button, since everything streams live. Providers are a horizontal chip switcher with quick-search; **All Streams** shows every stream as a filterable card grid. Cards offer quick-play (floating picture-in-picture), a big-player expand into the editor's Player tab, logs, Start/Stop and Delete without opening the editor; providers add **Start all** / **Stop all** / **Delete all**.

The player starts automatically and always plays HLS — the direct fMP4 and `.mp4` download links sit beside it as explicit opt-ins. **Quality** and **Audio** dropdowns appear only when there's more than one to pick, backed by hls.js's `levels`/`audioTracks`. hls.js runs in debug mode, so its full event log is in the browser console for troubleshooting playback without server logs.

Dark floating-glass panels with a gradient accent by default, plus a light-theme toggle and a uniform 24×24 line-icon set. Installable as an app (manifest + service worker). No build step — plain HTML/CSS/JS in `public/`, with hls.js vendored locally so playback doesn't depend on a CDN, an ad-blocker's mood, or being online.

## Command line

`restreamair` dispatches on its first argument; no argument means `serve`.

| Subcommand | Purpose |
|---|---|
| `serve` | The web panel. The default. |
| `dash` | DASH/MPD inspection. |
| `live` | Live MPD → HLS worker — what the panel spawns per stream. |
| `cdmprobe` | Print the DRM challenge (KIDs + PSSH) for a manifest, and run a CDM script against it. |
| `selftest` | Known-answer crypto vectors plus the C-vs-Swift parity check. Used by CI. |

### `restreamair dash`

```sh
./restreamair dash                                              # help
./restreamair dash --info <MPD_URL>                             # representations
./restreamair dash --list --count 5 <MPD_URL>                   # segment URLs (init included)
./restreamair dash --list --representation V300 --audio-only --period 0 --no-init <MPD_URL>
./restreamair dash --list -H 'Authorization: Bearer T' --proxy http://127.0.0.1:8888 --timeout 10 <MPD_URL>
./restreamair dash --info --json <MPD_URL>                      # JSON output
./restreamair dash --list --sleep-requests 2 <LIVE_MPD_URL>     # poll a live MPD
```

**Action mode** prints JSON — `{"ok":true,"action":"…","result":{}}`, or `{"ok":false,"error":"…"}`:

```sh
./restreamair dash action=<function> key=value key2=value2
```

Shared by MPD actions: `mpd` (required, absolute URL), `timeout` (default 30), `header` (`Name: value`, `|`-separated for several), `proxy`. Booleans accept `true`/`false`/`1`/`0`/`yes`/`no`.

| Action | Does |
|---|---|
| `describeFunctions` | Lists the exported actions, arguments and output contract. |
| `info` | Parses an MPD into periods / adaptation sets / representations. |
| `readMPD` | Flexible read — parsed model, raw XML tree, selected `elements`/`attributes`, expanded segments, raw XML, in any combination. |
| `listSegments` | Expands matching `SegmentTemplate` entries into segment URLs. |
| `parseDuration` | ISO 8601 DASH duration (`PT1M30S`) → seconds. |
| `fillTemplate` | Substitutes `$RepresentationID$`, `$Bandwidth$`, `$Number$`, `$Time$`, `$SubNumber$`. |

`$SubNumber$` is DASH-IF low-latency chunked addressing: when a `SegmentTimeline` `<S>` carries a `k` attribute and the media template uses `$SubNumber$`, each position expands into `k` separately fetchable CMAF chunks.

### `restreamair live`

**`createLiveM3U8`** polls a live DASH MPD, downloads new segments into a temp folder, keeps a bounded queue, prunes old files, and writes a rolling HLS playlist. Static MPDs are rejected unless `forceOffline=true`.

| Argument | Purpose |
|---|---|
| `mpd` | Live MPD URL. Required. |
| `representation` / `period` | Which representation and period to convert. Representation recommended. |
| `output` / `tempDir` | Playlist path and download directory. |
| `playlistSegments` / `keepSegments` / `downloadAhead` | Windowing controls. |
| `pollInterval` | Seconds between polls. Defaults to the MPD's `minimumUpdatePeriod`, else 2. |
| `decryptionKeys` | `KID:KEY` hex pairs for CENC clearkey. |
| `segmentUrlParams` | Query fragment appended to every segment/init URL. |
| `manifestHeader` | Header used only for the MPD fetch (falls back to `header`). |
| `forceOffline` / `maxPolls` | Testing and offline controls. |
| `timeout` / `header` / `proxy` | As above. |

```sh
./restreamair live action=createLiveM3U8 mpd=<URL> representation=2 playlistSegments=8 keepSegments=12
```

Returns `playlist`, `tempDir`, `downloaded`, `kept`, `playlistSegments`, `live`, and prints one compact JSON line per fetch — which is exactly what the panel's Logs view collects.

Two robustness details worth knowing. A single failed fetch doesn't kill the worker: it retries, then logs and skips, and the next poll resumes from the timeline. For chunked (`$SubNumber$`) sources, chunks are always requested in order, so a failure keeps the gapless prefix already downloaded and never serves anything past it — a hole in the middle of a GOP breaks decoding in a way a short segment doesn't. And `EXT-X-MEDIA-SEQUENCE` is its own counter, incrementing by exactly 1 per segment as RFC 8216 §4.3.3.2 requires, rather than echoing a chunked source's large, unevenly spaced numbering — which made strict clients read a normal advance as being thousands of segments behind and reload in a loop.

## macOS menu bar

`restreamair-menubar` shows a status item with a single Start/Stop toggle reflecting the current state, "Open Panel", and a live running-stream count from `/api/state`.

```sh
./restreamair-menubar
```

It looks for `restreamair` next to its own binary; override with `RESTREAMAIR_PATH`, and the port with `PORT`.

### Launching without a terminal

The menu bar binary is a bare Unix executable, so double-clicking it in Finder opens Terminal.app to run it. Wrapping it in a minimal `.app` bundle fixes that — LaunchServices runs a bundled executable directly. Point `RESTREAMAIR_PATH` at the real binary via `LSEnvironment` so the bundle can live in `/Applications` while `state.json`, `public/` and the rest stay in the project directory:

```sh
APP="ReStreamAir.app"
mkdir -p "$APP/Contents/MacOS"
cp restreamair-menubar "$APP/Contents/MacOS/"
cat > "$APP/Contents/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>restreamair-menubar</string>
    <key>CFBundleIdentifier</key><string>com.restreamair.menubar</string>
    <key>CFBundleName</key><string>ReStreamAir</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>LSUIElement</key><true/>
    <key>LSEnvironment</key>
    <dict>
        <key>RESTREAMAIR_PATH</key><string>$(pwd)/restreamair</string>
    </dict>
</dict>
</plist>
EOF
open "$APP"
```

`LSUIElement` keeps it out of the Dock; the bundle is what removes the Terminal window. Re-run this if the project directory moves, since the path is baked into `Info.plist`.

## Deployment

The panel speaks plain HTTP and has no TLS listener of its own. On a trusted LAN that is fine. Anywhere else — anything reachable from the internet — put a terminating proxy in front of it, because otherwise the sign-in form and the session cookie cross the network in the clear.

### Docker

```sh
docker build -t restreamair .
docker run -d --name restreamair -p 8787:8787 -v restreamair-data:/data restreamair
```

Everything mutable (`state.json`, `runtime/`, `logs/`, `logo-cache.json`) resolves relative to the working directory, so `/data` is the only volume that matters: back that up and you have backed up the install. The image runs as an unprivileged user and has a healthcheck on `/ping`.

### Docker with HTTPS

`docker-compose.yml` runs the panel behind [Caddy](https://caddyserver.com), which obtains and renews a real certificate on its own:

```sh
RESTREAMAIR_DOMAIN=panel.example.com docker compose up -d
```

The panel port is not published — only Caddy can reach it — so the plain-HTTP listener is never exposed. After the first sign-in, set **Trusted proxies** (below).

### systemd

`deploy/restreamair.service` is a hardened unit for a bare-metal install; the header comment carries the install steps. It binds `127.0.0.1` by default, expecting a proxy in front.

### Running behind a reverse proxy

Behind a proxy every request arrives from the proxy's address over plain HTTP. Taken literally that means one "client" for the entire internet — the monitoring view, the connection table and the login throttle all collapse onto a single address — and the connection looks insecure even when the user is on HTTPS, so the session cookie never gets its `Secure` flag.

**Settings → Trusted proxies** fixes that by naming the hops to believe:

| Value | Meaning |
|---|---|
| *(empty)* | Trust nothing; use the peer address. The default, and correct for a directly exposed server. |
| `loopback` | A proxy on the same machine (`127.0.0.0/8`, `::1`). |
| `private` | Any RFC 1918 / RFC 4193 / link-local address — the right answer for the Docker Compose setup. |
| `10.0.0.0/8, 192.168.1.7` | An explicit list of addresses and CIDR blocks, either family. |
| `any` | Believe everyone. Only safe when nothing but the proxy can open a connection. |

With a match, `X-Forwarded-Proto: https` makes the session cookie `Secure` and adds HSTS, and `X-Forwarded-For` resolves to the real client — walking right-to-left past your own trusted hops, so a client that prepends a forged entry gains nothing. Without a match, both headers are ignored outright, which is what stops an unauthenticated client forging its own address past the login throttle. The value is validated when you save it, because a typo would otherwise fail silently: it would simply never match, leaving you believing your proxy is trusted when it is not.

A proxy also has to be told not to buffer: `/api/events` is an SSE stream open for the life of the panel, and `/direct/` is a fMP4 tail that never ends. `deploy/Caddyfile` sets `flush_interval -1` for exactly this; for nginx it is `proxy_buffering off;` plus a long `proxy_read_timeout`.

## Data layout

Everything lives directly in the working directory:

- `state.json` — providers, streams, admin accounts, API keys, settings, all-time bandwidth and peak totals.
- `runtime/<streamId>/` — downloaded segments and playlists for running DASH restreams, one subfolder per representation. Only reachable through the key-checked `/restream/`, `/play/`, `/direct/` and `/download/` routes, never as static files.
- `logo-cache.json` — name → logo URL cache (empty string records a confirmed miss).
- `logs/<yyyy-MM-dd>.jsonl` — persisted log history, rotated daily.

`state.json` is created `0600` and re-tightened on every save. It holds admin password hashes, session token hashes, API keys and — for script providers — account passwords in the clear, since the script has to be handed the real thing. Treat a provider export the same way: it embeds those passwords too.

Installs still using the old `data/` + `public/runtime/` layout are migrated automatically on the next start, and nothing is overwritten if a new-layout file already exists.

## License

MIT — see [LICENSE](LICENSE).

Two vendored components keep their own terms. [cJSON](https://github.com/DaveGamble/cJSON) is MIT and [hls.js](https://github.com/video-dev/hls.js) is Apache-2.0, both compatible. [mongoose](https://github.com/cesanta/mongoose) is **GPLv2 or a commercial license from Cesanta**, and it is linked only into the C server (`librestream_core` / `restreamair-server`) — never into the Swift binary, which has its own HTTP server. That distinction matters if you distribute builds: the released `restreamair` binaries contain no mongoose, but a `restreamair-server` binary you distribute is a combined work under mongoose's terms.
