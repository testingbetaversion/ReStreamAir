# ReStreamAir external HTTP API

The web panel and external clients use the same HTTP routes. This reference
covers the C/C++ server in this repository. Routes are relative to your server,
for example `https://streams.example.com` or `http://127.0.0.1:8787` locally.

- [Authentication](#authentication)
- [Browser access and CORS](#browser-access-and-cors)
- [Refresh intervals](#refresh-intervals)
- [State and logs](#state-and-logs)
- [Providers](#providers)
- [Streams](#streams)
- [Scripts and scheduled events](#scripts-and-scheduled-events)
- [Accounts and playback keys](#accounts-and-playback-keys)
- [Server settings and tools](#server-settings-and-tools)
- [Playback and Xtream](#playback-and-xtream)
- [Errors and compatibility](#errors-and-compatibility)

See [EVENTS.md](EVENTS.md) for the complete SSE payload, a browser streaming
example, log event names, scheduled-event fields, and outgoing error webhooks.
See [SCRIPTING.md](SCRIPTING.md) for the provider subprocess protocol.

## Authentication

| Surface | Authentication |
|---|---|
| `/api/*` management | Panel username/password in an explicit HTTP Basic header, or a panel session cookie. |
| `/api/auth/*` | Public setup/login/status/logout routes, described below. |
| Playback | Playback key in `?key=…` or `Authorization: Bearer …`. Open when no playback keys exist. |
| Xtream | Playback key label as `username`, key value as `password`. |
| `/ping`, panel assets, `OPTIONS` | Public. |

An admin may manage everything. A viewer may use management GET routes, except
provider export, which requires an admin. Non-GET management requests from
viewers return `403`. Read access includes sensitive configuration and playback
keys: a viewer is a trusted operator, not an untrusted playback customer.

Playback keys do not authenticate management requests. Basic authentication is
checked per request, shares the login failure throttle, and creates no session.
If an Authorization header is present, it takes precedence over cookies; invalid
explicit credentials do not fall back to a cookie. Use HTTPS outside a trusted
local environment.

```sh
base=http://127.0.0.1:8787
auth='admin:your-panel-password'
curl --fail-with-body --user "$auth" "$base/api/state"
```

### Setup and cookie sessions

| Method and route | Request | Successful response |
|---|---|---|
| `GET /api/auth/status` | None | `{"needsSetup":false,"authenticated":true,"username":"admin","role":"admin"}`. Signed out: username is `null`; only use role when authenticated. |
| `POST /api/auth/setup` | `username`, `password`, optional `remember` | `200 {}` plus session cookie. Only available before the first account exists. |
| `POST /api/auth/login` | Same fields | `200 {}` plus session cookie. |
| `POST /api/auth/logout` | Existing cookie, if any | `200 {}` and expired cookie. Ends that cookie session; Basic credentials remain valid. |

Passwords must contain at least 8 characters. For the current implementation,
`remember` is the **string** `"true"`, not a JSON boolean. It selects a 30-day
session; otherwise the session lasts 24 hours. The cookie is named
`restreamair_session`, is HttpOnly and SameSite=Lax, and gains Secure when the
server recognizes HTTPS. Sessions persist across restarts.

```sh
curl --fail-with-body -c cookies.txt -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"your-panel-password","remember":"true"}' \
  "$base/api/auth/login"
curl --fail-with-body -b cookies.txt "$base/api/state"
curl --fail-with-body -b cookies.txt -c cookies.txt -X POST "$base/api/auth/logout"
```

## Browser access and CORS

CORS is enabled for every origin without an allowlist. API responses, SSE,
playlists, media, redirects, static files and application errors include:

```http
Access-Control-Allow-Origin: *
Access-Control-Expose-Headers: Content-Length, Content-Range, Accept-Ranges, Retry-After, Content-Disposition, Location, X-Accel-Buffering
```

`OPTIONS` is answered before authentication, on any path, with `204` and no body:

```http
Access-Control-Allow-Methods: GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS
Access-Control-Allow-Headers: Authorization, Content-Type, Range, Accept, Last-Event-ID, <requested custom headers>
Access-Control-Max-Age: 86400
Vary: Access-Control-Request-Headers
```

Requested header names are validated and echoed, so custom client headers work
without configuring the server. A successful preflight does not authenticate a
request or add a route/method that is absent from this reference.

For an external website use `credentials: "omit"` and an **explicit** Basic
Authorization header. Wildcard CORS does not support `credentials: "include"`;
`Access-Control-Allow-Credentials` is intentionally absent. Same-origin panel
sessions continue to work. Native EventSource cannot set custom authorization
headers; use the [fetch streaming example](EVENTS.md#external-browser-client).
These rules follow the [Fetch CORS standard](https://fetch.spec.whatwg.org/#http-cors-protocol)
and [EventSource interface](https://html.spec.whatwg.org/multipage/server-sent-events.html#the-eventsource-interface).

CORS cannot override HTTPS-to-HTTP mixed-content blocking, a website's CSP
`connect-src`, browser local-network permissions, TLS certificate errors, or
network/firewall reachability. For an HTTPS website use an HTTPS API URL. A
source redirect also depends on that source server's own CORS policy. Configure
a reverse proxy to pass these headers through without adding a second
`Access-Control-Allow-Origin` header.

### Copyable browser request helper

Obtain the credentials from your application's sign-in input; do not embed an
administrator password in a publicly distributed JavaScript bundle.

```js
const base = "https://streams.example.com";

function basicAuthorization(username, password) {
  if (username.includes(":")) throw new Error("Basic usernames cannot contain ':'");
  const bytes = new TextEncoder().encode(`${username}:${password}`);
  return "Basic " + btoa(Array.from(bytes, byte => String.fromCharCode(byte)).join(""));
}

async function api(path, authorization, { method = "GET", body, signal } = {}) {
  const response = await fetch(new URL(path, base), {
    method, signal, credentials: "omit", cache: "no-store",
    headers: {
      Authorization: authorization,
      ...(body === undefined ? {} : { "Content-Type": "application/json" }),
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const payload = await response.json();
  if (!response.ok) {
    const error = new Error(payload.error || `HTTP ${response.status}`);
    error.status = response.status;
    error.retryAfterSeconds = Number(response.headers.get("Retry-After")) || 0;
    throw error;
  }
  return payload;
}

// username and password come from your sign-in form.
const authorization = basicAuthorization(username, password);
const state = await api("/api/state", authorization);
const stream = state.providers.flatMap(provider => provider.streams)[0];
if (stream) {
  await api(`/api/streams/${encodeURIComponent(stream.id)}/start`, authorization,
    { method: "POST" });
}
```

For text/binary routes use `response.text()` or `response.arrayBuffer()` instead
of this JSON helper. For SSE read `response.body` incrementally.

## Refresh intervals

All four settings are saved in `state.json` and configurable from **Settings →
Refresh intervals** or `POST /api/settings`. Fields are optional: omitted values
remain unchanged. Units are **milliseconds**, accepted values are integer
`100`–`3600000` inclusive, or `0` to pause periodic updates. Invalid types,
fractional values and out-of-range numbers return `400` without changing any
settings in the request.

| Field | Default | Controls |
|---|---:|---|
| `eventsIntervalMs` | 1000 | Default SSE monitoring snapshot interval. Existing subscribers without an override adopt it without reconnecting. |
| `stateRefreshMs` | 4000 | Built-in panel `/api/state` polling. |
| `logsRefreshMs` | 2000 | Built-in Logs view polling while the view is open and not manually paused. |
| `activityRefreshMs` | 1000 | Built-in script, stream-script test, catalogue import and installer progress polling. |

```sh
curl --fail-with-body --user "$auth" -H 'Content-Type: application/json' \
  -d '{"eventsIntervalMs":500,"stateRefreshMs":5000,"logsRefreshMs":2000,"activityRefreshMs":750}' \
  "$base/api/settings"
```

The saving panel applies changes immediately, including polls already running.
Other panels adopt settings on their next state or event snapshot. If both state
polling and event snapshots are paused, reload the page or open Settings to read
new settings. Manual actions and their final responses still work with polling
paused. External applications choose their own state/log polling schedules;
these settings do not force an external application's timers.

Each SSE connection may override the default:

```sh
curl --no-buffer --fail-with-body --user "$auth" \
  "$base/api/events?intervalMs=2500"
```

`intervalMs` uses the same range and units. Omit it to inherit the live default;
`intervalMs=0` sends the initial snapshot only, followed by connection keepalives.
Reconnect with a different query to change an override. A default change does
not overwrite an explicit override. Snapshots are best-effort, subject to the
server event loop, processing and network latency, not a hard real-time clock.
Pausing monitoring never pauses streams or server maintenance.

### Source polling is a separate setting

Stream `pollInterval` is in **seconds**, accepts fractions, and uses `0` for
**automatic**, not paused. Automatic DASH polling uses MPD `minimumUpdatePeriod`
or 2 seconds. The current live engine caps its polling period at 10 seconds and
its calculated safe source window; `reducedManifestPolling` can raise the period
to one segment duration. Failures use retry backoff. HLS passthrough fetches
playlists on player requests; `pollInterval` does not set a browser player's
playlist reload interval. HLS segment duration, playback buffering and provider
heartbeat settings also do not control `/api/events`.

To edit a stream, read it from state, merge your changes into the full object,
and send PUT. The current PUT implementation resets its status to stopped; call
Start afterward when it should be running again.

```js
const state = await api("/api/state", authorization);
const stream = state.providers.flatMap(p => p.streams).find(s => s.id === streamId);
if (!stream) throw new Error("Stream not found");
const wasRunning = stream.running;
await api(`/api/streams/${encodeURIComponent(stream.id)}`, authorization,
  { method: "PUT", body: { ...stream, pollInterval: 5 } });
if (wasRunning) await api(`/api/streams/${encodeURIComponent(stream.id)}/start`,
  authorization, { method: "POST" });
```

## State and logs

| Method and route | Result |
|---|---|
| `GET /api/state` | `200` full configuration view described below. |
| `GET /api/events[?intervalMs=N]` | `200 text/event-stream`; monitoring snapshots, not configuration state or lifecycle notifications. See [EVENTS.md](EVENTS.md). |
| `GET /api/logs[?streamId=ID&limit=150]` | `200 {"entries":[…],"availableDates":[]}`; newest first. |
| `DELETE /api/logs[?streamId=ID]` | Clear matching visible history, return the same log envelope. Omit ID to clear all logs. |
| `GET /ping` | `200 {"status":"ok","build":"…"}`; binary build date/time, no authentication. |

State's top-level fields:

| Field | Shape / meaning |
|---|---|
| `providers` | Array of provider configurations, each containing `streams`. |
| `apiKeys` | Array of playback key objects, including the actual key values. |
| `refresh` | Four effective refresh settings, including defaults for older installations. |
| `settings` | Legacy state view containing saved `port`. Use `/api/settings` for the active listener and complete settings. |
| `system`, `bandwidth`, `version` | Currently empty placeholder objects in this state view. Live system/global metrics come from `/api/events`. |

Stream state contains stored configuration plus `running` (boolean), `status`
(`running` or `stopped`), `lastError` (nullable), `activeClients`, `bandwidth` and
`inputBandwidth` (`bytesPerSecond`, `allTimeBytes`), and the URL fields `playUrl`,
`sourceUrl`, `directUrl`, `directStreamUrls`, `downloadUrls`. These URLs may use a
unique name slug; IDs are preferable for integrations. Generated absolute URLs
currently use `http://` and the request Host. Behind HTTPS, construct playback
URLs from your configured public base URL and stream ID rather than trusting
the returned scheme.

IDs are opaque. Read them from responses and percent-encode them when inserting
into paths. There is no state pagination, field selection, conditional update,
PATCH operation, event replay cursor, or API version prefix. Tolerate additional
fields and missing optional fields. Do not modify `state.json` while running.

Log `limit` defaults to 150 for omitted/nonpositive values and is capped at
20000. `streamId` omitted/empty means all, `__panel__` means management activity,
and `script:<providerId>` means provider-script output. These are exact filters;
level, text and date filtering are client-side. See [log schema](EVENTS.md#logs).

## Providers

| Method and route | Body / successful response |
|---|---|
| `POST /api/providers` | `{"name":"News","logo":"https://…"}`; `200` state. Name required; creation accepts name/logo and shared network/webhook fields below. Set scripting fields with PUT. |
| `PUT /api/providers/<id>` | Full provider settings object; `200` state. |
| `DELETE /api/providers/<id>` | No body; `200` state. Removes provider and streams, and deletes script session files. Stop streams before deleting their provider. |
| `GET /api/providers/<id>/export` | Admin only; JSON attachment described below. |
| `POST /api/providers/import` | Export envelope; `200` state with a newly assigned provider ID and stream IDs. |
| `GET /api/providers/<id>/playlist.m3u8` | M3U text for that provider. |
| `GET /api/playlist.m3u8` | M3U text for all streams. |
| `GET /api/providers/<id>/epg` | Last stored script EPG as XML or JSON; `404` if absent. |
| `POST /api/providers/<id>/webhook/test` | `202 {"ok":true,"queued":true}`; delivery is asynchronous. |
| `GET /api/logo-lookup?name=<encoded-name>` | `200 {"url":"…"}`; no match returns `404`. |

Both M3U export routes accept `?key=<playback-key>` to embed that account's
credential in every complete playback URL. With no selection, they use the
first configured key; with no keys, links are open. An invalid selected key
returns 400. Exports use the request's HTTP/HTTPS scheme, honoring configured
trusted proxies. Downloading these management exports still requires panel
authentication; use the credentialed Xtream `/get.php` URL in an IPTV client.

Provider PUT requires `name`. Most omitted fields reset to these defaults;
merge your edits into the provider object from state to retain other settings.
`logo` and `scriptActions` are preserved when omitted. The nested `options` object merges supplied fields and preserves omitted fields. `streams` and computed
`scriptSessionDir` are not writable through provider PUT.

| Editable fields | Type / default / purpose |
|---|---|
| `name`, `logo` | Strings. Nonblank name; logo is optional. |
| `proxy`, `headers` | Strings, `""`. Proxy list and newline-separated `Header: value` lines. |
| `downloader`, `downloaderParams` | Strings, `""`. Downloader: `native`, `curl`, `wget`, `aria2c`; `internal`/`libcurl` normalize to `native`. Availability depends on the pipeline. |
| `forceIpv6`, `rotateProxies` | Booleans, `false`. |
| `segmentUrlParams`, `inheritUrlParams` | String `""`, boolean `false`. Fixed segment query and inheritance of the effective manifest URL's query. |
| `errorWebhookUrl` | String `""`. Outgoing provider-error destination; [webhook contract](EVENTS.md#outgoing-error-webhooks). |
| `scriptPath` | String `""`. Path on the **server** filesystem. |
| `scriptBind`, `scriptDoh`, `scriptWorker` | Strings, `""`, passed to the script. |
| `scriptAccounts` | Array, `[]`. Entries: `id`, `name` (account username), `password`, optional `enabled` boolean. |
| `activeScriptAccountId` | String `""`. Unknown ID falls back to first account ID. |
| `accountSelectionMode` | `fixed` (default), `rotate`, `random`. |
| `scriptActions` | String array. New providers default to `login`, `pair`, `channels`, `events`. Empty/absent provider action lists use those legacy defaults. |

Provider **Streaming options** are saved in `options` on create/PUT and included
in provider export/import. `GET /api/state` also returns `providerOptionFields`,
with each field's label, group, type, default, bounds, hint and `inactive` reason.
Invalid types, out-of-range or fractional numbers, and multiline text return
`400` before changing the provider. Single-line text is limited to 4096 characters.
Restart running streams after changing playback options. Request headers and
script timeouts apply to subsequent requests/actions.

| Active option | Behaviour |
|---|---|
| `userAgent`, `xForwardedFor` | Dedicated upstream headers; blank uses downloader defaults. Matching generic `headers` entries take precedence. |
| `scriptTimeoutSeconds` | Each manual or playback script action, 1–3600 seconds; default 30. |
| `pipeCommand` | Fallback when a Program pipe stream has no command of its own. |
| `hlsFragmentDurationSeconds` | Internal DASH / FFmpeg HLS output target, 1–30 seconds; 0 keeps stream settings. |
| `hlsPlaylistDurationSeconds` | Converts target duration to output fragment count, rounded up and bounded to 3–240; 0 keeps stream settings. |
| `outputFragmentsCount` | Output window of 3–240 fragments; nonzero overrides playlist duration. 0 uses duration or stream settings. |
| `playbackDelaySeconds` | Internal DASH playout delay, 1–120 seconds; 0 keeps stream settings. |
| `dontWaitForFullPlaylist` | Internal DASH can publish after one complete, released segment. Default false. |
| `maxStreamsConcurrency` | Maximum streams marked running under this provider; excess starts return `409`. 0 = unlimited. Existing running streams are retained when lowering the cap. |
| `maxEventsCount` | Maximum valid events processed per Load events import. 0 = unlimited; existing events beyond the cap are retained. |

HLS output overrides apply to generated output. All provider controls have
backend behavior; pipeline-specific controls identify their scope in the schema.

| Additional option | Behaviour |
|---|---|
| `alwaysResetSession` | Clears an idle provider's stored session before login or its first stream start. Concurrent streams retain their shared active session. |
| `useSessionCookies` | Uses the provider script cookie jar for upstream requests, with serialized built-in HTTP requests to protect the jar. |
| `httpGetTimeoutSeconds`, `httpGetAttempts` | Per-attempt timeout and attempt limit for manifest/media/probe downloaders. Permanent 4xx responses are not retried, except 408/429. FFmpeg receives the socket timeout and uses its own reconnect policy. |
| `maxDownloadConcurrency` | Shared provider budget for manifest/media/probe downloaders; FFmpeg owns its internal connections. |
| `detectJsonRedirect` | Follows recognized HTTP(S) JSON URL fields at the root or under `data`, at most five hops. Relative playlist paths resolve against the final URL. |
| `defaultCdn` | Selects `Name`/`name` from the manifest script's `Cdn` list. An unmatched configured name fails visibly. |
| `defaultVideo`, `defaultAudio` | Internal DASH ordered comma-separated preferences: `best`, `worst`, `id=ID`, `lang=ur`, `codec=avc`, `height<=720`, `bandwidth<=2000000`. Explicit stream selections win. |
| `legacyDashParser` | Internal DASH XML recovery mode; default parsing is strict. |
| `useDashDelay` | Honors MPD `suggestedPresentationDelay`, bounded to 120s, taking the larger of the source delay and stream/provider buffer. |
| `ignoreDashStaticFlag` | Continues polling a static MPD. Otherwise a drained static source publishes ENDLIST. |
| `noRestartOnError`, `restartFinishedBroadcast` | Controls DASH/FFmpeg recovery after an error or completion. Finished DASH buffers drain before restarting. Finished FFmpeg output remains available. |
| `noRestartOnTrackChange` | Switches discovered DASH tracks in place. When false, changing selected IDs schedules a clean restart. |
| `stalledStreamTimeoutSeconds` | DASH/FFmpeg recovery when new media/output timestamps stop advancing. |
| `restartDelaySeconds`, `coolDownAutoRestart` | Base restart delay; optional exponential cooldown for repeated failures, capped at 300s or the configured delay when larger. Healthy operation clears the failure count. |
| `retryNewManifestCount` | Additional fresh DASH reads per failed poll. Every configured CDN mirror still gets a chance. |
| `offAirFallback`, `offAirFallbackUrl` | Internal DASH switches to a configured clear, publicly reachable fallback MPD on failure/completion. Source headers, cookies, keys and track selections are not sent to the fallback. Manual/timed restart returns to the primary. Enabling fallback requires its URL. |
| `autoRefreshEvents`, `eventsRefreshSeconds` | Background events-script imports on the server timer, waiting while other provider script jobs are active. Requires a script and declared events action. |
| `autoRemoveMissingChannels` | Successful imports stop/delete missing imported channels; manually added streams remain. |
| `autoRemoveFinishedEvents` | Stops/deletes imported events at their epoch `End`; no end means retain. |
| `reuseEventIndex` | Reuses an ended, stopped event's public ID for a new event, clearing its old source and keys. Matching names retain identity. |
| `autoRestartPeriodSeconds` | Periodically restarts running streams, with the configured restart delay. Zero disables. |
| `sequentialAutostartPeriodSeconds`, `randomAutostartPeriodSeconds` | Starts one eligible stopped stream each interval in provider order or randomly. Requires stream `autostart: true`, respects event windows and provider concurrency. Zero disables. |
| `epgTimezone` | Re-expresses full XMLTV programme start/stop timestamps in UTC or fixed offsets such as `UTC+05:00`. Blank preserves input, partial timestamps remain unchanged. IANA names are rejected. |

Schedules run without an open panel. Explicit Stop cancels a pending delayed
restart or script start. Timed autostart can select the stream again if its
`autostart` flag remains enabled. Provider settings apply on the next stream start;
request-driven HLS and probes snapshot the latest request policy. Include
`providerId` in `/api/probe` to apply that provider's policy.

Existing `headers` and `inheritUrlParams` remain separate provider fields.

Export is `{"restreamairExport":1,"provider":{…},"streams":[…],"scriptFile":{…}}`.
`scriptFile` is optional and has `filename` and `contentBase64`; it is included
when the configured script can be read. Active script account selection is
exported as `activeScriptAccountName`. Import writes the embedded script to
`scripts/` and may choose a new filename. Export is a transfer format, not a
byte-for-byte backup of every setting; retain `state.json` for full backups.
Both include secrets. Imports are not idempotent: each invocation creates a
provider.

## Streams

| Method and route | Body / successful response |
|---|---|
| `POST /api/providers/<providerId>/streams` | Stream settings; `200` state with the new stream. |
| `PUT /api/streams/<id>` | Full stream editor object; `200` state. Omitted editable values reset to defaults. |
| `DELETE /api/streams/<id>` | Stop and delete; `200` state. |
| `POST /api/streams/<id>/start` | No body or `{}`; `200` state. Script-based starts wait for manifest/key work before replying. |
| `POST /api/streams/<id>/stop` | No body or `{}`; `200` state. Worker shutdown completes asynchronously. |
| `POST /api/probe` | Source probe request below; `200` probe result. |

Create requires nonblank `name` and HTTP(S) `url`. For `inputMode: "pipe"`,
`pipeCommand` is required instead of an HTTP URL. New streams are stopped.
Imported event/channel metadata is managed through script imports and carried
across PUT; it is not writable through the ordinary editor route.

| Stream fields | Type / default / limits |
|---|---|
| `name`, `url`, `logo`, `tvgId` | Strings; name and URL rules above. Others `""`. Blank `tvgId` uses stream ID in playlists/XMLTV. |
| `kind` | `mpd` (default) or `m3u8`. Probe first to detect the source. |
| `representation`, `period` | Strings, `""`; selected DASH representation and period. |
| `representations`, `representationOrder` | String arrays, `[]`; selected representations and their order. |
| `representationMeta` | Object, `{}`; representation metadata indexed by ID. |
| `proxy`, `downloader`, `downloaderParams` | Strings, `""`; stream overrides. Downloader names as above. |
| `manifestHeaders`, `mediaHeaders`, `hlsKeyHeaders` | Strings, `""`; newline-separated `Header: value` lines. |
| `playlistSegments` | Integer, default 6, minimum 3. |
| `hlsSegmentSeconds` | Integer seconds, default 10, clamped 1–30. |
| `playbackDelaySeconds` | Integer seconds, default 0, clamped 0–120. |
| `keepSegments` | Integer, default 10, clamped 1–240. |
| `downloadAhead` | Integer, default 20, minimum 1. |
| `parallelDownloads` | Integer, default 6, clamped 1–8. |
| `pollInterval` | Number of seconds, default 0; negative values normalize to 0. See source-polling constraints above. |
| `forceOffline`, `reducedManifestPolling`, `prioritizeOldest` | Booleans, `false`. Static MPD permission, reduced polling and backlog preference. |
| `audioDelayMs` | Signed integer milliseconds, default 0. |
| `decryptionKeys`, `hlsKey`, `hlsIV` | Strings, `""`. DASH KID:KEY pairs; HLS AES key/IV in hex. |
| `inputMode` | `internal` default, `hlsBuffered`, `ffmpegResident`, `ffmpegTsHls`, `ffmpegMultiTsHls`, `ffmpegFmp4Hls`, `pipe`, `nm3u8dlre`. |
| `outputMode`, `outputTarget` | `hls` default, `srtServer`, `udpSrt`, `custom`; destination string `""`. Some combinations are not implemented. |
| `pipeCommand`, `nm3u8dlreParams` | Strings, `""`; argv-style command / external-tool options. |
| `cdnUrls` | HTTP(S) URL string array, `[]`. Invalid entries are dropped. |
| `cdnHeaders` | Session output: per-CDN manifest/media headers keyed by manifest URL. Populated by the manifest script; preserved on editor saves. |
| `directSource` | Boolean, `false`; redirect playback to source. |
| `useCdm`, `sessionManifest` | Booleans, `false`; enable script key/session-manifest workflows. |
| `scriptParams`, `scriptOverride` | Strings, `""`; flat `key=value` arguments and optional server-side script path override. |
| `scriptActionsOverride` | `null` inherits provider actions; string array overrides them; `[]` disables all stream script actions. |
| `heartbeatEnabled`, `heartbeatSeconds` | Boolean `true`; integer seconds default 0, minimum 0. Stored provider-session metadata, separate from SSE keepalives. No periodic heartbeat scheduler is wired in this C server. |
| `proxyScript`, `proxyManifest`, `proxyMedia` | Booleans, `true`; proxy selection for each fetch category. |

`cdmMode` is normalized to `external`; ordinary stream creation normalizes
`cdmType` to empty. Catalogue imports may supply `CdmType`. Stored enums do not
guarantee a pipeline is available: check the Start response. A successful Start
means startup was accepted, not that a playable segment is already buffered.

`hlsBuffered` requires `kind: "m3u8"`, `outputMode: "hls"` and installed FFmpeg.
It forces `directSource` off. Start arms the stream; the first authenticated
viewer starts one shared downloader. FFmpeg copies the selected video/audio
tracks into a rolling disk buffer and every viewer receives local playlists
and segments. An empty selection uses the first video and audio track.
`playlistSegments` and `hlsSegmentSeconds` control the output window. DASH-only
download-ahead, keep-count and playout-delay settings do not tune this mode.
The initial playlist request waits up to 20 seconds for the buffer, then returns
`503` if it is still unavailable. After 30 seconds
without viewer requests, downloading stops; a later viewer starts a fresh buffer.
Explicit Stop disarms the stream. The private local FFmpeg feed retains the
server's upstream headers, proxy policy and HLS manifest recovery.

```sh
provider_id=provider_from_state
curl --fail-with-body --user "$auth" -H 'Content-Type: application/json' \
  -d '{"name":"News","kind":"m3u8","url":"https://origin.example/live.m3u8"}' \
  "$base/api/providers/$provider_id/streams"
# Read the new stream ID from that response.
stream_id=stream_from_response
curl --fail-with-body --user "$auth" -X POST "$base/api/streams/$stream_id/start"
```

Bulk start/stop/delete is a client loop over selected IDs. Use sequential calls
to avoid simultaneous source/script startup spikes. Do not blindly retry a
create/import or script action after an uncertain network failure: inspect
state/logs first, because the operation may already have completed.

### Probe

Request: `{"url":"https://…","providerId":"…","streamId":"…",
"cdnUrls":["https://backup.example/index.m3u8"],"proxy":"",
"headers":"User-Agent: MyClient","forceIpv6":false,"rotateProxies":false}`.
Supply a URL or a saved `streamId`. The server fetches it. `providerId` applies
provider defaults; `streamId` supplies the saved source, mirrors and stream
settings. Explicit request settings override those defaults, including an
explicit empty proxy. A URL different from the saved stream is probed separately
and cannot refresh or overwrite that stream.

The probe tries the primary and each distinct CDN in order after a failed fetch,
including HTTP 403. If all fail for a saved stream with `sessionManifest` and an
allowed manifest action, it runs that action once and retries the fresh list.
Automatic refresh has a 60-second per-stream cooldown and waits for a later
request if another provider script is busy. HLS proxy playlists use the same
recovery; relative rendition paths are rebased onto mirrors. Unrelated absolute
rendition URLs are not guessed. Recovery returns an error if fresh sources or
the provider script still fail.

Response fields: `kind` (`mpd`/`m3u8`), `representations` (array), `protection`
(object mapping representation ID to KID array), `drm` (object with `kids`,
`pssh`, `psshWidevine`, `psshPlayReady`, `keyUris`). Each representation includes
`id`, `type`, and optional/nullable `language`, `bandwidth`, `width`, `height`,
`codecs`, and DASH `frameRate`. HLS IDs may be variant URIs. An empty representation list is possible
for a simple media playlist. Treat nullable metadata as unknown. `sourceUrl`
identifies the source that answered. A successful session refresh also returns
`session` with the saved `url`, `cdnUrls`, `cdnHeaders`, `manifestHeaders`,
`mediaHeaders` and `heartbeatSeconds`; use these to avoid saving expired values
back from an open editor. Probing does not start a stopped stream.

## Scripts and scheduled events

| Method and route | Request / response |
|---|---|
| `POST /api/providers/<id>/script/<action>` | Optional `streamId`; `200` log envelope plus `output` string and `exitCode` integer. |
| `POST /api/providers/<id>/script/run` | `{"action":"manifest","streamId":"…"}`; same response. |
| `POST /api/providers/<id>/script/clear-session` | No body; delete that provider's durable session files; `200` log envelope. |

Actions must be declared by the provider or stream. Known actions and their
inputs/outputs are in [SCRIPTING.md](SCRIPTING.md). `downloadinit` and
`downloadmedia` are reserved and rejected. Direct `/script/run` does not
recreate the complete playback pipeline. Start is the route that runs a session
manifest, discovers DRM and obtains missing keys as necessary.

A script's nonzero exit code may still return **HTTP 200** with `exitCode != 0`;
check both HTTP status and exitCode. `channels`/`events` parse stdout and import
entries before replying; invalid import output is logged. Read `/api/state`
afterwards to see the result, and inspect `scriptImport` logs. EPG output is
stored for the EPG route. This is a long-running HTTP request, not a `202` job
with a job ID. While waiting, poll
`/api/logs?streamId=script%3A<providerId>&limit=500` for progress.

`events` catalogue imports are unrelated to `GET /api/events`. Imported event
windows are metadata; they do not schedule recording or automatically start or
stop a stream. See [scheduled event contract](EVENTS.md#scheduled-provider-events).

## Accounts and playback keys

| Method and route | Request / response |
|---|---|
| `GET /api/users` | `{"users":[…]}`. |
| `POST /api/users` | `{"username":"watcher","password":"8-or-more-characters","role":"viewer"}`; same users envelope. Role defaults to admin. |
| `DELETE /api/users/<id>` | Same users envelope. Final admin cannot be removed. |
| `GET /api/keys` | `{"keys":[…]}`. |
| `POST /api/keys` | `{"label":"living-room"}`; same keys envelope including generated key. |
| `DELETE /api/keys/<id>` | Same keys envelope. Revokes playback credentials. |

User fields: `id`, `username`, `role`, `createdAt` (ISO 8601 UTC). Password hashes
are not returned. Key fields: `id`, `label`, `key`, `createdAt`, `requests`,
`bytes`, `lastSeenAt`. The latter three are currently placeholders (0, 0, null);
use SSE connection/stream metrics for actual traffic. Keys have no expiry,
per-stream scope or management privileges. Use unique nonempty labels for
Xtream logins.

## Server settings and tools

| Method and route | Request / response |
|---|---|
| `GET /api/settings` | `port` (active), `storedPort` (saved preference), `bindAddress`, `trustedProxies`, and all four refresh fields. |
| `POST /api/settings` | Partial JSON object; returns settings plus `note`. Fields below. |
| `GET /api/service` | Systemd status; unavailable platforms return `{"systemdAvailable":false}`. |
| `POST /api/service/install` | Optional `port`, `bindAddress`; `200 {"installed":true}`. |
| `POST /api/service/restart` | `200 {"restarting":true}`; restart is deferred until after the response. |
| `GET /api/ffmpeg-status` | `status`, `available`, `canAutoInstall`, `installCommand`; optional `path`, `version`, `hasDash`, `hasHls`, `hasLibxml2`, `hasSrt`, `hasHttps`. |
| `GET /api/ffmpeg-install` | `{"command":"…"}` or `501`. |
| `POST /api/ffmpeg-install` | `202 {"started":true}`; follow `ffmpeg-install` logs. `409` if already running. Unsupported on Windows. |
| `GET /api/nm3u8dlre-status` | `{"status":"available|missing","path":"…"}`; path present only when found. |
| `GET /api/nm3u8dlre-install` | `{"command":"…"}` or `501`. |
| `POST /api/nm3u8dlre-install` | Also returns the installation command; does **not** execute it. |

Settings POST accepts `port` (1–65535), `bindAddress` (string),
`trustedProxies` (string: comma/space-separated addresses, CIDRs, `loopback`,
`private`, `any`; empty trusts none) and the [refresh fields](#refresh-intervals).
Port/bind changes require restart; trusted proxies and refresh changes apply
live. CLI overrides remain authoritative for the listener. Unknown settings
fields are ignored. Invalid port values are currently ignored; refresh fields
are strictly validated. Service availability/permissions depend on platform
and the user running the process. Where available, service status includes
`unitInstalled`, `unitName`, `active`, `enabled`, `canManage`, `execStart`,
`selfPath`, `workingDir`.

## Playback and Xtream

| Method and route | Result |
|---|---|
| `GET /play/<id>/index.m3u8` | Primary HLS master/media playlist. `?rep=<id>` selects a DASH representation. |
| `GET /restream/<id>/<generated-filename>` | Internal init/media/key/subtitle route. Follow the filenames and query strings emitted by playlists. |
| `GET /direct/<id>` or `/direct/<id>/<representation>` | Continuous fMP4 tail for DASH; source redirect for HLS. |
| `GET /direct/<id>.ts` | Continuous muxed MPEG-TS for DASH. |
| `GET /download/<id>.mp4` or `/download/<id>/<representation>.mp4` | Buffered DASH media download, not a complete historical recording. |
| `GET /source/<id>` | Redirect to current source URL. |

`/play/<id>/index.mpd` is recognized but generated DASH output is not implemented
in this build. The legacy state `directUrl` may point to `/restream/<id>/live.m3u8`,
which is also not a working master alias; use `/play/<id>/index.m3u8` for HLS. Playback errors can be JSON
or plain text. Warming streams return `503` with Retry-After; stopped/missing
streams commonly return `404`. Some unavailable modes return `501`.

Follow the segment/init/key/variant URLs emitted in playlists rather than
constructing internal `/proxy` or `/play` child query strings. Forward `Range`
when required; proxied partial content includes `206`, Content-Range and
Accept-Ranges. Supply a playback key on every protected request. `?key=…` is
propagated through rewritten playlist URLs; with Bearer auth configure the
player to attach the header to child requests too. Playlist exports under
`/api/` require management auth and do not automatically append playback keys.

```js
const play = new URL(`/play/${encodeURIComponent(streamId)}/index.m3u8`, base);
play.searchParams.set("key", playbackKey);
// Hand play.href to a browser player that supports HLS.
```

Xtream uses the same base URL and playback credentials, independent of panel
accounts. Percent-encode credential query/path components.

| Route | Result |
|---|---|
| `GET /player_api.php?username=…&password=…` | Account/server handshake with `user_info` and `server_info`. Invalid credentials return HTTP 200 with `user_info.auth: 0`. |
| Same with `action=get_live_categories` | Providers as `category_id`, `category_name`, `parent_id`. |
| Same with `action=get_live_streams[&category_id=…]` | Channel array. Empty/0 category selects all. Use returned numeric `stream_id` values. |
| `GET /get.php?username=…&password=…&type=m3u_plus&output=m3u8` | Credentialed M3U export. |
| `GET /xmltv.php?username=…&password=…` | XMLTV channel metadata; no programme schedules. |
| `GET /live/<username>/<password>/<numeric-stream-id>.m3u8` | Authenticated playback redirect. |

Xtream numeric IDs differ from management IDs. VOD, series and catch-up are
not implemented; discovery actions return empty collections. EPG actions return
an empty `epg_listings` array. Unknown discovery actions return an empty array.

## Errors and compatibility

JSON API errors use `{"error":"message"}`; validate response Content-Type before
parsing playback, file, EPG, or streaming responses as JSON.

| Status | Typical meaning / client handling |
|---|---|
| `200` | Completed request. Inspect script `exitCode` or Xtream `auth` where applicable. |
| `202` | Webhook queued or FFmpeg installer started; completion is in logs. |
| `204` | CORS preflight accepted. No body. |
| `302` | Playback/source redirect. |
| `400` | Invalid settings, request fields, script action, or pipeline configuration. |
| `401` | Missing/invalid management or playback credentials. |
| `403` | Viewer mutation or admin-only export denied. |
| `404` | Missing object, stored EPG, stopped stream, or missing media. |
| `405` | A non-GET request to an Xtream route. |
| `409` | FFmpeg installation already running. |
| `429` | Authentication throttle; wait Retry-After seconds. |
| `500` | Internal allocation, persistence, or engine failure. |
| `501` | Unimplemented endpoint, unsupported method or build/platform capability. |
| `502` | Upstream source/proxy failure. |
| `503` | Media warming or temporarily unavailable; respect Retry-After. |

There are no general-purpose signed webhooks for every mutation, WebSocket API,
GraphQL interface, durable event subscriptions, management bearer tokens, or
idempotency keys. For integration recovery, fetch state again and resume
monitoring. Serve the API through a proxy with streaming enabled; see
[EVENTS.md](EVENTS.md#reverse-proxy-and-connection-lifecycle).

Verify a build with:

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
python3 scripts/api-smoke.py --binary build/restreamair-server
```

The smoke suite uses a temporary state directory and local fixture servers. It
checks authentication, cross-origin headers/preflights, refresh validation and
persistence, independent SSE intervals, pause/resume, keepalives, scripts, and
playback routing.
