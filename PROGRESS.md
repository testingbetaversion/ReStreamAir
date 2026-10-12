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
- **Whole script errors on Discord** (`14773b1`): one `scriptFailed` alert per
  failed script run with its full stderr in a code block (up to ~3,900 chars,
  keeping the end), instead of per-line alerts where only the traceback's top
  line got through the burst limit. JWTs and `Bearer` tokens are redacted.
  Tested on moonmuaaz with a local webhook listener.
- **One proxy connection per thread** (`f6e2391`): sticky proxy per thread
  under rotation, and `CURLOPT_MAXCONNECTS 1` on proxied handles. Four Disney
  streams at 2 parallel downloads had held all 20 Proxy-Seller connections
  (10 per IP), so the script's manifest calls got 429 PROXY_MAX_CONNS.
- **Idle-connection limit scoped to rotating lists** (`440ddca`): single-proxy
  providers (claro's tunnel) keep normal connection reuse.
- **Script falls over to the next proxy** (`85e1e28`): a stream-start script
  action that fails with a proxy refusal (429 PROXY_MAX_CONNS, tunnel/SOCKS/auth)
  is rerun through the next proxy line; the script used to get only the first
  line, which was full. Tested on moonmuaaz with a throwaway server.
- **Muxed-TS viewer buffer 8 → 24 MB** (`2ff2907`): ~27 s at Disney 720p. The
  downstream restreamer kept slipping out of the 8 MB (~9 s) window and being
  jumped to the live edge (up to 156 `tsResync` per stream in 2 h).

### 2026-10-10 — cdm action gets `kids=` instead of `keyUri=`
**What was done:** The provider script's `cdm` call no longer receives `keyUri=` (the PSSH boxes repeated as long `data:` URIs). It now receives `kids=<kid>,<kid>,…` with every discovered KID, the one the video is encrypted under first. `kid=` is unchanged. Docs updated.

**Files changed:** core/src/server.c, SCRIPTING.md, public/docs/SCRIPTING.md, PROGRESS.md

**Commands run:**
```bash
python3 scripts/sync-panel-docs.py
```
Copies the root docs into public/docs so the panel and CI see the same text.

```bash
git add -A && git commit -m "cdm action gets kids= (every KID) instead of keyUri=" && git pull --rebase && git push
```
Commits the change and pushes it to main.

```bash
ssh root@moonmuaaz.xyz 'cd /root/ReStreamAir && git pull && cmake --build build && cd build && ctest'
```
Builds the server on moonmuaaz and runs the test suites (both passed).

```bash
ssh root@moonmuaaz.xyz 'screen -S 610526.restream -p 0 -X stuff "^C"; screen -S 610526.restream -p 0 -X stuff "./restreamair-server -p 1234\n"'
```
Restarts the moonmuaaz server on the new build.

**Unfinished:** 23.237.79.50 needs a rebuild to pick this up. Not yet seen in a real Disney `cdm` call.

### 2026-10-12 — Reduce log volume and memory use on production
**What was done:** Checked both servers. moonmuaaz is idle (server 19 MB). vpsrestream1 (23.237.79.50): ReStreamAir at 8.9 GB of 11.4 GB RAM, ~60% of one of 12 cores, 99 Mbit/s each way, logs 1.2 GB/day. Two fixes, neither touching stream quality:
- The engine's routine per-segment/per-poll info lines (93% of log bytes) are no longer written to `logs/*.jsonl`; they stay in the in-memory panel log. Errors and lifecycle events are still written.
- glibc builds cap malloc arenas at 4 (`mallopt(M_ARENA_MAX, 4)`) and call `malloc_trim(0)` once a minute. Production's memory sat in 70 arenas over 60 MB each holding freed segment buffers.

**Files changed:** core/src/server.c, apps/server/main.cpp, API.md, public/docs/API.md, PROGRESS.md

**Commands run:**
```bash
ssh root@23.237.79.50 'ps -o pid,pcpu,pmem,rss -p <pid>; free -m; grep -E "^(Rss|Anonymous)" /proc/<pid>/smaps_rollup; du -sh /root/restream/app/logs'
```
Read-only resource check on production: CPU, memory, memory map, log size.

```bash
python3 scripts/sync-panel-docs.py
```
Copies the root docs into public/docs.

```bash
ssh root@moonmuaaz.xyz 'cd /root/ReStreamAir && git pull && cmake --build build && cd build && ctest && nm -D restreamair-server | grep -E "mallopt|malloc_trim"'
```
Builds on moonmuaaz, runs the test suites (passed) and confirms the memory calls are linked.

**Unfinished:** vpsrestream1 needs a rebuild to get this; memory and log size should be re-checked there a few hours after.

### 2026-10-12 — Script proxy fallback when the script exits 0 with nothing
**What was done:** On vpsrestream1, Ajax x NEC stayed stopped: the Disney script's `cdm` call got `429` from Proxy-Seller, crashed (`UnboundLocalError`) and still exited 0 with no keys, so the proxy fallback (which only reacted to a non-zero exit) never tried the next proxy. A proxy refusal in the script output now moves to the next proxy unless the action produced its result: KID:KEY pairs for `cdm`, a `ManifestUrl` for `manifest`. Also checked production: memory 2.0 GB, logs ~60–90 MB/day, 16 streams running.

**Files changed:** core/src/server.c, PROGRESS.md

**Commands run:**
```bash
ssh root@moonmuaaz.xyz 'cd /root/ReStreamAir && git pull && cmake --build build && cd build && ctest'
```
Builds on moonmuaaz and runs the test suites (passed).

```bash
ssh root@moonmuaaz.xyz '... throwaway server on 127.0.0.1:8797 with a script that exits 0 after a 429 on the first proxy ...'
```
End-to-end check: the start logged `scriptProxy … retrying through second-proxy` and started. Test files moved to /tmp/rsa-test/trash/pf4-*.

**Unfinished:** vpsrestream1 needs a rebuild. Proxy-Seller IPs are still at their 10-connection cap and br53 (NordVPN) keeps failing with SSL errors.

### 2026-10-12 — Home SSH tunnel for claro instead of Pinggy
**What was done:** Replaced the claro Pinggy proxy with a reverse dynamic SSH tunnel from the home Windows PC (164.163.208.85, Pernambuco) to vpsrestream1: the PC's SSH acts as a SOCKS proxy on the server's 127.0.0.1:1080, used as `socks5h://127.0.0.1:1080`. Checked that sshd allows forwarding, that port 1080 was free, and that the host-key fingerprint the PC accepted is the server's real one. The tunnel was confirmed listening. Step-by-step guide written to the user's Documents folder at their request.

**Files changed:** /Users/moon/Documents/ReStreamAir - Home proxy tunnel (claro).md (new, outside the repo), PROGRESS.md

**Commands run:**
```bash
ssh root@23.237.79.50 'sshd -T | grep -iE "allowtcpforwarding|gatewayports|permitlisten"; ss -ltn | grep :1080'
```
Read-only check that the server allows the tunnel and the port is free.

```bash
ssh root@23.237.79.50 'ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub; ss -ltn | grep :1080'
```
Confirms the server's host-key fingerprint and that the tunnel is listening.

**Unfinished:** Key login from the PC (send the existing key's .pub to authorized_keys), switching the claro proxy to `socks5h://127.0.0.1:1080`, then re-checking the Globo gaps and slow manifest reads.
