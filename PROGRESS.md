# Progress

## 2026-10-09

- **Proxy rotation spreads load** (`76d9608`, `apps/server/net.c`). Rotation now
  advances when a proxy is handed out, not when a request finishes, so concurrent
  segment downloads use every healthy proxy. Cancelled requests and origin
  404/410/416 answers no longer quarantine a working proxy. Verified on
  vpsrestream1: connections split ~17/20/20 across br145/br147/br148.
- **Manifest reads get priority over segment downloads** (`7bc6e6e`,
  `apps/server/net.c`, `apps/server/rs_dash.c`). With 16 streams x 6 download
  connections against `maxDownloadConcurrency` 50, playlist polls waited 20-90 s
  for a budget slot (`pollSlow`), segments aged out and streams stalled into
  provider restarts. MPD/playlist reads now get headroom of a quarter of the
  budget (minimum 2) above the limit.
- **Automatic restarts show as "restarting"** (`a20a3a1`). During the
  `restartDelaySeconds` gap after a stall restart, `/api/state` reports
  `status: "restarting"` with `restartAt`, the card badge is amber, and
  `/direct/<id>.ts` answers `503 retry in Ns` instead of `404 start it first`.
- Diagnosed vpsrestream1's claro drops: its single Pinggy tunnel proxy delivers
  ~13.5 Mbit/s for 6 streams (~2.2 Mbit/s per connection), so manifest reads
  take 20-70 s and streams stall into restarts. Needs a faster or additional
  proxy; not a code fault.
- **Exact quality rules** (`5890b9a`). `defaultVideo` accepts `height=N`
  (exactly N) and `height>=N` alongside `height<=N`, for DASH and HLS. The
  engine's `renditions` log entry names the chosen video size.
- **Proxy health is logged** (`be9ebe6`): `proxyHealth` entries when a proxy is
  benched (error + cooldown) or comes back. The restarting `.ts` 503 says
  "fetching a fresh source" once the restart timer has passed.
- **Gray picture on Disney 720p fixed** (`8ffd889`). Disney+ keys each quality
  separately; the key step read the first (lower) HLS variant, found its KID
  covered by cached keys, and the engine decrypted 720p with the wrong key.
  The key step now inspects the variant the engine will play (provider
  video/audio filters) plus its audio playlist. The engine matches
  PlayReady-order KIDs and logs `keyGuess` when it falls back to a lone key
  with a different KID.
- **SOCKS proxies resolve remotely** (`f9526e0`). `socks5://` made the client
  hand the proxy an IPv6 address from the dual-stack server, which the v4-only
  SOCKS proxy refused (reply 4), so every Disney script call failed. Server
  fetches and the script's `proxy=` argument now use `socks5h://` /
  `socks4a://`.
- **Proxy entries are trimmed** (`02b7bff`). A single provider proxy saved with
  a trailing space reached libcurl untrimmed ("Could not resolve proxy name"),
  stopping every Disney stream on vpsrestream1. The fetch path trims it, and
  provider/stream proxy fields are stored trimmed, one entry per line.
- **Init-segment KID decides the keys** (`24cfcee`). Two Disney channels
  (Xangai, Falando de Quem) had playlists naming a different KID from the
  init's tenc, so cached keys were reused and the video decoded as garbage
  downstream. The key step now always reads the init and runs the CDM when its
  KID is not covered.
- **Licence requested for the init's KID** (`70033ba`). On Disney "Playlist
  Rádio Disney" the playlist PSSH named KID c184d3cd while the 720p init used
  a2ba9a84; the script licensed the advertised box and got only c184's key.
  The key step now builds the Widevine PSSH (and `kid=`) from the init's KID
  when the playlist does not name it, and logs when the init can't be fetched.
- **Wrong keys stop the stream** (`443ec00`): a KID with no matching key logs
  `wrongKeys` and ends the engine instead of playing a gray picture.
- **Engine's KID drives the next licence** (`39a3ec7`): the supervisor stores
  the wrongKeys KID on the stream (`requiredKid`); the next key step adds it,
  so a stale stored key no longer counts as covering, and licenses that KID.
  Fixes the Playlist Rádio Disney loop (video a2ba9a84, key c184d3cd).
- **Provider-wide parallel downloads** (`e468adb`): `importParallelDownloads`
  (0 = keep 6, else 1–8) for imported streams, plus an Apply to all streams
  button (bulk `fields.parallelDownloads`). Sized for Proxy-Seller IPs, which
  allow 10 connections each: ~3 per Disney stream at 2 downloads.
