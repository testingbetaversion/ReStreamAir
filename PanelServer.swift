import Foundation
#if canImport(Network)
import Network
#elseif canImport(Musl)
import Musl
#elseif canImport(Glibc)
import Glibc
#endif

struct PanelState: Codable {
    var providers: [Provider] = []
    var apiKeys: [APIKey] = []
    var bandwidthTotals: BandwidthTotals = BandwidthTotals()
    var systemPeaks: SystemPeaks = SystemPeaks()
    var settings: PanelSettings = PanelSettings()
    var adminUsers: [AdminUser] = []

    init(providers: [Provider] = [], apiKeys: [APIKey] = [], bandwidthTotals: BandwidthTotals = BandwidthTotals(), systemPeaks: SystemPeaks = SystemPeaks(), settings: PanelSettings = PanelSettings(), adminUsers: [AdminUser] = []) {
        self.providers = providers
        self.apiKeys = apiKeys
        self.bandwidthTotals = bandwidthTotals
        self.systemPeaks = systemPeaks
        self.settings = settings
        self.adminUsers = adminUsers
    }

    // Custom decoder: Swift's synthesized Decodable does NOT fall back to a
    // property's default value when a key is missing, so this is required
    // for old data/state.json files (written before these fields existed)
    // to keep loading instead of throwing keyNotFound.
    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        providers = try container.decodeIfPresent([Provider].self, forKey: .providers) ?? []
        apiKeys = try container.decodeIfPresent([APIKey].self, forKey: .apiKeys) ?? []
        bandwidthTotals = try container.decodeIfPresent(BandwidthTotals.self, forKey: .bandwidthTotals) ?? BandwidthTotals()
        systemPeaks = try container.decodeIfPresent(SystemPeaks.self, forKey: .systemPeaks) ?? SystemPeaks()
        settings = try container.decodeIfPresent(PanelSettings.self, forKey: .settings) ?? PanelSettings()
        adminUsers = try container.decodeIfPresent([AdminUser].self, forKey: .adminUsers) ?? []
    }
}

struct PanelSettings: Codable {
    var port: UInt16 = 8787
    /// Empty means "all interfaces" (the historical default). Set to e.g.
    /// "127.0.0.1" to only accept connections from the same machine, or a
    /// specific LAN IP to bind just that interface.
    var bindAddress: String = ""

    init(port: UInt16 = 8787, bindAddress: String = "") {
        self.port = port
        self.bindAddress = bindAddress
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        port = try container.decodeIfPresent(UInt16.self, forKey: .port) ?? 8787
        bindAddress = try container.decodeIfPresent(String.self, forKey: .bindAddress) ?? ""
    }
}

struct RepresentationMetaEntry: Codable {
    var type: String
    var bandwidth: Int?
    var width: Int?
    var height: Int?
    var codecs: String?
    var language: String?
}

// A named credential/session profile for a script provider — e.g. two
// different Disney+ logins run through the same script. `username`/
// `password` are sent as user=/password= (matching the param names
// o11-based scripts like disneyplus.py already read via
// o11.parse_params(sys.argv, 'user'/'password')) when set — either can be
// left blank if a script doesn't need it (a token-only login, or pairing,
// which is typically credential-less/code-based). `params` is an
// additional free-text "key=value key2=value2" string merged in after
// user=/password= for anything else a script wants (region, device id,
// PIN, ...). `enabled` lets an account be kept configured but temporarily
// excluded from login/pair/channels/events — disabled accounts are skipped
// when resolving which account to actually use, without deleting the
// account's credentials outright. ReStreamAir never inspects or persists
// session state itself — the script is entirely responsible for its own
// session/cookie file, keyed off whatever's in here.
struct ScriptAccount: Codable {
    var id: String
    var name: String
    var username: String = ""
    var password: String = ""
    var params: String = ""
    var enabled: Bool = true

    init(id: String, name: String, username: String = "", password: String = "", params: String = "", enabled: Bool = true) {
        self.id = id
        self.name = name
        self.username = username
        self.password = password
        self.params = params
        self.enabled = enabled
    }

    // Custom decoder: accounts saved before username/password/enabled
    // existed only have id/name/params.
    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(String.self, forKey: .id)
        name = try container.decode(String.self, forKey: .name)
        username = try container.decodeIfPresent(String.self, forKey: .username) ?? ""
        password = try container.decodeIfPresent(String.self, forKey: .password) ?? ""
        params = try container.decodeIfPresent(String.self, forKey: .params) ?? ""
        enabled = try container.decodeIfPresent(Bool.self, forKey: .enabled) ?? true
    }
}

// A provider bundled up as one portable file — its settings, script
// accounts (credentials and all — this file is exactly as sensitive as
// state.json itself), every stream, and the script's own source embedded
// directly (base64, so any file type/encoding round-trips safely) rather
// than just a path reference, so moving a working setup to another install
// is "download this file, upload it there," no separate copying of the
// script by hand. Deliberately reuses ScriptAccount/StreamConfig as-is
// (not a parallel export-only shape) — ids are meaningless across
// installs and get regenerated on import; activeScriptAccountName carries
// the active account across that regeneration by name instead.
struct ProviderExport: Codable {
    var restreamairExport: Int = 1
    var provider: ExportedProvider
    var scriptFile: ExportedScriptFile?
    var streams: [StreamConfig]
}

struct ExportedProvider: Codable {
    var name: String
    var logo: String
    var proxy: String
    var headers: String
    var segmentUrlParams: String
    var inheritUrlParams: Bool
    var scriptBind: String
    var scriptDoh: String
    var scriptWorker: String
    var scriptAccounts: [ScriptAccount]
    var activeScriptAccountName: String
    var accountSelectionMode: String = "fixed"

    init(name: String, logo: String, proxy: String, headers: String, segmentUrlParams: String, inheritUrlParams: Bool, scriptBind: String, scriptDoh: String, scriptWorker: String, scriptAccounts: [ScriptAccount], activeScriptAccountName: String, accountSelectionMode: String = "fixed") {
        self.name = name
        self.logo = logo
        self.proxy = proxy
        self.headers = headers
        self.segmentUrlParams = segmentUrlParams
        self.inheritUrlParams = inheritUrlParams
        self.scriptBind = scriptBind
        self.scriptDoh = scriptDoh
        self.scriptWorker = scriptWorker
        self.scriptAccounts = scriptAccounts
        self.activeScriptAccountName = activeScriptAccountName
        self.accountSelectionMode = accountSelectionMode
    }

    // Custom decoder: exports made before accountSelectionMode existed
    // don't have the key at all — synthesized Decodable would throw
    // keyNotFound despite the default value above (property defaults only
    // apply to the memberwise initializer, not synthesized decoding).
    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        name = try container.decode(String.self, forKey: .name)
        logo = try container.decode(String.self, forKey: .logo)
        proxy = try container.decode(String.self, forKey: .proxy)
        headers = try container.decode(String.self, forKey: .headers)
        segmentUrlParams = try container.decode(String.self, forKey: .segmentUrlParams)
        inheritUrlParams = try container.decode(Bool.self, forKey: .inheritUrlParams)
        scriptBind = try container.decode(String.self, forKey: .scriptBind)
        scriptDoh = try container.decode(String.self, forKey: .scriptDoh)
        scriptWorker = try container.decode(String.self, forKey: .scriptWorker)
        scriptAccounts = try container.decode([ScriptAccount].self, forKey: .scriptAccounts)
        activeScriptAccountName = try container.decode(String.self, forKey: .activeScriptAccountName)
        accountSelectionMode = try container.decodeIfPresent(String.self, forKey: .accountSelectionMode) ?? "fixed"
    }
}

struct ExportedScriptFile: Codable {
    var filename: String
    var contentBase64: String
}

struct Provider: Codable {
    var id: String
    var name: String
    var logo: String
    var streams: [StreamConfig]
    var proxy: String = ""
    var headers: String = ""
    var segmentUrlParams: String = ""
    var inheritUrlParams: Bool = false
    // Script provider fields — see ScriptRunner.swift. An empty scriptPath
    // means this is a normal URL-based provider; non-empty turns it into a
    // script provider (no separate kind/enum needed, same convention as the
    // empty-string-means-off proxy/headers fields above).
    var scriptPath: String = ""
    // Pass-through only — ReStreamAir doesn't implement source-interface
    // binding or DoH resolution itself, it just forwards these to the
    // script verbatim as bind=/doh=, same as the o11.py-based scripts this
    // is modeled on already expect.
    var scriptBind: String = ""
    var scriptDoh: String = ""
    var scriptWorker: String = ""
    var scriptAccounts: [ScriptAccount] = []
    var activeScriptAccountId: String = ""
    // How login/pair/channels/events picks which account to use.
    // "fixed" (default) — always activeScriptAccountId (falling back to the
    // first enabled account). "rotate" — cycles through every enabled
    // account in turn, one per call, tracked by lastRotatedAccountId.
    // "random" — a different random enabled account each call. Useful for
    // spreading calls across several credentialed accounts instead of
    // hammering one (rate limits, per-account concurrent-stream caps, etc).
    var accountSelectionMode: String = "fixed"
    // Rotation pointer only — not meaningful across installs, so it's
    // deliberately left out of ExportedProvider/export-import.
    var lastRotatedAccountId: String = ""

    init(id: String, name: String, logo: String, streams: [StreamConfig], proxy: String = "", headers: String = "", segmentUrlParams: String = "", inheritUrlParams: Bool = false, scriptPath: String = "", scriptBind: String = "", scriptDoh: String = "", scriptWorker: String = "", scriptAccounts: [ScriptAccount] = [], activeScriptAccountId: String = "", accountSelectionMode: String = "fixed") {
        self.id = id
        self.name = name
        self.logo = logo
        self.streams = streams
        self.proxy = proxy
        self.headers = headers
        self.segmentUrlParams = segmentUrlParams
        self.inheritUrlParams = inheritUrlParams
        self.scriptPath = scriptPath
        self.scriptBind = scriptBind
        self.scriptDoh = scriptDoh
        self.scriptWorker = scriptWorker
        self.scriptAccounts = scriptAccounts
        self.activeScriptAccountId = activeScriptAccountId
        self.accountSelectionMode = accountSelectionMode
    }

    // Custom decoder: see PanelState.init(from:) — old data/state.json files
    // predate proxy/headers/segmentUrlParams/inheritUrlParams/script*.
    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(String.self, forKey: .id)
        name = try container.decode(String.self, forKey: .name)
        logo = try container.decode(String.self, forKey: .logo)
        streams = try container.decode([StreamConfig].self, forKey: .streams)
        proxy = try container.decodeIfPresent(String.self, forKey: .proxy) ?? ""
        headers = try container.decodeIfPresent(String.self, forKey: .headers) ?? ""
        segmentUrlParams = try container.decodeIfPresent(String.self, forKey: .segmentUrlParams) ?? ""
        inheritUrlParams = try container.decodeIfPresent(Bool.self, forKey: .inheritUrlParams) ?? false
        scriptPath = try container.decodeIfPresent(String.self, forKey: .scriptPath) ?? ""
        scriptBind = try container.decodeIfPresent(String.self, forKey: .scriptBind) ?? ""
        scriptDoh = try container.decodeIfPresent(String.self, forKey: .scriptDoh) ?? ""
        scriptWorker = try container.decodeIfPresent(String.self, forKey: .scriptWorker) ?? ""
        scriptAccounts = try container.decodeIfPresent([ScriptAccount].self, forKey: .scriptAccounts) ?? []
        activeScriptAccountId = try container.decodeIfPresent(String.self, forKey: .activeScriptAccountId) ?? ""
        accountSelectionMode = try container.decodeIfPresent(String.self, forKey: .accountSelectionMode) ?? "fixed"
        lastRotatedAccountId = try container.decodeIfPresent(String.self, forKey: .lastRotatedAccountId) ?? ""
    }
}

struct StreamConfig: Codable {
    var id: String
    var name: String
    var kind: String
    var url: String
    var representation: String
    var period: String
    var proxy: String
    var playlistSegments: Int
    var keepSegments: Int
    var downloadAhead: Int
    var pollInterval: Double
    var forceOffline: Bool
    var status: String
    var lastError: String?
    var logo: String = ""
    var decryptionKeys: String = ""
    var hlsKey: String = ""
    var hlsIV: String = ""
    var representations: [String] = []
    var representationMeta: [String: RepresentationMetaEntry] = [:]
    var manifestHeaders: String = ""
    var mediaHeaders: String = ""
    var hlsKeyHeaders: String = ""
    /// Lip-sync offset in milliseconds (signed), applied only to whichever
    /// selected representation is type "audio" — see AudioDelay.swift.
    var audioDelayMs: Int = 0

    // Channel/event-import fields (see the Script providers README section)
    // — populated exclusively by importing a provider's `channels`/`events`
    // script output, never by the manual stream editor, so they're left out
    // of the big explicit init below (already 23 params) and just mutated
    // after construction by the import code. None of these feed the
    // playback pipeline yet (that's session-manifest live playback, still
    // planned) — they're stored now so a re-import is idempotent and the
    // data isn't silently discarded before that lands.
    var sourceType: String = ""       // "" (manual) | "channel" | "event"
    var mode: String = "live"         // "live" | "vod", the script JSON's "Mode" — independent of `kind` (mpd/m3u8 protocol)
    var sessionManifest: Bool = false
    var scriptParams: String = ""
    var cdmType: String = ""
    var useCdm: Bool = false
    var scriptVideoSelector: String = ""  // e.g. "best" — which representation(s) to use, not yet acted on
    var scriptAudioSelector: String = ""  // e.g. "desc:en"
    var onDemand: Bool = false
    var speedUp: Bool = false
    var autostart: Bool = false
    var scriptStart: Int? = nil       // unix seconds, event on-air window
    var scriptEnd: Int? = nil
    var recordEvent: Bool = false

    // Input/output pipeline (Phase 1 — data model + UI only, see README's
    // "Input/output pipeline" section). "internal" input + "hls" output is
    // the only combination start()/servePlay actually act on right now;
    // every other value is accepted, saved, and shown in the UI, but not
    // yet wired to real behavior — picking one doesn't change what a
    // stream does until a later phase implements it.
    var inputMode: String = "internal"     // "internal" | "ffmpegResident" | "ffmpegTsHls" | "ffmpegMultiTsHls" | "ffmpegFmp4Hls"
    var outputMode: String = "hls"         // "hls" | "srtServer" | "udpSrt" | "custom"
    var outputTarget: String = ""          // address/command for outputMode, e.g. srt://:9000 or a custom pipe command — meaning depends on outputMode

    /// Ordered CDN mirror URLs for the same source. `url` is tried first; if a
    /// manifest fetch fails the worker fails over to these in order (and sticks
    /// with whichever responds). Segments resolve against the active mirror, so
    /// a mirror only has to serve the same content under its own host/path.
    var cdnUrls: [String] = []

    init(id: String, name: String, kind: String, url: String, representation: String, period: String, proxy: String, playlistSegments: Int, keepSegments: Int, downloadAhead: Int, pollInterval: Double, forceOffline: Bool, status: String, lastError: String?, logo: String = "", decryptionKeys: String = "", hlsKey: String = "", hlsIV: String = "", representations: [String] = [], representationMeta: [String: RepresentationMetaEntry] = [:], manifestHeaders: String = "", mediaHeaders: String = "", hlsKeyHeaders: String = "", audioDelayMs: Int = 0) {
        self.id = id
        self.name = name
        self.kind = kind
        self.url = url
        self.representation = representation
        self.period = period
        self.proxy = proxy
        self.playlistSegments = playlistSegments
        self.keepSegments = keepSegments
        self.downloadAhead = downloadAhead
        self.pollInterval = pollInterval
        self.forceOffline = forceOffline
        self.status = status
        self.lastError = lastError
        self.logo = logo
        self.decryptionKeys = decryptionKeys
        self.hlsKey = hlsKey
        self.hlsIV = hlsIV
        self.representations = representations
        self.representationMeta = representationMeta
        self.manifestHeaders = manifestHeaders
        self.mediaHeaders = mediaHeaders
        self.hlsKeyHeaders = hlsKeyHeaders
        self.audioDelayMs = audioDelayMs
    }

    // Custom decoder: see PanelState.init(from:) — old data/state.json files
    // predate several of these fields. The old single `headers` field (if
    // present) migrates into `mediaHeaders`, its closest equivalent.
    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(String.self, forKey: .id)
        name = try container.decode(String.self, forKey: .name)
        kind = try container.decode(String.self, forKey: .kind)
        url = try container.decode(String.self, forKey: .url)
        representation = try container.decode(String.self, forKey: .representation)
        period = try container.decode(String.self, forKey: .period)
        proxy = try container.decode(String.self, forKey: .proxy)
        playlistSegments = try container.decode(Int.self, forKey: .playlistSegments)
        keepSegments = try container.decode(Int.self, forKey: .keepSegments)
        downloadAhead = try container.decode(Int.self, forKey: .downloadAhead)
        pollInterval = try container.decode(Double.self, forKey: .pollInterval)
        forceOffline = try container.decode(Bool.self, forKey: .forceOffline)
        status = try container.decode(String.self, forKey: .status)
        lastError = try container.decodeIfPresent(String.self, forKey: .lastError)
        logo = try container.decodeIfPresent(String.self, forKey: .logo) ?? ""
        decryptionKeys = try container.decodeIfPresent(String.self, forKey: .decryptionKeys) ?? ""
        hlsKey = try container.decodeIfPresent(String.self, forKey: .hlsKey) ?? ""
        hlsIV = try container.decodeIfPresent(String.self, forKey: .hlsIV) ?? ""
        representations = try container.decodeIfPresent([String].self, forKey: .representations) ?? []
        representationMeta = try container.decodeIfPresent([String: RepresentationMetaEntry].self, forKey: .representationMeta) ?? [:]
        let legacyContainer = try decoder.container(keyedBy: LegacyCodingKeys.self)
        let legacyHeaders = try legacyContainer.decodeIfPresent(String.self, forKey: .headers) ?? ""
        manifestHeaders = try container.decodeIfPresent(String.self, forKey: .manifestHeaders) ?? ""
        mediaHeaders = try container.decodeIfPresent(String.self, forKey: .mediaHeaders) ?? legacyHeaders
        hlsKeyHeaders = try container.decodeIfPresent(String.self, forKey: .hlsKeyHeaders) ?? ""
        audioDelayMs = try container.decodeIfPresent(Int.self, forKey: .audioDelayMs) ?? 0
        sourceType = try container.decodeIfPresent(String.self, forKey: .sourceType) ?? ""
        mode = try container.decodeIfPresent(String.self, forKey: .mode) ?? "live"
        sessionManifest = try container.decodeIfPresent(Bool.self, forKey: .sessionManifest) ?? false
        scriptParams = try container.decodeIfPresent(String.self, forKey: .scriptParams) ?? ""
        cdmType = try container.decodeIfPresent(String.self, forKey: .cdmType) ?? ""
        useCdm = try container.decodeIfPresent(Bool.self, forKey: .useCdm) ?? false
        scriptVideoSelector = try container.decodeIfPresent(String.self, forKey: .scriptVideoSelector) ?? ""
        scriptAudioSelector = try container.decodeIfPresent(String.self, forKey: .scriptAudioSelector) ?? ""
        onDemand = try container.decodeIfPresent(Bool.self, forKey: .onDemand) ?? false
        speedUp = try container.decodeIfPresent(Bool.self, forKey: .speedUp) ?? false
        autostart = try container.decodeIfPresent(Bool.self, forKey: .autostart) ?? false
        scriptStart = try container.decodeIfPresent(Int.self, forKey: .scriptStart)
        scriptEnd = try container.decodeIfPresent(Int.self, forKey: .scriptEnd)
        recordEvent = try container.decodeIfPresent(Bool.self, forKey: .recordEvent) ?? false
        inputMode = try container.decodeIfPresent(String.self, forKey: .inputMode) ?? "internal"
        outputMode = try container.decodeIfPresent(String.self, forKey: .outputMode) ?? "hls"
        outputTarget = try container.decodeIfPresent(String.self, forKey: .outputTarget) ?? ""
        cdnUrls = try container.decodeIfPresent([String].self, forKey: .cdnUrls) ?? []
    }

    private enum LegacyCodingKeys: String, CodingKey {
        case headers
    }
}

struct APIKey: Codable {
    var id: String
    var key: String
    var label: String
    var createdAt: Date
    var requests: Int = 0
    var bytes: Int64 = 0
    var lastSeenAt: Date?
}

struct BandwidthTotals: Codable {
    var allTimeBytes: Int64 = 0
    var perStreamAllTimeBytes: [String: Int64] = [:]
    var inputAllTimeBytes: Int64 = 0
    var perStreamInputAllTimeBytes: [String: Int64] = [:]

    init(allTimeBytes: Int64 = 0, perStreamAllTimeBytes: [String: Int64] = [:], inputAllTimeBytes: Int64 = 0, perStreamInputAllTimeBytes: [String: Int64] = [:]) {
        self.allTimeBytes = allTimeBytes
        self.perStreamAllTimeBytes = perStreamAllTimeBytes
        self.inputAllTimeBytes = inputAllTimeBytes
        self.perStreamInputAllTimeBytes = perStreamInputAllTimeBytes
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        allTimeBytes = try container.decodeIfPresent(Int64.self, forKey: .allTimeBytes) ?? 0
        perStreamAllTimeBytes = try container.decodeIfPresent([String: Int64].self, forKey: .perStreamAllTimeBytes) ?? [:]
        inputAllTimeBytes = try container.decodeIfPresent(Int64.self, forKey: .inputAllTimeBytes) ?? 0
        perStreamInputAllTimeBytes = try container.decodeIfPresent([String: Int64].self, forKey: .perStreamInputAllTimeBytes) ?? [:]
    }
}

struct SystemPeaks: Codable {
    var maxCPUPercent: Double = 0
    var maxMemoryBytes: UInt64 = 0
}

struct HTTPRequest {
    var method: String
    var path: String
    var query: [String: String]
    var headers: [String: String]
    var body: Data
    var remoteAddress: String = ""
}

enum PanelError: Error, CustomStringConvertible {
    case badRequest(String)
    case notFound(String)
    case server(String)
    case unauthorized(String)

    var description: String {
        switch self {
        case .badRequest(let message), .notFound(let message), .server(let message), .unauthorized(let message):
            return message
        }
    }
}

final class RestreamJob {
    let process: Process
    let representationId: String
    var stdout = Data()
    var stderr = Data()

    init(process: Process, representationId: String) {
        self.process = process
        self.representationId = representationId
    }
}

enum HeaderCategory {
    case manifest, media, hlsKey
}

/// A live, "no m3u8" direct link — the client opens `/direct/<streamId>/<repId>`
/// once and the connection stays open, with the init segment plus every media
/// segment appended to the body as they're produced (delimited by connection
/// close, not chunked-encoding framing — the simplest thing every player
/// that can already do a raw byte pipe already understands). This is a
/// concatenated fragmented-MP4 stream, not literal MPEG-TS; most players
/// that accept a raw stream URL (ffplay/mpv/VLC) handle that natively.
final class DirectStreamConnection {
    let connection: ClientConnection
    let streamId: String
    let repId: String
    let identity: String
    let clientIP: String
    let userAgent: String
    var sentInit = false
    var lastSentSegmentNumber: Int?

    init(connection: ClientConnection, streamId: String, repId: String, identity: String, clientIP: String, userAgent: String) {
        self.connection = connection
        self.streamId = streamId
        self.repId = repId
        self.identity = identity
        self.clientIP = clientIP
        self.userAgent = userAgent
    }
}

final class PanelServer {
    var port: UInt16
    private let portPinned: Bool
    var bindAddress: String
    private let bindAddressPinned: Bool
    private let adminUsernameOverride: String?
    private let adminPasswordOverride: String?
    let root: URL
    let publicDir: URL
    let runtimeDir: URL
    let stateFile: URL
    let selfBinary: URL
    #if canImport(Network)
    var listener: NWListener?
    #endif
    var jobs: [String: [RestreamJob]] = [:]
    private var lastRestartAttempt: [String: Date] = [:]
    private var restartFailureStreak: [String: Int] = [:]
    /// Per-stream index into [primary url] + cdnUrls, advanced on each ffmpeg
    /// (re)start so a resident ffmpeg cycles to the next CDN mirror when a
    /// source outage makes it exit and the supervisor restarts it.
    private var cdnRotation: [String: Int] = [:]
    let lock = NSLock()
    // Every readState()+mutate+writeState() cycle funnels through this serial
    // queue. Requests are handled concurrently (handle(_:) starts each
    // connection on the global queue) and the background metrics timer reads
    // and writes state.json too — without serializing the whole
    // read-modify-write cycle, two of those interleaving is a classic lost
    // update: whichever writeState() lands last silently wins and discards
    // the other's change (e.g. a stream added a moment earlier vanishing
    // when persistMetrics's already-in-flight read gets written back).
    private let stateQueue = DispatchQueue(label: "restreamair.state")
    let metrics: MetricsStore
    let logoLookup: LogoLookup
    let systemStats = SystemStats()
    let authStore = AuthStore()
    let logStore: LogStore
    var sseConnections: [ObjectIdentifier: ClientConnection] = [:]
    var directStreamConnections: [ObjectIdentifier: DirectStreamConnection] = [:]
    var sseTimer: DispatchSourceTimer?

    init(portOverride: UInt16? = nil, bindAddressOverride: String? = nil, adminUsernameOverride: String? = nil, adminPasswordOverride: String? = nil) {
        let envPort = ProcessInfo.processInfo.environment["PORT"].flatMap { UInt16($0) }
        self.port = portOverride ?? envPort ?? 8787
        self.portPinned = portOverride != nil || envPort != nil
        let envBind = ProcessInfo.processInfo.environment["BIND_ADDRESS"]
        self.bindAddress = bindAddressOverride ?? envBind ?? ""
        self.bindAddressPinned = bindAddressOverride != nil || envBind != nil
        self.adminUsernameOverride = adminUsernameOverride?.trimmingCharacters(in: .whitespaces).isEmpty == false ? adminUsernameOverride : nil
        self.adminPasswordOverride = adminPasswordOverride?.isEmpty == false ? adminPasswordOverride : nil
        self.root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        self.publicDir = root.appendingPathComponent("public", isDirectory: true)
        self.runtimeDir = root.appendingPathComponent("runtime", isDirectory: true)
        self.stateFile = root.appendingPathComponent("state.json")
        self.selfBinary = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
        self.metrics = MetricsStore()
        self.logoLookup = LogoLookup(cacheFile: root.appendingPathComponent("logo-cache.json"))
        self.logStore = LogStore(logsDir: root.appendingPathComponent("logs", isDirectory: true))
    }

    /// Older builds kept everything under `data/` (config, cache, logs) and
    /// segments under `public/runtime`. Config/cache/logs now live directly
    /// in the working directory and segments under a top-level `runtime/`,
    /// so an existing install's state isn't silently reset to empty on
    /// first run with the new layout — move anything found at the old
    /// locations over before the rest of startup reads/creates them.
    func migrateLegacyDataLayout() {
        let fm = FileManager.default
        let legacyDataDir = root.appendingPathComponent("data", isDirectory: true)
        let legacyRuntimeDir = publicDir.appendingPathComponent("runtime", isDirectory: true)
        let moves: [(URL, URL)] = [
            (legacyDataDir.appendingPathComponent("state.json"), stateFile),
            (legacyDataDir.appendingPathComponent("logo-cache.json"), logoLookup.cacheFile),
            (legacyDataDir.appendingPathComponent("logs", isDirectory: true), logStore.logsDir),
            (legacyRuntimeDir, runtimeDir)
        ]
        for (from, to) in moves {
            guard fm.fileExists(atPath: from.path), !fm.fileExists(atPath: to.path) else { continue }
            try? fm.moveItem(at: from, to: to)
        }
        if let remaining = try? fm.contentsOfDirectory(atPath: legacyDataDir.path), remaining.isEmpty {
            try? fm.removeItem(at: legacyDataDir)
        }
    }

    func start() throws {
        migrateLegacyDataLayout()
        try ensureData()
        if let username = adminUsernameOverride, let password = adminPasswordOverride {
            do {
                try applyAdminOverride(username: username, password: password)
            } catch {
                fputs("error: --admin-username/--admin-password: \(error)\n", stderr)
                exit(1)
            }
        }
        if let initial = try? readState() {
            metrics.seed(bandwidth: initial.bandwidthTotals)
            if !portPinned { port = initial.settings.port }
            if !bindAddressPinned { bindAddress = initial.settings.bindAddress }
        }
        let boundPort = port
        let boundAddress = bindAddress
        let portInUseMessage = "error: Port \(boundPort) is already in use — another instance of restreamair (or something else) is already listening there.\nStop it first (e.g. `lsof -ti:\(boundPort) | xargs kill`), or run with --port <n> to use a different port.\n"

        #if canImport(Network)
        let params = NWParameters.tcp
        if !bindAddress.isEmpty {
            // requiredLocalEndpoint already carries the port; passing a
            // separate `on:` port to NWListener as well conflicts with it
            // (EINVAL), so this variant of the initializer takes only params.
            params.requiredLocalEndpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(bindAddress), port: NWEndpoint.Port(rawValue: port)!)
            listener = try NWListener(using: params)
        } else {
            listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: port)!)
        }
        listener?.stateUpdateHandler = { state in
            switch state {
            case .ready:
                let host = boundAddress.isEmpty ? "127.0.0.1" : boundAddress
                print("ReStreamAir Swift panel running at http://\(host):\(boundPort)")
            case .failed(let error):
                if case .posix(let code) = error, code == .EADDRINUSE {
                    fputs(portInUseMessage, stderr)
                } else {
                    fputs("error: Listener failed: \(error)\n", stderr)
                }
                exit(1)
            default:
                break
            }
        }
        listener?.newConnectionHandler = { [weak self] connection in
            self?.handle(ClientConnection(connection))
        }
        listener?.start(queue: .global(qos: .userInitiated))
        #else
        do {
            try POSIXServer.serve(bindAddress: bindAddress, port: port) { [weak self] connection in
                self?.handle(connection)
            }
            let host = boundAddress.isEmpty ? "127.0.0.1" : boundAddress
            print("ReStreamAir Swift panel running at http://\(host):\(boundPort)")
        } catch let error as POSIXServer.BindError {
            if error.errnoValue == EADDRINUSE {
                fputs(portInUseMessage, stderr)
            } else {
                fputs("error: Could not bind port \(boundPort): \(String(cString: strerror(error.errnoValue)))\n", stderr)
            }
            exit(1)
        }
        #endif
        startMetricsTimer()
        DispatchQueue.global(qos: .utility).async { [weak self] in self?.checkForUpdate() }
        dispatchMain()
    }

    // MARK: - Server-Sent Events (live metrics)

    func startMetricsTimer() {
        let timer = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        timer.schedule(deadline: .now() + 1, repeating: 1)
        var tick = 0
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            tick += 1
            self.metrics.pruneExpired()
            self.broadcastMetrics()
            self.pumpAllDirectStreams()
            if tick % 2 == 0 { self.superviseWorkers() }
            if tick % 10 == 0 { self.persistMetrics() }
            // Re-check for a newer release every ~6 hours.
            if tick % 21600 == 0 { DispatchQueue.global(qos: .utility).async { self.checkForUpdate() } }
        }
        timer.resume()
        sseTimer = timer
    }

    func broadcastMetrics() {
        lock.lock()
        let connections = Array(sseConnections.values)
        lock.unlock()
        guard !connections.isEmpty else { return }
        guard let state = try? readState() else { return }
        let streamIds = state.providers.flatMap { $0.streams.map(\.id) }
        var payload = metrics.snapshot(streamIds: streamIds)
        payload["system"] = systemStats.snapshotView(peaks: state.systemPeaks)
        payload["connections"] = enrichConnections(payload["connections"] as? [[String: Any]] ?? [], state: state)
        guard let data = try? JSONSerialization.data(withJSONObject: DashSegmentsCLI.normalizeJSON(payload)),
              let json = String(data: data, encoding: .utf8) else { return }
        let frame = "data: \(json)\n\n".data(using: .utf8)!
        for connection in connections {
            connection.send(frame) { [weak self] error in
                if error != nil { self?.removeSSEConnection(connection) }
            }
        }
    }

    func persistMetrics() {
        stateQueue.sync {
            guard var state = try? readState() else { return }
            state.bandwidthTotals = metrics.snapshotBandwidthTotals()
            let cpu = systemStats.cpuPercent()
            let mem = systemStats.memoryUsedBytes()
            state.systemPeaks.maxCPUPercent = max(state.systemPeaks.maxCPUPercent, cpu)
            state.systemPeaks.maxMemoryBytes = max(state.systemPeaks.maxMemoryBytes, mem)
            try? writeState(state)
        }
    }

    private let updateInfoLock = NSLock()
    private var updateInfoCache: [String: Any] = [:]

    /// The build/version block shown in the UI (Server tab + an "update
    /// available" banner). Merges the compiled-in identity with the latest
    /// cached result of the GitHub release check.
    func versionView() -> [String: Any] {
        updateInfoLock.lock(); let cache = updateInfoCache; updateInfoLock.unlock()
        var view: [String: Any] = [
            "version": AppVersion.version,
            "commit": AppVersion.commit,
            "isRelease": AppVersion.isReleaseBuild,
        ]
        for (key, value) in cache { view[key] = value }
        return view
    }

    /// Best-effort: ask GitHub for the repo's latest release tag and compare it
    /// to this binary's version. The repo is private, so the API needs a token
    /// — read from GITHUB_TOKEN / GH_TOKEN in the environment. Without one the
    /// check degrades to a clear "needs a token" note rather than failing hard.
    func checkForUpdate() {
        let env = ProcessInfo.processInfo.environment
        let token = (env["GITHUB_TOKEN"] ?? env["GH_TOKEN"] ?? "").trimmingCharacters(in: .whitespaces)
        var headers: [(String, String)] = [("Accept", "application/vnd.github+json"), ("User-Agent", "ReStreamAir")]
        if !token.isEmpty { headers.append(("Authorization", "Bearer \(token)")) }
        var result: [String: Any] = ["checkedAt": ISO8601DateFormatter().string(from: Date())]
        defer { updateInfoLock.lock(); updateInfoCache = result; updateInfoLock.unlock() }

        guard let url = URL(string: "https://api.github.com/repos/\(AppVersion.repoOwner)/\(AppVersion.repoName)/releases/latest") else { return }
        do {
            let data = try HTTPClient(options: HTTPRequestOptions(timeout: 15, headers: headers)).get(url)
            guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let tag = object["tag_name"] as? String else {
                result["error"] = "no releases published yet"
                return
            }
            let latest = tag.hasPrefix("v") ? String(tag.dropFirst()) : tag
            result["latest"] = latest
            result["updateAvailable"] = AppVersion.isReleaseBuild && Self.isNewerVersion(latest, than: AppVersion.version)
        } catch {
            result["error"] = token.isEmpty
                ? "update check needs a GitHub token (private repo) — set GITHUB_TOKEN"
                : "\(error)"
        }
    }

    /// Numeric dotted-version comparison (1.10.0 > 1.9.0). Non-numeric suffixes
    /// are ignored.
    static func isNewerVersion(_ candidate: String, than current: String) -> Bool {
        func parts(_ s: String) -> [Int] {
            s.split(separator: ".").map { Int($0.prefix { $0.isNumber }) ?? 0 }
        }
        let a = parts(candidate), b = parts(current)
        for index in 0..<max(a.count, b.count) {
            let x = index < a.count ? a[index] : 0
            let y = index < b.count ? b[index] : 0
            if x != y { return x > y }
        }
        return false
    }

    func handleEvents(_ connection: ClientConnection, request: HTTPRequest) {
        let headerText = [
            "HTTP/1.1 200 OK",
            "Content-Type: text/event-stream; charset=utf-8",
            "Cache-Control: no-cache",
            "Connection: keep-alive",
            "Access-Control-Allow-Origin: *",
            "", ""
        ].joined(separator: "\r\n")
        let key = ObjectIdentifier(connection)
        connection.onClose = { [weak self] in self?.removeSSEConnection(connection) }
        connection.send(headerText.data(using: .utf8)) { [weak self] error in
            guard let self else { return }
            if error != nil {
                self.removeSSEConnection(connection)
                return
            }
            self.lock.lock()
            self.sseConnections[key] = connection
            self.lock.unlock()
        }
    }

    func removeSSEConnection(_ connection: ClientConnection) {
        lock.lock()
        sseConnections.removeValue(forKey: ObjectIdentifier(connection))
        lock.unlock()
        connection.cancel()
    }

    // MARK: - Direct stream (no m3u8, one persistent link)

    func handleDirectStream(_ connection: ClientConnection, request: HTTPRequest, identity: String, clientIP: String, userAgent: String) throws {
        guard let match = match(request.path, #"^/direct/([^/]+)(?:/([^/]+))?$"#) else {
            throw PanelError.notFound("Not found.")
        }
        let streamId = match[0]
        let repId = match.count > 1 ? match[1] : ""
        let state = try readState()
        guard providerAndStream(streamId, in: state) != nil else { throw PanelError.notFound("Stream not found.") }
        let dir = directRuntimeDir(streamId: streamId, repId: repId)
        guard FileManager.default.fileExists(atPath: dir.path) else {
            throw PanelError.notFound("Stream isn't running yet — start it first.")
        }

        let headerText = [
            "HTTP/1.1 200 OK",
            "Content-Type: video/mp4",
            "Cache-Control: no-store",
            "Access-Control-Allow-Origin: *",
            "", ""
        ].joined(separator: "\r\n")
        let key = ObjectIdentifier(connection)
        let directConnection = DirectStreamConnection(connection: connection, streamId: streamId, repId: repId, identity: identity, clientIP: clientIP, userAgent: userAgent)
        connection.onClose = { [weak self] in self?.removeDirectStreamConnection(connection) }
        connection.send(headerText.data(using: .utf8)) { [weak self] error in
            guard let self else { return }
            if error != nil {
                self.removeDirectStreamConnection(connection)
                return
            }
            self.lock.lock()
            self.directStreamConnections[key] = directConnection
            self.lock.unlock()
            self.pumpDirectStream(directConnection)
        }
    }

    /// One-shot MPEG-TS download: remuxes whatever's currently buffered on
    /// disk for a representation (init + every kept segment, the same
    /// window backing the live HLS playlist) into a single .ts file via
    /// `ffmpeg -c copy` (container-only, no re-encode) and serves it as a
    /// normal `Content-Disposition: attachment` download — a fixed-size
    /// file a browser's download manager can handle, unlike the open-ended
    /// live byte-tail at /direct/.
    /// One-shot download of whatever's currently buffered for a
    /// representation (init + every kept segment — the same window backing
    /// the live HLS playlist), served with `Content-Disposition:
    /// attachment` so clicking the link just downloads a file. No ffmpeg
    /// involved: the init segment plus its fragments is already a valid,
    /// playable fragmented MP4 (each fragment is a self-contained moof+mdat
    /// pair, decodable on its own once the init/moov has been seen — that's
    /// the same property that lets it be tailed live at /direct/), so this
    /// just concatenates the bytes we already have on disk and serves them
    /// as-is rather than remuxing to another container.
    func handleDownloadStream(_ request: HTTPRequest) throws -> Data {
        guard let match = match(request.path, #"^/download/([^/.]+)(?:/([^/.]+))?(?:\.mp4)?$"#) else {
            throw PanelError.notFound("Not found.")
        }
        let streamId = match[0]
        let repId = match.count > 1 ? match[1] : ""
        let state = try readState()
        guard let (_, stream) = providerAndStream(streamId, in: state) else { throw PanelError.notFound("Stream not found.") }
        let dir = directRuntimeDir(streamId: streamId, repId: repId)
        let (initURL, segments) = listRuntimeSegments(in: dir)
        guard let initURL, !segments.isEmpty else {
            throw PanelError.notFound("Stream isn't running yet — start it and let a few segments download first.")
        }
        var fmp4 = Data()
        if let initData = try? Data(contentsOf: initURL) { fmp4.append(initData) }
        for segment in segments {
            if let data = try? Data(contentsOf: segment.url) { fmp4.append(data) }
        }
        let filename = sanitizeFilename(repId.isEmpty ? stream.name : "\(stream.name)-\(repId)")
        return response(
            status: 200,
            body: fmp4,
            type: "video/mp4",
            noStore: true,
            extraHeaders: ["Content-Disposition": "attachment; filename=\"\(filename).mp4\""]
        )
    }

    func sanitizeFilename(_ text: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "._- "))
        let cleaned = text.unicodeScalars.map { allowed.contains($0) ? Character($0) : "_" }.map(String.init).joined()
        return cleaned.isEmpty ? "stream" : cleaned
    }

    /// Quotes can't appear inside an M3U attribute value (tvg-logo="...",
    /// group-title="...") without breaking the parse for every player —
    /// strip them rather than trying to escape, since provider/stream names
    /// are free text an M3U consumer never expected to need escaping for.
    private func m3uAttrSafe(_ text: String) -> String {
        text.replacingOccurrences(of: "\"", with: "").replacingOccurrences(of: "\n", with: " ")
    }

    func m3uPlaylist(providers: [Provider], host: String) -> String {
        var lines = ["#EXTM3U"]
        for provider in providers {
            for stream in provider.streams {
                let logo = stream.logo.isEmpty ? provider.logo : stream.logo
                var attrs = ""
                if !logo.isEmpty { attrs += " tvg-logo=\"\(m3uAttrSafe(logo))\"" }
                attrs += " group-title=\"\(m3uAttrSafe(provider.name))\""
                lines.append("#EXTINF:-1\(attrs),\(m3uAttrSafe(stream.name))")
                lines.append("http://\(host)/play/\(stream.id)/index.m3u8")
            }
        }
        return lines.joined(separator: "\n") + "\n"
    }

    func removeDirectStreamConnection(_ connection: ClientConnection) {
        lock.lock()
        directStreamConnections.removeValue(forKey: ObjectIdentifier(connection))
        lock.unlock()
        connection.cancel()
    }

    func directRuntimeDir(streamId: String, repId: String) -> URL {
        let base = runtimeDir.appendingPathComponent(streamId, isDirectory: true)
        return repId.isEmpty ? base : base.appendingPathComponent(repId, isDirectory: true)
    }

    /// Directory listing of one representation's downloaded segments, init
    /// segment located separately (its name doesn't carry a sequence
    /// number). Segment filenames come from LiveMPDToM3U8's
    /// localSegmentURL(for:) — "seg_<number>_<name>.<ext>" / "init_<name>.<ext>".
    func listRuntimeSegments(in dir: URL) -> (initURL: URL?, segments: [(number: Int, url: URL)]) {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { return (nil, []) }
        var initURL: URL?
        var segments: [(Int, URL)] = []
        for name in names {
            if name.hasPrefix("init_") {
                initURL = dir.appendingPathComponent(name)
                continue
            }
            guard name.hasPrefix("seg_") else { continue }
            let rest = name.dropFirst(4)
            guard let underscore = rest.firstIndex(of: "_"), let number = Int(rest[rest.startIndex..<underscore]) else { continue }
            segments.append((number, dir.appendingPathComponent(name)))
        }
        segments.sort { $0.0 < $1.0 }
        return (initURL, segments)
    }

    /// Sends whatever this connection hasn't seen yet: the init segment
    /// once, then every media segment newer than the last one sent, in
    /// order. Called right after connect (to catch up) and again on every
    /// metrics timer tick (~1s) to tail new segments as they're downloaded.
    func pumpDirectStream(_ directConnection: DirectStreamConnection) {
        let dir = directRuntimeDir(streamId: directConnection.streamId, repId: directConnection.repId)
        let (initURL, segments) = listRuntimeSegments(in: dir)
        var payload = Data()
        if !directConnection.sentInit, let initURL, let initData = try? Data(contentsOf: initURL) {
            payload.append(initData)
            directConnection.sentInit = true
        }
        let pending = segments.filter { seg in directConnection.lastSentSegmentNumber.map { seg.number > $0 } ?? true }
        for segment in pending {
            guard let data = try? Data(contentsOf: segment.url) else { continue }
            payload.append(data)
            directConnection.lastSentSegmentNumber = segment.number
        }
        guard !payload.isEmpty else { return }
        metrics.recordRequest(streamId: directConnection.streamId, identity: directConnection.identity, bytes: payload.count, clientIP: directConnection.clientIP, userAgent: directConnection.userAgent)
        directConnection.connection.send(payload) { [weak self] error in
            if error != nil { self?.removeDirectStreamConnection(directConnection.connection) }
        }
    }

    func pumpAllDirectStreams() {
        lock.lock()
        let connections = Array(directStreamConnections.values)
        lock.unlock()
        for directConnection in connections { pumpDirectStream(directConnection) }
    }

    func handle(_ connection: ClientConnection) {
        connection.start()
        let remoteAddress = connection.remoteAddress
        readRequest(connection) { [weak self] rawRequest in
            guard let self else { return }
            var request = rawRequest
            request.remoteAddress = remoteAddress
            if request.method == "GET", request.path == "/api/events" {
                self.handleEvents(connection, request: request)
                return
            }
            let userAgent = request.headers["user-agent"] ?? ""
            if request.method == "GET", request.path.hasPrefix("/direct/") {
                do {
                    let identity = try self.authorizePlayback(request)
                    try self.handleDirectStream(connection, request: request, identity: identity, clientIP: remoteAddress, userAgent: userAgent)
                } catch {
                    let response = self.jsonResponse(status: 401, ["error": "\(error)"])
                    connection.send(response) { _ in connection.cancel() }
                }
                return
            }
            do {
                var identity: String?
                if self.isPlaybackPath(request.path) {
                    identity = try self.authorizePlayback(request)
                }
                let response = try self.route(request)
                if let identity {
                    self.metrics.recordRequest(streamId: self.extractStreamId(request.path), identity: identity, bytes: response.count, clientIP: remoteAddress, userAgent: userAgent)
                }
                connection.send(response) { _ in connection.cancel() }
            } catch PanelError.notFound(let message) {
                self.recordPlaybackError(request, remoteAddress: remoteAddress)
                let response = self.jsonResponse(status: 404, ["error": message])
                connection.send(response) { _ in connection.cancel() }
            } catch PanelError.badRequest(let message) {
                self.recordPlaybackError(request, remoteAddress: remoteAddress)
                let response = self.jsonResponse(status: 400, ["error": message])
                connection.send(response) { _ in connection.cancel() }
            } catch PanelError.unauthorized(let message) {
                let response = self.jsonResponse(status: 401, ["error": message])
                connection.send(response) { _ in connection.cancel() }
            } catch {
                self.recordPlaybackError(request, remoteAddress: remoteAddress)
                let response = self.jsonResponse(status: 500, ["error": "\(error)"])
                connection.send(response) { _ in connection.cancel() }
            }
        }
    }

    func isPlaybackPath(_ path: String) -> Bool {
        path.hasPrefix("/play/") || path.hasPrefix("/proxy/") || path.hasPrefix("/restream/") || path.hasPrefix("/direct/") || path.hasPrefix("/download/") || path.hasPrefix("/source/")
    }

    func extractStreamId(_ path: String) -> String? {
        let parts = path.split(separator: "/", omittingEmptySubsequences: true)
        guard parts.count >= 2 else { return nil }
        return String(parts[1])
    }

    func bearerToken(_ header: String?) -> String? {
        guard let header, header.lowercased().hasPrefix("bearer ") else { return nil }
        return String(header.dropFirst(7)).trimmingCharacters(in: .whitespaces)
    }

    @discardableResult
    func authorizePlayback(_ request: HTTPRequest) throws -> String {
        let state = try readState()
        guard !state.apiKeys.isEmpty else { return "ip:\(request.remoteAddress)" }
        let supplied = request.query["key"] ?? bearerToken(request.headers["authorization"])
        guard let supplied, let matched = state.apiKeys.first(where: { $0.key == supplied }) else {
            throw PanelError.unauthorized("Valid API key required. Pass ?key=<key> or an Authorization: Bearer header.")
        }
        return "key:\(matched.id)"
    }

    /// Attributes a failed playback-path request to its connection's error
    /// count for the monitoring table. Re-checking auth here is cheap (an
    /// in-memory key lookup) and only runs on the error path.
    func recordPlaybackError(_ request: HTTPRequest, remoteAddress: String) {
        guard isPlaybackPath(request.path), let streamId = extractStreamId(request.path) else { return }
        guard let identity = try? authorizePlayback(request) else { return }
        metrics.recordConnectionError(streamId: streamId, identity: identity, clientIP: remoteAddress)
    }

    func authorizeAdmin(_ request: HTTPRequest, state: PanelState) throws {
        guard !state.adminUsers.isEmpty else { return }
        if let username = authStore.username(forSessionCookie: request.headers["cookie"]),
           state.adminUsers.contains(where: { $0.username == username }) {
            return
        }
        if let credentials = authStore.basicCredentials(request.headers["authorization"]),
           let user = state.adminUsers.first(where: { $0.username == credentials.username }),
           AuthStore.verifyPassword(credentials.password, hash: user.passwordHash, salt: user.salt) {
            return
        }
        throw PanelError.unauthorized("Sign in required.")
    }

    func readRequest(_ connection: ClientConnection, completion: @escaping (HTTPRequest) -> Void) {
        var buffer = Data()
        func receive() {
            connection.receive { data, complete, error in
                if let data { buffer.append(data) }
                if error != nil || complete { connection.cancel(); return }
                if let request = self.parseRequest(buffer) {
                    completion(request)
                    return
                }
                receive()
            }
        }
        receive()
    }

    func parseRequest(_ data: Data) -> HTTPRequest? {
        guard let marker = "\r\n\r\n".data(using: .utf8),
              let headerRange = data.range(of: marker),
              let headerText = String(data: data[..<headerRange.lowerBound], encoding: .utf8) else { return nil }
        let lines = headerText.split(separator: "\r\n").map(String.init)
        guard let requestLine = lines.first else { return nil }
        let parts = requestLine.split(separator: " ", maxSplits: 2).map(String.init)
        guard parts.count >= 2 else { return nil }
        var headers: [String: String] = [:]
        for line in lines.dropFirst() {
            guard let separator = line.firstIndex(of: ":") else { continue }
            headers[String(line[..<separator]).lowercased()] = String(line[line.index(after: separator)...]).trimmingCharacters(in: .whitespaces)
        }
        let contentLength = Int(headers["content-length"] ?? "0") ?? 0
        let bodyStart = headerRange.upperBound
        guard data.count >= bodyStart + contentLength else { return nil }
        let body = Data(data[bodyStart..<(bodyStart + contentLength)])
        let url = URL(string: parts[1], relativeTo: URL(string: "http://localhost"))!
        let query = URLComponents(url: url, resolvingAgainstBaseURL: true)?.queryItems?.reduce(into: [String: String]()) {
            $0[$1.name] = $1.value ?? ""
        } ?? [:]
        return HTTPRequest(method: parts[0], path: url.path, query: query, headers: headers, body: body)
    }

    func route(_ request: HTTPRequest) throws -> Data {
        if request.path.hasPrefix("/api/") { return try api(request) }
        if request.path.hasPrefix("/play/") { return try servePlay(request) }
        if request.path.hasPrefix("/proxy/") { return try serveProxy(request) }
        if request.path.hasPrefix("/restream/") { return try serveRestream(request) }
        if request.path.hasPrefix("/download/") { return try handleDownloadStream(request) }
        if request.path.hasPrefix("/source/") { return try serveSourceRedirect(request) }
        return try servePublic(request.path == "/" ? "/index.html" : request.path)
    }

    /// `/source/<id>` — a 302 redirect straight to the stream's origin source
    /// URL. This is the "direct link" for an m3u8 passthrough: instead of
    /// pulling every segment through the panel, hand the player the origin (or
    /// its current CDN mirror) and let it fetch there. Same API-key gate as the
    /// other playback routes.
    func serveSourceRedirect(_ request: HTTPRequest) throws -> Data {
        guard let id = match(request.path, #"^/source/([^/]+)$"#)?.first else { throw PanelError.notFound("Not found.") }
        let state = try readState()
        guard let (_, stream) = providerAndStream(id, in: state) else { throw PanelError.notFound("Stream not found.") }
        // Prefer the CDN mirror a resident ffmpeg last rotated to, else primary.
        let sources = [stream.url] + stream.cdnUrls
        lock.lock(); let rotation = cdnRotation[stream.id] ?? 0; lock.unlock()
        let target = sources.isEmpty ? stream.url : sources[max(0, rotation - 1) % sources.count]
        guard !target.isEmpty else { throw PanelError.badRequest("Stream has no source URL.") }
        return response(status: 302, body: Data(), type: "text/plain", noStore: true, extraHeaders: ["Location": target])
    }

    func api(_ request: HTTPRequest) throws -> Data {
        try stateQueue.sync { try apiLocked(request) }
    }

    private func apiLocked(_ request: HTTPRequest) throws -> Data {
        var state = try readState()
        if !request.path.hasPrefix("/api/auth/") {
            try authorizeAdmin(request, state: state)
        }
        if request.method == "GET", request.path == "/api/state" {
            return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
        }

        if request.method == "GET", request.path == "/api/logo-lookup" {
            let name = request.query["name"] ?? ""
            let logo = logoLookup.bestMatch(for: name)
            return jsonResponse(status: 200, ["logo": logo as Any])
        }

        // Read-only probe the stream editor's Input/Output pipeline section
        // polls once on open to decide which ffmpeg-dependent options to
        // show as available vs. needing an install first.
        if request.method == "GET", request.path == "/api/ffmpeg-status" {
            let ffmpegPath = FFmpegLocator.resolve("ffmpeg")
            let ffprobePath = FFmpegLocator.resolve("ffprobe")
            let plan = FFmpegInstaller.plan()
            return jsonResponse(status: 200, [
                "available": ffmpegPath != nil,
                "ffmpegPath": ffmpegPath as Any,
                "ffprobePath": ffprobePath as Any,
                "installCommand": plan?.displayCommand ?? FFmpegInstaller.manualCommand,
                "canAutoInstall": plan != nil
            ])
        }

        // Kicks off the platform's package-manager install in the
        // background and returns immediately, same fire-and-poll shape as
        // the script login/pair endpoints — output streams into logStore
        // under a synthetic "ffmpeg-install" id, polled the same way the
        // provider settings terminal box already polls "script:<id>".
        if request.method == "POST", request.path == "/api/ffmpeg-install" {
            guard let plan = FFmpegInstaller.plan() else {
                throw PanelError.badRequest("Can't install automatically here — run `\(FFmpegInstaller.manualCommand)` yourself, then check again.")
            }
            let logStreamId = "ffmpeg-install"
            let logStore = self.logStore
            logStore.record(streamId: logStreamId, level: "info", event: "installStart", url: nil, message: plan.displayCommand)
            let process = Process()
            process.executableURL = URL(fileURLWithPath: plan.executable)
            process.arguments = plan.arguments
            let stdoutPipe = Pipe()
            let stderrPipe = Pipe()
            process.standardOutput = stdoutPipe
            process.standardError = stderrPipe
            // Same leak PanelServer.start()/ScriptRunner already hit once: a
            // closed pipe reads as perpetually "readable", so the handler
            // must be cleared in terminationHandler or its fd-monitoring
            // thread spins forever.
            let forward: (FileHandle) -> Void = { handle in
                let data = handle.availableData
                guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
                for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
                    logStore.record(streamId: logStreamId, level: "info", event: "installOutput", url: nil, message: String(line), raw: String(line))
                }
            }
            stdoutPipe.fileHandleForReading.readabilityHandler = forward
            stderrPipe.fileHandleForReading.readabilityHandler = forward
            process.terminationHandler = { proc in
                stdoutPipe.fileHandleForReading.readabilityHandler = nil
                stderrPipe.fileHandleForReading.readabilityHandler = nil
                let status = proc.terminationStatus
                logStore.record(
                    streamId: logStreamId, level: status == 0 ? "info" : "error", event: "installExit", url: nil,
                    message: status == 0 ? "ffmpeg installed" : "exited with code \(status)"
                )
            }
            do {
                try process.run()
            } catch {
                logStore.record(streamId: logStreamId, level: "error", event: "installExit", url: nil, message: "\(error)")
                throw PanelError.server("Couldn't start install: \(error)")
            }
            return jsonResponse(status: 202, ["started": true, "logStreamId": logStreamId])
        }

        if request.method == "POST", request.path == "/api/providers" {
            let input = try decodeJSON([String: JSONValue].self, request.body)
            guard let name = input["name"]?.string?.trimmingCharacters(in: .whitespaces), !name.isEmpty else {
                throw PanelError.badRequest("Provider name is required.")
            }
            var logo = input["logo"]?.string ?? ""
            if logo.trimmingCharacters(in: .whitespaces).isEmpty {
                logo = logoLookup.bestMatch(for: name) ?? ""
            }
            state.providers.append(Provider(
                id: safeId("provider"), name: name, logo: logo, streams: [],
                proxy: input["proxy"]?.string ?? "", headers: input["headers"]?.string ?? "",
                segmentUrlParams: input["segmentUrlParams"]?.string ?? "", inheritUrlParams: input["inheritUrlParams"]?.bool ?? false
            ))
            try writeState(state)
            return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
        }

        if request.method == "PUT", let match = match(request.path, #"^/api/providers/([^/]+)$"#) {
            guard let providerIndex = state.providers.firstIndex(where: { $0.id == match[0] }) else {
                throw PanelError.notFound("Provider not found.")
            }
            let input = try decodeJSON([String: JSONValue].self, request.body)
            let raw = try? JSONSerialization.jsonObject(with: request.body) as? [String: Any]
            guard let name = input["name"]?.string?.trimmingCharacters(in: .whitespaces), !name.isEmpty else {
                throw PanelError.badRequest("Provider name is required.")
            }
            state.providers[providerIndex].name = name
            state.providers[providerIndex].logo = input["logo"]?.string ?? state.providers[providerIndex].logo
            state.providers[providerIndex].proxy = input["proxy"]?.string ?? ""
            state.providers[providerIndex].headers = input["headers"]?.string ?? ""
            state.providers[providerIndex].segmentUrlParams = input["segmentUrlParams"]?.string ?? ""
            state.providers[providerIndex].inheritUrlParams = input["inheritUrlParams"]?.bool ?? false
            state.providers[providerIndex].scriptPath = input["scriptPath"]?.string ?? ""
            state.providers[providerIndex].scriptBind = input["scriptBind"]?.string ?? ""
            state.providers[providerIndex].scriptDoh = input["scriptDoh"]?.string ?? ""
            state.providers[providerIndex].scriptWorker = input["scriptWorker"]?.string ?? ""
            state.providers[providerIndex].scriptAccounts = scriptAccountsFromBody(raw)
            let accountIds = Set(state.providers[providerIndex].scriptAccounts.map(\.id))
            let requestedActiveId = input["activeScriptAccountId"]?.string ?? ""
            state.providers[providerIndex].activeScriptAccountId = accountIds.contains(requestedActiveId) ? requestedActiveId : (state.providers[providerIndex].scriptAccounts.first?.id ?? "")
            let requestedMode = input["accountSelectionMode"]?.string ?? "fixed"
            state.providers[providerIndex].accountSelectionMode = ["fixed", "rotate", "random"].contains(requestedMode) ? requestedMode : "fixed"
            try writeState(state)
            return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
        }

        if request.method == "DELETE", let match = match(request.path, #"^/api/providers/([^/]+)$"#) {
            guard let providerIndex = state.providers.firstIndex(where: { $0.id == match[0] }) else {
                throw PanelError.notFound("Provider not found.")
            }
            for stream in state.providers[providerIndex].streams { stop(stream: stream) }
            state.providers.remove(at: providerIndex)
            try writeState(state)
            return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
        }

        // M3U playlists for external players (VLC, Kodi, TiviMate, ...) —
        // one #EXTINF/URL pair per stream, pointing at the same universal
        // /play/<id>/index.m3u8 URL the panel's own player and "copy direct
        // link" already use, so a stream that works in-panel works from the
        // playlist too, kind (mpd/m3u8) and running-or-not aside. group-title
        // is the de-facto standard M3U extension IPTV players use to group
        // channels by provider in their UI.
        if request.method == "GET", request.path == "/api/playlist.m3u8" {
            let body = m3uPlaylist(providers: state.providers, host: request.headers["host"] ?? "127.0.0.1:\(port)")
            return response(
                status: 200, body: Data(body.utf8), type: "audio/x-mpegurl", noStore: true,
                extraHeaders: ["Content-Disposition": "attachment; filename=\"restreamair-all.m3u8\""]
            )
        }

        if request.method == "GET", let match = match(request.path, #"^/api/providers/([^/]+)/playlist\.m3u8$"#) {
            guard let provider = state.providers.first(where: { $0.id == match[0] }) else {
                throw PanelError.notFound("Provider not found.")
            }
            let body = m3uPlaylist(providers: [provider], host: request.headers["host"] ?? "127.0.0.1:\(port)")
            return response(
                status: 200, body: Data(body.utf8), type: "audio/x-mpegurl", noStore: true,
                extraHeaders: ["Content-Disposition": "attachment; filename=\"\(sanitizeFilename(provider.name)).m3u8\""]
            )
        }

        if request.method == "GET", let match = match(request.path, #"^/api/providers/([^/]+)/export$"#) {
            guard let provider = state.providers.first(where: { $0.id == match[0] }) else {
                throw PanelError.notFound("Provider not found.")
            }
            var scriptFile: ExportedScriptFile?
            let normalizedScriptPath = ScriptRunner.normalizePath(provider.scriptPath)
            if !normalizedScriptPath.isEmpty, let fileData = FileManager.default.contents(atPath: normalizedScriptPath) {
                scriptFile = ExportedScriptFile(filename: (normalizedScriptPath as NSString).lastPathComponent, contentBase64: fileData.base64EncodedString())
            }
            let activeAccountName = provider.scriptAccounts.first(where: { $0.id == provider.activeScriptAccountId })?.name ?? ""
            let export = ProviderExport(
                provider: ExportedProvider(
                    name: provider.name, logo: provider.logo, proxy: provider.proxy, headers: provider.headers,
                    segmentUrlParams: provider.segmentUrlParams, inheritUrlParams: provider.inheritUrlParams,
                    scriptBind: provider.scriptBind, scriptDoh: provider.scriptDoh, scriptWorker: provider.scriptWorker,
                    scriptAccounts: provider.scriptAccounts, activeScriptAccountName: activeAccountName,
                    accountSelectionMode: provider.accountSelectionMode
                ),
                scriptFile: scriptFile,
                streams: provider.streams
            )
            let data = try JSONEncoder.pretty.encode(export)
            return response(
                status: 200, body: data, type: "application/json", noStore: true,
                extraHeaders: ["Content-Disposition": "attachment; filename=\"\(sanitizeFilename(provider.name)).restreamair-provider.json\""]
            )
        }

        // Reverses /export: regenerates every id (provider, streams, script
        // accounts — none of them mean anything on this install) and
        // re-resolves activeScriptAccountName back to an id via name match,
        // since the id it referenced on the exporting install doesn't
        // exist here. The script file (if the export carried one) gets
        // written under scripts/, named after whatever the original
        // filename was, suffixed if that name's already taken here rather
        // than silently overwriting an unrelated script of the same name.
        if request.method == "POST", request.path == "/api/providers/import" {
            let importData = try JSONDecoder().decode(ProviderExport.self, from: request.body)
            guard !importData.provider.name.trimmingCharacters(in: .whitespaces).isEmpty else {
                throw PanelError.badRequest("Import file has no provider name.")
            }

            var scriptPath = ""
            if let scriptFile = importData.scriptFile, let fileData = Data(base64Encoded: scriptFile.contentBase64) {
                let scriptsDir = root.appendingPathComponent("scripts", isDirectory: true)
                try FileManager.default.createDirectory(at: scriptsDir, withIntermediateDirectories: true)
                let ext = (scriptFile.filename as NSString).pathExtension
                let base = (scriptFile.filename as NSString).deletingPathExtension
                var destURL = scriptsDir.appendingPathComponent(scriptFile.filename)
                var suffix = 1
                while FileManager.default.fileExists(atPath: destURL.path) {
                    destURL = scriptsDir.appendingPathComponent(ext.isEmpty ? "\(base)_\(suffix)" : "\(base)_\(suffix).\(ext)")
                    suffix += 1
                }
                try fileData.write(to: destURL)
                try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: destURL.path)
                scriptPath = destURL.path
            }

            var newAccounts: [ScriptAccount] = []
            var activeId = ""
            for account in importData.provider.scriptAccounts {
                let newId = safeId("account")
                newAccounts.append(ScriptAccount(id: newId, name: account.name, username: account.username, password: account.password, params: account.params))
                if account.name == importData.provider.activeScriptAccountName { activeId = newId }
            }
            if activeId.isEmpty { activeId = newAccounts.first?.id ?? "" }

            let newStreams: [StreamConfig] = importData.streams.map { original in
                var stream = original
                stream.id = safeId("stream")
                stream.status = "stopped"
                stream.lastError = nil
                return stream
            }

            let newProvider = Provider(
                id: safeId("provider"), name: importData.provider.name, logo: importData.provider.logo, streams: newStreams,
                proxy: importData.provider.proxy, headers: importData.provider.headers, segmentUrlParams: importData.provider.segmentUrlParams,
                inheritUrlParams: importData.provider.inheritUrlParams, scriptPath: scriptPath, scriptBind: importData.provider.scriptBind,
                scriptDoh: importData.provider.scriptDoh, scriptWorker: importData.provider.scriptWorker,
                scriptAccounts: newAccounts, activeScriptAccountId: activeId,
                accountSelectionMode: importData.provider.accountSelectionMode
            )
            state.providers.append(newProvider)
            try writeState(state)
            return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
        }

        // Kicks off a login/pair script run in the background and returns
        // immediately — process.run() doesn't block on the child finishing,
        // only on the (near-instant) spawn itself, so this is safe to leave
        // inside the same stateQueue-guarded critical section as everything
        // else in this function. Output streams into logStore under a
        // synthetic "script:<providerId>" id (recordWorkerLine requires
        // JSON lines and would silently drop plain-text script output, so
        // this goes through logStore.record directly instead) — the panel
        // polls GET /api/logs?streamId=script:<providerId> to show it live,
        // same as the existing Logs tab does for real streams.
        if request.method == "POST", let match = match(request.path, #"^/api/providers/([^/]+)/script/(login|pair)$"#) {
            guard let providerIndex = state.providers.firstIndex(where: { $0.id == match[0] }) else {
                throw PanelError.notFound("Provider not found.")
            }
            let provider = state.providers[providerIndex]
            guard !provider.scriptPath.trimmingCharacters(in: .whitespaces).isEmpty else {
                throw PanelError.badRequest("This provider has no script configured.")
            }
            guard let account = try resolveScriptAccount(providerIndex: providerIndex, state: &state) else {
                throw PanelError.badRequest("Add or enable an account before running \(match[1]).")
            }
            let action = match[1]
            // Not every login is username+password (some scripts only need
            // a token already sitting in extra params, or nothing at all
            // beyond re-checking an existing session) — send whatever the
            // account has and let the script itself decide what's missing,
            // the same way it already would if run by hand.
            let logStreamId = "script:\(provider.id)"
            let args = ScriptRunner.commonArgs(action: action, bind: provider.scriptBind, proxy: provider.proxy, doh: provider.scriptDoh, worker: provider.scriptWorker, account: account)
            logStore.record(streamId: logStreamId, level: "info", event: "scriptStart", url: nil, message: "\(action) · \(provider.scriptPath) \(args.joined(separator: " "))")
            let logStore = self.logStore
            ScriptRunner.runStreaming(scriptPath: provider.scriptPath, args: args, onLine: { line in
                logStore.record(streamId: logStreamId, level: "info", event: "scriptOutput", url: nil, message: line, raw: line)
            }, onExit: { error in
                if let error {
                    logStore.record(streamId: logStreamId, level: "error", event: "scriptExit", url: nil, message: "\(error)")
                } else {
                    logStore.record(streamId: logStreamId, level: "info", event: "scriptExit", url: nil, message: "finished")
                }
            })
            return jsonResponse(status: 202, ["started": true, "logStreamId": logStreamId])
        }

        // Same fire-and-poll shape as login/pair, but the channels/events
        // scripts are expected to finish on their own (no human waiting on
        // a code) and return one JSON blob rather than a stream of
        // progress lines — so this uses runSync instead of runStreaming,
        // and critically runs it on a background queue rather than
        // inline: runSync blocks for up to its timeout, and blocking
        // inside this stateQueue-guarded handler would freeze the entire
        // panel (every other request, the metrics timer) for that whole
        // window. importScriptEntries re-acquires stateQueue itself once
        // the script is done and there's an actual (fast) write to make.
        if request.method == "POST", let match = match(request.path, #"^/api/providers/([^/]+)/script/(channels|events)$"#) {
            guard let providerIndex = state.providers.firstIndex(where: { $0.id == match[0] }) else {
                throw PanelError.notFound("Provider not found.")
            }
            let provider = state.providers[providerIndex]
            guard !provider.scriptPath.trimmingCharacters(in: .whitespaces).isEmpty else {
                throw PanelError.badRequest("This provider has no script configured.")
            }
            guard let account = try resolveScriptAccount(providerIndex: providerIndex, state: &state) else {
                throw PanelError.badRequest("Add or enable an account before running \(match[1]).")
            }
            let action = match[1]
            let providerId = provider.id
            let scriptPath = provider.scriptPath
            let logStreamId = "script:\(provider.id)"
            let args = ScriptRunner.commonArgs(action: action, bind: provider.scriptBind, proxy: provider.proxy, doh: provider.scriptDoh, worker: provider.scriptWorker, account: account)
            logStore.record(streamId: logStreamId, level: "info", event: "scriptStart", url: nil, message: "\(action) · \(scriptPath) \(args.joined(separator: " "))")
            let logStore = self.logStore
            DispatchQueue.global(qos: .utility).async { [weak self] in
                guard let self else { return }
                do {
                    let (stdout, _) = try ScriptRunner.runSync(scriptPath: scriptPath, args: args, timeout: 60)
                    let count = try self.importScriptEntries(stdout: stdout, action: action, providerId: providerId)
                    logStore.record(streamId: logStreamId, level: "info", event: "scriptExit", url: nil, message: "imported \(count) \(action)")
                } catch {
                    logStore.record(streamId: logStreamId, level: "error", event: "scriptExit", url: nil, message: "\(error)")
                }
            }
            return jsonResponse(status: 202, ["started": true, "logStreamId": logStreamId])
        }

        if request.method == "POST", request.path == "/api/probe" {
            let input = try decodeJSON([String: JSONValue].self, request.body)
            guard let urlString = input["url"]?.string, let url = URL(string: urlString), url.scheme?.hasPrefix("http") == true else {
                throw PanelError.badRequest("A valid http(s) url is required.")
            }
            let proxy = input["proxy"]?.string ?? ""
            let headers = parseHeaders(input["headers"]?.string ?? "")
            do {
                let result = try probeSource(url: url, proxy: proxy.isEmpty ? nil : proxy, headers: headers)
                return jsonResponse(status: 200, result)
            } catch {
                throw PanelError.badRequest("Could not probe \(url.absoluteString): \(error)")
            }
        }

        if request.method == "GET", request.path == "/api/logs" {
            let streamId = request.query["streamId"]
            let limit = request.query["limit"].flatMap { Int($0) } ?? 100
            let entries = request.query["date"].map { logStore.historicalEntries(date: $0, streamId: streamId, limit: limit) }
                ?? logStore.recent(streamId: streamId, limit: limit)
            return jsonResponse(status: 200, ["entries": entries, "availableDates": logStore.availableRunDates()])
        }

        if request.method == "GET", request.path == "/api/settings" {
            return jsonResponse(status: 200, ["port": state.settings.port, "bindAddress": state.settings.bindAddress])
        }

        if request.method == "POST", request.path == "/api/settings" {
            let input = try decodeJSON([String: JSONValue].self, request.body)
            if let newPort = input["port"]?.int, newPort > 0, newPort <= 65535 {
                state.settings.port = UInt16(newPort)
            }
            if let newBind = input["bindAddress"]?.string {
                state.settings.bindAddress = newBind.trimmingCharacters(in: .whitespaces)
            }
            try writeState(state)
            return jsonResponse(status: 200, ["port": state.settings.port, "bindAddress": state.settings.bindAddress, "note": "Takes effect after restart."])
        }

        if request.method == "GET", request.path == "/api/auth/status" {
            let username = authStore.username(forSessionCookie: request.headers["cookie"])
            return jsonResponse(status: 200, ["needsSetup": state.adminUsers.isEmpty, "authenticated": username != nil, "username": username as Any])
        }

        if request.method == "POST", request.path == "/api/auth/setup" {
            guard state.adminUsers.isEmpty else { throw PanelError.badRequest("An admin account already exists.") }
            let input = try decodeJSON([String: String].self, request.body)
            guard let username = input["username"]?.trimmingCharacters(in: .whitespaces), !username.isEmpty,
                  let password = input["password"], password.count >= 8 else {
                throw PanelError.badRequest("Username and an 8+ character password are required.")
            }
            let (hash, salt) = try AuthStore.hashPassword(password)
            state.adminUsers.append(AdminUser(id: safeId("user"), username: username, passwordHash: hash, salt: salt, createdAt: Date()))
            try writeState(state)
            let remember = input["remember"] == "true"
            let token = authStore.createSession(username: username, remember: remember)
            return response(status: 200, body: Data("{\"ok\":true}".utf8), type: "application/json; charset=utf-8", noStore: true, extraHeaders: ["Set-Cookie": authStore.setCookieHeader(token: token, remember: remember)])
        }

        if request.method == "POST", request.path == "/api/auth/login" {
            let input = try decodeJSON([String: String].self, request.body)
            guard let username = input["username"], let password = input["password"],
                  let user = state.adminUsers.first(where: { $0.username == username }),
                  AuthStore.verifyPassword(password, hash: user.passwordHash, salt: user.salt) else {
                throw PanelError.unauthorized("Invalid username or password.")
            }
            let remember = input["remember"] == "true"
            let token = authStore.createSession(username: username, remember: remember)
            return response(status: 200, body: Data("{\"ok\":true}".utf8), type: "application/json; charset=utf-8", noStore: true, extraHeaders: ["Set-Cookie": authStore.setCookieHeader(token: token, remember: remember)])
        }

        if request.method == "POST", request.path == "/api/auth/logout" {
            return response(status: 200, body: Data("{\"ok\":true}".utf8), type: "application/json; charset=utf-8", noStore: true, extraHeaders: ["Set-Cookie": authStore.clearCookieHeader()])
        }

        if request.method == "GET", request.path == "/api/users" {
            return jsonResponse(status: 200, ["users": state.adminUsers.map { ["id": $0.id, "username": $0.username, "createdAt": ISO8601DateFormatter.sharedTimestamp.string(from: $0.createdAt)] }])
        }

        if request.method == "POST", request.path == "/api/users" {
            let input = try decodeJSON([String: String].self, request.body)
            guard let username = input["username"]?.trimmingCharacters(in: .whitespaces), !username.isEmpty,
                  let password = input["password"], password.count >= 8 else {
                throw PanelError.badRequest("Username and an 8+ character password are required.")
            }
            guard !state.adminUsers.contains(where: { $0.username == username }) else {
                throw PanelError.badRequest("That username is already taken.")
            }
            let (hash, salt) = try AuthStore.hashPassword(password)
            state.adminUsers.append(AdminUser(id: safeId("user"), username: username, passwordHash: hash, salt: salt, createdAt: Date()))
            try writeState(state)
            return jsonResponse(status: 200, ["users": state.adminUsers.map { ["id": $0.id, "username": $0.username, "createdAt": ISO8601DateFormatter.sharedTimestamp.string(from: $0.createdAt)] }])
        }

        if request.method == "DELETE", let match = match(request.path, #"^/api/users/([^/]+)$"#) {
            guard state.adminUsers.count > 1 else { throw PanelError.badRequest("Cannot remove the last admin account.") }
            state.adminUsers.removeAll { $0.id == match[0] }
            try writeState(state)
            return jsonResponse(status: 200, ["users": state.adminUsers.map { ["id": $0.id, "username": $0.username, "createdAt": ISO8601DateFormatter.sharedTimestamp.string(from: $0.createdAt)] }])
        }

        if request.method == "GET", request.path == "/api/keys" {
            return jsonResponse(status: 200, ["keys": state.apiKeys.map(keyView)])
        }

        if request.method == "POST", request.path == "/api/keys" {
            let input = try decodeJSON([String: String].self, request.body)
            let label = input["label"]?.trimmingCharacters(in: .whitespaces).isEmpty == false ? input["label"]! : "Key \(state.apiKeys.count + 1)"
            let created = APIKey(id: safeId("key"), key: generateKey(), label: label, createdAt: Date())
            state.apiKeys.append(created)
            try writeState(state)
            return jsonResponse(status: 200, ["keys": state.apiKeys.map(keyView)])
        }

        if request.method == "DELETE", let match = match(request.path, #"^/api/keys/([^/]+)$"#) {
            state.apiKeys.removeAll { $0.id == match[0] }
            try writeState(state)
            return jsonResponse(status: 200, ["keys": state.apiKeys.map(keyView)])
        }

        if request.method == "POST", let match = match(request.path, #"^/api/providers/([^/]+)/streams$"#) {
            guard let providerIndex = state.providers.firstIndex(where: { $0.id == match[0] }) else {
                throw PanelError.notFound("Provider not found.")
            }
            let stream = try streamFromBody(request.body, existingId: safeId("stream"))
            state.providers[providerIndex].streams.append(stream)
            try writeState(state)
            return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
        }

        if let match = match(request.path, #"^/api/streams/([^/]+)$"#) {
            guard let location = findStream(match[0], in: state) else { throw PanelError.notFound("Stream not found.") }
            if request.method == "PUT" {
                let existing = state.providers[location.provider].streams[location.stream]
                var updated = try streamFromBody(request.body, existingId: match[0])
                // The manual editor form has no inputs for the channel/event-import
                // fields, so a plain replace would silently zero them out every time
                // an imported stream is opened and saved manually. Carry them over.
                updated.sourceType = existing.sourceType
                updated.mode = existing.mode
                updated.sessionManifest = existing.sessionManifest
                updated.scriptParams = existing.scriptParams
                updated.cdmType = existing.cdmType
                updated.useCdm = existing.useCdm
                updated.scriptVideoSelector = existing.scriptVideoSelector
                updated.scriptAudioSelector = existing.scriptAudioSelector
                updated.onDemand = existing.onDemand
                updated.speedUp = existing.speedUp
                updated.autostart = existing.autostart
                updated.scriptStart = existing.scriptStart
                updated.scriptEnd = existing.scriptEnd
                updated.recordEvent = existing.recordEvent
                state.providers[location.provider].streams[location.stream] = updated
                try writeState(state)
                return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
            }
            if request.method == "DELETE" {
                stop(stream: state.providers[location.provider].streams[location.stream])
                state.providers[location.provider].streams.remove(at: location.stream)
                try writeState(state)
                return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
            }
        }

        if request.method == "POST", let match = match(request.path, #"^/api/streams/([^/]+)/(start|stop)$"#) {
            guard let location = findStream(match[0], in: state) else { throw PanelError.notFound("Stream not found.") }
            if match[1] == "start" {
                try start(provider: state.providers[location.provider], stream: &state.providers[location.provider].streams[location.stream])
            } else {
                stop(stream: state.providers[location.provider].streams[location.stream])
                state.providers[location.provider].streams[location.stream].status = "stopped"
            }
            try writeState(state)
            return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
        }

        throw PanelError.notFound("Not found.")
    }

    func effectiveRepresentationIds(for stream: StreamConfig) -> [String] {
        if !stream.representations.isEmpty { return stream.representations }
        return [stream.representation]
    }

    func start(provider: Provider, stream: inout StreamConfig) throws {
        // A plain HLS passthrough (internal input, no re-mux) is served straight
        // from its origin by the m3u8 proxy — no worker to spawn. An ffmpeg
        // input mode, though, re-mixes even an m3u8 source, so it does spawn.
        if stream.kind == "m3u8" && stream.inputMode == "internal" {
            stream.status = "running"
            stream.lastError = nil
            return
        }
        lock.lock(); let alreadyRunning = !(jobs[stream.id] ?? []).isEmpty; lock.unlock()
        if alreadyRunning { stream.status = "running"; return }

        if stream.inputMode != "internal" {
            try startFfmpegResident(provider: provider, stream: &stream)
            return
        }

        guard FileManager.default.isExecutableFile(atPath: selfBinary.path) else {
            throw PanelError.server("Could not locate the restreamair binary at \(selfBinary.path) to spawn the live worker.")
        }

        let representationIds = effectiveRepresentationIds(for: stream)
        let multi = representationIds.count > 1
        let proxy = effectiveProxy(provider: provider, stream: stream)
        let manifestHeaders = swiftHeader(headerText(effectiveHeaders(provider: provider, stream: stream, category: .manifest)))
        let mediaHeaders = swiftHeader(headerText(effectiveHeaders(provider: provider, stream: stream, category: .media)))
        let keys = joinLines(stream.decryptionKeys)
        let segmentParams = effectiveSegmentUrlParams(provider: provider, stream: stream).trimmingCharacters(in: .whitespaces)
        logStore.clearStream(stream.id)

        var spawned: [RestreamJob] = []
        for repId in representationIds {
            let tempDir = multi
                ? runtimeDir.appendingPathComponent(stream.id, isDirectory: true).appendingPathComponent(repId.isEmpty ? "default" : repId, isDirectory: true)
                : runtimeDir.appendingPathComponent(stream.id, isDirectory: true)
            // Segments from a previous run of this stream (same tempDir path)
            // would otherwise sit on disk forever — the worker's in-memory
            // prune queue only ever knows about files it downloaded itself,
            // so stale files from an earlier process never get cleaned up.
            try? FileManager.default.removeItem(at: tempDir)
            try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
            let output = tempDir.appendingPathComponent("live.m3u8")
            var args = [
                "live",
                "action=createLiveM3U8",
                "mpd=\(stream.url)",
                "playlistSegments=\(stream.playlistSegments)",
                "keepSegments=\(stream.keepSegments)",
                "downloadAhead=\(stream.downloadAhead)",
                "pollInterval=\(stream.pollInterval)",
                "tempDir=\(tempDir.path)",
                "output=\(output.path)"
            ]
            if !repId.isEmpty { args.append("representation=\(repId)") }
            if !stream.period.isEmpty { args.append("period=\(stream.period)") }
            if !proxy.isEmpty { args.append("proxy=\(proxy)") }
            if stream.forceOffline { args.append("forceOffline=true") }
            if !manifestHeaders.isEmpty { args.append("manifestHeader=\(manifestHeaders)") }
            if !mediaHeaders.isEmpty { args.append("header=\(mediaHeaders)") }
            if !keys.isEmpty { args.append("decryptionKeys=\(keys)") }
            if !segmentParams.isEmpty { args.append("segmentUrlParams=\(segmentParams)") }
            // CDN mirrors: newline-joined (safe — a valid URL has no literal
            // newline), tried in order when the primary manifest fetch fails.
            if !stream.cdnUrls.isEmpty { args.append("fallbackUrls=\(stream.cdnUrls.joined(separator: "\n"))") }
            // Lip-sync offset only makes sense applied to the audio track —
            // apply it to whichever worker owns the representation flagged
            // as audio (or to every worker when there's no explicit
            // per-representation typing, e.g. a single-quality stream).
            let repIsAudio = stream.representationMeta[repId]?.type == "audio"
            let noTypedReps = stream.representationMeta.isEmpty
            if stream.audioDelayMs != 0, repIsAudio || noTypedReps {
                args.append("audioDelayMs=\(stream.audioDelayMs)")
            }

            let process = Process()
            process.executableURL = selfBinary
            process.arguments = args
            let stdoutPipe = Pipe()
            let stderrPipe = Pipe()
            process.standardOutput = stdoutPipe
            process.standardError = stderrPipe
            let job = RestreamJob(process: process, representationId: repId)
            let streamId = stream.id
            // A closed/EOF'd pipe is reported as perpetually "readable" by
            // the OS (it always has 0 bytes ready, immediately, forever) —
            // readabilityHandler doesn't detach itself on that, so unless we
            // explicitly nil it out once the child exits, the underlying
            // GCD/NSFileHandle fd-monitoring thread spins in a tight loop
            // reading empty data forever. With streams started/stopped
            // repeatedly over a long-running server, each leaked pair of
            // handlers (stdout+stderr) permanently pins another CPU core —
            // this is what actually produced multi-hundred-percent CPU usage
            // after a while, not any one single request.
            stdoutPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
                let data = handle.availableData
                guard !data.isEmpty else { return }
                job.stdout.append(data)
                if let text = String(data: data, encoding: .utf8) {
                    for line in text.split(separator: "\n") {
                        let lineString = String(line)
                        self?.logStore.recordWorkerLine(lineString, streamId: streamId)
                        if let lineData = lineString.data(using: .utf8),
                           let object = (try? JSONSerialization.jsonObject(with: lineData)) as? [String: Any],
                           object["event"] as? String == "downloadSegment",
                           (object["status"] as? String) != "error",
                           let bytes = object["bytes"] as? Int {
                            self?.metrics.recordInput(streamId: streamId, bytes: bytes)
                        }
                    }
                }
            }
            stderrPipe.fileHandleForReading.readabilityHandler = { handle in job.stderr.append(handle.availableData) }
            process.terminationHandler = { [weak self] _ in
                stdoutPipe.fileHandleForReading.readabilityHandler = nil
                stderrPipe.fileHandleForReading.readabilityHandler = nil
                try? stdoutPipe.fileHandleForReading.close()
                try? stderrPipe.fileHandleForReading.close()
                self?.lock.lock()
                self?.jobs[streamId]?.removeAll { $0.process === job.process }
                self?.lock.unlock()
            }
            try process.run()
            spawned.append(job)
        }

        lock.lock(); jobs[stream.id] = spawned; lock.unlock()
        stream.status = "running"
        stream.lastError = nil
    }

    /// Spawns one resident `ffmpeg` for the stream (as opposed to the internal
    /// per-representation Swift workers). ffmpeg reads the source directly,
    /// decrypts CENC clearkey itself, and remuxes with `-c copy` to HLS (served
    /// from the same tempDir the internal path uses) or to an SRT/UDP/TCP
    /// target. Wrapped in the same RestreamJob machinery so stop(), the
    /// supervisor restart loop, and per-stream logging all work unchanged.
    func startFfmpegResident(provider: Provider, stream: inout StreamConfig) throws {
        guard let ffmpegPath = FFmpegLocator.resolve("ffmpeg") else {
            throw PanelError.server("ffmpeg isn't installed — required for the '\(stream.inputMode)' input mode. Install it (e.g. brew install ffmpeg) or switch the stream back to the Internal remuxer.")
        }
        let tempDir = runtimeDir.appendingPathComponent(stream.id, isDirectory: true)
        try? FileManager.default.removeItem(at: tempDir)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        let output = tempDir.appendingPathComponent("live.m3u8")

        let repIds = effectiveRepresentationIds(for: stream).filter { !$0.isEmpty }
        // Multi-variant HLS needs a subdirectory per variant to exist up front.
        if stream.inputMode == "ffmpegMultiTsHls" {
            for index in 0..<max(1, repIds.filter { stream.representationMeta[$0]?.type != "audio" }.count) {
                try? FileManager.default.createDirectory(at: tempDir.appendingPathComponent("\(index)"), withIntermediateDirectories: true)
            }
        }

        logStore.clearStream(stream.id)
        // Pick the source for this attempt, rotating through the CDN mirrors on
        // each (re)start so a mirror outage recovers on the supervisor restart.
        let sources = [stream.url] + stream.cdnUrls
        lock.lock(); let rotation = cdnRotation[stream.id, default: 0]; cdnRotation[stream.id] = rotation + 1; lock.unlock()
        let sourceURL = sources[rotation % sources.count]
        if sources.count > 1 {
            logStore.recordWorkerLine(#"{"event":"cdnFailover","status":"info","message":\#(jsonString("source: " + sourceURL))}"#, streamId: stream.id)
        }
        let inputs = FFmpegResident.Inputs(
            ffmpegPath: ffmpegPath,
            sourceURL: sourceURL,
            kind: stream.kind,
            inputMode: stream.inputMode,
            outputMode: stream.outputMode,
            outputTarget: stream.outputTarget,
            decryptionKeys: stream.decryptionKeys,
            headers: headerText(effectiveHeaders(provider: provider, stream: stream, category: .media)),
            proxy: effectiveProxy(provider: provider, stream: stream),
            segmentUrlParams: effectiveSegmentUrlParams(provider: provider, stream: stream).trimmingCharacters(in: .whitespaces),
            representationIds: repIds,
            representationTypes: stream.representationMeta.mapValues { $0.type },
            tempDir: tempDir,
            outputPlaylist: output,
            playlistSegments: stream.playlistSegments,
            segmentSeconds: max(1, Int(stream.pollInterval.rounded()))
        )
        let command = FFmpegResident.command(inputs)

        let process = Process()
        process.executableURL = URL(fileURLWithPath: ffmpegPath)
        process.arguments = command.arguments
        if !command.environment.isEmpty {
            var env = ProcessInfo.processInfo.environment
            for (key, value) in command.environment { env[key] = value }
            process.environment = env
        }
        // Record the exact command so the panel log shows what ran.
        logStore.recordWorkerLine(#"{"event":"ffmpegStart","status":"start","message":\#(jsonString("ffmpeg " + command.arguments.joined(separator: " ")))}"#, streamId: stream.id)

        let stdoutPipe = Pipe()
        let stderrPipe = Pipe()
        process.standardOutput = stdoutPipe
        process.standardError = stderrPipe
        let job = RestreamJob(process: process, representationId: "")
        let streamId = stream.id
        // ffmpeg logs to stderr; forward each line into the per-stream log so
        // the panel shows progress/errors just like the internal worker's JSON.
        let logLines: (FileHandle) -> Void = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            job.stderr.append(data)
            if let text = String(data: data, encoding: .utf8) {
                for line in text.split(whereSeparator: { $0 == "\n" || $0 == "\r" }) {
                    let trimmed = line.trimmingCharacters(in: .whitespaces)
                    guard !trimmed.isEmpty else { continue }
                    self?.logStore.recordWorkerLine(#"{"event":"ffmpeg","status":"info","message":\#(self?.jsonString(trimmed) ?? "\"\"")}"#, streamId: streamId)
                }
            }
        }
        stderrPipe.fileHandleForReading.readabilityHandler = logLines
        stdoutPipe.fileHandleForReading.readabilityHandler = { handle in _ = handle.availableData }
        process.terminationHandler = { [weak self] proc in
            stdoutPipe.fileHandleForReading.readabilityHandler = nil
            stderrPipe.fileHandleForReading.readabilityHandler = nil
            try? stdoutPipe.fileHandleForReading.close()
            try? stderrPipe.fileHandleForReading.close()
            self?.logStore.recordWorkerLine(#"{"event":"ffmpegExit","status":"\#(proc.terminationStatus == 0 ? "success" : "error")","message":"exit \#(proc.terminationStatus)"}"#, streamId: streamId)
            self?.lock.lock()
            self?.jobs[streamId]?.removeAll { $0.process === job.process }
            self?.lock.unlock()
        }
        try process.run()

        lock.lock(); jobs[stream.id] = [job]; lock.unlock()
        stream.status = "running"
        stream.lastError = nil
    }

    func stop(stream: StreamConfig) {
        lock.lock()
        let jobsForStream = jobs.removeValue(forKey: stream.id) ?? []
        lock.unlock()
        for job in jobsForStream { job.process.terminate() }
    }

    /// Live workers already retry through routine network hiccups on their
    /// own (see fetchManifestWithRetries/downloadWithRetries in
    /// LiveMPDToM3U8.swift), but a persistent origin outage or an unexpected
    /// crash still exits the worker process — and until now nothing noticed:
    /// process.terminationHandler only removed the dead job from `jobs`, so
    /// a stream the user marked "running" would silently stop producing new
    /// segments forever, with every consumer (any player, any protocol)
    /// eventually running dry once the retention window passed. Called every
    /// couple of seconds off the existing metrics timer: for any stream
    /// still marked "running" with fewer live jobs than it should have (one
    /// per representation), do a clean stop+restart. A per-stream cooldown
    /// keeps a persistently-broken source from being hammered in a tight
    /// restart loop.
    func superviseWorkers() {
        stateQueue.sync {
            guard let state = try? readState() else { return }
            var mutableState = state
            var changed = false
            let now = Date()
            for provider in state.providers {
                for streamSnapshot in provider.streams {
                    guard streamSnapshot.status == "running" else { continue }
                    // A plain internal HLS passthrough has no worker to
                    // supervise; an ffmpeg-mode m3u8 does.
                    if streamSnapshot.kind == "m3u8" && streamSnapshot.inputMode == "internal" { continue }
                    // ffmpeg modes run exactly one resident process regardless
                    // of how many representations are selected; the internal
                    // path runs one worker per representation.
                    let expectedCount = streamSnapshot.inputMode == "internal"
                        ? effectiveRepresentationIds(for: streamSnapshot).count
                        : 1
                    lock.lock()
                    let activeCount = jobs[streamSnapshot.id]?.count ?? 0
                    lock.unlock()
                    guard activeCount < expectedCount else {
                        lock.lock()
                        restartFailureStreak[streamSnapshot.id] = 0
                        lock.unlock()
                        continue
                    }

                    lock.lock()
                    let streak = restartFailureStreak[streamSnapshot.id] ?? 0
                    // Exponential backoff (5s, 10s, 20s, 40s, capped at 60s)
                    // — a source that's persistently down used to get
                    // hammered with a stop+restart on a flat 5s cooldown
                    // forever (seen in practice: 10+ restarts inside one
                    // minute), which is its own source of choppiness on top
                    // of whatever network issue triggered it in the first
                    // place. Back off the longer it stays broken; reset to 0
                    // above as soon as it's healthy again.
                    let cooldown = min(60.0, 5.0 * pow(2.0, Double(streak)))
                    let last = lastRestartAttempt[streamSnapshot.id]
                    let cooldownOK = last.map { now.timeIntervalSince($0) >= cooldown } ?? true
                    if cooldownOK {
                        lastRestartAttempt[streamSnapshot.id] = now
                        restartFailureStreak[streamSnapshot.id] = streak + 1
                    }
                    lock.unlock()
                    guard cooldownOK else { continue }

                    guard let location = findStream(streamSnapshot.id, in: mutableState) else { continue }
                    stop(stream: mutableState.providers[location.provider].streams[location.stream])
                    logStore.recordWorkerLine(#"{"event":"supervisorRestart","status":"start"}"#, streamId: streamSnapshot.id)
                    do {
                        try start(provider: mutableState.providers[location.provider], stream: &mutableState.providers[location.provider].streams[location.stream])
                    } catch {
                        mutableState.providers[location.provider].streams[location.stream].status = "stopped"
                        mutableState.providers[location.provider].streams[location.stream].lastError = "Auto-restart failed: \(error)"
                        logStore.recordWorkerLine(#"{"event":"supervisorRestart","status":"error","message":"\#(error)"}"#, streamId: streamSnapshot.id)
                    }
                    changed = true
                }
            }
            if changed { try? writeState(mutableState) }
        }
    }

    func servePlay(_ request: HTTPRequest) throws -> Data {
        if request.path.hasSuffix("/index.mpd"), let id = match(request.path, #"^/play/([^/]+)/index\.mpd$"#)?.first {
            let state = try readState()
            guard let (_, stream) = providerAndStream(id, in: state) else { throw PanelError.notFound("Stream not found.") }
            guard stream.kind != "m3u8" else { throw PanelError.badRequest("index.mpd is only available for kind=mpd streams.") }
            return serveLiveMPD(stream: stream)
        }
        if let runtimeMatch = match(request.path, #"^/play/([^/]+)/(.+)$"#), runtimeMatch[1] != "index.m3u8" {
            return try serveRuntime(streamId: runtimeMatch[0], file: String(runtimeMatch[1].removingPercentEncoding ?? runtimeMatch[1]))
        }
        guard let id = match(request.path, #"^/play/([^/]+)/index\.m3u8$"#)?.first else { throw PanelError.notFound("Not found.") }
        let state = try readState()
        guard let (provider, stream) = providerAndStream(id, in: state) else { throw PanelError.notFound("Stream not found.") }
        // ffmpeg input modes write their own HLS into the runtime dir — serve it
        // directly (multi-variant writes a master.m3u8, everything else a single
        // combined live.m3u8). This precedes the m3u8-passthrough and internal
        // master-playlist branches, which assume the internal worker's layout.
        if stream.inputMode != "internal" {
            let file = stream.inputMode == "ffmpegMultiTsHls" ? "master.m3u8" : "live.m3u8"
            return try serveRuntime(streamId: id, file: file)
        }
        if stream.kind == "m3u8" {
            // Unlike kind=mpd (a background RestreamJob worker that logs every
            // fetch), m3u8 is pulled through live on each player request with
            // no background process at all — it had no logging whatsoever, so
            // a broken remote URL just failed silently with nothing in the
            // Logs tab to diagnose it. Mirrors the DASH worker's
            // fetchManifest event naming so both kinds show up consistently.
            //
            // `variant=` is how a rewritten master playlist's own variant/
            // audio URIs loop back here (see rewriteMasterM3U8 below) rather
            // than the configured stream.url — a master playlist's entries
            // are themselves other playlists, not segments, so they need the
            // same manifest-fetch-and-rewrite treatment recursively, not the
            // raw-bytes /proxy/ pipe every actual segment goes through.
            let manifestURL: URL
            if let variant = request.query["variant"], let variantURL = URL(string: variant) {
                manifestURL = variantURL
            } else {
                manifestURL = URL(string: stream.url)!
            }
            let data: Data
            do {
                data = try fetchRemote(provider: provider, stream: stream, url: manifestURL, category: .manifest)
            } catch {
                logStore.record(streamId: stream.id, level: "error", event: "fetchManifest", url: manifestURL.absoluteString, message: "\(error)")
                throw error
            }
            logStore.record(streamId: stream.id, level: "info", event: "fetchManifest", url: manifestURL.absoluteString, message: "success · \(data.count) bytes")
            let text = String(data: data, encoding: .utf8) ?? ""
            if M3U8Rewriter.isMasterPlaylist(text) {
                let rewritten = rewriteMasterM3U8(text, base: manifestURL, streamId: stream.id)
                return response(status: 200, body: Data(rewritten.utf8), type: "application/vnd.apple.mpegurl; charset=utf-8", noStore: true)
            }
            let rewritten = rewriteM3U8(text, base: manifestURL, streamId: stream.id, decrypt: !stream.hlsKey.isEmpty)
            return response(status: 200, body: Data(rewritten.utf8), type: "application/vnd.apple.mpegurl; charset=utf-8", noStore: true)
        }
        if stream.representations.count > 1 {
            return serveMasterPlaylist(stream: stream)
        }
        return try serveRuntime(streamId: id, file: "live.m3u8")
    }

    /// A live DASH MPD generated alongside the HLS playlist, reading the
    /// exact same downloaded segments off disk (via listRuntimeSegments,
    /// the same helper /direct/ and /download/ use) rather than fetching or
    /// storing anything twice. Uses <SegmentList> — literal per-segment
    /// URLs — instead of <SegmentTemplate>'s $Number$ token substitution,
    /// since our on-disk filenames (seg_<number>_<sanitizedName>.<ext>)
    /// aren't a fixed-width formula SegmentTemplate could reconstruct; a
    /// nominal per-segment duration is used since segment durations here
    /// are already close to uniform and SegmentList's `duration` attribute
    /// is exactly the "list of URLs, roughly this long each" primitive.
    func serveLiveMPD(stream: StreamConfig, timescale: Int = 1000) -> Data {
        let repIds = effectiveRepresentationIds(for: stream)
        let multiReps = repIds.count > 1
        let videoReps = repIds.filter { stream.representationMeta[$0]?.type != "audio" }
        let audioReps = repIds.filter { stream.representationMeta[$0]?.type == "audio" }

        func adaptationSet(mimeType: String, reps: [String]) -> String {
            guard !reps.isEmpty else { return "" }
            var block = "    <AdaptationSet mimeType=\"\(mimeType)\" segmentAlignment=\"true\">\n"
            for repId in reps {
                let meta = stream.representationMeta[repId]
                let diskRepId = multiReps ? repId : ""
                let dir = directRuntimeDir(streamId: stream.id, repId: diskRepId)
                let (initURL, segments) = listRuntimeSegments(in: dir)
                guard let initURL, !segments.isEmpty else { continue }
                let urlPrefix = multiReps ? "\(repId)/" : ""
                var attrs = ["id=\"\(xmlEscape(repId))\""]
                if let bandwidth = meta?.bandwidth { attrs.append("bandwidth=\"\(bandwidth)\"") }
                if let w = meta?.width, let h = meta?.height { attrs.append("width=\"\(w)\" height=\"\(h)\"") }
                if let codecs = meta?.codecs, !codecs.isEmpty { attrs.append("codecs=\"\(xmlEscape(codecs))\"") }
                block += "      <Representation \(attrs.joined(separator: " "))>\n"
                block += "        <SegmentList timescale=\"\(timescale)\" duration=\"\(timescale * 2)\">\n"
                block += "          <Initialization sourceURL=\"\(urlPrefix)\(xmlEscape(initURL.lastPathComponent))\"/>\n"
                for segment in segments {
                    block += "          <SegmentURL media=\"\(urlPrefix)\(xmlEscape(segment.url.lastPathComponent))\"/>\n"
                }
                block += "        </SegmentList>\n"
                block += "      </Representation>\n"
            }
            block += "    </AdaptationSet>\n"
            return block
        }

        var xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        xml += "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" profiles=\"urn:mpeg:dash:profile:isoff-live:2011\" type=\"dynamic\" "
        xml += "availabilityStartTime=\"1970-01-01T00:00:00Z\" minimumUpdatePeriod=\"PT2S\" timeShiftBufferDepth=\"PT30S\" minBufferTime=\"PT2S\">\n"
        xml += "  <Period id=\"0\" start=\"PT0S\">\n"
        xml += adaptationSet(mimeType: "video/mp4", reps: videoReps)
        xml += adaptationSet(mimeType: "audio/mp4", reps: audioReps)
        xml += "  </Period>\n"
        xml += "</MPD>\n"
        return response(status: 200, body: Data(xml.utf8), type: "application/dash+xml; charset=utf-8", noStore: true)
    }

    func xmlEscape(_ text: String) -> String {
        text.replacingOccurrences(of: "&", with: "&amp;")
            .replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
            .replacingOccurrences(of: "\"", with: "&quot;")
    }

    func serveMasterPlaylist(stream: StreamConfig) -> Data {
        let audioReps = stream.representations.filter { stream.representationMeta[$0]?.type == "audio" }
        let videoReps = stream.representations.filter { stream.representationMeta[$0]?.type != "audio" }
        let hasAudioGroup = !audioReps.isEmpty
        var lines = ["#EXTM3U", "#EXT-X-INDEPENDENT-SEGMENTS"]
        for (index, repId) in audioReps.enumerated() {
            let meta = stream.representationMeta[repId]
            let language = meta?.language ?? "und"
            let name = meta?.language ?? repId
            lines.append(#"#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="\#(name)",LANGUAGE="\#(language)",AUTOSELECT=YES,DEFAULT=\#(index == 0 ? "YES" : "NO"),URI="\#(repId)/live.m3u8""#)
        }
        for (index, repId) in videoReps.enumerated() {
            let meta = stream.representationMeta[repId]
            // Every variant needs a distinct BANDWIDTH or hls.js (and other
            // spec-compliant HLS clients) may treat identical-bandwidth,
            // no-RESOLUTION entries as duplicates of the same rendition and
            // collapse them into a single selectable level — silently
            // hiding the other qualities. Real bandwidth from the detected
            // manifest is used when we have it; otherwise a distinct
            // placeholder (ordered high-to-low so the picker's "first" is
            // still the nominal top quality) keeps every variant distinct.
            let bandwidth = meta?.bandwidth ?? (videoReps.count - index) * 100_000
            var attrs = ["BANDWIDTH=\(bandwidth)"]
            if let w = meta?.width, let h = meta?.height { attrs.append("RESOLUTION=\(w)x\(h)") }
            if let codecs = meta?.codecs, !codecs.isEmpty { attrs.append(#"CODECS="\#(codecs)""#) }
            if hasAudioGroup { attrs.append(#"AUDIO="audio""#) }
            lines.append("#EXT-X-STREAM-INF:\(attrs.joined(separator: ","))")
            lines.append("\(repId)/live.m3u8")
        }
        let body = lines.joined(separator: "\n") + "\n"
        return response(status: 200, body: Data(body.utf8), type: "application/vnd.apple.mpegurl; charset=utf-8", noStore: true)
    }

    func serveProxy(_ request: HTTPRequest) throws -> Data {
        // The trailing extension is cosmetic as far as this handler is concerned
        // (the real target is always ?u=), but it has to be *accepted* here
        // because proxyPath now emits one — see proxyExtension for why.
        // Kept optional so links minted by an older build still resolve.
        guard let id = match(request.path, #"^/proxy/([^/]+)/item(?:\.[A-Za-z0-9]+)?$"#)?.first,
              let target = request.query["u"], let requestedUrl = URL(string: target) else { throw PanelError.badRequest("Invalid proxy request.") }
        let state = try readState()
        guard let (provider, stream) = providerAndStream(id, in: state) else { throw PanelError.notFound("Stream not found.") }
        let kind = request.query["kind"] ?? "segment"
        let category: HeaderCategory = kind == "key" ? .hlsKey : .media
        let url = kind == "key" ? requestedUrl : appendingQueryParams(requestedUrl, effectiveSegmentUrlParams(provider: provider, stream: stream))
        var data: Data
        do {
            data = try fetchRemote(provider: provider, stream: stream, url: url, category: category)
        } catch {
            // Error-only, unlike fetchManifest's success logging above — this
            // runs once per client per segment (not once per shared worker
            // loop like kind=mpd), so logging every success here would flood
            // the log for any stream with more than a couple of viewers.
            logStore.record(streamId: stream.id, level: "error", event: "downloadSegment", url: url.absoluteString, message: "\(error)")
            throw error
        }
        metrics.recordInput(streamId: stream.id, bytes: data.count)
        if !stream.hlsKey.isEmpty, kind != "key" {
            let sequence = request.query["seq"].flatMap { Int($0) } ?? 0
            data = try HLSDecryptor.decryptSegment(data, keyHex: stream.hlsKey, ivHex: stream.hlsIV, sequence: sequence)
        }
        return response(status: 200, body: data, type: contentType(url.path), noStore: true)
    }

    func serveRestream(_ request: HTTPRequest) throws -> Data {
        guard let match = match(request.path, #"^/restream/([^/]+)/(.+)$"#) else { throw PanelError.notFound("Not found.") }
        return try serveRuntime(streamId: match[0], file: String(match[1].removingPercentEncoding ?? match[1]))
    }

    func serveRuntime(streamId: String, file: String) throws -> Data {
        let base = runtimeDir.appendingPathComponent(streamId, isDirectory: true).standardizedFileURL
        let url = base.appendingPathComponent(file).standardizedFileURL
        guard url.path.hasPrefix(base.path) else { throw PanelError.badRequest("Invalid path.") }
        guard FileManager.default.fileExists(atPath: url.path) else {
            throw PanelError.notFound("Runtime file is not ready.")
        }
        let data = try Data(contentsOf: url)
        return response(status: 200, body: data, type: contentType(url.path), noStore: url.pathExtension == "m3u8")
    }

    func servePublic(_ path: String) throws -> Data {
        let trimmed = path.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        // Segment/playlist files physically live under public/runtime for deploy
        // convenience, but must only ever be reachable through the API-key-gated
        // /restream/ and /play/ routes (see isPlaybackPath) — never served directly
        // as if they were static assets.
        guard trimmed != "runtime", !trimmed.hasPrefix("runtime/") else {
            throw PanelError.notFound("Not found.")
        }
        let url = publicDir.appendingPathComponent(trimmed).standardizedFileURL
        guard url.path.hasPrefix(publicDir.standardizedFileURL.path) else { throw PanelError.badRequest("Invalid path.") }
        // Disk first (dev builds serve public/ live), then the copy embedded in
        // the binary — a standalone single-file build has no public/ on disk.
        if let data = try? Data(contentsOf: url) {
            return response(status: 200, body: data, type: contentType(url.path), noStore: url.lastPathComponent == "sw.js")
        }
        if let base64 = PublicAssets.files[trimmed], let data = Data(base64Encoded: base64) {
            return response(status: 200, body: data, type: contentType(trimmed), noStore: trimmed.hasSuffix("sw.js"))
        }
        throw PanelError.notFound("Not found.")
    }

    func fetchRemote(provider: Provider, stream: StreamConfig, url: URL, category: HeaderCategory) throws -> Data {
        let proxy = effectiveProxy(provider: provider, stream: stream)
        let headers = effectiveHeaders(provider: provider, stream: stream, category: category)
        return try HTTPClient(options: HTTPRequestOptions(timeout: 30, headers: headers, proxy: proxy.isEmpty ? nil : proxy)).get(url)
    }

    // MARK: - Stream editor probing
    //
    // This is deliberately thin: it fetches and sniffs the source, then hands
    // the actual parsing off to DashSegmentsCLI.probe (MPD) or
    // M3U8Rewriter.probeMaster (HLS) — PanelServer never parses manifest
    // formats itself.

    func probeSource(url: URL, proxy: String?, headers: [(String, String)]) throws -> [String: Any] {
        let path = url.path.lowercased()
        if path.hasSuffix(".m3u8") {
            return try probeHLS(url: url, proxy: proxy, headers: headers)
        }
        if path.hasSuffix(".mpd") {
            return try DashSegmentsCLI.probe(mpdURL: url, headers: headers, proxy: proxy, timeout: 20)
        }
        let data = try HTTPClient(options: HTTPRequestOptions(timeout: 20, headers: headers, proxy: proxy)).get(url)
        let text = String(data: data, encoding: .utf8) ?? ""
        if text.contains("#EXTM3U") {
            return try probeHLS(url: url, proxy: proxy, headers: headers, preloaded: data)
        }
        return try DashSegmentsCLI.probe(mpdURL: url, headers: headers, proxy: proxy, timeout: 20)
    }

    func probeHLS(url: URL, proxy: String?, headers: [(String, String)], preloaded: Data? = nil) throws -> [String: Any] {
        let data = try preloaded ?? HTTPClient(options: HTTPRequestOptions(timeout: 20, headers: headers, proxy: proxy)).get(url)
        let text = String(data: data, encoding: .utf8) ?? ""
        guard M3U8Rewriter.isMasterPlaylist(text) else {
            return ["kind": "m3u8", "representations": [] as [Any], "protection": [String: [String]]()]
        }
        let (variants, audioTracks) = M3U8Rewriter.probeMaster(text, base: url)
        var representations: [[String: Any]] = []
        for (index, variant) in variants.enumerated() {
            representations.append([
                "id": "v\(index)", "type": "video",
                "bandwidth": variant.bandwidth as Any, "width": variant.width as Any, "height": variant.height as Any,
                "codecs": variant.codecs as Any, "uri": variant.uri
            ])
        }
        for (index, track) in audioTracks.enumerated() {
            representations.append([
                "id": "a\(index)", "type": "audio",
                "language": track.language as Any, "name": track.name, "uri": track.uri as Any
            ])
        }
        return ["kind": "m3u8", "representations": representations, "protection": [String: [String]]()]
    }

    // Playlist parsing/classification lives in M3U8Rewriter (LiveMPDToM3U8.swift)
    // — this is just the routing glue: turn a resolved URI into our own
    // `/proxy/<id>/item` path.
    func rewriteM3U8(_ text: String, base: URL, streamId: String, decrypt: Bool = false) -> String {
        M3U8Rewriter.rewrite(text, base: base, dropKey: decrypt) { uri, kind, seq in
            guard let url = URL(string: uri) else { return uri }
            let kindString = kind == .key ? "key" : (kind == .map ? "map" : "segment")
            return self.proxyPath(streamId: streamId, url: url, seq: seq, kind: kindString)
        }
    }

    // A master playlist's variant/audio URIs point at other playlists, not
    // segments — loop them back through /play/<id>/index.m3u8?variant=...
    // (re-entering the m3u8 branch above) instead of /proxy/.../item, so
    // each one gets fetched and rewritten the same way the top-level URL
    // was rather than proxied through as unrewritten raw bytes.
    func rewriteMasterM3U8(_ text: String, base: URL, streamId: String) -> String {
        M3U8Rewriter.rewriteMaster(text, base: base) { uri in
            let encoded = uri.addingPercentEncoding(withAllowedCharacters: Self.queryValueAllowed) ?? uri
            return "/play/\(streamId)/index.m3u8?variant=\(encoded)"
        }
    }

    func proxyPath(streamId: String, url: URL, seq: Int? = nil, kind: String = "segment") -> String {
        let encoded = url.absoluteString.addingPercentEncoding(withAllowedCharacters: Self.queryValueAllowed) ?? url.absoluteString
        var path = "/proxy/\(streamId)/item.\(proxyExtension(for: url, kind: kind))?u=\(encoded)&kind=\(kind)"
        if let seq { path += "&seq=\(seq)" }
        return path
    }

    /// ffmpeg/libav's HLS demuxer validates every segment URL's *file extension*
    /// against its `allowed_segment_extensions` list and hard-refuses the whole
    /// playlist if one doesn't match. Our proxy path used to end in a bare
    /// `/item` — no extension at all — so every ffmpeg-based player (VLC, Kodi,
    /// TiviMate, most set-top boxes / IPTV apps) rejected proxied m3u8 streams
    /// outright with "is not in allowed_segment_extensions", even though the
    /// bytes behind it were perfectly good. Browsers and hls.js don't look at
    /// the extension, which is why this only ever showed up on real players.
    ///
    /// Carry the source segment's own extension through when it's one ffmpeg
    /// accepts; otherwise fall back to a sane default for the kind. The
    /// extension is only ever a label here — the actual bytes are still fetched
    /// from `?u=` and the Content-Type is still derived from the source URL —
    /// and ffmpeg content-probes the segment regardless, so a fallback label
    /// can't misparse it. (The DASH worker already does the equivalent when it
    /// names its on-disk segments — see localSegmentURL in LiveMPDToM3U8.)
    private func proxyExtension(for url: URL, kind: String) -> String {
        let allowed: Set<String> = [
            "ts", "m4s", "mp4", "m4a", "m4v", "aac", "mp3", "ac3", "eac3",
            "mpegts", "mpeg", "mov", "vtt", "webvtt", "jpg", "jpeg", "png"
        ]
        let ext = url.pathExtension.lowercased()
        if allowed.contains(ext) { return ext }
        // An EXT-X-MAP init section is always ISOBMFF; anything else in an HLS
        // playlist is overwhelmingly MPEG-TS when it isn't self-describing.
        return kind == "map" ? "mp4" : "ts"
    }

    // CharacterSet.urlQueryAllowed permits "&"/"="/"+" since those are
    // legal *within* a query component per RFC 3986 — but that means it
    // doesn't escape them, so a signed CDN URL that already has its own
    // "&"-separated params (e.g. Mux's ?cdn=...&expires=...&signature=...)
    // gets truncated at the first unescaped "&" once embedded as a single
    // value inside our own query string (?variant=... / ?u=...):
    // parseRequest's URLComponents splits on "&"/"=" with no notion of
    // nesting, so everything after that point becomes separate top-level
    // params we never read, instead of part of the encoded value. Only the
    // RFC 3986 unreserved set round-trips safely when nested like this.
    private static let queryValueAllowed: CharacterSet = {
        var set = CharacterSet.alphanumerics
        set.insert(charactersIn: "-._~")
        return set
    }()

    func viewState(_ state: PanelState, host: String) -> Any {
        [
            "providers": state.providers.map { provider in providerView(provider, host: host) },
            "apiKeys": state.apiKeys.map(keyView),
            "system": systemStats.snapshotView(peaks: state.systemPeaks),
            "bandwidth": metrics.globalBandwidthView(),
            "settings": ["port": state.settings.port],
            "version": versionView()
        ]
    }

    /// MetricsStore only knows raw stream ids and playback identities
    /// ("key:<id>" / "ip:<addr>") — this attaches the human-facing labels
    /// (provider/stream name, kind, and API key label) for the monitoring
    /// connections table.
    func enrichConnections(_ raw: [[String: Any]], state: PanelState) -> [[String: Any]] {
        var streamInfo: [String: (provider: String, name: String, kind: String)] = [:]
        for provider in state.providers {
            for stream in provider.streams {
                streamInfo[stream.id] = (provider.name, stream.name, stream.kind)
            }
        }
        var keyLabels: [String: String] = [:]
        for key in state.apiKeys { keyLabels[key.id] = key.label }
        return raw.map { entry in
            var enriched = entry
            if let streamId = entry["streamId"] as? String, let info = streamInfo[streamId] {
                enriched["providerName"] = info.provider
                enriched["streamName"] = info.name
                enriched["kind"] = info.kind
            }
            if let identity = entry["identity"] as? String {
                if identity.hasPrefix("key:") {
                    enriched["user"] = keyLabels[String(identity.dropFirst(4))] ?? "Revoked key"
                } else {
                    enriched["user"] = "Open access"
                }
            }
            return enriched
        }
    }

    // Picks which enabled account a script action (login/pair/channels/
    // events) runs with, honoring the provider's accountSelectionMode.
    // "fixed" (default) just returns the marked-active account, falling
    // back to the first enabled one — the original, only behavior before
    // rotate/random existed. "rotate" persists lastRotatedAccountId so
    // consecutive calls, even across separate requests, cycle through
    // every enabled account instead of always hitting the same one —
    // useful for spreading load or rate limits across several credentialed
    // accounts. "random" picks a different enabled account each call with
    // no memory between calls. Disabled accounts are skipped in every mode
    // — enabled/disabled is a kill switch on using that account's
    // credentials at all, not just a display preference.
    func resolveScriptAccount(providerIndex: Int, state: inout PanelState) throws -> ScriptAccount? {
        let provider = state.providers[providerIndex]
        let enabledAccounts = provider.scriptAccounts.filter(\.enabled)
        guard !enabledAccounts.isEmpty else { return nil }
        switch provider.accountSelectionMode {
        case "random":
            return enabledAccounts.randomElement()
        case "rotate":
            let nextIndex: Int
            if let lastIndex = enabledAccounts.firstIndex(where: { $0.id == provider.lastRotatedAccountId }) {
                nextIndex = (lastIndex + 1) % enabledAccounts.count
            } else {
                nextIndex = 0
            }
            let chosen = enabledAccounts[nextIndex]
            state.providers[providerIndex].lastRotatedAccountId = chosen.id
            try writeState(state)
            return chosen
        default:
            return enabledAccounts.first(where: { $0.id == provider.activeScriptAccountId }) ?? enabledAccounts.first
        }
    }

    func providerView(_ provider: Provider, host: String) -> [String: Any] {
        [
            "id": provider.id,
            "name": provider.name,
            "logo": provider.logo,
            "proxy": provider.proxy,
            "headers": provider.headers,
            "segmentUrlParams": provider.segmentUrlParams,
            "inheritUrlParams": provider.inheritUrlParams,
            "scriptPath": provider.scriptPath,
            "scriptBind": provider.scriptBind,
            "scriptDoh": provider.scriptDoh,
            "scriptWorker": provider.scriptWorker,
            "scriptAccounts": provider.scriptAccounts.map { account in
                ["id": account.id, "name": account.name, "username": account.username, "password": account.password, "params": account.params, "enabled": account.enabled] as [String: Any]
            },
            "activeScriptAccountId": provider.activeScriptAccountId,
            "accountSelectionMode": provider.accountSelectionMode,
            "streams": provider.streams.map { stream in streamView(stream, host: host) }
        ]
    }

    func keyView(_ key: APIKey) -> [String: Any] {
        let usage = metrics.keyUsage(id: key.id)
        return [
            "id": key.id,
            "key": key.key,
            "label": key.label,
            "createdAt": ISO8601DateFormatter.sharedTimestamp.string(from: key.createdAt),
            "requests": usage.requests,
            "bytes": usage.bytes,
            "lastSeenAt": usage.lastSeenAt.map { ISO8601DateFormatter.sharedTimestamp.string(from: $0) } as Any
        ]
    }

    func generateKey() -> String {
        "rsa_" + UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
    }

    func joinLines(_ text: String) -> String {
        text.components(separatedBy: .newlines).map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }.joined(separator: "|")
    }

    func streamView(_ stream: StreamConfig, host: String) -> [String: Any] {
        lock.lock(); let running = !(jobs[stream.id] ?? []).isEmpty || stream.kind == "m3u8" && stream.status == "running"; lock.unlock()
        // Per-representation "no m3u8" direct byte-stream links — one
        // persistent connection per representation, tailing new segments as
        // they're downloaded. Only meaningful for live/dash sources, not
        // proxied m3u8 (those have no representation-scoped runtime dir).
        var directUrls: [String: String] = [:]
        var downloadUrls: [String: String] = [:]
        if stream.kind == "m3u8" {
            // A proxied m3u8 has no representation-scoped runtime dir to tail,
            // so its "direct link" is a 302 straight to the origin source —
            // hand the player the CDN instead of proxying every segment.
            directUrls["source"] = "http://\(host)/source/\(stream.id)"
        } else {
            let repIds = effectiveRepresentationIds(for: stream)
            let multiReps = repIds.count > 1
            for repId in repIds {
                let diskRepId = repId.isEmpty ? "default" : repId
                let suffix = multiReps ? "/\(stream.id)/\(diskRepId)" : "/\(stream.id)"
                directUrls[diskRepId] = "http://\(host)/direct\(suffix)"
                downloadUrls[diskRepId] = "http://\(host)/download\(suffix).mp4"
            }
        }
        return [
            "id": stream.id,
            "name": stream.name,
            "kind": stream.kind,
            "url": stream.url,
            "representation": stream.representation,
            "representations": stream.representations,
            "representationMeta": stream.representationMeta.mapValues { meta in
                [
                    "type": meta.type,
                    "bandwidth": meta.bandwidth as Any,
                    "width": meta.width as Any,
                    "height": meta.height as Any,
                    "codecs": meta.codecs as Any,
                    "language": meta.language as Any
                ] as [String: Any]
            },
            "period": stream.period,
            "manifestHeaders": stream.manifestHeaders,
            "mediaHeaders": stream.mediaHeaders,
            "hlsKeyHeaders": stream.hlsKeyHeaders,
            "proxy": stream.proxy,
            "playlistSegments": stream.playlistSegments,
            "keepSegments": stream.keepSegments,
            "downloadAhead": stream.downloadAhead,
            "pollInterval": stream.pollInterval,
            "forceOffline": stream.forceOffline,
            "logo": stream.logo,
            "decryptionKeys": stream.decryptionKeys,
            "hlsKey": stream.hlsKey,
            "hlsIV": stream.hlsIV,
            "audioDelayMs": stream.audioDelayMs,
            "running": running,
            "status": running ? "running" : stream.status,
            "lastError": stream.lastError as Any,
            "activeClients": metrics.activeClients(streamId: stream.id),
            "bandwidth": metrics.bandwidthView(streamId: stream.id),
            "inputBandwidth": metrics.inputBandwidthView(streamId: stream.id),
            "playUrl": "http://\(host)/play/\(stream.id)/index.m3u8",
            "directUrl": stream.kind == "m3u8" ? "http://\(host)/play/\(stream.id)/index.m3u8" : "http://\(host)/restream/\(stream.id)/live.m3u8",
            "directStreamUrls": directUrls,
            "downloadUrls": downloadUrls,
            "sourceType": stream.sourceType,
            "mode": stream.mode,
            "sessionManifest": stream.sessionManifest,
            "scriptParams": stream.scriptParams,
            "cdmType": stream.cdmType,
            "useCdm": stream.useCdm,
            "scriptVideoSelector": stream.scriptVideoSelector,
            "scriptAudioSelector": stream.scriptAudioSelector,
            "onDemand": stream.onDemand,
            "speedUp": stream.speedUp,
            "autostart": stream.autostart,
            "scriptStart": stream.scriptStart as Any,
            "scriptEnd": stream.scriptEnd as Any,
            "recordEvent": stream.recordEvent,
            "inputMode": stream.inputMode,
            "outputMode": stream.outputMode,
            "outputTarget": stream.outputTarget,
            "cdnUrls": stream.cdnUrls
        ]
    }

    func representationsFromBody(_ raw: [String: Any]?) -> ([String], [String: RepresentationMetaEntry]) {
        let ids = (raw?["representations"] as? [Any])?.compactMap { $0 as? String } ?? []
        var meta: [String: RepresentationMetaEntry] = [:]
        if let metaRaw = raw?["representationMeta"] as? [String: Any] {
            for (key, value) in metaRaw {
                guard let dict = value as? [String: Any], let type = dict["type"] as? String else { continue }
                meta[key] = RepresentationMetaEntry(
                    type: type,
                    bandwidth: (dict["bandwidth"] as? NSNumber)?.intValue,
                    width: (dict["width"] as? NSNumber)?.intValue,
                    height: (dict["height"] as? NSNumber)?.intValue,
                    codecs: dict["codecs"] as? String,
                    language: dict["language"] as? String
                )
            }
        }
        return (ids, meta)
    }

    // JSONValue (used for the rest of the flat provider/stream request
    // bodies) only decodes scalars — same reason representationsFromBody
    // above bypasses it for arrays/objects. Existing accounts keep their id
    // (so an in-place edit doesn't orphan Login/Pair history tagged to that
    // id); accounts missing an id (newly added client-side) get a fresh one.
    func scriptAccountsFromBody(_ raw: [String: Any]?) -> [ScriptAccount] {
        guard let list = raw?["scriptAccounts"] as? [Any] else { return [] }
        return list.compactMap { entry in
            guard let dict = entry as? [String: Any], let name = dict["name"] as? String, !name.trimmingCharacters(in: .whitespaces).isEmpty else { return nil }
            let id = (dict["id"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? safeId("account")
            let enabled = (dict["enabled"] as? Bool) ?? true
            return ScriptAccount(id: id, name: name, username: dict["username"] as? String ?? "", password: dict["password"] as? String ?? "", params: dict["params"] as? String ?? "", enabled: enabled)
        }
    }

    /// Parses a `channels`/`events` script's stdout (the `{"Channels": [...]}"`
    /// / `{"Events": [...]}"` shape from the README) and upserts one stream
    /// per entry under the given provider. Matched by (name, sourceType) so
    /// re-running an import updates existing entries in place rather than
    /// piling up duplicates every time — nothing here identifies entries by
    /// a stable id, only by name, since that's all the protocol provides.
    /// Streams created this way don't yet do anything different at playback
    /// time (the session-manifest pipeline these fields describe isn't
    /// wired up); this only stores what the script reported.
    func importScriptEntries(stdout: String, action: String, providerId: String) throws -> Int {
        guard let data = stdout.data(using: .utf8),
              let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else {
            throw PanelError.server("Script did not print valid JSON.")
        }
        let key = action == "channels" ? "Channels" : "Events"
        guard let list = object[key] as? [Any] else {
            throw PanelError.server("Script output has no '\(key)' array.")
        }
        let sourceType = action == "channels" ? "channel" : "event"

        // Logo lookups are a network call each (see LogoLookup) — resolve
        // them all before taking stateQueue, not inside it, so importing a
        // few dozen channels doesn't hold the whole panel's state lock for
        // however long that takes. LogoLookup caches per name internally,
        // so this is cheap on any re-import.
        var logosByName: [String: String] = [:]
        for entry in list {
            guard let dict = entry as? [String: Any],
                  let name = (dict["Name"] as? String)?.trimmingCharacters(in: .whitespaces), !name.isEmpty,
                  logosByName[name] == nil else { continue }
            logosByName[name] = logoLookup.bestMatch(for: name) ?? ""
        }

        var imported = 0
        try stateQueue.sync {
            var state = try readState()
            guard let providerIndex = state.providers.firstIndex(where: { $0.id == providerId }) else {
                throw PanelError.notFound("Provider not found.")
            }
            for entry in list {
                guard let dict = entry as? [String: Any],
                      let name = (dict["Name"] as? String)?.trimmingCharacters(in: .whitespaces), !name.isEmpty else { continue }
                var stream = state.providers[providerIndex].streams.first { $0.name == name && $0.sourceType == sourceType }
                    ?? StreamConfig(id: safeId("stream"), name: name, kind: "mpd", url: "", representation: "", period: "", proxy: "", playlistSegments: 6, keepSegments: 10, downloadAhead: 8, pollInterval: 2, forceOffline: false, status: "stopped", lastError: nil)
                stream.name = name
                if stream.logo.trimmingCharacters(in: .whitespaces).isEmpty, let logo = logosByName[name], !logo.isEmpty {
                    stream.logo = logo
                }
                stream.sourceType = sourceType
                stream.mode = (dict["Mode"] as? String) ?? "live"
                stream.sessionManifest = (dict["SessionManifest"] as? Bool) ?? false
                stream.scriptParams = (dict["ScriptParams"] as? String) ?? ""
                stream.cdmType = (dict["CdmType"] as? String) ?? ""
                stream.useCdm = (dict["UseCdm"] as? Bool) ?? false
                stream.scriptVideoSelector = (dict["Video"] as? String) ?? ""
                stream.scriptAudioSelector = (dict["Audio"] as? String) ?? ""
                stream.onDemand = (dict["OnDemand"] as? Bool) ?? false
                stream.speedUp = (dict["SpeedUp"] as? Bool) ?? false
                stream.autostart = (dict["Autostart"] as? Bool) ?? false
                stream.scriptStart = (dict["Start"] as? NSNumber)?.intValue
                stream.scriptEnd = (dict["End"] as? NSNumber)?.intValue
                stream.recordEvent = (dict["RecordEvent"] as? Bool) ?? false
                if let existingIndex = state.providers[providerIndex].streams.firstIndex(where: { $0.name == name && $0.sourceType == sourceType }) {
                    state.providers[providerIndex].streams[existingIndex] = stream
                } else {
                    state.providers[providerIndex].streams.append(stream)
                }
                imported += 1
            }
            try writeState(state)
        }
        return imported
    }

    func streamFromBody(_ body: Data, existingId: String) throws -> StreamConfig {
        let input = try decodeJSON([String: JSONValue].self, body)
        let raw = try? JSONSerialization.jsonObject(with: body) as? [String: Any]
        let name = input["name"]?.string ?? ""
        let url = input["url"]?.string ?? ""
        guard !name.trimmingCharacters(in: .whitespaces).isEmpty else { throw PanelError.badRequest("Stream name is required.") }
        guard URL(string: url)?.scheme?.hasPrefix("http") == true else { throw PanelError.badRequest("Stream URL must be http or https.") }
        var logo = input["logo"]?.string ?? ""
        if logo.trimmingCharacters(in: .whitespaces).isEmpty {
            logo = logoLookup.bestMatch(for: name) ?? ""
        }
        let (representations, representationMeta) = representationsFromBody(raw)
        var stream = StreamConfig(
            id: existingId,
            name: name,
            kind: input["kind"]?.string == "m3u8" ? "m3u8" : "mpd",
            url: url,
            representation: input["representation"]?.string ?? "",
            period: input["period"]?.string ?? "",
            proxy: input["proxy"]?.string ?? "",
            playlistSegments: max(3, input["playlistSegments"]?.int ?? 6),
            keepSegments: max(1, input["keepSegments"]?.int ?? 60),
            downloadAhead: max(1, input["downloadAhead"]?.int ?? 8),
            pollInterval: max(0.25, input["pollInterval"]?.double ?? 2),
            forceOffline: input["forceOffline"]?.bool ?? false,
            status: "stopped",
            lastError: nil,
            logo: logo,
            decryptionKeys: input["decryptionKeys"]?.string ?? "",
            hlsKey: input["hlsKey"]?.string ?? "",
            hlsIV: input["hlsIV"]?.string ?? "",
            representations: representations,
            representationMeta: representationMeta,
            manifestHeaders: input["manifestHeaders"]?.string ?? "",
            mediaHeaders: input["mediaHeaders"]?.string ?? "",
            hlsKeyHeaders: input["hlsKeyHeaders"]?.string ?? "",
            audioDelayMs: input["audioDelayMs"]?.int ?? 0
        )
        let validInputModes = ["internal", "ffmpegResident", "ffmpegTsHls", "ffmpegMultiTsHls", "ffmpegFmp4Hls"]
        let requestedInputMode = input["inputMode"]?.string ?? "internal"
        stream.inputMode = validInputModes.contains(requestedInputMode) ? requestedInputMode : "internal"
        let validOutputModes = ["hls", "srtServer", "udpSrt", "custom"]
        let requestedOutputMode = input["outputMode"]?.string ?? "hls"
        stream.outputMode = validOutputModes.contains(requestedOutputMode) ? requestedOutputMode : "hls"
        stream.outputTarget = input["outputTarget"]?.string ?? ""
        // CDN mirror URLs: accept either a JSON array of strings or a single
        // newline-separated string; keep only valid http(s) URLs, in order.
        var cdnCandidates: [String] = []
        if let array = raw?["cdnUrls"] as? [Any] {
            cdnCandidates = array.compactMap { $0 as? String }
        } else if let text = input["cdnUrls"]?.string {
            cdnCandidates = text.components(separatedBy: .newlines)
        }
        stream.cdnUrls = cdnCandidates
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty && URL(string: $0)?.scheme?.hasPrefix("http") == true }
        return stream
    }

    /// This process is the *only* writer of state.json (see writeState — every
    /// mutation funnels through it), so once we've decoded the file we can keep
    /// the decoded value in memory and hand it back on subsequent reads instead
    /// of re-reading and re-JSON-decoding the whole config on every request.
    /// This matters on the streaming hot path: each playback/segment request
    /// used to call readState() at least twice (authorizePlayback + the serve
    /// handler), and the metrics timer reads it once a second on top — all of
    /// which decoded the entire providers/streams tree from disk each time.
    /// The cache is refreshed by writeState after each successful write, so it
    /// stays coherent with what's on disk.
    private let stateCacheLock = NSLock()
    private var cachedState: PanelState?

    func readState() throws -> PanelState {
        stateCacheLock.lock()
        if let cachedState {
            stateCacheLock.unlock()
            return cachedState
        }
        stateCacheLock.unlock()
        try ensureData()
        let data = try Data(contentsOf: stateFile)
        let decoded = try JSONDecoder().decode(PanelState.self, from: data)
        stateCacheLock.lock()
        cachedState = decoded
        stateCacheLock.unlock()
        return decoded
    }

    func writeState(_ state: PanelState) throws {
        let data = try JSONEncoder.pretty.encode(state)
        try data.write(to: stateFile, options: [.atomic])
        stateCacheLock.lock()
        cachedState = state
        stateCacheLock.unlock()
    }

    func ensureData() throws {
        try FileManager.default.createDirectory(at: runtimeDir, withIntermediateDirectories: true)
        if !FileManager.default.fileExists(atPath: stateFile.path) {
            try writeState(PanelState())
        }
    }

    /// `--admin-username`/`--admin-password` (or the RESTREAMAIR_ADMIN_USERNAME
    /// / RESTREAMAIR_ADMIN_PASSWORD env vars) replace whatever admin accounts
    /// already exist with exactly this one — a deliberate reset path for
    /// "I'm locked out" or scripted deployments, not a merge/add.
    private func applyAdminOverride(username: String, password: String) throws {
        guard password.count >= 8 else {
            throw PanelError.badRequest("admin password override must be at least 8 characters.")
        }
        var state = try readState()
        let (hash, salt) = try AuthStore.hashPassword(password)
        state.adminUsers = [AdminUser(id: safeId("user"), username: username, passwordHash: hash, salt: salt, createdAt: Date())]
        try writeState(state)
        print("Admin credentials overwritten for user \"\(username)\" (via --admin-username/--admin-password).")
    }

    func response(status: Int, body: Data, type: String, noStore: Bool, extraHeaders: [String: String] = [:]) -> Data {
        var lines = [
            "HTTP/1.1 \(status) \(statusText(status))",
            "Content-Type: \(type)",
            "Content-Length: \(body.count)",
            "Access-Control-Allow-Origin: *",
            "Cache-Control: \(noStore ? "no-store" : "public, max-age=60")",
            "Connection: close"
        ]
        for (name, value) in extraHeaders { lines.append("\(name): \(value)") }
        lines.append("")
        lines.append("")
        var headers = lines.joined(separator: "\r\n").data(using: .utf8)!
        headers.append(body)
        return headers
    }

    func jsonResponse(status: Int, _ object: Any) -> Data {
        let normalized = DashSegmentsCLI.normalizeJSON(object)
        let body = (try? JSONSerialization.data(withJSONObject: normalized, options: [.prettyPrinted, .sortedKeys])) ?? Data("{}".utf8)
        return response(status: status, body: body, type: "application/json; charset=utf-8", noStore: true)
    }

    func decodeJSON<T: Decodable>(_ type: T.Type, _ data: Data) throws -> T {
        try JSONDecoder().decode(T.self, from: data.isEmpty ? Data("{}".utf8) : data)
    }

    func findStream(_ id: String, in state: PanelState) -> (provider: Int, stream: Int)? {
        for providerIndex in state.providers.indices {
            if let streamIndex = state.providers[providerIndex].streams.firstIndex(where: { $0.id == id }) {
                return (providerIndex, streamIndex)
            }
        }
        return nil
    }

    func streamById(_ id: String, in state: PanelState) -> StreamConfig? {
        findStream(id, in: state).map { state.providers[$0.provider].streams[$0.stream] }
    }

    func providerAndStream(_ id: String, in state: PanelState) -> (provider: Provider, stream: StreamConfig)? {
        guard let location = findStream(id, in: state) else { return nil }
        return (state.providers[location.provider], state.providers[location.provider].streams[location.stream])
    }

    func effectiveProxy(provider: Provider, stream: StreamConfig) -> String {
        stream.proxy.isEmpty ? provider.proxy : stream.proxy
    }

    /// When a provider has `inheritUrlParams` enabled, every stream's segment/init
    /// requests reuse that same stream's own Source URL query string (e.g. a CDN
    /// auth token embedded in `?token=...`) instead of a manually typed, easy-to-
    /// go-stale fallback string.
    func effectiveSegmentUrlParams(provider: Provider, stream: StreamConfig) -> String {
        guard provider.inheritUrlParams else { return provider.segmentUrlParams }
        guard let url = URL(string: stream.url), let query = URLComponents(url: url, resolvingAgainstBaseURL: false)?.query else {
            return ""
        }
        return query
    }

    func effectiveHeaders(provider: Provider, stream: StreamConfig, category: HeaderCategory) -> [(String, String)] {
        var merged: [String: String] = [:]
        var order: [String] = []
        func apply(_ text: String) {
            for (name, value) in parseHeaders(text) {
                if merged[name] == nil { order.append(name) }
                merged[name] = value
            }
        }
        apply(provider.headers)
        switch category {
        case .manifest: apply(stream.manifestHeaders)
        case .media: apply(stream.mediaHeaders)
        case .hlsKey: apply(stream.hlsKeyHeaders)
        }
        return order.map { ($0, merged[$0]!) }
    }

    func headerText(_ pairs: [(String, String)]) -> String {
        pairs.map { "\($0.0): \($0.1)" }.joined(separator: "\n")
    }

    func parseHeaders(_ text: String) -> [(String, String)] {
        text.components(separatedBy: .newlines).compactMap { line in
            guard let separator = line.firstIndex(of: ":") else { return nil }
            return (
                String(line[..<separator]).trimmingCharacters(in: .whitespaces),
                String(line[line.index(after: separator)...]).trimmingCharacters(in: .whitespaces)
            )
        }
    }

    func swiftHeader(_ text: String) -> String {
        parseHeaders(text).map { "\($0.0): \($0.1)" }.joined(separator: "|")
    }

    /// JSON-encodes a string as a quoted JSON value (with the surrounding
    /// quotes) so it can be interpolated straight into a hand-built log line.
    func jsonString(_ text: String) -> String {
        if let data = try? JSONSerialization.data(withJSONObject: [text]),
           let array = String(data: data, encoding: .utf8) {
            // ["..."] -> "..."
            return String(array.dropFirst().dropLast())
        }
        return "\"\""
    }

    // Every route check runs through match(), compiling the same handful of
    // string-literal path patterns over and over — a fresh NSRegularExpression
    // per call, several times per request as the route ladder falls through.
    // The pattern set is fixed and finite (all literals in the routing code),
    // so caching each compiled regex by its pattern keeps the cache bounded and
    // takes regex compilation off the per-request path entirely.
    private static let regexCacheLock = NSLock()
    private static var regexCache: [String: NSRegularExpression] = [:]

    private static func compiledRegex(_ pattern: String) -> NSRegularExpression? {
        regexCacheLock.lock(); defer { regexCacheLock.unlock() }
        if let cached = regexCache[pattern] { return cached }
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return nil }
        regexCache[pattern] = regex
        return regex
    }

    func match(_ text: String, _ pattern: String) -> [String]? {
        guard let regex = PanelServer.compiledRegex(pattern) else { return nil }
        let range = NSRange(text.startIndex..<text.endIndex, in: text)
        guard let result = regex.firstMatch(in: text, range: range) else { return nil }
        return (1..<result.numberOfRanges).compactMap { index in
            Range(result.range(at: index), in: text).map { String(text[$0]) }
        }
    }

    func safeId(_ prefix: String) -> String {
        "\(prefix)_\(Int(Date().timeIntervalSince1970 * 1000))_\(UUID().uuidString.prefix(6))"
    }

    func contentType(_ file: String) -> String {
        switch URL(fileURLWithPath: file).pathExtension.lowercased() {
        case "html": return "text/html; charset=utf-8"
        case "css": return "text/css; charset=utf-8"
        case "js": return "application/javascript; charset=utf-8"
        case "json": return "application/json; charset=utf-8"
        case "webmanifest": return "application/manifest+json; charset=utf-8"
        case "m3u8": return "application/vnd.apple.mpegurl; charset=utf-8"
        case "ts": return "video/mp2t"
        case "mp4": return "video/mp4"
        case "m4s", "dash": return "video/iso.segment"
        case "jpg", "jpeg": return "image/jpeg"
        case "png": return "image/png"
        case "svg": return "image/svg+xml"
        default: return "application/octet-stream"
        }
    }

    func statusText(_ status: Int) -> String {
        [200: "OK", 302: "Found", 400: "Bad Request", 401: "Unauthorized", 404: "Not Found", 500: "Internal Server Error"][status] ?? "OK"
    }
}

enum JSONValue: Decodable {
    case string(String), number(Double), bool(Bool), null

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() { self = .null }
        else if let value = try? container.decode(Bool.self) { self = .bool(value) }
        else if let value = try? container.decode(Double.self) { self = .number(value) }
        else { self = .string((try? container.decode(String.self)) ?? "") }
    }

    var string: String? {
        switch self {
        case .string(let value): return value
        case .number(let value): return value.truncatingRemainder(dividingBy: 1) == 0 ? "\(Int(value))" : "\(value)"
        case .bool(let value): return value ? "true" : "false"
        case .null: return nil
        }
    }

    var int: Int? {
        switch self {
        case .number(let value): return Int(value)
        case .string(let value): return Int(value)
        default: return nil
        }
    }

    var double: Double? {
        switch self {
        case .number(let value): return value
        case .string(let value): return Double(value)
        default: return nil
        }
    }

    var bool: Bool? {
        switch self {
        case .bool(let value): return value
        case .string(let value): return ["true", "1", "yes"].contains(value.lowercased())
        default: return nil
        }
    }
}

extension JSONEncoder {
    static var pretty: JSONEncoder {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return encoder
    }
}

extension ISO8601DateFormatter {
    /// A single reusable formatter. Creating an ISO8601DateFormatter is
    /// surprisingly expensive (it spins up an underlying NSDateFormatter/
    /// locale machinery), and several call sites — including per-log-line
    /// timestamping — were allocating a fresh one for every single string.
    /// `string(from:)` is safe to call concurrently on one shared instance.
    static let sharedTimestamp = ISO8601DateFormatter()
}

@main
struct RestreamAirMain {
    static var server: PanelServer?

    static func main() {
        let arguments = Array(CommandLine.arguments.dropFirst())
        switch arguments.first {
        case "dash":
            DashSegmentsCLI.main(Array(arguments.dropFirst()))
        case "live":
            LiveM3U8ActionRuntime.run(Array(arguments.dropFirst()))
        case "serve":
            runServe(Array(arguments.dropFirst()))
        case "selftest":
            // Known-answer crypto vectors — verifies the pure-Swift SHA/PBKDF2/
            // AES produce correct bytes on this platform (used by Linux CI).
            CryptoSelfTest.runOrExit()
        default:
            runServe(arguments)
        }
    }

    static func runServe(_ arguments: [String]) {
        do {
            var portOverride: UInt16?
            var bindOverride: String?
            var adminUsernameOverride: String?
            var adminPasswordOverride: String?
            var index = 0
            while index < arguments.count {
                switch arguments[index] {
                case "--port" where index + 1 < arguments.count:
                    portOverride = UInt16(arguments[index + 1])
                    index += 1
                case "--bind" where index + 1 < arguments.count:
                    bindOverride = arguments[index + 1]
                    index += 1
                case "--admin-username" where index + 1 < arguments.count:
                    adminUsernameOverride = arguments[index + 1]
                    index += 1
                case "--admin-password" where index + 1 < arguments.count:
                    adminPasswordOverride = arguments[index + 1]
                    index += 1
                default:
                    break
                }
                index += 1
            }
            let envUsername = ProcessInfo.processInfo.environment["RESTREAMAIR_ADMIN_USERNAME"]
            let envPassword = ProcessInfo.processInfo.environment["RESTREAMAIR_ADMIN_PASSWORD"]
            server = PanelServer(
                portOverride: portOverride, bindAddressOverride: bindOverride,
                adminUsernameOverride: adminUsernameOverride ?? envUsername,
                adminPasswordOverride: adminPasswordOverride ?? envPassword
            )
            try server?.start()
        } catch {
            fputs("error: \(error)\n", stderr)
            exit(1)
        }
    }
}
