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
