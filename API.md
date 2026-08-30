# ReStreamAir HTTP API

Every server-side action in the web panel goes through this HTTP API. The panel
is an API client, not a privileged control path, so the same operations can be
used from shell scripts, schedulers, monitoring systems, or another UI.

## Authentication and conventions

All `/api/*` routes except `/api/auth/*` require either a panel session cookie
or HTTP Basic authentication. Basic auth is the simplest choice for scripts:

```sh
base=http://127.0.0.1:8787
auth='admin:your-password'
curl --fail-with-body --user "$auth" "$base/api/state"
```

JSON requests use `Content-Type: application/json`. Errors have an HTTP 4xx/5xx
status and a body shaped as `{"error":"..."}`. Viewer accounts may call GET
routes but receive `403` for mutations. Provider and stream mutations return a
fresh `/api/state` document, so a client can replace its local state directly.

IDs are opaque strings. Read them from `/api/state`; do not construct them.
Playback API keys are separate from panel authentication and are described
under [Playback](#playback).

## Panel state and monitoring

| Method and route | Panel operation / result |
|---|---|
| `GET /api/state` | Full provider and stream state, computed playback/download URLs, status, metrics, and version data. |
| `GET /api/events` | Server-Sent Events stream carrying the same live monitoring view. |
| `GET /api/logs?streamId=<id>&limit=150` | Read logs. Omit `streamId` for all logs; use `__panel__` for control-plane logs or `script:<providerId>` for script output. |
| `DELETE /api/logs?streamId=<id>` | Clear all logs or the selected stream's visible history. |
| `GET /ping` | Unauthenticated liveness probe. |

## Providers

| Method and route | Panel operation |
|---|---|
| `POST /api/providers` | Create a provider. Body: `{"name":"Name","logo":"https://..."}`. |
| `PUT /api/providers/<id>` | Save provider settings. Send the complete provider settings form; omitted editable values are reset to defaults. |
| `DELETE /api/providers/<id>` | Delete the provider, its streams, and its script session store. |
| `GET /api/providers/<id>/export` | Download the provider, streams, accounts, and embedded script as JSON. Treat it as a secret. |
| `POST /api/providers/import` | Import the JSON document returned by the export route. |
| `GET /api/providers/<id>/playlist.m3u8` | Export that provider's M3U playlist. |
| `GET /api/playlist.m3u8` | Export one M3U playlist containing every stream. |
| `GET /api/providers/<id>/epg` | Read the last EPG document produced by the provider script. |
| `POST /api/providers/<id>/webhook/test` | Queue the same error-webhook test as the provider dialog. |
| `GET /api/logo-lookup?name=<name>` | Run the panel's Find logo lookup. |

Example:

```sh
curl --fail-with-body --user "$auth" \
  -H 'Content-Type: application/json' \
  -d '{"name":"News"}' \
  "$base/api/providers"
```

## Streams

| Method and route | Panel operation |
|---|---|
| `POST /api/providers/<providerId>/streams` | Create a stream from the supplied stream object. `name`, `kind`, and the source/pipeline fields are the principal inputs. |
| `PUT /api/streams/<id>` | Save the complete stream editor object. |
| `DELETE /api/streams/<id>` | Stop and delete a stream. |
| `POST /api/streams/<id>/start` | Start the live engine or configured pipeline. |
| `POST /api/streams/<id>/stop` | Stop it. |
| `POST /api/probe` | Run source auto-detection. Body fields: `url`, `proxy`, `headers`, `forceIpv6`, and `rotateProxies`. |

The All Streams panel's Start all, Stop all, and Delete all buttons call the
individual routes above sequentially for the currently filtered IDs. A script
can make the same selection from `/api/state` and call those routes; sequential
calls avoid a startup spike and concurrent `state.json` writes.

Create and start a basic HLS stream:

```sh
provider_id=provider_...
state=$(curl --fail-with-body --user "$auth" \
  -H 'Content-Type: application/json' \
  -d '{"name":"Channel","kind":"m3u8","url":"https://example/live.m3u8"}' \
  "$base/api/providers/$provider_id/streams")

# Read stream_id from the returned state with your JSON tool of choice.
curl --fail-with-body --user "$auth" -X POST \
  "$base/api/streams/$stream_id/start"
```

## Provider script actions

| Method and route | Panel operation / result |
|---|---|
| `POST /api/providers/<id>/script/<action>` | Run Login, Pair, Load channels, Load events, Load EPG, or another provider action. An action the provider (or the stream, via `scriptActionsOverride`) hasn't declared is refused with `400`. The response includes `entries`, combined `output`, and `exitCode`. For `channels` and `events` the printed document is also imported: one stream per entry, matched to existing ones by name, with the result logged as a `scriptImport` entry. |
| `POST /api/providers/<id>/script/run` | Exercise an action for a selected stream. Body: `{"action":"pssh","streamId":"stream_..."}`. The script receives `id=` and `url=` in addition to the normal arguments. |
| `POST /api/providers/<id>/script/clear-session` | Recursively delete the provider's stored script session and cookies. |

`POST /api/streams/<id>/start` also runs the script when the stream is
configured for it: `manifest` for a fresh source URL, then `cdm` for clear
keys. That reply therefore waits for both, and the refreshed URL, CDN mirrors,
headers and keys are in the state it returns.

Script execution is asynchronous internally but the HTTP response completes
when that invocation exits. Long-running output is also available while it runs
from `GET /api/logs?streamId=script:<providerId>`. See [SCRIPTING.md](SCRIPTING.md)
for the argv and output contracts.

## Accounts, playback keys, and settings

| Method and route | Panel operation |
|---|---|
| `GET /api/users` | List panel accounts without password hashes. |
| `POST /api/users` | Add an account: `{"username":"...","password":"...","role":"admin|viewer"}`. |
| `DELETE /api/users/<id>` | Remove an account and its sessions. The final admin cannot be removed. |
| `GET /api/keys` | List playback API keys and their usage view. |
| `POST /api/keys` | Generate a playback key: `{"label":"Living room"}`. |
| `DELETE /api/keys/<id>` | Revoke a playback key. |

## Xtream Codes-compatible live TV

ReStreamAir implements the live-TV subset used by common Xtream clients. Create
a playback key in the Keys view, then use its **label as the username** and its
generated **key as the password**. The panel account password is never used.

Configure a client with the ReStreamAir base URL plus those credentials. The
following public, credential-protected endpoints are available:

| Route | Purpose |
|---|---|
| `GET /player_api.php?username=...&password=...` | Account/server handshake. |
| `GET /player_api.php?...&action=get_live_categories` | Providers as live categories. |
| `GET /player_api.php?...&action=get_live_streams` | Live channels. |
| `GET /get.php?username=...&password=...&type=m3u_plus&output=m3u8` | Xtream-style M3U export. |
| `GET /xmltv.php?username=...&password=...` | XMLTV channel metadata. |
| `GET /live/<username>/<password>/<stream-id>.m3u8` | Authenticated live playback. |

VOD, series, catch-up, and programme schedules are not currently provided;
their discovery actions return empty collections so live-only clients can
finish setup cleanly.
| `GET /api/settings` | Read saved bind/port/trusted-proxy settings plus the active port. |
| `POST /api/settings` | Save `port`, `bindAddress`, and/or `trustedProxies`; bind changes apply after restart. |
| `GET /api/service` | Read systemd availability, installation, and running state. |
| `POST /api/service/install` | Install/reinstall the service with optional `port` and `bindAddress`. |
| `POST /api/service/restart` | Request a service restart after the response is sent. |
| `GET /api/ffmpeg-status` | Inspect FFmpeg availability and capabilities. |
| `GET /api/ffmpeg-install` | Return the supported installation command. |
| `POST /api/ffmpeg-install` | Start the same installer offered by the panel. |
| `GET /api/nm3u8dlre-status` | Inspect N_m3u8DL-RE availability. |
| `GET /api/nm3u8dlre-install` | Return its supported installation command. |

## Authentication routes

| Method and route | Purpose |
|---|---|
| `GET /api/auth/status` | Report setup/authentication state and the current role. |
| `POST /api/auth/setup` | Create the first admin. |
| `POST /api/auth/login` | Start a cookie session. Body: `username`, `password`, and optional `remember`. |
| `POST /api/auth/logout` | End the current cookie session. |

## Playback

Playback URLs are returned per stream by `/api/state`. The common routes are
`/play/<id>/index.m3u8`, `/play/<id>/index.mpd`, `/direct/<id>`,
`/download/<id>/<representation>`, and `/source/<id>`. Once any playback key
exists, supply it as `?key=<key>` or `Authorization: Bearer <key>`.

Browser-only affordances such as opening a dialog, changing a filter, copying a
returned URL to the clipboard, selecting an HLS.js quality, and entering
Picture-in-Picture do not mutate server state and therefore have no server API.
