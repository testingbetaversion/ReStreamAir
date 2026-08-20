# ReStreamAir

A local DASH → HLS restreaming toolkit with a web control panel. Point it at a live DASH or HLS source and it serves that stream back out as HLS to any player on your network — decrypting CENC clearkey or HLS AES-128 along the way, without shelling out to ffmpeg.

Written in C. One binary — `restreamair` — serves the panel, runs the live engines and does its own decryption; there is no runtime, no interpreter and no worker subprocess behind it. The two libraries it needs, libcurl and libxml2, are on every distro. See [Architecture](#architecture).

**Contents** — [Install](#install) · [Quick start](#quick-start) · [How it fits together](#how-it-fits-together) · [Web panel](#web-panel) · [Playback endpoints](#playback-endpoints) · [Script providers](#script-providers) · [Building](#building-from-source) · [Architecture](#architecture) · [Deployment](#deployment) · [Data layout](#data-layout) · [Troubleshooting](#troubleshooting) · [License](#license)

## Install

Grab a tarball from the [latest release](https://github.com/testingbetaversion/ReStreamAir/releases/latest), or the [nightly](https://github.com/testingbetaversion/ReStreamAir/releases/tag/nightly) for the newest features.

| Platform | Asset |
|---|---|
| macOS (Apple Silicon / Intel) | `restreamair-<tag>-macos.tar.gz` |
| Linux x86-64 | `restreamair-<tag>-linux-x86_64.tar.gz` |

Each holds the `restreamair` binary and the `public/` directory it serves. Unpack and run it from inside the folder:

```bash
tar -xzf restreamair-vX-linux-x86_64.tar.gz && cd restreamair-vX-linux-x86_64 && ./restreamair
```

Two shared libraries are resolved at runtime. If you hit `error while loading shared libraries: libcurl.so.4`:

```bash
sudo apt-get update && sudo apt-get install -y libcurl4 libxml2
```

The Linux binary is built against the oldest glibc still supported, so it runs on newer distributions too — the other direction does not work. There is no Windows release: the panel needs libcurl and libxml2, which Windows CI has no supply of without vcpkg, so it compiles and self-tests the core there but ships nothing. [Build from source](#building-from-source) on any platform — it is two cmake commands.

## Quick start

```bash
chmod +x restreamair && ./restreamair
```

Open `http://127.0.0.1:8787`. The first launch serves a **Create admin account** screen — nothing else works until that account exists. Everything you set up afterwards persists in `state.json` and is not reset between runs.

```bash
./restreamair --port 9000
```

```bash
./restreamair --bind 127.0.0.1
```

```bash
./restreamair --root /srv/restreamair/public
```

**Port** resolves in this order: `--port`, the Settings panel's saved value, then `8787`. **Bind address** works the same way (`--bind`, Settings, then all interfaces). Both take effect on the next restart — a live listener can't rebind. A leading `serve` is accepted and ignored, so `./restreamair serve --port 9000` works too.

The panel's static files are found automatically when `public/` sits next to the working directory or one level up; `--root` is for everything else. Without a web root it serves `/ping` and says so on startup rather than pretending.

Starting a second instance on a taken port fails immediately with the address it could not bind, rather than hanging silently.

**A lost admin password means editing `state.json`** — there is no reset flag. Stop the server, delete the account's object from the `users` array, restart, and the create-admin screen comes back.

To run headless: `nohup ./restreamair > restreamair.log 2>&1 & disown`, or install the [systemd unit](#systemd).

## How it fits together

**Providers** hold **streams**. A provider carries the settings its streams share — proxy, headers, downloader, and optionally a login script. A stream is one channel or event.

Each stream has a **kind**:

- **`kind=mpd`** — a live DASH source. It runs through a background engine that polls the MPD, downloads segments, CENC-decrypts them and republishes them as a rolling HLS playlist. Nothing is re-encoded.
- **`kind=m3u8`** — an HLS source. The remote playlist is fetched and rewritten on the fly, and segments are proxied (decrypting AES-128 if a key is configured).

And an **input pipeline**, chosen per stream in **Input/output pipeline**:

| Input | What runs |
|---|---|
| **Internal remuxer** (default) | The built-in engine. No ffmpeg anywhere; decrypts CENC clearkey and HLS AES-128 itself. |
| **FFmpeg resident** / **TS HLS** / **MultiTS HLS** / **FMP4 HLS** | **Not implemented.** The panel still offers these and stores the choice, but starting such a stream answers `501`. |
| **N_m3u8DL-RE (live)** | **Not implemented**, same as above. |

Only the internal remuxer runs today, so **Output target** (SRT Server, UDP/SRT, Custom) has nothing behind it either — output is HLS. The ffmpeg-backed modes are the one part of the panel whose UI is ahead of the engine; see [Architecture](#architecture).

Whatever the pipeline, players always use one URL: `/play/<id>/index.m3u8`.

## Web panel

Seven views in one top bar: **Providers**, **All Streams**, **Server**, **Logs**, **API Keys**, **Settings**, **Help**. There's no refresh button — everything streams live.

Every view has its own address — `/providers`, `/streams`, `/server`, `/monitoring`, `/logs`, `/keys`, `/settings`, `/help` — so back/forward, reload and bookmarks all work. The two views that carry context put it in the query string (`/streams?provider=<id>`, `/logs?stream=<id>`), so a deep link lands pre-filtered exactly the way the in-app shortcuts do. Both servers serve `index.html` for that fixed list of paths and nothing else: a mistyped asset or API path still gets a `404` rather than being quietly answered with the panel's HTML.

### Accounts and access

Every `/api/*` endpoint except `/api/auth/*` needs a signed-in session cookie or an `Authorization: Basic` header. Add more accounts from the **Settings → Accounts** panel. **Remember me** gives a 30-day persistent cookie; unchecked gives a browser-session cookie. Server-side sessions are capped at 24h either way.

**Roles.** An account is either an **Admin** (full control) or a **Viewer** (read-only: it can see the panel, the monitoring view and the logs, but every write is refused with a `403`). Enforcement is on the HTTP method rather than a list of routes, so a route added later is read-only for viewers by default. Provider **export** is admin-only too, since it embeds script-account passwords. Accounts created before roles existed are admins, and the last admin can't be deleted.

**Sessions survive a restart.** They're persisted in `state.json` and reloaded on boot, so "remember me for 30 days" means 30 days rather than "until the next restart". What's stored is the SHA-256 of each token, never the token: `state.json` is a file you back up and copy around, and a live session token in it would be a spare key to the panel. Deleting an account ends its sessions immediately.

**Failed sign-ins are throttled** per username + client address: five free attempts, then a doubling delay (2s, 4s, 8s …) capped at 15 minutes, answered with `429` and a `Retry-After`. An hour of quiet clears the record. The counter is deliberately in memory only — persisting it would let anyone who can reach the login form lock an account out across restarts. Keying on the address as well as the username means one attacker can't lock a real operator out by guessing at their username from somewhere else.

Passwords are hashed with PBKDF2-HMAC-SHA256 (100k iterations), identically on every platform — no system crypto library is involved, so the same password hash verifies wherever you move `state.json`.

**API keys** (generated in the **API Keys** view) are separate and gate *playback* only. Once at least one key exists, every playback path — `/play/`, `/restream/`, `/proxy/`, `/direct/`, `/download/`, `/source/` — requires `?key=<key>` or `Authorization: Bearer <key>`. With no keys, playback is open.

### Stream editor

Paste a source URL and the panel probes it as soon as you pause typing. The small refresh button at the right edge forces a new probe whenever an expiring URL or changing manifest needs to be enumerated again. It sniffs DASH vs HLS (using the same parsers the live engine does, not a second implementation), lists every video quality and audio track, and fetches DASH init segments to report any CENC KID it finds — pre-filling **Decryption keys** with `KID:0000…` placeholders so you only paste the real key. Probing uses your configured proxy and **Manifest headers**, so a manifest that needs auth is probed with it.

**DASH options**

| Field | Meaning |
|---|---|
| **Representation** / **Period** | Used when no qualities are ticked above. |
| **Playlist count** | Segments advertised in the rolling playlist. Floor of 3 — fewer starves player buffers and causes stalls. |
| **Playback delay (s)** | Hold playback this many seconds behind the live edge. 0 = live. |
| **Keep count** | Segments retained behind the playlist window. Raise it if players ask for segments that have already aged out. |
| **Download ahead** | How far ahead of the playlist the engine fetches. Also the working depth of the pending queue — see [the live engine](#the-live-engine) for why a bigger number is not automatically better. |
| **Parallel downloads** | Segment requests in flight at once, **shared by every rendition of the stream** rather than allowed to each. Default 6, capped at 8. Changing it restarts the stream's download threads; every other field here is picked up on the next poll. |
| **Prioritize oldest** | Vestigial. The download threads already take the oldest queued segment first, because that is the one the playlist is waiting on. |
| **Poll seconds** | MPD poll period. Defaults to the MPD's `minimumUpdatePeriod`, else 2. |
| **Audio delay (ms)** | Lip-sync offset, positive or negative. Applied by rewriting each audio segment's `tfdt` baseMediaDecodeTime through its `mdhd` timescale — a standards-correct timestamp shift, not a re-encode. |
| **EPG channel id** | Exported as `tvg-id` in the M3U, which is how an external player binds guide data to this channel. Blank uses the stream id. |
| **Allow offline/static MPD** | Static (`type="static"`) manifests are rejected unless this is ticked. |
| **Reduce manifest polling** | Off by default. Enable it for an origin that caps concurrent sessions or answers `429`/`403`; it backs the director off from a 15s to a 300s re-read once renditions are known. |

**Multi-quality** — every track is selected by default. Select more than one and the stream serves an HLS master playlist, one worker per representation. Each variant gets a distinct `BANDWIDTH`, real when detected and a placeholder otherwise, since some clients collapse identical-bandwidth entries into one.

### Subtitles and captions

Picked up automatically — there is nothing to configure.

| Source form | What happens |
|---|---|
| **TTML / IMSC1**, either plain `application/ttml+xml` segments or wrapped per-sample in `stpp` fMP4 | Converted to **WebVTT** by the rendition worker and served as an HLS `TYPE=SUBTITLES` group. Handles namespace-prefixed documents, clock and offset times (`ttp:tickRate`, `ttp:frameRate` with its multiplier), `hh:mm:ss:ff` frame counts, `<br/>`, entities, and italic/bold spans. |
| **WebVTT** | Passed through as-is. |
| **CEA-608 / CEA-708** | Already inside the video's SEI messages, and segments are passed through byte-for-byte, so nothing has to be demuxed. The MPD's SCTE 214 `<Accessibility>` descriptor is read and republished as `EXT-X-MEDIA:TYPE=CLOSED-CAPTIONS` with the right `INSTREAM-ID` — without that tag a player has no reason to look for them. Streams that declare none get `CLOSED-CAPTIONS=NONE` rather than leaving players to hunt. |
| **DVB subtitles** | MPEG-TS only, and only on the ffmpeg input modes with an **SRT/UDP** output, where `-map 0:s?` copies them across. There is no HLS container that can carry a bitmap subtitle, so an HLS output cannot keep them without burning them into the video — which would mean transcoding a pipeline that is copy-only by design. |

TTML becomes WebVTT rather than being passed through because IMSC1-in-fMP4 is legal HLS that only Safari and recent hls.js actually render; the conversion happens once per segment on the way in, not per request. Cue times are anchored to the media timeline from the segment's `tfdt` (or the manifest's `$Time$` for a bare text segment), and a fragment authored from zero instead of on the presentation timeline is detected and lifted onto it.

The subtitle rendition never blocks playback: it is `DEFAULT=NO`, and a text track that is slow or broken is not counted when deciding whether the stream is ready to serve.

**Direct source** makes `/play/<id>/index.m3u8` answer with a `302` to the source URL (or the CDN mirror a resident ffmpeg last rotated to) instead of restreaming. That flips an existing stream to direct delivery without re-pointing clients. The API-key gate still applies to the redirect.

**Logo** — leave blank and the server looks one up by name via [Clearbit's](https://clearbit.com) keyless autocomplete endpoint, caching results in `logo-cache.json`. Best effort; falls back to the name's first letter. Note this sends the provider or channel name to Clearbit. **Find logo** does the same lookup on demand (`GET /api/logo-lookup?name=`).

### Network settings

A provider sets a default **Proxy**, generic **Headers**, and a **Downloader** for every request its streams make. Each stream can override the proxy and adds three header buckets on top: **Manifest headers**, **Media headers**, and **HLS key headers** (`kind=m3u8` only). **Use proxy for** scopes the proxy to any of Script / Manifest / Media independently. Proxies may be HTTP or SOCKS5 (`socks5h://user:pass@host:port`), and credentials embedded in the proxy URL are answered on a Basic-auth challenge.

**Downloader** picks the tool that fetches manifests and segments:

| Value | Behaviour |
|---|---|
| `native` (default) | In-process libcurl. No subprocess per fetch. |
| `curl` / `wget` / `aria2c` | Spawns that external binary; **Downloader params** appends extra CLI args (`--retry 3`, `-x 8`). |

The internal fetch tries **HTTP/2 first and downgrades per host**. Some origins run a flaky HTTP/2 stack that drops mid-segment with a framing-layer error — reproducible with plain `curl`, so it is the origin, not the client. That host is remembered and used over HTTP/1.1 from then on, and the request that tripped it is retried immediately, so one broken origin costs one request once instead of giving up multiplexing for everything else.

**Connections are reused per thread**: one long-lived handle each, reset between requests, which keeps its socket, TLS session and DNS cache. On a high-latency path the handshake rather than the bandwidth is what a live stream cannot afford — a TCP connect measured 0.34–0.94s, TLS adding ~0.15s, which a rendition publishing one 2s segment every 2s has nowhere to put, and which presents as "not enough bandwidth" when it is entirely latency. A handle is never touched by two threads (libcurl does not support sharing connections concurrently); the DNS and TLS session caches, which are documented as safe to share, are process-wide.

For CDNs that carry an auth token in the query string and expect it on every segment, enable **Append each stream's own Source URL query params to its segments**. The params are taken from the URL that *actually answered* the manifest poll, not the URL you configured — some CDNs sign the token only onto the 302 target, and reading the configured URL would find nothing there. For a fixed query string that isn't part of any stream's URL, use the provider-wide **Segment URL params** instead.

### Decryption

**CENC (DASH clearkey)** — paste `KID:KEY` hex pairs into **Decryption keys**, one per line. The engine decrypts each init/media segment before it enters the output playlist, matching the segment's declared KID against your pairs. Supply exactly one pair and it's used regardless of the segment's KID; supply several and a segment whose KID matches none of them is left undecrypted and logged as an error rather than silently mangled with the wrong key.

This works **whatever DRM the manifest advertises** — Widevine, PlayReady, FairPlay and clearkey are just different license-delivery wrappers around the same CENC bytes, so a multi-DRM asset decrypts fine given its clearkey pair. What is *not* possible is **acquiring** a key from a real license server without already having it; that needs a certified proprietary CDM. `cbcs` pattern encryption is also unsupported (full-sample `cenc`/CTR only).

**HLS AES-128** — fill in **HLS key** (hex) and optionally **HLS IV**. Segments are decrypted server-side and `#EXT-X-KEY` is stripped from the playlist served. A blank IV defaults to the segment's media sequence number, per RFC 8216 §5.2.

Both run on every platform, with no ffmpeg involved.

## Playback endpoints

| Route | Purpose |
|---|---|
| `/play/<id>/index.m3u8` | The universal HLS link — what the panel player, copy-link and M3U export all use. |
| `/play/<id>/index.mpd` | Live DASH manifest for the same `kind=mpd` stream, generated from the same segments. Uses `<SegmentList>` rather than `<SegmentTemplate>`, since filenames aren't a fixed-width formula; segment durations are nominal, not frame-exact. |
| `/restream/<id>/<name>` | The segments the playlists point at. |
| `/direct/<id>/<repId>` | Raw fMP4 tail. The connection stays open: init segment once, then each new segment appended to the same response. No playlist, no polling. `<repId>` is optional for single-representation streams. |
| `/direct/<id>.ts` | The same never-ending tail, but with video and audio muxed together into one MPEG-TS stream, for players that won't assemble two separate fMP4 renditions themselves. A rendition that stalls is dropped from the mux and the mux is rebuilt when it comes back, which re-seats every viewer on the next keyframe. |
| `/download/<id>/<repId>` | One-shot download of whatever is currently buffered (`.mp4` suffix optional). Every segment is a self-contained fragmented-MP4 chunk, so concatenating them *is* a playable file — no ffmpeg, no subprocess. |
| `/source/<id>` | Redirect to the origin (or current CDN mirror). |
| `/ping` | Liveness probe: `{"status":"ok",...}`. Deliberately unauthenticated so a supervisor, container healthcheck or uptime monitor can use it without holding a credential, and deliberately says nothing about the install. |

**Live DASH segment URLs are opaque.** A `kind=mpd` playlist names segments `<repIndex>_<sequence>.m4s` and `<repIndex>_init.mp4`, and carries no upstream address at all. Earlier builds embedded the origin URL in the playlist, which handed every viewer the CDN address complete with its session token. There is nothing to fetch inline anyway: the engine has already downloaded and decrypted a segment before it is ever advertised, so a request is a memcpy out of memory. A miss returns `404` immediately so the player reseeks to the live edge — the recovery path every player already implements — instead of stalling the server on a synchronous origin fetch for a token that has usually expired.

`kind=m3u8` passthrough keeps the `?u=<url>` form, because there is no engine behind it and it is a genuine per-request proxy.

Streams also export as M3U for external players: the list icon on a provider card (`GET /api/providers/<id>/playlist.m3u8`), or **Export playlist (all)** (`GET /api/playlist.m3u8`). Entries carry `tvg-id`, `tvg-name`, `group-title` and `tvg-logo`. `tvg-id` is what binds guide data to a channel in TiviMate, Kodi or Jellyfin — set it per stream with **EPG channel id** in the editor, or leave it blank to use the stream id. The playlist lists every stream, but opening one doesn't start it — `kind=mpd` streams must be **Start**ed from the panel first, or the URL 404s.

## Script providers

Instead of a fixed manifest URL, a provider can delegate to an external script — Python, `sh`, or any executable — that owns login, session persistence, manifest resolution and optionally DRM key acquisition. ReStreamAir only spawns it and exchanges text on stdin/stdout. **[SCRIPTING.md](SCRIPTING.md)** is the how-to guide; this is the summary.

Configure a **Script path** (plus optional **Bind** / **DoH URL** / **Worker**, forwarded verbatim) and one or more **Accounts** — a Username/Password pair, either of which may be blank for token-only or pairing-only flows, with an **Enabled** toggle. **Account selection** picks between *Active* (one marked account), *Rotate* (each call uses the next enabled account) and *Random*.

**Script actions are opt-in.** A provider declares which actions its script implements as a checkbox grid in Provider settings; ReStreamAir never calls anything unticked, so a script that only does `channels` is never handed a `start` it can't answer. Each stream inherits that set and can override it — including overriding to nothing, to keep the script away from one particular stream.

| | Actions |
|---|---|
| **Account** | `login`, `pair` |
| **Catalogue** | `channels`, `events`, `epg` |
| **Lifecycle** | `start`, `stop`, `heartbeat` |
| **Pipeline** | `manifest` (session manifest), `url` (rewrite a URL before fetching), `downloadmanifest`, `pssh`, `initparse`, `cdm` |
| **Not wired yet** | `downloadinit`, `downloadmedia` — configurable now, but nothing calls them: a subprocess per segment needs a persistent worker rather than one spawn per fetch |

The pipeline hooks fail soft — a hook that errors, times out or prints nothing is logged and ReStreamAir carries on with its built-in behaviour, so a broken script degrades rather than taking the stream down.

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

## Monitoring and logs

The **Server** view is the landing page, pushed over Server-Sent Events at `/api/events` about once a second:

- **Input / output bandwidth** — bytes pulled from sources vs served to viewers, global and per stream, current and all-time, persisted across restarts.
- **Active clients per stream** — distinct clients (by API key, else IP) in the last ~30–60s, counted per stream, so one viewer moving between two streams is counted against the one they're actually watching.
- **Server specs** — CPU model and cores, load average, live CPU/memory/disk/uptime, and all-time peaks.
- **Monitoring tab / active connections** — one row per playback connection: stream, provider, client identity, IP, user agent, uptime, errors and live per-client bandwidth. Searchable. Host CPU, memory, disk, uptime and build information live separately on the **Server** tab.

The **Logs** view shows structured events from each running worker, newest first. It polls `/api/logs` every 2s **only while open**. Toggle Normal/Verbose for the raw JSON, filter by stream, level, or run date, and jump straight to a stream's logs from the icon on its card. Per-stream **Clear** hides everything logged for that stream up to now while leaving new activity visible.

Events worth recognising:

| Event | Means |
|---|---|
| `manifest` | An MPD/playlist poll, with the URL that answered it. |
| `renditions` | The director thread settled on the video/audio/subtitle representations; the master playlist is ready. |
| `discontinuity` | A timeline splice was detected from `tfdt` and marked in the playlist. |
| `subtitleConvert` | A subtitle segment was neither TTML nor WebVTT, so it was skipped instead of being served as something the player can't parse. |
| `pollQueued` / `pollDone` | One line per poll cycle per rendition: what was queued, how deep the pending queue is and how it splits (fetching / waiting / ready / failed), and how many of the stream's shared requests are busy. `pollDone` also carries **media published per second of wall clock** — `1.00x realtime` is keeping up, and sustained below that is the number that matters. |
| `downloadSegment` | One segment: how much media it holds, how long the transfer took, and the achieved KB/s. The duration and the transfer time are deliberately both there — "is the link slow or is it dead" is the first question these logs get asked. |
| `headOfLine` | The segment at the head of the queue was abandoned mid-fetch because it was holding up segments already downloaded behind it. Normal in ones and twos on a flaky path; continuous means the path can't carry the rendition. |
| `lagWatch` / `fallingBehind` | A ~60s window published below 0.95x realtime. The first such window is `lagWatch` — burst-published sources routinely have one quiet minute then catch up — and it becomes `fallingBehind` when a second window agrees or new segments were actually lost. |
| `liveEdgeSkip` | Segments were skipped to stay at the live edge, split into never queued / evicted from a full queue / dropped out of the source's window. A lower bitrate is the real fix; a smaller **Download ahead** keeps the delay down meanwhile. |
| `pollSlow` | A poll cycle overran its own interval, so the engine went straight back to the origin instead of sleeping. Consistently slow means a longer **Poll seconds** or fewer segments per poll, not more polling. |
| `error` | Includes the origin's own response body when it sent one — a `403` from a CDN that explains itself (`{"status":"FRUITION_EXCEED","message":"Limit of concurrent streams reached."}`) now shows that text instead of just the status code. |

Logs are an in-memory ring of the most recent 20,000 entries. They survive a page reload, not a restart — if you need history kept, capture the process's own stdout.

## Interface

A single top bar — brand, view switcher, sign-out. No sidebar. Providers are a horizontal chip switcher with quick-search; **All Streams** shows every stream as a filterable card grid. Cards offer quick-play (floating picture-in-picture), a big-player expand into the editor's Player tab, logs, Start/Stop and Delete without opening the editor; providers add **Start all** / **Stop all** / **Delete all**.

The player starts automatically and always plays HLS — the direct fMP4 and `.mp4` download links sit beside it as explicit opt-ins. **Quality** and **Audio** dropdowns appear only when there's more than one to pick, backed by hls.js's `levels`/`audioTracks`. hls.js runs in debug mode, so its full event log is in the browser console for troubleshooting playback without server logs; it also retries rather than giving up on a recoverable network or media error, which is what a warming stream looks like from the client side.

Dark floating-glass panels with a gradient accent by default, plus a light-theme toggle and a uniform 24×24 line-icon set. Installable as an app (manifest + service worker). No build step — plain HTML/CSS/JS in `public/`, with hls.js vendored locally so playback doesn't depend on a CDN, an ad-blocker's mood, or being online.

## Command line

The binary does one thing — serve the panel — so its argv is short:

```
restreamair [serve] [-p|--port N] [-b|--bind ADDRESS] [--root DIR] [--verbose]
```

| Flag | Purpose |
|---|---|
| `serve` | Optional and a no-op. Accepted because that is how the panel is documented and deployed. |
| `-p`, `--port` | Port to listen on. Defaults to the panel's stored value, else 8787. |
| `-b`, `--bind` | Address to bind. Defaults to the stored value, else all interfaces. |
| `--root` | Serve the panel's static files from this directory. Auto-detects `public/` next to the working directory or one level up. |
| `--verbose` | Full HTTP trace logging; without it, errors only. |

An explicit flag always beats the stored setting — that is what a flag is for — and the Settings page's "takes effect after restart" means exactly that.

`build/restream_selftest` is the second executable: known-answer crypto vectors, frozen goldens for the ported modules, and the live engine's policy tables. It takes no arguments and is what CI runs.

There are no `dash` / `live` / `cdmprobe` subcommands. Manifest inspection now happens through the panel's `/api/probe` (the stream editor's paste-to-detect), and live DASH runs in-process rather than as a worker the panel spawns for itself.


## Building from source

**Prerequisites**: cmake, a C11/C++17 compiler, and the development headers for libcurl and libxml2. On Debian/Ubuntu:

```bash
sudo apt-get install -y cmake build-essential libcurl4-openssl-dev libxml2-dev
```

On macOS, Xcode Command Line Tools plus `brew install libxml2 pkg-config` (libcurl comes with the SDK). There is nothing to vendor and no package manager step — cJSON and mongoose are already in `core/deps/`.

```bash
cmake -S . -B build && cmake --build build
```

That produces `build/restreamair-server` (the panel) and `build/restream_selftest`. Run it from the repo root so it finds `public/`, or point it at the directory:

```bash
./build/restreamair-server --port 8787 --root public
```

The same commands build on macOS, Linux and Windows, under Clang, GCC and MSVC. `-DRS_BUILD_SERVER=OFF` builds the libraries and the self-test alone, without libcurl or libxml2 — which is how Windows CI verifies the core.

Tests:

```bash
ctest --test-dir build --output-on-failure
```

```bash
python3 scripts/api-smoke.py --binary build/restreamair-server
```

For the parsers that chew on origin-supplied bytes there's a sanitizer build, which is the tool for a crash that only shows up after hours of real traffic — the process aborts at the instruction that corrupted memory instead of dying somewhere unrelated later:

```bash
cmake -S . -B build-asan -DRS_SANITIZE=ON && cmake --build build-asan
```

## Architecture

Everything is C, in two libraries and two executables:

- **`librestream_base`** — portable logic, no sockets and no third-party code: crypto, AES, URL resolution, the playlist rewriter and parser, state and auth, the panel's control-plane transforms, CENC and HLS decryption, TTML→WebVTT, MPEG-TS muxing, system stats.
- **`librestream_core`** — that plus the live engine and the HTTP server, built on vendored [mongoose](https://github.com/cesanta/mongoose).
- **`restreamair-server`** — the panel executable. Thin: it parses argv, registers the handlers that need libcurl (fetch) and libxml2 (MPD parsing) through function pointers, and runs the poll loop.
- **`restream_selftest`** — links `librestream_base` alone, which is why the core has to stay free of the server's dependencies.

That split is load-bearing rather than tidy: because the fetch, DASH and probe handlers are registered at runtime, the core and the self-test compile on a platform where libcurl and libxml2 are awkward to get — `-DRS_BUILD_SERVER=OFF` is how Windows CI verifies the core on every run.

`/ping` reports the build time, so a stale binary is one request away from being identified.

**What it does:**

- **Auth** — create an account, sign in, sign out, with roles, session persistence across restarts and login throttling. What is stored is the SHA-256 of the session token, never the token itself: `state.json` is a file an operator copies around, and a bearer token in it would be a spare key to the panel.
- **Full management** — create, edit and delete providers, streams, users and API keys.
- **Live monitoring** — the Monitoring view's `/api/events` SSE stream with bandwidth, per-stream viewer counts, and per-client/IP rates; host stats have their own Server view.
- **Source auto-detect** — `/api/probe` fetches a DASH/HLS source and lists its qualities, audio tracks and any DRM KIDs, so the stream editor's paste-to-detect works.
- **Live DASH playback** — the background engine below, with ClearKey CENC decryption, the multi-quality master playlist and the audio-delay `tfdt` shift.
- **HLS passthrough** — playlist rewriting, segment proxying, AES-128 decryption.
- **Direct-source playback** — `/source/<id>` and a direct-source stream's `/play` link redirect to the origin, gated by the same API-key rule.
- **Direct links and downloads** — `/direct/<id>` (fMP4 tail), `/direct/<id>.ts` (video and audio muxed into one MPEG-TS stream) and `/download/<id>`.
- **Script providers** — every wired action, run off the request thread.
- **Export and import** — `/api/playlist.m3u8`, per-provider M3U, provider export and import, and the stored EPG.

**What it does not do yet.** The **ffmpeg** and **N_m3u8DL-RE** input modes are not implemented — the process supervisor for them exists (`apps/server/ffrun.c`) but nothing is wired to it, so a stream configured for one of those input modes gets an honest `501` rather than a silent failure. **Logs are an in-memory ring** (20,000 entries) behind `/api/logs`: they survive a page reload, not a restart. `state.json` is never rewritten field-for-field — the whole file is held as a DOM and written back in full, so a field this build doesn't model is preserved rather than dropped.


### The live engine

`kind=mpd` playback is a background pipeline, not a translation done inside the request handler. Per stream it runs a **director thread** (rendition discovery and the master playlist), and splits each representation's work three ways:

- a **poller** that re-reads the MPD on a strict cadence and queues whatever is new,
- a small pool of **persistent download threads** that fetch queued segments concurrently, and
- a **writer** that CENC-decrypts and commits them strictly in queue order into a bounded in-memory queue (capped by both segment count and 64MB), and renders the media playlist.

Those three used to be one thread doing poll → download → publish in a loop, and that coupling was the worst thing about the engine: a poll whose downloads took 135 seconds did not merely deliver late, it stopped the manifest being re-read for 135 seconds, so the next poll asked for a wider window, which took longer again. Splitting the roles is what both reference implementations do — streamlink's `HLSStreamWorker`/`HLSStreamWriter`, N_m3u8DL-RE's producer feeding a buffer block. Committing in queue order *without* waiting for each batch is the other half: segment N+3 can finish long before N and simply waits its turn, so a slow segment never leaves the network idle.

Playlist requests are a string copy and segment requests a memcpy, so **no playback route touches the network**, and mongoose's single event loop is never blocked. Running streams resume by themselves after a restart.

Some details that matter in practice:

- **Media sequence** is the append counter, per RFC 8216 §4.3.3.2 — not derived from `$Time$`, which doesn't advance by 1 on a timeline with varying `@d`.
- **Segment identity** is the manifest's own `$Time$` plus the segment filename, not the URL. Token-authenticated origins mint a fresh URL for the same segment on every poll (and rotate CDN hostname too); keying on the URL made the whole window look new every time and raced the media sequence ahead of the real timeline.
- **The poll interval is a period, not a delay** — time already spent is subtracted from the sleep, and a poll that overruns goes straight back to the origin. Sleeping the full interval *after* the work meant a two-second `minimumUpdatePeriod` with a one-to-three-second poll advanced four seconds of media every four to five seconds of wall clock, which never builds a cushion.
- **The queue bound is the catch-up policy**, and which end it drops from is the whole of it. When the pending queue is full, the *oldest* queued segments are evicted to make room for the newest the poll just found. Doing it the other way round — refusing the new ones because the queue is full of old ones — is worse than not dropping at all: it pins the engine a fixed distance behind the live edge forever, publishing forty-second-old media and discarding everything current even after the path recovers. Segments already being fetched are never dropped, since the work is paid for; what is dropped leaves a `tfdt` jump, which surfaces honestly as an `EXT-X-DISCONTINUITY`.
- **A queued segment takes the fresh URL from every poll.** Signed CDN URLs expire in about a minute, and an expired one is often answered with silence rather than a refusal, so the fetch burns its entire timeout — and because the writer commits in order, one of those stalls everything already downloaded behind it. A queued item therefore keeps its identity and re-adopts the URL the current manifest advertises, and anything the source has stopped advertising is dropped rather than left to rot. That is the age bound, asked as "is this still on offer" rather than as a number of seconds.
- **The head of the queue is abandoned** once it has been fetching for several times its own duration with finished work stuck behind it, or at a looser bound when nothing is finishing at all. Slots carry a generation stamp, so a late result from an abandoned fetch is discarded instead of landing on whatever occupies that slot next. Two attempts per segment, not three: against an origin that answers a dead URL with silence, a third attempt is another full timeout spent on a segment already too late to play.
- **Timeouts are sized for bytes, not seconds.** A segment's whole-request timeout is 10× its media duration, floored at 20s and capped at 30s. Duration says nothing about how many bytes have to move: on one production proxy a 2s video segment is ~745 KB and needs about twelve and a half seconds at the ~60 KB/s a single connection gets there, so a duration-derived timeout killed every video segment fractionally short while the 25 KB audio segments beside it cleared the same bar trivially. There is deliberately **no low-speed abort** — libcurl's low-speed clock runs while waiting for the first byte, and time-to-first-byte on these paths is routinely several seconds, so it killed requests that were about to succeed, including the init segment that carries the timescale and the decryption key.
- **Connections are one budget per stream, with a floor for each rendition.** `parallelDownloads` (default 6, capped at 8) is shared by video and audio rather than allowed to each. Per-connection rate on a proxied path is roughly fixed, so aggregate throughput is very nearly a function of how many connections are open — a 3 Mbps rendition wanting ~370 KB/s is six connections at that rate and simply unobtainable from two. Each rendition reserves a slot only for the *other* renditions that actually have work waiting, and may use everything else: audio can't be shut out by video's much deeper queue, and isn't held down to one connection when video is idle. Note that this is the engine's budget — the stream's own `parallelDownloads` still caps it.
- **Blocking calls run on worker threads.** Probe, logo lookup, script actions and HLS passthrough fetches all hand off to a worker and return through `mg_wakeup()`; a cold or geo-blocked stream answers `503` + `Retry-After` immediately rather than making every other stream wait.
- **Reduce manifest polling** (`reducedManifestPolling`, off by default) backs the director off from a 15s to a 300s re-read once renditions are known. Enable it when a token caps concurrent sessions low enough that the director plus rendition workers trip the limit.

### Testing

Three kinds of check, all in `restream_selftest`:

- **Published vectors** — NIST/FIPS/RFC test vectors for SHA-256, PBKDF2 and AES, so a build proves it produces the right bytes on that platform rather than merely compiling. This is what keeps an admin password hashed by an older build still verifying today.
- **Frozen goldens** — the expected output of the URL resolver, playlist rewriter and ffmpeg argument builder for the fixtures in `core/tests/fixtures.h`. These are a contract, not a snapshot: changing one changes what viewers and ffmpeg actually receive, so it takes a deliberate edit with a reason, never a paste of what the code now prints.
- **Policy tables** — the live engine's decisions under load (the catch-up eviction plan, the falling-behind classifier) are written as pure functions precisely so they can be tested exhaustively, including their edge cases. What a stream does when the path degrades is exactly what cannot be exercised reliably against a live origin.

`scripts/api-smoke.py` covers the layer no unit test reaches: it runs the real server in a scratch directory and drives it over HTTP — the auth gate, the viewer role, the login throttle, cookie attributes, session persistence across a restart, the mode of `state.json`, the M3U export and the provider export/import round trip.

CI runs both suites on Linux and macOS, plus a third pass under AddressSanitizer and UBSan, and compiles the core on Windows. Fixtures are generated: rerun `scripts/gen-fixtures.py` after editing its tables.

Trusted-proxy IP/CIDR matching lives in the core rather than in the request handler for the same reason the goldens are frozen — getting `10.0.0.0/8` wrong is a security bug in either direction, so it is one implementation with its own tests instead of a comparison written twice.


## Deployment

The panel speaks plain HTTP and has no TLS listener of its own. On a trusted LAN that is fine. Anywhere else — anything reachable from the internet — put a terminating proxy in front of it, because otherwise the sign-in form and the session cookie cross the network in the clear.

### Docker

```sh
docker build -t restreamair .
docker run -d --name restreamair -p 8787:8787 -v restreamair-data:/data restreamair
```

Everything mutable (`state.json`, `runtime/scripts/`, `logo-cache.json`) resolves relative to the working directory, so `/data` is the only volume that matters: back that up and you have backed up the install. The image builds from source in an `ubuntu:24.04` stage, runs the self-test before shipping, and runs as an unprivileged user with a healthcheck on `/ping`.

### Docker with HTTPS

`docker-compose.yml` runs the panel behind [Caddy](https://caddyserver.com), which obtains and renews a real certificate on its own:

```sh
RESTREAMAIR_DOMAIN=panel.example.com docker compose up -d
```

The panel port is not published — only Caddy can reach it — so the plain-HTTP listener is never exposed. After the first sign-in, set **Trusted proxies** (below).

### systemd

`deploy/restreamair.service` is a hardened unit for a bare-metal install; the header comment carries the install steps. It binds `127.0.0.1` by default, expecting a proxy in front. Copy `public/` alongside the binary — the unit sets `WorkingDirectory=/var/lib/restreamair`, and the panel looks for its static files there.

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

- `state.json` — providers, streams, admin accounts, API keys, settings, all-time bandwidth and peak totals. **Contains script account passwords** — treat it as a credentials file.
- `runtime/scripts/<providerId>/` — a script provider's durable session directory. Created by the panel, never read by it, deleted with the provider.
- `logo-cache.json` — name → logo URL cache (empty string records a confirmed miss).

Segments are **not** on disk. The live engine holds each rendition's queue in memory, bounded by both a segment count and 64MB, and a playback request is a memcpy out of it — so there is nothing here to clean up, and nothing to leak through the static file handler.

`state.json` is created `0600` and re-tightened on every save. It holds admin password hashes, session token hashes, API keys and — for script providers — account passwords in the clear, since the script has to be handed the real thing. Treat a provider export the same way: it embeds those passwords too.

Installs still using the old `data/` + `public/runtime/` layout are migrated automatically on the next start, and nothing is overwritten if a new-layout file already exists.

## Troubleshooting

**`/play` returns 503 for a few seconds after Start.** Expected — the engine answers `503` + `Retry-After` while the first segments are still arriving rather than blocking. Players retry on their own. A stream that *stays* at 503 never became ready: check the Logs view for the manifest fetch.

**Players stall and the logs show segment 404s.** They're asking for segments that already aged out of the window. Raise **Keep count** (and **Playlist count**), or **Poll seconds** if `pollSlow` also appears — a poll that consistently overruns can't stay ahead of the live edge.

**The origin returns 403 and it isn't obvious why.** The error line now carries the origin's own response body when it sent text back; concurrent-stream limits in particular usually say so explicitly.

**A concurrent-stream limit trips with only one stream running.** A video+audio stream normally makes three concurrent manifest fetches. Tick **Reduce manifest polling** in the stream's DASH options to drop it to two.

**Video plays but audio drifts.** Set **Audio delay (ms)** — positive or negative — in the stream's DASH options.

**Segments download but produce no picture.** If the source is `cbcs` pattern-encrypted rather than `cenc`, decryption isn't supported. Check the Logs view for a key-mismatch error, which is now reported rather than silently decrypting with the wrong key.

**A static/VOD MPD is rejected.** Tick **Allow offline/static MPD** in DASH options (`forceOffline=true` on the CLI).

**Behind a reverse proxy, every viewer looks like one client.** Set **Trusted proxies** — see [Running behind a reverse proxy](#running-behind-a-reverse-proxy).

## License

MIT — see [LICENSE](LICENSE).

Three vendored components keep their own terms. [cJSON](https://github.com/DaveGamble/cJSON) is MIT and [hls.js](https://github.com/video-dev/hls.js) is Apache-2.0, both compatible. [mongoose](https://github.com/cesanta/mongoose) is **GPLv2 or a commercial license from Cesanta**, and it is the HTTP server every build now links — so a `restreamair` binary you distribute is a combined work under mongoose's terms. Running it yourself is unaffected; distributing it means either complying with GPLv2 or holding a licence from Cesanta.
