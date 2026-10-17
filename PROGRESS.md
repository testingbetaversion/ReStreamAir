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

### 2026-10-12 — Tunnel guide: another PC or VPS
**What was done:** Added an "Another PC or VPS" section to the home-tunnel guide: one server port per machine (1080, 1081, …) with all of them in the provider list and Rotate proxies on; another Windows PC; a Linux PC or Raspberry Pi as a systemd service; a Brazilian VPS either tunnelled from the server (`ssh -D`) or running its own SOCKS proxy restricted to 23.237.79.50; and how to list the connected tunnels.

**Files changed:** /Users/moon/Documents/ReStreamAir - Home proxy tunnel (claro).md (outside the repo), PROGRESS.md

**Commands run:** None (documentation only).

**Unfinished:** Same as the previous entry: key login from the PC, switching claro to `socks5h://127.0.0.1:1080`, then re-checking the Globo channels.

### 2026-10-13 — Step down to the quality the licence unlocks
**What was done:** On vpsrestream1, 7 Disney events were stuck: the 720p video's KID was requested (`kid=`), but Disney's licence returned only the lower-quality KID's key, so starts were refused ("do not cover all discovered KIDs"). Now, when the returned keys don't unlock the preferred quality, the key step walks the master's video heights from the top, reads each candidate's init KID and plays the best quality the returned key unlocks. The rule (e.g. `height=540`) is stored on the stream as `keyLimitedVideo` and overrides the provider's Default video for it; a manual Start clears it and retries the preferred quality. Claro, now on the home SSH tunnel, was also checked: about 1 slow manifest read per channel per hour, no restarts.

**Files changed:** core/src/server.c, core/src/panel.c, core/include/rs_panel.h, API.md, public/docs/API.md, PROGRESS.md

**Commands run:**
```bash
ssh root@moonmuaaz.xyz 'cd /root/ReStreamAir && git pull && cmake --build build && cd build && ctest'
```
Builds on moonmuaaz and runs the test suites (passed).

```bash
ssh root@moonmuaaz.xyz '... fake HLS (720p under KID B, 540p under KID A) on 127.0.0.1:8798, a cdm script returning only A, throwaway server on 8797 ...'
```
End-to-end check: the start requested `kid=bbbb…`, got only A, logged the step-down and started on `hlsv-540p-3000000` with `keyLimitedVideo=height=540`. Test files moved to /tmp/rsa-test/trash/sd-*, test servers stopped.

**Unfinished:** vpsrestream1 needs a rebuild; the 7 stuck Disney events then start at the lower quality automatically.

### 2026-10-13 — Export saved keys; kids= only for missing keys
**What was done:** Added `GET /api/keys/export` (admin only): every saved decryption key per stream, optionally for one provider (`provider=<id or name>`), as JSON or `format=txt` (`KID:KEY` lines with a `# provider / stream (id)` comment per stream). An **Export keys** button on All Streams downloads the shown provider's keys, or all of them. Each export is logged as `keysExport`. The cdm action's `kids=` now lists only discovered KIDs with no saved key yet, the media's own first.

**Files changed:** core/src/server.c, public/app.js, public/index.html, API.md, SCRIPTING.md, public/docs/API.md, public/docs/SCRIPTING.md, PROGRESS.md

**Commands run:**
```bash
ssh root@moonmuaaz.xyz 'cd /root/ReStreamAir && git pull && cmake --build build && cd build && ctest'
```
Builds on moonmuaaz and runs the test suites (passed).

```bash
ssh root@moonmuaaz.xyz '... throwaway server on 127.0.0.1:8797 with two streams holding keys; curl /api/keys/export (txt, json, unauthenticated) ...'
```
txt and JSON exports listed all three keys with the provider/stream comments; without sign-in it answered 401. Test files moved to /tmp/rsa-test/trash/kx-*.

**Unfinished:** vpsrestream1 needs a rebuild. Detecting a key whose KID matches but whose value is wrong (test-decrypting one frame) is not built yet.

### 2026-10-13 — Fix the quality step-down (216p by mistake)
**What was done:** On vpsrestream1, Cleveland Guardians x Tampa Bay Rays played at 216p. The cdm action had returned the 720p video's own key, but the start was refused because the playlist also named another quality's KID. The step-down then could not read the higher qualities' init segments (proxy at its connection cap) and treated that as "no key". Fixed: keys covering the KID the video is encrypted under are accepted. The step-down retries a failed init read up to 3 times, skips a quality it could not check, and stores nothing when the result is the preferred quality. Production had 358 needless step-downs (all but one back to 720p) from the same over-strict check.

**Files changed:** core/src/server.c, PROGRESS.md

**Commands run:**
```bash
ssh root@23.237.79.50 'python3 -c "...list streams with keyLimitedVideo..."; grep "the licence did not include" logs/2026-10-13.jsonl'
```
Read-only: found 4 streams with a stored step-down and 359 step-downs today (358 to 720p, 1 to 216p).

```bash
ssh root@moonmuaaz.xyz 'cd /root/ReStreamAir && git pull && cmake --build build && cd build && ctest'
```
Builds on moonmuaaz and runs the test suites (passed).

```bash
ssh root@moonmuaaz.xyz '... fake HLS source + two cdm scripts, throwaway server on 127.0.0.1:8797 ...'
```
Case A (720p key returned): played 720p, nothing stored. Case B (only the lower key): stepped down to 540p. Test files in /tmp/rsa-test/trash/sd2-*.

**Unfinished:** vpsrestream1 needs a rebuild, then Start pressed on Cleveland x Tampa Bay (clears its stored 216p). Xangai, Al Nassr x Neftchi and Linha de Passe have a harmless stored `height=720` that Start also clears.

### 2026-10-14 — Stream log review on vpsrestream1 + Windows tunnel script
**What was done:** Read today's logs for the 16 running streams on vpsrestream1 (read-only). Findings: 633 of 634 `fallingBehind` alerts had no lost segments (whole ~6s segments counted in a ~64s window read as 0.93x); the claro 01:37–01:53 restart storm was the reverse SSH tunnel on 127.0.0.1:1080 dying while sshd (`ClientAliveInterval 0`) kept the port held; the disney script prints access tokens to stderr, which land in the logs as `scriptError`. Added a Windows batch script that keeps a reverse SOCKS tunnel up: asks server, ports and user once, installs its own SSH key with the password typed once, then reconnects forever with keepalives and `ExitOnForwardFailure`.

**Files changed:** scripts/windows/reverse-socks-tunnel.bat (new), PROGRESS.md

**Commands run:**
```bash
ssh root@23.237.79.50 'python3 /tmp/rsa_tbl.py; ...grep/journalctl/ss...'
```
Read-only: per-stream error table, tunnel origin and sshd keepalive settings; the temporary script was removed.

```bash
perl -pi -e 's/\r?\n/\r\n/' scripts/windows/reverse-socks-tunnel.bat
```
Gives the batch file Windows line endings.

**Unfinished:** The script is untested on Windows. Fix the fallingBehind segment-rounding in core/src/live.c; set `ClientAliveInterval 15` on vpsrestream1; remove the token/debug prints from disneyplus.py and rotate the Disney session.

### 2026-10-14 — Second log review on vpsrestream1 (13:23–15:23 UTC)
**What was done:** Read-only review of the last two hours on vpsrestream1, plus a live check of each proxy from that box. 16 streams are running. The service has been up since 2026-10-13 14:31; load is 1.3 and 7.7 GB is free, so the server itself is fine.

Every proxy is up. All of their failures were brief, and each proxy was back in rotation within about 15s:

| proxy | failures in 2h | live check from vpsrestream1 |
|---|---|---|
| 79.127.168.43:50100 | 19 × could not connect | 3/6: every other connection refused, ~2.1s when it answers |
| kdwjgspsyy.a.pinggy.link:12227 | 16 × timeout, 1 × dropped | 12/12, ~1.4s (0.8s of it TLS) |
| 178.92.209.173:50100 | 9 × could not connect | 6/6, 0.6s |
| br53.proxy.nordvpn.com:89 | 5 × SSL, 2 × dropped, 1 × timeout | 6/6, 0.8s |

The failures bunch up at the same minutes (13:28, 14:04, 14:20, 14:45), when many streams load the proxies at once. During each burst, affected streams briefly lose a rendition ("produced nothing for 15s") and publish at 0.4–0.9x real time, then recover. The disney script's `CONNECT tunnel failed` errors (20) fall in the same minutes.

- claro GLOBO BH, RJ and BSB: manifest reads of 38–50s during the bursts, over the 18s margin (6–11 `pollSlow` and 3–5 `fallingBehind` each). curta got one HTTP 403 at 13:29 and restarted itself.
- `lagWatch` now logs one-off slow windows as info, so most of them no longer raise `fallingBehind`.
- SportsCenter with Scott Van Pelt: no usable keys at 15:00, started on the scheduler's retry and has played cleanly since.
- stream_1791964957000_15c4cf published nothing for about 24 minutes before it was stopped at 13:30.
- The disney script still prints access tokens to stderr (logged as `scriptError`; 267 of those on SportsCenter alone).

**Files changed:** PROGRESS.md

**Commands run:**
```bash
ssh -o BatchMode=yes root@23.237.79.50 'python3 /tmp/rsa_s2.py; rm /tmp/rsa_s2.py'
```
Read-only: errors per stream for the last 2h, grouped by minute and by proxy. The temporary scripts were removed from the server.

```bash
ssh -o BatchMode=yes root@23.237.79.50 'curl -x <proxy> https://ipinfo.io/json  (6–12 times per proxy)'
```
Times each proxy from vpsrestream1 itself.

**Follow-up (same day):** 79.127.168.43:50100 is not faulty. The seller caps it at 10 connections, and vpsrestream1 held 9 to it steadily, so every extra connection was refused. The engine rotates disney's 4 proxies without knowing that cap. claro does not use the pinggy `kdwjgspsyy` tunnel: it goes through the SSH SOCKS tunnel on 127.0.0.1:1080 (~104 connections), with pinggy `kcsilonspa` as a backup. A claro manifest read through that tunnel normally takes ~1.8s (16 KB, one redirect). The 77 slow reads in 6h (median 21.7s, max 88s) are spikes, likely the small manifest stuck behind ~48 video downloads in the one SSH connection (not verified). The provider budget is not the cause: claro uses 48 of 50 slots, and manifests get extra headroom. Manifest reads use the provider's full 30s timeout × 2 attempts, and there is no separate manifest proxy (only the proxyManifest/proxyMedia on-off switches).

**Unfinished:** Possible code changes: a per-proxy connection limit, a shorter manifest-read timeout that fails over to the next proxy, and a separate manifest proxy. The fallingBehind rounding fix in core/src/live.c, `ClientAliveInterval 15`, and removing the token prints from disneyplus.py are still open. vpsrestream1 still runs the 2026-10-13 14:31 build and needs a rebuild for the step-down fix.

### 2026-10-14 — Third log review on vpsrestream1 (15:23–17:15 UTC)
**What was done:** Read-only review. Much quieter than the earlier window: no `pollSlow` at all, one small burst (16:15, 5 errors), and only 8 proxy failures (79.127.168.43 timeout ×3, kdwjgspsyy timeout ×3, nordvpn SSL ×2). Connections dropped: SOCKS tunnel 52 (was 104), 79.127.168.43 at 6 of its 10. claro is nearly clean (worst 0.88x). Globo Recife skipped 13 segments at 15:49 and GLOBO BH skipped 3 at 15:43. disney turned over most events on the hour and half hour, and all of them started normally. One event (stream_1791993777000_230eeb) got wrong keys at 16:10: it is encrypted under KID 6024b91e…, the script returned keys for 42270234…, the same mismatch as stream_1791986572000_0d23c9 at 14:10. It restarted and the logs don't show it running after that. stream_1791993777000_4310b3 published nothing for 2 minutes at 16:09 before it stopped. Token prints from the disney script continue (9 per start).

**Files changed:** PROGRESS.md

**Commands run:**
```bash
ssh -o BatchMode=yes root@23.237.79.50 'python3 - <<EOF ... EOF; ss -tn state established ...'
```
Read-only: per-stream errors since 15:23, proxy failures and open connections per proxy.

**Unfinished:** Same as above (per-proxy connection limit, shorter manifest timeout with failover, token prints, fallingBehind rounding, rebuild vpsrestream1).

### 2026-10-14 — Fourth log review on vpsrestream1 (17:15–19:31 UTC)
**What was done:** Read-only review. Quiet except for one event: at 18:24–18:25 requests through every proxy timed out together, both claro's SOCKS tunnel and all four disney proxies. All 6 claro channels and several disney events restarted at once and recovered within about a minute. Because every path failed at the same moment, the cause is probably vpsrestream1's own network for that minute rather than any one proxy (not verified). curta: the claro source itself jumped its timeline at 18:48 (+39–48s) and 19:07 (+36–40s); the engine logs "no download failed, so the source spliced". Those jumps account for most of curta's 466 skipped segments; one 404 on stream_05 at 18:47 added a real drop. One disney event (stream_1792002783000_cc7a5b) never started: two rounds of 4 cdm tries at 18:33 returned no usable keys. The other 5 disney events run fine (worst 0.58x on Estados Unidos x Espanha). sshd on vpsrestream1 allows root password login, and the journal shows 7653 failed password attempts in 24h.

**Files changed:** PROGRESS.md

**Commands run:**
```bash
ssh -o BatchMode=yes root@23.237.79.50 'python3 - <<EOF ... EOF; journalctl ...; grep sshd_config'
```
Read-only: per-stream errors since 17:15, the 18:24 outage timeline, curta's discontinuities, the failed cdm start, sshd settings.

**Unfinished:** As before, plus: consider key-only SSH on vpsrestream1 (`PasswordAuthentication no`) once the Windows tunnel uses its key.

### 2026-10-15 — Fifth log review on vpsrestream1 (2026-10-14 19:31 – 04:31 UTC)
**What was done:** Read-only review of the night. 15 streams are running (5 claro, 10 disney, which is disney's maxStreamsConcurrency). Errors per hour: 12–36 until 22:00, then 376 (22h) and 352 (23h), 112 at 00h, 30–160 afterwards. The bad hours are a proxy problem across the board: all four disney proxies failed at the same time, each 30–60 times an hour from 22:00 to 02:00 (timeouts, "could not connect" and a new "Transferred a partial file", where a connection was cut mid-transfer). The number of disney events stayed at 10 all night and the SSH tunnel did not reconnect, so neither of those changed. Follow-up check: our own load was not higher (fewest event starts, 4, in the 23h hour with 170 proxy failures), and claro on the same server was fine. claro's SSH tunnel and disney's pinggy proxy `kdwjgspsyy` both come from the same Brazilian machine (164.163.208.85); in the bad hours the tunnel had about 5 failures an hour and kdwjgspsyy 34–62. So the shared factor is the destination, Disney's CDN (`*-akc-sa-east-1.media.dssott.com`), most likely slow during Brazil's evening peak (19:00–23:00 there). Not confirmed with a direct measurement. At 22:22 many streams restarted together (GLOBO BSB and 8 disney events), and the worst events lost 35–47 segments to live-edge skips. claro was mostly fine: pollSlow 52 (median 23s), 17 segments skipped on GLOBO BSB around its 22:22 restart, all channels running now. The disney script still prints tokens (980 lines).

**Files changed:** PROGRESS.md

**Commands run:**
```bash
ssh -o BatchMode=yes root@23.237.79.50 'python3 - <<EOF ... EOF; journalctl ...; ss -tn ...'
```
Read-only: per-stream errors since 19:31, proxy failures per hour, disney concurrency per 30 min, tunnel reconnects.

**Unfinished:** a451f93 (proxy connection limit, manifest stall timeout, alerts only on lasting proxy failure) and its selftest update 265e017 are on main. The functional test of the connection limit on moonmuaaz was stopped mid-run; its leftover test processes were stopped and the files moved to /tmp/rsa-test/trash/pl-*. vpsrestream1 has not been rebuilt.

### 2026-10-15 — Proxy connection limit, manifest stall timeout, quieter proxy alerts, proxy table and live traffic view
**What was done:**
- **`proxyConnectionLimit`** (provider option, 0 = off): caps the connections kept open through each proxy in the provider's list. A request waits for a free slot or uses another proxy instead of being refused, and an idle kept-alive connection gives up its slot when a request is waiting.
- **`manifestStallSeconds`** (default 15, 0 = off): a manifest or playlist request that receives no data for that long is abandoned and retried on a fresh connection or the next proxy, instead of waiting out the 30s timeout.
- **`proxyHealth`:** one failed request sets the proxy aside for 15s and is logged as `info`. The `error` (and Discord webhook) comes once, when the proxy is set aside 3 times in a row.
- **Panel:** Provider settings shows a live **Proxy connections** table: open TCP connections from the server to each proxy (from /proc/net/tcp; idle connections and the script's included), the limit, requests in use, and status. A new eye icon on every stream card opens that channel's **live traffic**: download rate, requests and errors per minute, per-proxy totals for the channel, and the latest 60 requests (type, proxy, file, status, size, time, speed). New endpoints: `GET /api/providers/<id>/proxies` and `GET /api/streams/<id>/traffic`. Proxy credentials are never sent.

**Files changed:** apps/server/net.c, apps/server/net.h, apps/server/main.cpp, core/include/restream.h, core/include/rs_source_policy.h, core/src/server.c, core/src/provider_options.c, core/tests/selftest_main.c, public/app.js, public/index.html, public/styles.css, API.md, EVENTS.md, public/docs/API.md, public/docs/EVENTS.md, PROGRESS.md

**Commands run:**
```bash
ssh root@moonmuaaz.xyz 'cd /root/ReStreamAir && git pull && cmake --build build && cd build && ctest'
```
Builds on moonmuaaz and runs the test suites (2 of 2 passed; no new compiler warnings).

```bash
ssh root@moonmuaaz.xyz 'cd /tmp/rsa-test/pl2 && ./run.sh 0; ./run.sh 3'
```
Throwaway server on 127.0.0.1:8797. 3 engine channels with 4 downloads each, through 2 test proxies that refuse anything over 4 connections, fed by a live fMP4 origin answering each segment after 3s. Without a limit: 452 refused connections, 7 errors/min. With limit 3: 0 refused, 0 errors, same download rate (37.7 vs 37.8 KB/s). `/proxies` open counts matched the proxies' own; `/traffic` listed each request. Test files moved to /tmp/rsa-test/trash/pl2-*; processes stopped.

**Unfinished:** vpsrestream1 needs a rebuild to get all of this; then set the disney provider's "Connections per proxy" to 8 (the seller allows 10; the script's requests are not counted by the engine). The panel UI has only been syntax-checked (node --check), not opened in a browser.

### 2026-10-15 — Log review on vpsrestream1 after its rebuild (04:31–09:38 UTC)
**What was done:** Read-only review. vpsrestream1 now runs the new build (binary 05:09, service restarted 06:52). Quieter alerts confirmed: 54 single proxy failures logged as `info`, only 1 proxy `error` (claro's tunnel returned HTTP 502 at 04:44, before the restart). disney's "Connections per proxy" is not set yet (0), so the limit is off: 79.127.168.43 had 9 and 178.92.209.173 had 10 connections open (at the seller's cap). claro: most skipped segments came in 06:30–06:50 on the old build (404s, HTTP/2 framing errors through both claro proxies, provider restarts at 06:38, then the rebuild restart); since 07:00 only GLOBO BH (18) and Globo Recife (2). disney: DP World India Championship has dropped segments steadily since 06:50 (about 40–140 per 10 min). Since 07:00 that is 264 evicted from a full queue plus 34 that left the source's window, meaning its 720p video (7.4 Mbps) arrives slower than it plays over its 2 download connections. Other disney events have the same settings and are not skipping. Wrong keys on 5 event starts (the script returned another event's KID). The script still prints tokens (546 lines). One SSH connection attempt timed out; the box and service were fine on retry.

**Files changed:** PROGRESS.md

**Commands run:**
```bash
ssh -o BatchMode=yes root@23.237.79.50 'python3 - <<EOF ... EOF; ss -tn ...'
```
Read-only: errors per stream and hour since 04:31, segments skipped per 10 min, provider options, open proxy connections.

**Unfinished:** Set disney's "Connections per proxy" to 8. Check DP World India in the new live traffic view (eye icon) to see which proxy its downloads use and how fast they are.

### 2026-10-15 — License the key at the live edge (fixes most "Wrong decryption keys")
**What was done:** On vpsrestream1 almost every Disney start logged "Wrong decryption keys" and restarted 30s later (35 in 24h). Traced on Linha de Passe at 13:25: the start inspected the right variant (`cmaf-cenc-ctr-7000K`), but `hls_init_url` read the playlist's first `#EXT-X-MAP` (`map96`, KID `7929…`). The engine starts at the live edge, under `map97` (KID `9aac…`). Disney rotates the key with the MAP, so around a rotation the live window lists both. `hls_init_url` now returns the MAP the newest segment uses. The start also licenses any older MAP still under the newest 8 segments, so a start mid-rotation gets both keys and nothing else. The 720p step-down check and pass-through use the same function and get the same fix.

**Files changed:** core/src/server.c, PROGRESS.md

**Commands run:**
```bash
ssh root@23.237.79.50 'python3 - <<EOF ... EOF'
```
Read-only: traced the Linha de Passe start (variant and init URLs, KIDs) and counted wrongKeys in the last 24h.

```bash
ssh root@moonmuaaz.xyz 'cd /tmp/rsa-test/edge && cc -fsanitize=address,undefined edge.c -o edge && ./edge'
```
Standalone test of the new playlist logic: rotation before the edge → `map97` only; rotation inside the edge → `map96` and `map97`; single MAP; MAP with no segments; TS without MAP; MAP with BYTERANGE. All correct, sanitizer-clean. Harness moved to /tmp/rsa-test/trash/edge-*.

```bash
ssh root@moonmuaaz.xyz 'cd /root/ReStreamAir && git pull && cmake --build build && cd build && ctest'
```
Builds on moonmuaaz and runs the test suites (2 of 2 passed).

**Unfinished:** vpsrestream1 needs a rebuild to get this. A key rotation while a stream is already running still needs a new key mid-stream, which the engine does not fetch yet (see restreamair-cdm-scripting: key rotation pending).

### 2026-10-15 — Key rotation while a stream plays (no more restart)
**What was done:** When Disney rotates the key on a running stream, the engine no longer stops with "Wrong decryption keys" and restarts 30s later. Each HLS segment now carries the `#EXT-X-MAP` it is under, and the engine records every init's KID and decrypts each segment with its own init's key, so segments queued before a rotation keep the old key. When a new KID has no key, the engine asks the server, which runs the stream's `cdm` action for that KID in the background (the start-time key step without the manifest). The engine holds the new-key segments meanwhile, re-asks every 20s, and after 60s falls back to the old stop-and-restart. The keys go to the running engine and are added to the stream's saved keys. Logged as `keyRotation`. Documented in SCRIPTING.md.

**Files changed:** core/src/live.c, core/include/rs_live.h, core/src/server.c, core/src/m3u8.c, core/include/rs_m3u8.h, apps/server/rs_dash.c, core/tests/selftest_main.c, SCRIPTING.md, public/docs/SCRIPTING.md, PROGRESS.md

**Commands run:**
```bash
ssh root@moonmuaaz.xyz 'cd /root/ReStreamAir && git pull && cmake --build build && cd build && ctest'
```
Builds on moonmuaaz and runs the test suites (2 of 2 passed, including new playlist-parser tests for per-segment init URLs).

```bash
ssh root@moonmuaaz.xyz 'cd /tmp/rsa-test/kr && ./make.sh; packager ...; ./run.sh'
```
End-to-end on a throwaway server (127.0.0.1:8797). The same 120s video was packaged twice with shaka-packager (CENC, KID 1111… / KID 2222…) and served as a live HLS stream that switches init and key at segment 21 (~30s in), with a stub `cdm` script that answers only for the KIDs in `kids=`. Result: the key was requested at 28.2s and arrived 3.3s later, with no wrongKeys and no restart, and both keys were saved on the stream. A control run without a switch had 0 decoder errors. The whole output window across the switch decoded 2,900 frames with 0 errors, and 100% of them are identical to frames of the unencrypted source. (A first attempt with ffmpeg-encrypted media was invalid: ffmpeg cannot decrypt its own `-encryption_scheme` output.) Test files moved to /tmp/rsa-test/trash/kr-*.

**Unfinished:** vpsrestream1 needs a rebuild to get this, along with the live-edge key fix (e3dbb56).

### 2026-10-17 — Disney events stopped loading: provider scripts now fall over to the next proxy
**What was done:** On vpsrestream1 the disney provider had 0 events and nothing running since about 01:00 UTC. Both seller proxies (`178.92.209.173:50100`, `79.127.168.43:50100`) have answered `407 Proxy Authentication Required / PROXY_AUTH_FAILED` since 00:36 UTC on 2026-10-17. The proxies are up, but their username/password is rejected (likely an expired or renewed plan). Provider script actions were always given the first proxy line, a seller proxy, so all 51 events runs failed with `CONNECT tunnel failed, response 407` even though NordVPN and pinggy work. Fixed in core/src/server.c and apps/server/net.c:
- Provider script jobs (Load events/channels, login, …) retry through the next proxy line when the failure is the proxy's, the same as stream starts already did.
- The proxy line a provider's script last got through is remembered (in memory) and tried first by later script runs and stream starts.
- A proxy's 407 is reported as "The proxy rejected its username/password (HTTP 407)" instead of "Could not connect to server".

**Files changed:** core/src/server.c, apps/server/net.c, PROGRESS.md

**Commands run:**
```bash
ssh root@23.237.79.50 'curl -sv -x <each proxy> https://ipinfo.io/json; nc -vz <proxy> 50100; python3 <log scan>'
```
Read-only diagnosis: per-proxy live test from vpsrestream1 (both seller proxies 407 PROXY_AUTH_FAILED, the others OK), TCP checks from three hosts, when the failures started, and why events stopped loading.

```bash
ssh root@moonmuaaz.xyz 'cd /root/ReStreamAir && git pull && cmake --build build && cd build && ctest'
```
Builds on moonmuaaz and runs the test suites (2 of 2 passed).

```bash
ssh root@moonmuaaz.xyz 'cd /tmp/rsa-test/pf && ./run.sh'
```
Throwaway server with a proxy that answers 407 listed first, a working proxy second, and a stub events script that fails like the Disney one. Run 1: failed through the 407 proxy, retried through the next and imported 2 events. Run 2: went straight to the remembered working proxy. A probe through the 407 proxy (http and https) reported "The proxy rejected its username/password (HTTP 407)". Test files moved to /tmp/rsa-test/trash/pf-*.

**Unfinished:** vpsrestream1 needs a rebuild to get this. Until then, move a working proxy (pinggy `kdwjgspsyy` or NordVPN `br53`) to the top of disney's proxy list and press Load events. The seller credentials need renewing either way.
