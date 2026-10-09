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
