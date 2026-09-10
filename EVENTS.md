# Events and external monitoring

ReStreamAir exposes three different kinds of events:

| Interface | What it contains |
|---|---|
| `GET /api/events` | Live monitoring snapshots over Server-Sent Events (SSE). |
| `GET /api/logs` | Diagnostic, lifecycle and script log entries. |
| Provider `events` script action | Scheduled programme/event metadata imported as streams. |

An optional provider error webhook pushes selected errors to an external HTTP
receiver. These interfaces do not share a common message format. Management
routes require a panel account; playback keys do not authenticate them.
See [API.md](API.md) for authentication, CORS and the full route reference.

## Monitoring stream

```http
GET /api/events?intervalMs=1000 HTTP/1.1
Authorization: Basic <base64(username:password)>
Accept: text/event-stream
Origin: https://your-website.example
```

The server replies `200 text/event-stream`, sends a reconnect hint, then a
snapshot immediately. Data is UTF-8, and every message ends with a blank line:

```text
retry: 3000

data: {"timestamp":1789041600000,"refresh":{"eventsIntervalMs":1000,"stateRefreshMs":4000,"logsRefreshMs":2000,"activityRefreshMs":1000},"global":{"bytesPerSecond":0,"allTimeBytes":0,"activeClients":0},"globalInput":{"bytesPerSecond":0,"allTimeBytes":0},"system":{},"streams":{},"connections":[]}

```

The example abbreviates `system`; its fields are listed below. Messages have
**no `event:` name and no `id:`**. Native EventSource dispatches them through
`onmessage` (event type `message`). There is no replay log, sequence guarantee,
Last-Event-ID resume, or separate `streamStarted`/`streamStopped` SSE event.
Every data message is a complete monitoring snapshot; replace your previous
metrics view rather than accumulating records. Fetch `/api/state` separately
for provider/stream configuration and lifecycle status.

`intervalMs` is optional, in milliseconds: `100`–`3600000` integer, or `0` for
initial data only. Omitted means use the saved `eventsIntervalMs`, default 1000.
Invalid values return JSON `400` before opening the stream. Explicit overrides
remain in effect for the life of that connection. Reconnect to change one.
Changes to the saved default apply to already-open connections that inherit it.

Paused or slow subscriptions receive a `: keepalive` comment after approximately
15 seconds without an outgoing frame. Ignore comments in a data handler.
The initial `retry: 3000` is an EventSource reconnection hint, **not** the data
refresh interval. Fetch clients implement their own reconnect delay.

### Payload schema

| Field | Type | Meaning |
|---|---|---|
| `timestamp` | Number | Snapshot time, Unix epoch **milliseconds**. |
| `refresh` | Object | Effective server defaults: `eventsIntervalMs`, `stateRefreshMs`, `logsRefreshMs`, `activityRefreshMs`. A subscriber's override is not written into this object. |
| `global` | Object | Aggregate output: `bytesPerSecond`, `allTimeBytes`, `activeClients`. |
| `globalInput` | Object | Aggregate input: `bytesPerSecond`, `allTimeBytes`. |
| `streams` | Object keyed by stream ID | One metrics object for each configured stream. |
| `connections` | Array | Active/recent playback connection records. |
| `system` | Object | Host resource snapshot. |

Each `streams[id]` object contains:

```json
{
  "bytesPerSecond": 250000,
  "allTimeBytes": 12500000,
  "inputBytesPerSecond": 200000,
  "inputAllTimeBytes": 10000000,
  "activeClients": 1
}
```

Rates are **bytes/second**, not bits/second; multiply by 8 for bits/second.
Counters are bytes. Inbound measures source acquisition; outbound measures
playback delivery. A running stream with no viewers can have inbound traffic
and zero outbound traffic. Metrics coverage depends on the pipeline; zero does
not by itself mean a source is broken. `allTimeBytes` is the server's cumulative
counter, not a durable audit log. Global values aggregate configured streams.

Each `connections[]` item contains:

| Field | Type / meaning |
|---|---|
| `streamId`, `streamName`, `providerName` | Strings identifying the stream and its provider. |
| `kind` | String `playback`. |
| `user` | Playback key label or `Anonymous`. |
| `uid` | Playback key ID or empty string. |
| `clientIP`, `userAgent` | Client address and user-agent strings. Trusted proxy settings determine the resolved address. |
| `uptimeSeconds` | Integer seconds since the tracked connection began. |
| `errors` | Integer error count in the tracked metrics record. |
| `bytesPerSecond`, `allTimeBytes` | Output rate and total bytes for that record. |

These are tracked playback identities, not guaranteed one-row-per-TCP-socket
or distinct human viewer. Expired records are pruned by server maintenance.

`system` fields:

| Fields | Type / unit |
|---|---|
| `cpuPercent`, `peakCPUPercent` | Numbers, percent. Peak since process start. |
| `cpuModel`, `osVersion` | Strings. |
| `coreCount` | Integer processor count. |
| `loadAverage` | Number; platform-specific load measurement. |
| `memoryUsedBytes`, `totalMemoryBytes`, `peakMemoryBytes` | Integer bytes; peak since process start. |
| `diskTotalBytes`, `diskAvailableBytes` | Integer bytes for the sampled filesystem. |
| `uptimeSeconds` | Integer host uptime seconds. |

Sampling is platform-dependent. Tolerate missing fields in older versions and
zero/empty values when the host cannot supply a measurement.

## External browser client

An external website uses streaming `fetch` with an explicit Authorization header
and `credentials: "omit"`. Native EventSource only accepts a URL and credentials
option; it cannot supply a Basic header. Do not put a panel password in the URL.
The helper below handles frames split across network chunks, comments, EOF,
reconnection, HTTP failures and cancellation. It accepts the Basic header made
by [the API helper](API.md#copyable-browser-request-helper).

```js
function waitForRetry(ms, signal) {
  return new Promise(resolve => {
    if (signal?.aborted) return resolve();
    const done = () => {
      clearTimeout(timer);
      signal?.removeEventListener("abort", done);
      resolve();
    };
    const timer = setTimeout(done, ms);
    signal?.addEventListener("abort", done, { once: true });
  });
}

async function watchMetrics({ base, authorization, intervalMs, onSnapshot,
                              onError = console.error, signal, retryMs = 3000 }) {
  const url = new URL("/api/events", base);
  if (intervalMs !== undefined) url.searchParams.set("intervalMs", String(intervalMs));
  while (!signal?.aborted) {
    let delay = retryMs;
    try {
      const response = await fetch(url, {
        credentials: "omit", cache: "no-store", signal,
        headers: { Authorization: authorization, Accept: "text/event-stream" },
      });
      if (!response.ok) {
        delay = Math.max(delay, (Number(response.headers.get("Retry-After")) || 0) * 1000);
        const error = new Error(`Events HTTP ${response.status}: ${await response.text()}`);
        if (response.status >= 400 && response.status < 500 && response.status !== 429) {
          onError(error); // Invalid interval/credentials need a caller change.
          return;
        }
        throw error;
      }
      if (!response.headers.get("Content-Type")?.startsWith("text/event-stream")) {
        await response.body?.cancel();
        throw new Error("Expected text/event-stream");
      }
      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      let pending = "";
      try {
        while (!signal?.aborted) {
          const { value, done } = await reader.read();
          if (done) break; // Reconnect after EOF; partial frame is discarded.
          pending += decoder.decode(value, { stream: true });
          let boundary;
          while ((boundary = /\r?\n\r?\n/.exec(pending))) {
            const frame = pending.slice(0, boundary.index);
            pending = pending.slice(boundary.index + boundary[0].length);
            const data = frame.split(/\r?\n/)
              .filter(line => line.startsWith("data:"))
              .map(line => line.slice(5).replace(/^ /, "")).join("\n");
            if (data) onSnapshot(JSON.parse(data));
          }
        }
      } finally {
        await reader.cancel().catch(() => {});
        reader.releaseLock();
      }
    } catch (error) {
      if (signal?.aborted) return;
      onError(error);
    }
    await waitForRetry(delay, signal);
  }
}

const controller = new AbortController();
const subscription = watchMetrics({
  base: "https://streams.example.com",
  authorization, // Explicit Basic header; keep credentials in application memory.
  intervalMs: 1500, // Omit to inherit the server default; 0 for initial data only.
  signal: controller.signal,
  onSnapshot: metrics => console.log(metrics.global, metrics.streams),
});
// controller.abort(); await subscription; // Stop, or reconnect at a new interval.
```

For a page served on the server's own origin with an authenticated panel cookie:

```js
const events = new EventSource("/api/events?intervalMs=1000");
events.onmessage = event => console.log(JSON.parse(event.data));
events.onerror = () => console.log("Disconnected; EventSource will retry");
// events.close();
```

For shell tools:

```sh
curl --no-buffer --fail-with-body --user 'admin:your-panel-password' \
  'https://streams.example.com/api/events?intervalMs=1000'
```

## Reverse proxy and connection lifecycle

SSE stays open until the client, server or proxy closes the connection. The
server sends `Cache-Control: no-cache, no-transform`, `X-Accel-Buffering: no`,
and wildcard CORS headers. Disable response buffering/caching for `/api/events`.
[deploy/Caddyfile](deploy/Caddyfile) already uses `flush_interval -1`.
For nginx, place these directives in the relevant location:

```nginx
proxy_http_version 1.1;
proxy_buffering off;
proxy_cache off;
proxy_read_timeout 1h;
```

Choose your public base URL explicitly, configure trusted proxies when using
forwarded addresses/HTTPS, and do not add conflicting CORS headers at the proxy.
No server setting can override a browser's mixed-content, CSP or network-access
restrictions. See [CORS requirements](API.md#browser-access-and-cors).

Authentication is checked when opening the SSE connection. Already-open streams
are not periodically reauthenticated; clients should close them on logout.
Reconnect after a restart/network failure, and refetch `/api/state` when you need
to resynchronize configuration. Slow readers with more than 1 MiB queued may be
disconnected to bound server memory. Share one subscription per application
where possible; browser/proxy concurrent-connection limits still apply.

## Logs

`GET /api/logs?streamId=<encoded-id>&limit=150` returns:

```json
{
  "entries": [
    {
      "timestamp": 1789041600000,
      "level": "info",
      "streamId": "stream_example",
      "event": "streamStart",
      "message": "START requested",
      "url": "https://origin.example/live.mpd",
      "status": 200,
      "bytes": 1024
    }
  ],
  "availableDates": []
}
```

`timestamp` is Unix epoch **milliseconds**. `level` is `info` or `error`;
`streamId` and `event` are strings. `message`, `url`, `status`, `bytes` are
optional. `status` is usually an HTTP status, but script/installer events may
use it for a process result; zero is omitted. `bytes` is omitted when no count
was recorded. Do not assume all entries have the same optional fields.

Entries are newest first, in a shared in-memory ring capped at 20000. They are
not durable/replayable events; restart, clearing, and ring rollover can remove
history. There is no `since`, `after`, date-range or offset cursor. Poll and
filter client-side; repeated responses overlap. `availableDates` is currently
empty. `DELETE /api/logs?streamId=…` hides previous history for that ID;
`DELETE /api/logs` clears the ring. Neither stops the stream.

| `streamId` filter | Contains |
|---|---|
| Omitted or empty | All matching recent entries. |
| `__panel__` | Management access, authentication, service and provider operations. |
| Real stream ID | That stream's engine/playback activity and startup script work. |
| `script:<providerId>` | Direct provider script calls and catalogue imports. |
| `ffmpeg-install` | Installer start/output/exit. |

### Event name reference

Names are diagnostic strings, not a closed versioned enum. Preserve unknown
names in your UI. The following groups cover the server's current log producers;
individual events may be info or error depending on the outcome.

| Names | Meaning |
|---|---|
| `access`, `poll` | HTTP management access; frequent state/log/event requests use `poll`. |
| `serverStart`, `login`, `logout`, `loginFailed`, `loginThrottled` | Server/authentication lifecycle. |
| `streamCreate`, `streamUpdate`, `streamDelete`, `streamStart`, `streamStop` | Requested stream configuration/lifecycle operations, including failures. |
| `providerImport`, `sessionCleanup`, `clearSession` | Import or provider session-file cleanup. |
| `serviceInstall`, `serviceRestart` | System service management. |
| `scriptStart`, `scriptCommand`, `scriptOutput`, `scriptError`, `scriptEnd` | Script execution and stdout/stderr. Full commands/output can contain credentials. |
| `scriptImport`, `scriptManifest`, `cdm` | Catalogue import, resolved source, and key-discovery/workflow results. |
| `installStart`, `installOutput`, `installExit` | FFmpeg installer progress/completion. |
| `liveConfig`, `liveStart`, `liveStop`, `ffmpegPipeline` | Internal engine configuration/lifecycle or external pipeline status. |
| `repStart`, `repStop`, `renditions` | Rendition worker lifecycle/discovery. |
| `manifest`, `manifestRetry`, `cdnFailover` | Manifest fetch, retry/backoff and mirror selection. |
| `pollStart`, `pollQueued`, `pollDone`, `pollIdle`, `pollSlow` | Manifest polling and scheduling diagnostics. |
| `initFetch`, `initReady`, `downloadInit`, `downloadSegment` | Origin initialization/media acquisition. |
| `decryptSegment`, `subtitleConvert`, `discontinuity` | Decryption, TTML/WebVTT conversion and stream timeline changes. |
| `headOfLine`, `fallingBehind`, `lagWatch`, `liveEdgeSkip` | Queue/backlog or live-edge recovery. |
| `master`, `masterCold`, `playlist`, `playlistReady` | Playlist serving and readiness. |
| `serveInit`, `serveSegment`, `cacheMissInit`, `cacheMissSegment`, `playbackDenied` | Playback delivery/cache window/authentication. |
| `directOpen`, `directSlow`, `download` | fMP4 direct playback or buffered downloads. |
| `tsOpen`, `tsMuxTrack`, `tsMuxReady`, `tsMuxStalled`, `tsMuxRecovered`, `tsResync` | Continuous MPEG-TS muxing and recovery. |
| `webhookDelivery` | Background webhook delivery failure; does not recursively trigger another webhook. |

Logs may contain source URLs, headers, provider credentials and subprocess
output. Give management read access only to trusted operators.

## Scheduled provider events

Run `POST /api/providers/<id>/script/events` after declaring the `events` action.
The provider script prints an object containing an `Events` array to stdout:

```json
{
  "Events": [
    {
      "Name": "Example live event",
      "Mode": "live",
      "ScriptParams": "id=event-42",
      "SessionManifest": true,
      "UseCdm": false,
      "Start": 1789041600,
      "End": 1789048800,
      "RecordEvent": false,
      "Autostart": false
    }
  ]
}
```

| Script output field | Stored stream field / meaning |
|---|---|
| `Name` | `name`; required nonblank string. Blank names/nonobject entries are skipped. |
| `Mode` | `mode`; defaults to `live`. |
| `ScriptParams` | `scriptParams`; flat `key=value` arguments passed to stream actions. |
| `ManifestScript` | Legacy alias for ScriptParams; ScriptParams wins if both exist. |
| `SessionManifest` | `sessionManifest`, boolean default false. |
| `UseCdm`, `CdmType` | `useCdm` boolean false, `cdmType` string empty by default. |
| `Video`, `Audio` | `scriptVideoSelector`, `scriptAudioSelector` strings. |
| `OnDemand`, `SpeedUp`, `Autostart`, `RecordEvent` | Corresponding lower-camel-case booleans, default false. |
| `Start`, `End` | `scriptStart`, `scriptEnd`: numeric Unix epoch **seconds**, or null if omitted/nonnumeric. |

Lower-camel-case aliases (`name`, `sessionManifest`, `start`, etc.) are accepted;
capitalized fields win when both are present. The wrapper also accepts `events`.
Each entry is matched by **(name, sourceType)** within its provider. Imported
events use `sourceType: "event"`; channels use `"channel"`. Reimport updates a
matching entry, does not duplicate it, and does not delete missing entries.
Renaming an event creates a new entry. New imports have no source URL: configure
a session-manifest script or edit the source URL before starting playback.
Arbitrary extra catalogue fields, including a direct `Url`, are not an editor
settings replacement.

These fields are stored/displayed metadata. The C server does not schedule
Start/Stop from the event window, automatically record `RecordEvent`, or run a
periodic provider-event import. An external scheduler should call the import,
select `state.providers[].streams` with `sourceType === "event"`, interpret the
seconds-based time window, and invoke the normal Start/Stop routes as needed.
SSE/Logs refresh settings do not trigger catalogue imports.

## Outgoing error webhooks

Set a provider's `errorWebhookUrl` to an HTTP(S) receiver. Stream errors and that
provider's script errors enqueue a Discord-compatible JSON POST. Use
`POST /api/providers/<id>/webhook/test` to queue a test; `202` means queued,
not delivered. No webhook or a full queue produces `400`.

```json
{
  "username": "ReStreamAir",
  "allowed_mentions": { "parse": [] },
  "embeds": [{
    "title": "Provider error",
    "color": 15548997,
    "description": "Origin fetch failed",
    "fields": [
      { "name": "Provider", "value": "Example provider", "inline": true },
      { "name": "Stream", "value": "Example live event", "inline": true },
      { "name": "Event", "value": "manifest", "inline": true },
      { "name": "HTTP status", "value": "502", "inline": true }
    ]
  }]
}
```

HTTP status is optional. A test uses title `Webhook test` and color `5763719`.
Descriptions are truncated to approximately 1000 characters and URLs in error
text are redacted. Extra burst errors are collapsed: one provider error is
queued per 5-second window, and a subsequent alert may carry `footer.text`
reporting the suppressed count. The shared queue is capped at 64 pending jobs.
Delivery is best-effort in a background worker; failures appear as
`webhookDelivery` logs. There is no durable queue, replay, signature, configurable
auth header, or guaranteed retry. CORS applies to browser calls into ReStreamAir;
this outgoing server-to-server POST is not a browser CORS request.
