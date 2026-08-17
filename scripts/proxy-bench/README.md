# proxy-bench

Decides whether a proxy can carry N live streams at a given quality, before you
wire it into a provider and find out from stuttering playback.

```bash
./proxy-bench.py http://user:pass@1.2.3.4:8080                        # 7 x 720p
./proxy-bench.py https://user:pass@br160.proxy.nordvpn.com:89 --quality 1080
./proxy-bench.py http://1.2.3.4:8080 --quality both --streams 4
```

**Run it on the restreaming server, not your laptop.** The route from a local
machine to the proxy is not the route the server takes, and that difference has
been a factor of four in practice — enough to reverse the verdict.

```bash
ssh root@yourserver
cd /root/proxy-bench && ./proxy-bench.py '<proxy-url>' --quality 720
```

Exit status is 0 when the proxy meets the target and 1 when it does not, so it
can gate a deploy.

## Why three separate measurements

A proxy can fail on any one of these while looking perfect on the other two.
Testing only bandwidth is how a proxy that "has enough speed" still ends up
stalling every stream.

**1. Manifest reliability.** Every rendition re-reads the MPD every couple of
seconds. The engine treats a failed manifest fetch as a throttle and backs off
15s, doubling on repeats — so a proxy that refuses even a third of those spends
most of its time in backoff no matter how much bandwidth it has. Rotating-exit
proxies fail here: a CDN that checks entitlement per IP accepts some exits and
403s the others, at random. The script reports distinct egress IPs for exactly
this reason; more than one is a warning sign.

**2. Aggregate throughput.** N streams share one proxy. What matters is the
ceiling with many connections open, not what a single connection gets — some
proxies cap per-connection, others cap in total, and only the second kind
scales with `parallelDownloads`.

**3. Per-stream realtime factor.** A live stream must be fetched *faster* than
it plays, or it can never recover from a hiccup. Below 1.0x it is falling
behind permanently. 1.2x is the pass mark here; between 1.0x and 1.2x is
reported as MARGINAL because there is no room for a slow poll.

## Reading the output

```
   stream 1: 12/12 segs     9.0 MB   1.82x realtime      <- green, comfortable
   stream 2: 10/12 segs     3.4 MB   0.61x realtime      <- red, falling behind
   aggregate: 76/84 segments, 26.1 MB in 42.9s = 4.86 Mbps
   FAIL — slowest stream 0.56x (needs >=1.0); only 90% of segments delivered
```

The verdict is driven by the **slowest** stream, not the average: one rendition
that cannot keep up stalls that channel regardless of how the others do.

## Options

| flag | default | meaning |
|---|---|---|
| `--quality` | `720` | `720`, `1080`, or `both` |
| `--streams` | `7` | concurrent streams to prove |
| `--parallel` | `6` | segment fetches per stream; match the stream's `parallelDownloads` |
| `--segments` | `12` | segments pulled per stream |
| `--probes` | `12` | manifest reliability probes |
| `--channels` | built-in | file of MPD URLs, one per line |
| `--timeout` | `60` | per-request timeout, seconds |

Renditions are chosen by **declared height**, not by representation id —
`stream_05` is 1080p video on one channel and audio on another, so ids are not
comparable across channels and picking by id silently tests the wrong thing.

To test a different source, put its MPD URLs in a file:

```bash
./proxy-bench.py '<proxy>' --channels my-channels.txt --quality 1080
```

## Results so far

Measured from the London server against the claro channels.

| proxy | manifest | aggregate | 7 x 720p |
|---|---|---|---|
| pinggy free tunnel | — | 0.18 Mbps, 6–13% delivered | no, by ~100x |
| `177.155.125.100:25256` | **60–70%** (2 rotating exits) | **24 Mbps** | fast enough, but the manifest failures stall the engine |
| `br160.proxy.nordvpn.com:89` | **100%** (1 static exit) | **4.9 Mbps** | no — every stream 0.56–0.73x |

The two paid proxies fail for opposite reasons, which is the whole argument for
measuring reliability and throughput separately. What 7 x 720p needs is roughly
**21 Mbps sustained with a stable exit IP** — about what a $5/month VPS in the
target country provides on its own.
