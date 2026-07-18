import Foundation
#if canImport(Musl)
import Musl
#elseif canImport(Glibc)
import Glibc
#endif

// Playlist text parsing/rewriting lives in M3U8Rewriter.swift, and
// ffmpeg/ffprobe discovery in FFmpegLocator.swift — both split out of this
// file since it was otherwise one large grab-bag of unrelated concerns
// (playlist text parsing, CLI argument parsing, and the actual live-poll
// worker loop). This file is just the worker: CLI docs/dispatch
// (LiveM3U8ActionRuntime) and the poll loop itself (LiveMPDToM3U8).

struct M3U8ArgumentDoc {
    let name: String
    let required: Bool
    let description: String
}

struct M3U8FunctionDoc {
    let name: String
    let purpose: String
    let arguments: [M3U8ArgumentDoc]
    let output: String
}

struct M3U8FunctionCall {
    let name: String
    let args: [String: String]
}

struct DownloadedSegment {
    let remoteURL: URL
    let localURL: URL
    let playlistURI: String
    let number: Int
    let duration: Double
    let groupKey: Int?
    /// Where this segment actually starts on the media timeline (moof>traf>tfdt
    /// baseMediaDecodeTime, in the track's mdhd timescale). Nil if the segment
    /// had no readable tfdt.
    var startTime: UInt64? = nil
    /// True when this segment does not continue the previous one's media
    /// timeline, so the playlist must carry an EXT-X-DISCONTINUITY before it.
    var discontinuity: Bool = false
}

struct LiveM3U8Options {
    var mpdURL: URL
    /// Ordered CDN mirror manifest URLs. When the primary (`mpdURL`) manifest
    /// fetch fails, the worker fails over to these in order and sticks with
    /// whichever responds; segments then resolve against that active mirror.
    var fallbackURLs: [URL] = []
    var representationID: String?
    var periodSelector: String?
    var timeout: TimeInterval
    var headers: [(String, String)]
    var proxy: String?
    var baseURLOverride: URL?
    var tempDir: URL
    var outputURL: URL
    var playlistSegments: Int
    var keepSegments: Int
    var downloadAhead: Int
    /// How many of the newest downloaded segments to withhold from the
    /// advertised playlist — the live-edge hold-back. Those segments are
    /// already on disk, so a player that live-syncs to the end of the playlist
    /// still has this many freshly-downloaded segments queued ahead of it,
    /// which is what stops it draining to the true edge and stalling. Defaults
    /// to `downloadAhead - playlistSegments`, i.e. the fetch-ahead the caller
    /// asked for becomes real headroom instead of just sitting unused on disk.
    var holdBackSegments: Int
    var pollInterval: TimeInterval?
    var defaultDuration: Double
    var probeDuration: Bool
    var forceOffline: Bool
    var maxPolls: Int?
    var decryptionKeys: [String: Data] = [:]
    var manifestHeaders: [(String, String)] = []
    var segmentUrlParams: String = ""
    /// Shifts this representation's segment presentation times by this many
    /// milliseconds (signed) — a lip-sync knob, meant to be set only on the
    /// audio representation's worker. 0 (the default) does nothing.
    var audioDelayMs: Int = 0
}

enum LiveM3U8Error: Error, CustomStringConvertible {
    case usage(String)
    case unsupported(String)
    case file(String)

    var description: String {
        switch self {
        case .usage(let message), .unsupported(let message), .file(let message):
            return message
        }
    }
}

struct LiveM3U8ActionRuntime {
    static let functions: [M3U8FunctionDoc] = [
        M3U8FunctionDoc(
            name: "describeFunctions",
            purpose: "Return exported M3U8 actions, arguments, and output contract.",
            arguments: [],
            output: "JSON object with a functions array."
        ),
        M3U8FunctionDoc(
            name: "createLiveM3U8",
            purpose: "Poll a live DASH MPD, download new media segments into a temp queue, prune old files, and continuously write an HLS M3U8 playlist.",
            arguments: [
                M3U8ArgumentDoc(name: "mpd", required: true, description: "Live MPD URL."),
                M3U8ArgumentDoc(name: "representation", required: false, description: "Representation@id to convert. Recommended."),
                M3U8ArgumentDoc(name: "period", required: false, description: "Period id or zero-based index."),
                M3U8ArgumentDoc(name: "output", required: false, description: "Playlist path. Defaults to tempDir/live.m3u8."),
                M3U8ArgumentDoc(name: "tempDir", required: false, description: "Download directory. Defaults to a folder in the system temp directory."),
                M3U8ArgumentDoc(name: "playlistSegments", required: false, description: "Number of media segments to keep in the written playlist. Default 6."),
                M3U8ArgumentDoc(name: "keepSegments", required: false, description: "Number of downloaded old media segments to keep on disk — the recovery cushion for a player (or the ingest itself) that stalls briefly. Deliberately much bigger than the advertised playlist window. Default 60."),
                M3U8ArgumentDoc(name: "downloadAhead", required: false, description: "Number of MPD media segments requested per poll. Default playlistSegments + 2."),
                M3U8ArgumentDoc(name: "pollInterval", required: false, description: "Seconds between MPD polls. Defaults to MPD minimumUpdatePeriod or 2."),
                M3U8ArgumentDoc(name: "defaultDuration", required: false, description: "Fallback EXTINF duration when MPD and ffprobe do not provide one. Default 4."),
                M3U8ArgumentDoc(name: "probeDuration", required: false, description: "true to use ffprobe duration as a last-resort fallback when a segment has no declared duration. Off by default — no ffmpeg/ffprobe dependency unless explicitly opted into."),
                M3U8ArgumentDoc(name: "forceOffline", required: false, description: "true to allow static/offline MPDs. Default false."),
                M3U8ArgumentDoc(name: "maxPolls", required: false, description: "Stop after N MPD polls. Useful for tests. Default is infinite for live."),
                M3U8ArgumentDoc(name: "baseUrl", required: false, description: "Override base URL used to resolve segment paths."),
                M3U8ArgumentDoc(name: "decryptionKeys", required: false, description: "CENC clearkey KID:KEY hex pairs, separated by |. Decrypts downloaded segments and patches the init segment. macOS only."),
                M3U8ArgumentDoc(name: "segmentUrlParams", required: false, description: "Raw query-string fragment appended to every media/init segment URL before download, e.g. token=abc&region=us."),
                M3U8ArgumentDoc(name: "audioDelayMs", required: false, description: "Shifts this representation's segment tfdt presentation time by this many milliseconds (signed — negative moves it earlier). Intended for the audio representation only, to fix lip-sync drift. Default 0."),
                M3U8ArgumentDoc(name: "manifestHeader", required: false, description: "HTTP header string used only for the MPD fetch. Separate multiple headers with |. Falls back to `header` when unset."),
                M3U8ArgumentDoc(name: "timeout", required: false, description: "HTTP timeout in seconds. Default 30."),
                M3U8ArgumentDoc(name: "header", required: false, description: "HTTP header string used for segment downloads (and the MPD fetch when manifestHeader is unset). Separate multiple headers with |."),
                M3U8ArgumentDoc(name: "proxy", required: false, description: "HTTP or HTTPS proxy URL.")
            ],
            output: "JSON object with playlist path, tempDir, downloaded count, kept count, and live/offline mode."
        )
    ]

    static func run(_ arguments: [String]) {
        #if os(Linux)
        // This worker is spawned by the panel via Foundation.Process, so it
        // inherits the panel's signal mask — and libdispatch blocks most
        // signals (SIGTERM included) on every thread it touches. With SIGTERM
        // blocked, the panel's Process.terminate() can never stop us: we'd keep
        // polling the manifest and writing log lines as an orphan after the
        // stream is "stopped". Unblock SIGTERM (and SIGINT) so a normal stop
        // takes effect; the panel still escalates to SIGKILL as a backstop.
        var mask = sigset_t()
        sigemptyset(&mask)
        sigaddset(&mask, SIGTERM)
        sigaddset(&mask, SIGINT)
        sigprocmask(SIG_UNBLOCK, &mask, nil)
        #endif
        do {
            let call = try parseCall(arguments)
            let result = try execute(call)
            printJSON([
                "ok": true,
                "action": call.name,
                "result": result
            ])
        } catch {
            printJSON([
                "ok": false,
                "error": "\(error)"
            ])
            exit(1)
        }
    }

    static func parseCall(_ arguments: [String]) throws -> M3U8FunctionCall {
        var name: String?
        var args: [String: String] = [:]
        var index = 0

        while index < arguments.count {
            let argument = arguments[index]
            if argument == "--action" {
                index += 1
                guard index < arguments.count else {
                    throw LiveM3U8Error.usage("--action requires a function name.")
                }
                name = arguments[index]
            } else if argument == "--arg" {
                index += 1
                guard index < arguments.count else {
                    throw LiveM3U8Error.usage("--arg requires key=value.")
                }
                try addKeyValue(arguments[index], args: &args, name: &name)
            } else if argument.contains("="), !argument.hasPrefix("-") {
                try addKeyValue(argument, args: &args, name: &name)
            } else {
                throw LiveM3U8Error.usage("Use action=NAME key=value or --action NAME --arg key=value.")
            }
            index += 1
        }

        return M3U8FunctionCall(name: name ?? "createLiveM3U8", args: args)
    }

    static func addKeyValue(_ text: String, args: inout [String: String], name: inout String?) throws {
        guard let separator = text.firstIndex(of: "=") else {
            throw LiveM3U8Error.usage("Expected key=value, got '\(text)'.")
        }
        let key = String(text[..<separator])
        let value = String(text[text.index(after: separator)...])
        if key == "action" || key == "function" {
            name = value
        } else {
            args[key] = value
        }
    }

    static func execute(_ call: M3U8FunctionCall) throws -> Any {
        switch call.name {
        case "describeFunctions":
            return ["functions": functions.map(functionDocJSON)]
        case "createLiveM3U8", "m3u8", "liveM3U8":
            return try LiveMPDToM3U8(options: options(from: call.args)).run()
        default:
            throw LiveM3U8Error.usage("Unknown action '\(call.name)'. Use action=describeFunctions.")
        }
    }

    static func options(from args: [String: String]) throws -> LiveM3U8Options {
        let mpd = try required("mpd", args)
        guard let mpdURL = URL(string: mpd), mpdURL.scheme != nil else {
            throw LiveM3U8Error.usage("mpd must be an absolute URL.")
        }
        // CDN mirrors: newline-separated absolute URLs, in preference order.
        let fallbackURLs = (args["fallbackUrls"] ?? "")
            .split(whereSeparator: { $0 == "\n" || $0 == "\r" })
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
            .compactMap { URL(string: $0) }
            .filter { $0.scheme != nil }

        let playlistSegments = try positiveInt("playlistSegments", args, defaultValue: 6)
        // Deliberately decoupled from playlistSegments (the advertised live
        // window) — this is the on-disk recovery cushion. A player or the
        // ingest itself stalling for even a few playlist windows shouldn't
        // land on already-deleted segments with no way back to live; a
        // small multiple of the advertised window (previously a flat +4,
        // ~20s total) gave almost no margin for that.
        let keepSegments = max(try positiveInt("keepSegments", args, defaultValue: 60), playlistSegments)
        let downloadAhead = max(try positiveInt("downloadAhead", args, defaultValue: playlistSegments + 2), playlistSegments)
        // Hold-back can never be so large that fewer than one full advertised
        // window remains on disk (keepSegments >= playlistSegments already).
        let holdBackDefault = max(0, downloadAhead - playlistSegments)
        let holdBackSegments = min(
            try nonNegativeInt("holdBackSegments", args, defaultValue: holdBackDefault),
            max(0, keepSegments - playlistSegments)
        )
        let tempDir = URL(fileURLWithPath: args["tempDir"] ?? defaultTempDir())
        let outputURL = URL(fileURLWithPath: args["output"] ?? tempDir.appendingPathComponent("live.m3u8").path)

        var baseURLOverride: URL?
        if let baseUrl = args["baseUrl"] ?? args["baseURL"] {
            guard let url = URL(string: baseUrl), url.scheme != nil else {
                throw LiveM3U8Error.usage("baseUrl must be an absolute URL.")
            }
            baseURLOverride = url
        }

        return LiveM3U8Options(
            mpdURL: mpdURL,
            fallbackURLs: fallbackURLs,
            representationID: args["representation"] ?? args["representationID"],
            periodSelector: args["period"],
            timeout: try positiveDouble("timeout", args, defaultValue: 30),
            headers: try parseHeaders(args["header"]),
            proxy: args["proxy"],
            baseURLOverride: baseURLOverride,
            tempDir: tempDir,
            outputURL: outputURL,
            playlistSegments: playlistSegments,
            keepSegments: keepSegments,
            downloadAhead: downloadAhead,
            holdBackSegments: holdBackSegments,
            pollInterval: try optionalPositiveDouble("pollInterval", args),
            defaultDuration: try positiveDouble("defaultDuration", args, defaultValue: 4),
            probeDuration: try bool("probeDuration", args, defaultValue: false),
            forceOffline: try bool("forceOffline", args, defaultValue: false),
            maxPolls: try optionalPositiveInt("maxPolls", args),
            decryptionKeys: CENCDecryptor.parseCENCKeys(args["decryptionKeys"] ?? ""),
            manifestHeaders: try parseHeaders(args["manifestHeader"]),
            segmentUrlParams: args["segmentUrlParams"] ?? "",
            audioDelayMs: try optionalInt("audioDelayMs", args) ?? 0
        )
    }

    static func defaultTempDir() -> String {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("DashLiveM3U8-\(ProcessInfo.processInfo.processIdentifier)")
            .path
    }

    static func functionDocJSON(_ doc: M3U8FunctionDoc) -> [String: Any] {
        [
            "name": doc.name,
            "purpose": doc.purpose,
            "arguments": doc.arguments.map {
                ["name": $0.name, "required": $0.required, "description": $0.description]
            },
            "output": doc.output
        ]
    }

    static func parseHeaders(_ text: String?) throws -> [(String, String)] {
        guard let text, !text.isEmpty else { return [] }
        return try text.split(separator: "|").map { item in
            guard let separator = item.firstIndex(of: ":") else {
                throw LiveM3U8Error.usage("header must look like 'Name: value' or 'Name: value|Name2: value2'.")
            }
            let name = item[..<separator].trimmingCharacters(in: .whitespaces)
            let value = item[item.index(after: separator)...].trimmingCharacters(in: .whitespaces)
            guard !name.isEmpty else {
                throw LiveM3U8Error.usage("header name cannot be empty.")
            }
            return (String(name), String(value))
        }
    }

    static func required(_ key: String, _ args: [String: String]) throws -> String {
        guard let value = args[key], !value.isEmpty else {
            throw LiveM3U8Error.usage("Missing required argument '\(key)'.")
        }
        return value
    }

    static func bool(_ key: String, _ args: [String: String], defaultValue: Bool) throws -> Bool {
        guard let value = args[key] else { return defaultValue }
        switch value.lowercased() {
        case "true", "1", "yes": return true
        case "false", "0", "no": return false
        default: throw LiveM3U8Error.usage("\(key) must be true or false.")
        }
    }

    static func positiveInt(_ key: String, _ args: [String: String], defaultValue: Int) throws -> Int {
        guard let value = args[key] else { return defaultValue }
        guard let number = Int(value), number > 0 else {
            throw LiveM3U8Error.usage("\(key) must be a positive integer.")
        }
        return number
    }

    static func optionalPositiveInt(_ key: String, _ args: [String: String]) throws -> Int? {
        guard let value = args[key] else { return nil }
        guard let number = Int(value), number > 0 else {
            throw LiveM3U8Error.usage("\(key) must be a positive integer.")
        }
        return number
    }

    static func nonNegativeInt(_ key: String, _ args: [String: String], defaultValue: Int) throws -> Int {
        guard let value = args[key] else { return defaultValue }
        guard let number = Int(value), number >= 0 else {
            throw LiveM3U8Error.usage("\(key) must be a non-negative integer.")
        }
        return number
    }

    /// Like optionalPositiveInt but allows negative values (e.g. a lip-sync
    /// offset that can shift either direction) and zero.
    static func optionalInt(_ key: String, _ args: [String: String]) throws -> Int? {
        guard let value = args[key] else { return nil }
        guard let number = Int(value) else {
            throw LiveM3U8Error.usage("\(key) must be an integer.")
        }
        return number
    }

    static func positiveDouble(_ key: String, _ args: [String: String], defaultValue: Double) throws -> Double {
        guard let value = args[key] else { return defaultValue }
        guard let number = Double(value), number > 0 else {
            throw LiveM3U8Error.usage("\(key) must be greater than zero.")
        }
        return number
    }

    static func optionalPositiveDouble(_ key: String, _ args: [String: String]) throws -> Double? {
        guard let value = args[key] else { return nil }
        guard let number = Double(value), number > 0 else {
            throw LiveM3U8Error.usage("\(key) must be greater than zero.")
        }
        return number
    }

    static func printJSON(_ object: Any) {
        let normalized = DashSegmentsCLI.normalizeJSON(object)
        if let data = try? JSONSerialization.data(withJSONObject: normalized, options: [.prettyPrinted, .sortedKeys]),
           let text = String(data: data, encoding: .utf8) {
            print(text)
        }
    }
}

final class LiveMPDToM3U8 {
    private let options: LiveM3U8Options
    private let manifestClient: HTTPClient
    private let client: HTTPClient
    private let fileManager = FileManager.default
    private var queue: [DownloadedSegment] = []
    private var downloadedURLs = Set<URL>()
    private var initSegment: DownloadedSegment?
    /// DASH-IF low-latency chunk groups (see SegmentURL.groupKey) with at
    /// least one failed chunk — a decoder can't tolerate a gap in the
    /// middle of one GOP the way it tolerates a whole missing segment, so
    /// once any sibling chunk fails the rest of that group is discarded
    /// too rather than served with a hole in it.
    private var excludedGroups = Set<Int>()
    private var downloadedCount = 0
    /// Total media segments ever appended to `queue` — the source of
    /// EXT-X-MEDIA-SEQUENCE. Deliberately independent of DownloadedSegment
    /// .number (which, for DASH-IF low-latency `$SubNumber$` chunks, is a
    /// synthesized `originalNumber * 1000 + subNumber` sort/dedup key, not
    /// a plain per-segment counter). Per RFC 8216 §4.3.3.2 the media
    /// sequence number of consecutive segments must differ by exactly 1 —
    /// using the synthesized key directly made it jump by ~1000 per real
    /// segment, which spec-literal clients (ffplay's hls demuxer) read as
    /// "thousands of segments behind" and reacted to with an endless
    /// reload/reseek loop instead of ever settling into steady playback.
    private var totalSegmentsQueued = 0
    /// Discontinuities that have already rolled off the front of `queue` — the
    /// base for EXT-X-DISCONTINUITY-SEQUENCE (see writePlaylist).
    private var droppedDiscontinuities = 0
    private let cencDecryptor: CENCDecryptor?
    private var mediaTimescale: UInt32?
    /// Primary manifest URL followed by any CDN mirrors, in preference order.
    private let candidateURLs: [URL]
    /// The manifest URL currently serving us — everything (parsing base URL,
    /// segment resolution, logging) uses this, not the static primary, so a
    /// CDN failover carries through to segment fetches too.
    private var activeURL: URL

    init(options: LiveM3U8Options) {
        self.options = options
        self.candidateURLs = [options.mpdURL] + options.fallbackURLs
        self.activeURL = options.mpdURL
        self.manifestClient = HTTPClient(options: HTTPRequestOptions(
            timeout: options.timeout,
            headers: options.manifestHeaders.isEmpty ? options.headers : options.manifestHeaders,
            proxy: options.proxy
        ))
        self.client = HTTPClient(options: HTTPRequestOptions(
            timeout: options.timeout,
            headers: options.headers,
            proxy: options.proxy
        ))
        self.cencDecryptor = options.decryptionKeys.isEmpty ? nil : CENCDecryptor(keys: options.decryptionKeys)
    }

    func run() throws -> [String: Any] {
        try fileManager.createDirectory(at: options.tempDir, withIntermediateDirectories: true)
        try fileManager.createDirectory(at: options.outputURL.deletingLastPathComponent(), withIntermediateDirectories: true)

        var polls = 0
        var liveMode = true

        while true {
            polls += 1
            logEvent("fetchManifest", url: activeURL.absoluteString, status: "start")
            let mpdData: Data
            do {
                // A single transient blip fetching the manifest (origin
                // hiccup, DNS blip, momentary timeout) used to kill this
                // entire worker process outright — every representation's
                // segment flow stopped, and nothing restarted it, so every
                // downstream player (regardless of type) eventually ran out
                // of fresh segments and got stuck. Retry a few times — and,
                // when CDN mirrors are configured, fail over to them — before
                // giving up so routine network noise doesn't end the stream.
                mpdData = try fetchManifestWithRetries()
            } catch {
                logEvent("fetchManifest", url: activeURL.absoluteString, status: "error", message: "\(error)")
                throw error
            }
            logEvent("fetchManifest", url: activeURL.absoluteString, status: "success", bytes: mpdData.count)
            let mpd = try MPDParser(sourceURL: activeURL).parse(data: mpdData)
            liveMode = mpd.type == "dynamic"
            if !liveMode && !options.forceOffline {
                throw LiveM3U8Error.unsupported("MPD is not live/dynamic. Pass forceOffline=true to create a one-shot playlist for offline MPDs.")
            }

            let dashOptions = dashOptionsForPoll()
            let segments = try DashSegmentsCLI.expandSegments(mpd: mpd, options: dashOptions)
            try downloadNewSegments(segments)
            try pruneOldSegments()
            try writePlaylist(endList: !liveMode)

            if !liveMode || polls >= (options.maxPolls ?? Int.max) {
                break
            }
            let sleepSeconds = options.pollInterval ?? mpd.minimumUpdatePeriod ?? 2
            Thread.sleep(forTimeInterval: max(0.25, sleepSeconds))
        }

        return [
            "playlist": options.outputURL.path,
            "tempDir": options.tempDir.path,
            "downloaded": downloadedCount,
            "kept": queue.count,
            "playlistSegments": min(queue.count, options.playlistSegments),
            "live": liveMode
        ]
    }

    private func dashOptionsForPoll() -> Options {
        var dashOptions = Options(mode: .list)
        // Use the currently-active mirror so segment URLs resolve against the
        // same CDN the manifest came from.
        dashOptions.mpdURL = activeURL
        dashOptions.count = options.downloadAhead
        dashOptions.includeInitialization = true
        dashOptions.headers = options.headers
        dashOptions.proxy = options.proxy
        dashOptions.timeout = options.timeout
        dashOptions.representationID = options.representationID
        dashOptions.periodSelector = options.periodSelector
        dashOptions.baseURLOverride = options.baseURLOverride
        return dashOptions
    }

    /// Downloads this poll's not-yet-fetched segments concurrently (network is
    /// the bottleneck, and the segments are independent files) and returns a
    /// map of source URL → bytes written for the ones that succeeded. Only the
    /// download is parallel; `downloadNewSegments` still processes the results
    /// serially in timeline order, so decrypt / audio-delay / discontinuity /
    /// EXT-X-MEDIA-SEQUENCE behaviour is unchanged.
    ///
    /// Sub-numbered CMAF chunks (groupKey != nil) are deliberately left to the
    /// serial path: their "keep the clean gapless prefix, exclude the rest of
    /// the group on the first failure" rule depends on strict ascending order,
    /// which a parallel fetch can't guarantee.
    private func prefetchSegments(_ segments: [SegmentURL]) -> [URL: Int] {
        let targets = segments.filter { segment in
            !segment.isInitialization
                && segment.groupKey == nil
                && !downloadedURLs.contains(segment.url)
        }
        guard targets.count > 1 else { return [:] }

        var results: [URL: Int] = [:]
        let resultsLock = NSLock()
        // Bounded fan-out: enough to hide per-request latency without opening an
        // unreasonable number of simultaneous connections to one origin.
        let maxParallel = min(8, targets.count)
        let slots = DispatchSemaphore(value: maxParallel)
        let group = DispatchGroup()
        let workQueue = DispatchQueue(label: "restreamair.prefetch", attributes: .concurrent)

        for segment in targets {
            slots.wait()
            group.enter()
            workQueue.async {
                defer { slots.signal(); group.leave() }
                let fetchURL = self.options.segmentUrlParams.isEmpty
                    ? segment.url
                    : appendingQueryParams(segment.url, self.options.segmentUrlParams)
                let localURL = self.localSegmentURL(for: segment)
                // Best-effort: a prefetch miss just falls through to the serial
                // loop, which downloads it (with the real error handling) there.
                guard let bytes = try? self.client.download(fetchURL, to: localURL) else {
                    try? self.fileManager.removeItem(at: localURL)
                    return
                }
                resultsLock.lock(); results[segment.url] = bytes; resultsLock.unlock()
            }
        }
        group.wait()
        return results
    }

    private func downloadNewSegments(_ segments: [SegmentURL]) throws {
        // Warm the disk cache for this poll's segments in parallel first. The
        // network round-trip is the bottleneck: downloading a poll's whole
        // batch strictly one-at-a-time (the original behaviour) meant the
        // worker could add segments no faster than serial latency allowed, so
        // it never built a cushion ahead of the player — the player drained the
        // playlist to the live edge and stalled ("player catches it faster").
        // The serial loop below is unchanged in every other respect; it simply
        // adopts an already-downloaded file instead of fetching it again,
        // preserving decrypt / audio-delay / discontinuity / group-exclusion
        // ordering exactly.
        let prefetched = prefetchSegments(segments)
        for segment in segments {
            guard !downloadedURLs.contains(segment.url) else { continue }
            if let groupKey = segment.groupKey, excludedGroups.contains(groupKey) {
                // A sibling chunk of this segment already failed — this
                // chunk would just be a fragment of a GOP with a hole in
                // it, which is worse than no segment at all. Mark it seen
                // (so we don't keep re-requesting it) without downloading.
                downloadedURLs.insert(segment.url)
                continue
            }
            let fetchURL = options.segmentUrlParams.isEmpty ? segment.url : appendingQueryParams(segment.url, options.segmentUrlParams)
            let localURL = localSegmentURL(for: segment)
            logEvent("downloadSegment", url: fetchURL.absoluteString, status: "start", proxy: options.proxy)
            let bytes: Int
            // Adopt the parallel prefetch if it already landed this segment on
            // disk — no second network round-trip.
            if let prefetchedBytes = prefetched[segment.url] {
                bytes = prefetchedBytes
            } else {
            do {
                // Low-latency CMAF chunks are commonly requested right at
                // (or just before) the moment the origin finishes writing
                // them — a couple of short retries clears up the routine
                // "not ready yet" case without waiting a full poll cycle,
                // where the group would otherwise be discarded outright.
                bytes = try downloadWithRetries(fetchURL, to: localURL, retries: segment.groupKey != nil ? 2 : 0)
            } catch {
                // A single segment 404/500 (after retries) is still routine
                // at the live edge — the origin may simply never produce
                // this one. Killing the whole worker over one miss would
                // drop every other representation too; skip it and let the
                // next poll's (shifted) window move past it.
                logEvent("downloadSegment", url: fetchURL.absoluteString, status: "error", proxy: options.proxy, message: "\(error)")
                try? fileManager.removeItem(at: localURL)
                // Chunks within a group are always attempted in ascending
                // subNumber order (see expandTimeline), so anything of this
                // group already queued is a clean, gapless prefix — keep
                // it. Excluding the group only stops *later* chunks (which
                // would open a gap) from being requested; some DASH-IF
                // low-latency origins consistently never produce the very
                // last chunk of a segment, and a slightly truncated segment
                // beats discarding otherwise-good data.
                if let groupKey = segment.groupKey {
                    excludedGroups.insert(groupKey)
                }
                continue
            }
            }
            logEvent("downloadSegment", url: fetchURL.absoluteString, status: "success", proxy: options.proxy, bytes: bytes)
            decryptIfNeeded(localURL, isInitialization: segment.isInitialization)
            if segment.isInitialization {
                if let data = try? Data(contentsOf: localURL) { mediaTimescale = AudioDelay.mdhdTimescale(data) }
            } else {
                applyAudioDelayIfNeeded(localURL)
            }
            downloadedURLs.insert(segment.url)
            downloadedCount += 1

            guard let segmentDuration = try? duration(for: segment, localURL: localURL) else { continue }
            // Read the media timeline position from the finished file (after any
            // decrypt/audio-delay rewrite, so it reflects what we actually serve).
            let startTime = segment.isInitialization
                ? nil
                : (try? Data(contentsOf: localURL)).flatMap { AudioDelay.baseMediaDecodeTime($0) }
            var downloaded = DownloadedSegment(
                remoteURL: segment.url,
                localURL: localURL,
                playlistURI: playlistURI(for: localURL),
                number: segment.number ?? 0,
                duration: segmentDuration,
                groupKey: segment.groupKey,
                startTime: startTime
            )

            if segment.isInitialization {
                initSegment = downloaded
            } else {
                // Compare against whatever is currently last: live segments
                // arrive in timeline order, so that's this one's predecessor.
                downloaded.discontinuity = isTimelineBreak(previous: queue.last, next: downloaded)
                queue.append(downloaded)
                totalSegmentsQueued += 1
            }
        }
        // Sort once after the whole batch rather than after every append: the
        // queue is only ever consumed (pruneOldSegments/writePlaylist) once
        // this returns, and re-sorting per segment made the loop O(n² log n).
        //
        // Order by the media-timeline position (tfdt), NOT the DASH segment
        // `number`. For $Time$-addressed SegmentTimelines (media="…$Time$…", no
        // $Number$/startNumber) the number is window-relative and collides —
        // every segment ends up numbered the same. A sort on colliding keys is
        // not stable, so it scrambled the queue; `queue.last` then stopped being
        // the true newest segment, and isTimelineBreak() compared each new
        // segment against the wrong predecessor, emitting a spurious
        // EXT-X-DISCONTINUITY on nearly every segment (disc-sequence climbing
        // ~1/segment → constant player re-buffering). tfdt is the real monotonic
        // timeline; fall back to `number` only when a segment has no readable tfdt.
        queue.sort { a, b in
            switch (a.startTime, b.startTime) {
            case let (x?, y?) where x != y: return x < y
            default: return a.number < b.number
            }
        }
    }

    private func fetchManifestWithRetries(retries: Int = 5, delay: Double = 0.5) throws -> Data {
        // No mirrors configured → original behaviour: keep retrying the one URL.
        guard candidateURLs.count > 1 else {
            var attempt = 0
            while true {
                do { return try manifestClient.get(activeURL) }
                catch {
                    guard attempt < retries else { throw error }
                    attempt += 1
                    Thread.sleep(forTimeInterval: delay)
                }
            }
        }

        // Mirrors configured: try the active one first, then fail over through
        // the rest in order. Fewer retries per mirror (so one dead CDN doesn't
        // stall the whole poll) but every mirror gets a chance before we give
        // up and let the poll loop surface the error.
        let ordered = candidatesStartingFromActive()
        let retriesPerCandidate = 1
        var lastError: Error?
        for url in ordered {
            var attempt = 0
            while true {
                do {
                    let data = try manifestClient.get(url)
                    if url != activeURL {
                        logEvent("cdnFailover", url: url.absoluteString, status: "success",
                                 message: "manifest source switched to \(url.host ?? url.absoluteString)")
                        activeURL = url
                    }
                    return data
                } catch {
                    lastError = error
                    guard attempt < retriesPerCandidate else { break }
                    attempt += 1
                    Thread.sleep(forTimeInterval: delay)
                }
            }
            logEvent("cdnFailover", url: url.absoluteString, status: "error",
                     message: "mirror unavailable, trying next")
        }
        throw lastError ?? LiveM3U8Error.unsupported("All manifest sources failed.")
    }

    /// The candidate list rotated so the currently-active mirror is tried
    /// first — we stay on a working mirror and only cycle on failure.
    private func candidatesStartingFromActive() -> [URL] {
        guard let index = candidateURLs.firstIndex(of: activeURL), index > 0 else { return candidateURLs }
        return Array(candidateURLs[index...] + candidateURLs[..<index])
    }

    private func downloadWithRetries(_ url: URL, to localURL: URL, retries: Int) throws -> Int {
        var attempt = 0
        while true {
            do {
                return try client.download(url, to: localURL)
            } catch {
                guard attempt < retries else { throw error }
                attempt += 1
                Thread.sleep(forTimeInterval: 0.25)
            }
        }
    }

    /// Prints one compact JSON line per operational event to stdout. The
    /// panel process (which spawned this worker) already captures stdout
    /// per-stream and forwards each line into its LogStore — this is the
    /// only channel needed, no separate IPC.
    private func logEvent(_ event: String, url: String? = nil, status: String? = nil, proxy: String? = nil, bytes: Int? = nil, message: String? = nil) {
        var object: [String: Any] = ["event": event]
        if let url { object["url"] = url }
        if let status { object["status"] = status }
        if let proxy, !proxy.isEmpty { object["proxy"] = proxy }
        if let bytes { object["bytes"] = bytes }
        if let message { object["message"] = message }
        guard let data = try? JSONSerialization.data(withJSONObject: object), let text = String(data: data, encoding: .utf8) else { return }
        print(text)
        fflush(stdout)
    }

    /// Best-effort: a single bad segment must not kill an otherwise-healthy
    /// live restream loop, so decryption failures are logged and swallowed
    /// rather than thrown.
    /// Best-effort, same rationale as decryptIfNeeded: a shift failure
    /// (missing tfdt, unrecognized box layout) should leave the segment
    /// untouched rather than break the stream.
    private func applyAudioDelayIfNeeded(_ url: URL) {
        guard options.audioDelayMs != 0, let timescale = mediaTimescale, timescale > 0 else { return }
        guard let data = try? Data(contentsOf: url) else { return }
        let deltaUnits = Int64(options.audioDelayMs) * Int64(timescale) / 1000
        let shifted = AudioDelay.shiftSegment(data, byTimescaleUnits: deltaUnits)
        try? shifted.write(to: url, options: [.atomic])
    }

    private func decryptIfNeeded(_ url: URL, isInitialization: Bool) {
        guard let cencDecryptor else { return }
        guard let data = try? Data(contentsOf: url) else { return }
        do {
            let output = isInitialization ? cencDecryptor.patchInitSegment(data) : try cencDecryptor.decryptMediaSegment(data)
            try output.write(to: url, options: [.atomic])
        } catch {
            FileHandle.standardError.write(Data("warning: CENC decrypt failed for \(url.lastPathComponent): \(error)\n".utf8))
        }
    }

    /// A segment continues the previous one only if it starts, on the media
    /// timeline, where the previous one ended. Sources that splice (SCTE-35 ad
    /// insertion — this is common, and the DASH-IF reference stream does it)
    /// jump the timeline while the manifest goes on advertising a uniform
    /// segment duration, so the tfdt is the only signal that reveals it.
    ///
    /// Getting this wrong is what makes playback "stick": the playlist claims
    /// segment N is 2.000s, the media actually resumes ~1s later, and MSE ends
    /// up with a hole in the buffer that it can never fill — the player sits at
    /// the gap forever. ffmpeg silently re-offsets instead, which is why the
    /// same stream is fine from the CLI and stalls in a browser.
    private func isTimelineBreak(previous: DownloadedSegment?, next: DownloadedSegment) -> Bool {
        guard let previous, let timescale = mediaTimescale, timescale > 0,
              let previousStart = previous.startTime, let nextStart = next.startTime else { return false }
        let expected = Double(previousStart) / Double(timescale) + previous.duration
        let actual = Double(nextStart) / Double(timescale)
        // Normal encoder jitter (AAC frame alignment) is single-digit
        // milliseconds; a real splice moves the timeline by ~a second.
        return abs(actual - expected) > 0.25
    }

    private func duration(for segment: SegmentURL, localURL: URL) throws -> Double {
        if let duration = segment.duration, duration > 0 {
            return duration
        }
        if options.probeDuration, let probed = FFProbe.duration(for: localURL), probed > 0 {
            return probed
        }
        return options.defaultDuration
    }

    private func pruneOldSegments() throws {
        while queue.count > options.keepSegments {
            let removed = queue.removeFirst()
            // A discontinuity that rolls off the queue still has to be counted,
            // or EXT-X-DISCONTINUITY-SEQUENCE drifts and players lose track of
            // which timeline they're on across reloads.
            if removed.discontinuity { droppedDiscontinuities += 1 }
            try? fileManager.removeItem(at: removed.localURL)
        }
        // NOTE: downloadedURLs is deliberately never pruned. It is the *only*
        // thing that stops a segment which has aged out of our local `queue`
        // (keepSegments) but is still listed in the manifest (whose window is
        // larger — timeShiftBufferDepth) from being re-downloaded and appended a
        // second time. A re-append inflates totalSegmentsQueued without the
        // playlist window advancing, which desyncs EXT-X-MEDIA-SEQUENCE and
        // makes hls.js reject every reload with "media sequence mismatch".
        //
        // Bounding it by rebinding to the live queue was tried and reverted: it
        // reintroduced exactly that mismatch. Bounding it with a
        // "highest segment number seen" watermark instead was also tried and
        // reverted — sources like DASH-IF's simulator jump their numbering
        // forward, and a single jumped-ahead segment poisons the watermark so
        // every subsequent (lower-numbered) real segment is skipped forever and
        // the worker silently stops producing.
        //
        // The cost is bounded in practice: entries are one URL per segment for
        // as long as the worker lives, which is a slow, modest growth that a
        // stream restart clears. Correct playback beats saving those bytes.
        //
        // excludedGroups only grows on failures, but a long-running stream
        // hitting persistent errors shouldn't accumulate this forever —
        // once we're this far past a group the live timeline has already
        // moved on and will never revisit it.
        if excludedGroups.count > 500, let newest = excludedGroups.max() {
            excludedGroups = excludedGroups.filter { newest - $0 < 500 }
        }
    }

    private func writePlaylist(endList: Bool) throws {
        // Live-edge hold-back: end the advertised window this many segments
        // before the newest one on disk, so the player always has that many
        // already-downloaded segments queued ahead of the playlist end and
        // can't drain to the true edge and stall. Never hold back so far that
        // a full advertised window isn't available yet (startup), and never on
        // a finished/offline playlist (endList) where "live edge" is moot.
        let holdBack = endList ? 0 : min(options.holdBackSegments, max(0, queue.count - options.playlistSegments))
        let windowEnd = queue.count - holdBack
        let windowStart = max(0, windowEnd - options.playlistSegments)
        let playlistWindow = Array(queue[windowStart..<windowEnd])
        let targetDuration = max(1, Int(ceil(playlistWindow.map(\.duration).max() ?? options.defaultDuration)))
        // Must increment by exactly 1 per segment (RFC 8216 §4.3.3.2) — see
        // totalSegmentsQueued's doc comment for why this can't just be
        // playlistWindow.first?.number. The held-back tail segments were still
        // counted into totalSegmentsQueued, so subtract them too.
        let mediaSequence = max(0, totalSegmentsQueued - holdBack - playlistWindow.count)

        var lines = [
            "#EXTM3U",
            "#EXT-X-VERSION:7",
            "#EXT-X-INDEPENDENT-SEGMENTS",
            "#EXT-X-TARGETDURATION:\(targetDuration)",
            "#EXT-X-MEDIA-SEQUENCE:\(mediaSequence)"
        ]

        // Every discontinuity that is no longer *in* the playlist still has to be
        // accounted for: dropped off the queue entirely, plus any still in the
        // queue but behind the playlist window (windowStart is the index of the
        // first advertised segment, computed above with the hold-back applied).
        let discontinuitySequence = droppedDiscontinuities + queue[0..<windowStart].filter(\.discontinuity).count
        if discontinuitySequence > 0 {
            lines.append("#EXT-X-DISCONTINUITY-SEQUENCE:\(discontinuitySequence)")
        }

        if let initSegment {
            lines.append("#EXT-X-MAP:URI=\"\(escapeM3U8(initSegment.playlistURI))\"")
        }

        for (index, segment) in playlistWindow.enumerated() {
            // The tag marks a break *between* two segments, so it's meaningless
            // on the first one in the window — that break is already carried by
            // EXT-X-DISCONTINUITY-SEQUENCE above.
            if segment.discontinuity, index > 0 {
                lines.append("#EXT-X-DISCONTINUITY")
            }
            lines.append(String(format: "#EXTINF:%.3f,", segment.duration))
            lines.append(segment.playlistURI)
        }

        if endList {
            lines.append("#EXT-X-ENDLIST")
        }

        do {
            try (lines.joined(separator: "\n") + "\n").write(to: options.outputURL, atomically: true, encoding: .utf8)
        } catch {
            throw LiveM3U8Error.file("Could not write playlist \(options.outputURL.path): \(error)")
        }
    }

    private func localSegmentURL(for segment: SegmentURL) -> URL {
        let rawName = segment.url.deletingPathExtension().lastPathComponent
        // Always normalize to mp4/m4s regardless of the source's own extension
        // (e.g. Unified Streaming Platform serves fMP4/CMAF fragments under a
        // literal ".dash" extension). These are ISOBMFF fragments no matter
        // what the source calls them, and ffmpeg's HLS demuxer hard-rejects
        // any segment URL whose extension isn't in its allowed_extensions
        // list — ".dash" isn't on it, so ffplay refused to even parse the
        // playlist ("is not in allowed_segment_extensions"). ".m4s"/"mp4" are.
        let ext = segment.isInitialization ? "mp4" : "m4s"
        let prefix = segment.isInitialization ? "init" : "seg_\(segment.number ?? queue.count)"
        return options.tempDir.appendingPathComponent("\(prefix)_\(sanitize(rawName)).\(ext)")
    }

    private func playlistURI(for localURL: URL) -> String {
        let playlistDir = options.outputURL.deletingLastPathComponent().standardizedFileURL
        let segmentDir = localURL.deletingLastPathComponent().standardizedFileURL
        if playlistDir.path == segmentDir.path {
            return localURL.lastPathComponent
        }
        return localURL.absoluteString
    }

    private func sanitize(_ text: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "._-"))
        return text.unicodeScalars.map { allowed.contains($0) ? Character($0) : "_" }.map(String.init).joined()
    }

    private func escapeM3U8(_ text: String) -> String {
        text.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
    }
}

