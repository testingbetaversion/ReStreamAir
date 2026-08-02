import Foundation
#if canImport(Network)
import Network
#elseif canImport(Musl)
import Musl
#elseif canImport(Glibc)
import Glibc
#endif
#if os(Windows)
// Windows has no POSIX pid_t/kill/SIGPIPE/SIGHUP; the process-supervisor and
// signal handling that use them are #if-guarded off below (Windows runs the
// server single-process). This alias just lets those signatures compile.
typealias pid_t = Int32
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
/// The provider-script actions ReStreamAir is willing to invoke. A provider
/// declares which ones its script actually implements (and a stream may
/// override that), so the panel never calls an action a script doesn't handle —
/// the previous behaviour, where any script-shaped stream got `init`/`start`/
/// `stop` fired at it regardless, produced a stream of "invalid action" noise
/// in the logs for scripts that only did, say, `channels`.
///
/// Raw values are exactly what goes out as `action=`, so they're also the
/// protocol names in SCRIPTING.md.
enum ScriptAction: String, CaseIterable, Codable {
    case start, stop
    case login, pair
    case channels, events, epg
    case manifest              // session manifest — a fresh source URL per start
    case downloadmanifest      // the script fetches the manifest itself
    case url                   // the script rewrites a URL before it's fetched
    case pssh                  // the script post-processes extracted PSSH boxes
    case initparse             // the script inspects a fetched init segment
    case downloadinit          // not wired — see `isWired`
    case downloadmedia         // not wired — see `isWired`
    case cdm, heartbeat

    /// Whether ReStreamAir calls this action today. The two download hooks are
    /// configurable but inert: routing every segment through a spawned
    /// subprocess needs a persistent worker rather than one process per fetch,
    /// which is a separate change. Storing the choice now means enabling them
    /// later doesn't require reconfiguring anything.
    var isWired: Bool {
        switch self {
        case .downloadinit, .downloadmedia: return false
        default: return true
        }
    }

    /// Actions a provider runs on its own behalf, rather than for one stream.
    var isProviderLevel: Bool {
        switch self {
        case .login, .pair, .channels, .events, .epg: return true
        default: return false
        }
    }

    /// What a brand-new provider gets: the account and catalogue actions that
    /// nearly every script implements. The per-stream playback hooks stay off
    /// until they're asked for.
    static let providerDefaults: [ScriptAction] = [.login, .pair, .channels, .events]
}

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
    /// Which ScriptAction raw values this provider's script implements. The
    /// default for every stream underneath it, overridable per stream via
    /// StreamConfig.scriptActionsOverride.
    var scriptActions: [String] = ScriptAction.providerDefaults.map(\.rawValue)

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
        // Predates per-action gating: a provider that already has a script was
        // being called for all of these, so keep doing that rather than
        // silently switching its behaviour off on upgrade.
        if let stored = try container.decodeIfPresent([String].self, forKey: .scriptActions) {
            scriptActions = stored
        } else {
            scriptActions = scriptPath.trimmingCharacters(in: .whitespaces).isEmpty
                ? []
                : ScriptAction.providerDefaults.map(\.rawValue)
        }
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
    /// Per-stream script path that overrides the provider's script for THIS
    /// stream's manifest/cdm/heartbeat actions (blank = use the provider script).
    var scriptOverride: String = ""
    /// Which ScriptAction raw values run for this stream. `nil` — the normal
    /// case — inherits the provider's set; a non-nil array replaces it wholesale
    /// (an empty array therefore means "run nothing for this stream").
    var scriptActionsOverride: [String]? = nil
    /// Legacy — kept for decode compat; CDM is always run in "external" mode
    /// now (the script returns clear keys; the internal-challenge mode was
    /// removed). Not surfaced in the UI.
    var cdmMode: String = "external"
    /// Which request categories the proxy applies to (default all). Lets the
    /// proxy cover, say, only the session-manifest/script calls while the MPD +
    /// media segments go direct.
    var proxyScript: Bool = true
    var proxyManifest: Bool = true
    var proxyMedia: Bool = true
    /// Master on/off for the heartbeat (default on). When off, no heartbeat runs
    /// even if the manifest advertises one. When on, the interval is
    /// heartbeatSeconds (or the manifest's Heartbeat.PeriodMs when that's 0).
    var heartbeatEnabled: Bool = true
    /// Periodic provider-script "heartbeat" interval in seconds (0 = auto from
    /// the manifest) — some providers expire a session unless pinged; the panel
    /// runs the script's `heartbeat` action this often while live. See start().
    var heartbeatSeconds: Int = 0
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

    var nm3u8dlreParams: String = ""

    /// When set, `/play/<id>/index.m3u8` (and `.mpd`) answer with a 302
    /// straight to the origin source URL (or current CDN mirror) instead of
    /// proxying/remuxing — the player fetches everything from the source
    /// itself. Same behavior the explicit `/source/<id>` route always had,
    /// now switchable per stream from the editor.
    var directSource: Bool = false

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
        scriptOverride = try container.decodeIfPresent(String.self, forKey: .scriptOverride) ?? ""
        cdmMode = try container.decodeIfPresent(String.self, forKey: .cdmMode) ?? "external"
        proxyScript = try container.decodeIfPresent(Bool.self, forKey: .proxyScript) ?? true
        proxyManifest = try container.decodeIfPresent(Bool.self, forKey: .proxyManifest) ?? true
        proxyMedia = try container.decodeIfPresent(Bool.self, forKey: .proxyMedia) ?? true
        heartbeatEnabled = try container.decodeIfPresent(Bool.self, forKey: .heartbeatEnabled) ?? true
        heartbeatSeconds = try container.decodeIfPresent(Int.self, forKey: .heartbeatSeconds) ?? 0
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
        directSource = try container.decodeIfPresent(Bool.self, forKey: .directSource) ?? false
        nm3u8dlreParams = try container.decodeIfPresent(String.self, forKey: .nm3u8dlreParams) ?? ""
        if let stored = try container.decodeIfPresent([String].self, forKey: .scriptActionsOverride) {
            scriptActionsOverride = stored
        } else if container.contains(.sessionManifest) || container.contains(.useCdm) {
            // Predates per-action gating. The three legacy toggles were the
            // only gate a stream had, so rebuild the equivalent action set from
            // them — otherwise upgrading would silently stop resolving keys or
            // refreshing session manifests for streams that relied on them.
            var synthesized: [ScriptAction] = []
            if sessionManifest { synthesized.append(.manifest) }
            if useCdm { synthesized += [.cdm, .pssh, .initparse] }
            if heartbeatEnabled && heartbeatSeconds > 0 { synthesized.append(.heartbeat) }
            // isScriptDriven's old definition — anything that made the stream
            // script-shaped also got the lifecycle actions.
            let driven = sessionManifest || useCdm
                || !scriptParams.trimmingCharacters(in: .whitespaces).isEmpty
                || !scriptOverride.trimmingCharacters(in: .whitespaces).isEmpty
                || (heartbeatEnabled && heartbeatSeconds > 0)
            if driven { synthesized += [.start, .stop] }
            scriptActionsOverride = synthesized.isEmpty ? nil : synthesized.map(\.rawValue)
        } else {
            scriptActionsOverride = nil
        }
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
    /// Per-stream heartbeat timers (lock-guarded) — a repeating source that
    /// re-runs the provider script's `heartbeat` action to keep a session alive.
    private var heartbeatTimers: [String: DispatchSourceTimer] = [:]
    /// Per-stream index into [primary url] + cdnUrls, advanced on each ffmpeg
    /// (re)start so a resident ffmpeg cycles to the next CDN mirror when a
    /// source outage makes it exit and the supervisor restarts it.
    private var cdnRotation: [String: Int] = [:]
    /// Script manifest-refresh throttling (lock-guarded): a stream whose
    /// source keeps failing shouldn't hammer the provider script — one
    /// attempt per stream per `scriptRefreshCooldown`, never two at once.
    private var scriptRefreshLastAttempt: [String: Date] = [:]
    private var scriptRefreshInFlight: Set<String> = []
    static let scriptRefreshCooldown: TimeInterval = 60
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
        // If a previous panel process died uncleanly (crash → supervisor
        // restart), its download/ffmpeg workers are still running with
        // nobody supervising them — kill them before spawning replacements.
        reapOrphanWorkers()
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
        listener?.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                let host = boundAddress.isEmpty ? "127.0.0.1" : boundAddress
                print("ReStreamAir Swift panel running at http://\(host):\(boundPort)")
                self?.logPanel("info", "server", "panel started on \(host):\(boundPort) (\(AppVersion.version))")
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
            logPanel("info", "server", "panel started on \(host):\(boundPort) (\(AppVersion.version))")
        } catch let error as POSIXServer.BindError {
            if error.errnoValue == EADDRINUSE {
                fputs(portInUseMessage, stderr)
            } else {
                fputs("error: Could not bind port \(boundPort): error code \(error.errnoValue)\n", stderr)
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

    /// Hard ceiling on one buffered HTTP request (headers + body). Anything a
    /// legitimate client sends (panel API JSON, playlist uploads) is far below
    /// this; without a cap, a client that streams bytes without ever
    /// terminating its headers grows the buffer without bound.
    static let maxRequestBytes = 32 * 1024 * 1024

    func readRequest(_ connection: ClientConnection, completion: @escaping (HTTPRequest) -> Void) {
        var buffer = Data()
        func receive() {
            connection.receive { data, complete, error in
                if let data { buffer.append(data) }
                if error != nil || complete || buffer.count > PanelServer.maxRequestBytes {
                    connection.cancel()
                    return
                }
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
        // Clamp before doing arithmetic with it: a negative Content-Length
        // would build a reversed (crashing) range below, and one near Int.max
        // would overflow `bodyStart + contentLength`. Either arrives in a
        // single hand-crafted request, so both must be survivable.
        let contentLength = min(max(Int(headers["content-length"] ?? "0") ?? 0, 0), PanelServer.maxRequestBytes)
        let bodyStart = headerRange.upperBound
        guard data.count - bodyStart >= contentLength else { return nil }
        let body = Data(data[bodyStart..<(bodyStart + contentLength)])
        // A request target that isn't URL-parseable (control bytes, stray
        // characters from a port scanner) must not take the server down —
        // percent-encode and retry, and as a last resort route it as "/"
        // (which serves the panel index; harmless for garbage input).
        let base = URL(string: "http://localhost")
        let url = URL(string: parts[1], relativeTo: base)
            ?? URL(string: parts[1].addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? "/", relativeTo: base)
            ?? URL(string: "http://localhost/")!
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
        return try sourceRedirect(stream: stream)
    }

    /// The 302-to-origin response shared by `/source/<id>` and the per-stream
    /// "Direct source" editor option on `/play/...`.
    func sourceRedirect(stream: StreamConfig) throws -> Data {
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

        if request.method == "GET", request.path == "/api/nm3u8dlre-status" {
            let binaryPath = NM3U8DLRELocator.resolve()
            let plan = NM3U8DLREInstaller.plan()
            return jsonResponse(status: 200, [
                "available": binaryPath != nil,
                "binaryPath": binaryPath as Any,
                "installCommand": plan?.displayCommand ?? NM3U8DLREInstaller.manualCommand,
                "canAutoInstall": plan != nil
            ])
        }

        if request.method == "POST", request.path == "/api/nm3u8dlre-install" {
            guard let plan = NM3U8DLREInstaller.plan() else {
                throw PanelError.badRequest("Can't install automatically here — run `\(NM3U8DLREInstaller.manualCommand)` yourself.")
            }
            let logStreamId = "nm3u8dlre-install"
            let logStore = self.logStore
            logStore.record(streamId: logStreamId, level: "info", event: "installStart", url: nil, message: plan.displayCommand)
            let process = Process()
            process.executableURL = URL(fileURLWithPath: plan.executable)
            process.arguments = plan.arguments
            let stdoutPipe = Pipe()
            let stderrPipe = Pipe()
            process.standardOutput = stdoutPipe
            process.standardError = stderrPipe
            let forward: (FileHandle) -> Void = { handle in
                let data = handle.availableData
                guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
                for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
                    logStore.record(streamId: logStreamId, level: "info", event: "install", url: nil, message: String(line))
                }
            }
            stdoutPipe.fileHandleForReading.readabilityHandler = forward
            stderrPipe.fileHandleForReading.readabilityHandler = forward
            process.terminationHandler = { proc in
                stdoutPipe.fileHandleForReading.readabilityHandler = nil
                stderrPipe.fileHandleForReading.readabilityHandler = nil
                try? stdoutPipe.fileHandleForReading.close()
                try? stderrPipe.fileHandleForReading.close()
                logStore.record(streamId: logStreamId, level: proc.terminationStatus == 0 ? "info" : "error", event: "installExit", url: nil, message: "exit \(proc.terminationStatus)")
            }
            try process.run()
            return jsonResponse(status: 202, ["started": true, "logStreamId": logStreamId])
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
            // Unknown names are dropped rather than stored: the set is a
            // whitelist the rest of the panel gates on, so it should only ever
            // hold actions this build understands.
            if let requestedActions = raw?["scriptActions"] as? [Any] {
                state.providers[providerIndex].scriptActions = requestedActions
                    .compactMap { $0 as? String }
                    .filter { ScriptAction(rawValue: $0) != nil }
            } else if state.providers[providerIndex].scriptActions.isEmpty,
                      !state.providers[providerIndex].scriptPath.trimmingCharacters(in: .whitespaces).isEmpty {
                // A provider that had no script until now starts with an empty
                // set. Seeding it the first time a script path appears keeps the
                // obvious next click (Login) from failing with "doesn't declare
                // the 'login' action". Only when the request said nothing about
                // actions — unticking them all is a real choice, and it stays.
                state.providers[providerIndex].scriptActions = ScriptAction.providerDefaults.map(\.rawValue)
            }
            try writeState(state)
            return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
        }

        if request.method == "DELETE", let match = match(request.path, #"^/api/providers/([^/]+)$"#) {
            guard let providerIndex = state.providers.firstIndex(where: { $0.id == match[0] }) else {
                throw PanelError.notFound("Provider not found.")
            }
            for stream in state.providers[providerIndex].streams { stop(stream: stream) }
            // The script's session/cookie jar goes with the provider that owned
            // it — leaving a stale login behind under runtime/scripts/ would
            // outlive the credentials it was made with.
            try? FileManager.default.removeItem(atPath: scriptSessionDir(providerId: match[0], create: false))
            state.providers.remove(at: providerIndex)
            try writeState(state)
            return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
        }

        // Wipes the script's stored session/cookies for this provider, so the
        // next login starts clean. The directory is recreated on demand by the
        // next invocation.
        if request.method == "POST", let match = match(request.path, #"^/api/providers/([^/]+)/script/clear-session$"#) {
            guard state.providers.contains(where: { $0.id == match[0] }) else {
                throw PanelError.notFound("Provider not found.")
            }
            try? FileManager.default.removeItem(atPath: scriptSessionDir(providerId: match[0], create: false))
            logPanel("info", "scriptSession", "cleared session store for provider \(match[0])")
            return jsonResponse(status: 200, ["cleared": true])
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
            guard let known = ScriptAction(rawValue: action), scriptAllows(known, provider: provider) else {
                throw PanelError.badRequest("This provider's script doesn't declare the '\(action)' action — enable it in Provider settings first.")
            }
            // Not every login is username+password (some scripts only need
            // a token already sitting in extra params, or nothing at all
            // beyond re-checking an existing session) — send whatever the
            // account has and let the script itself decide what's missing,
            // the same way it already would if run by hand.
            let logStreamId = "script:\(provider.id)"
            let args = ScriptRunner.commonArgs(action: action, bind: provider.scriptBind, proxy: provider.proxy, doh: provider.scriptDoh, worker: provider.scriptWorker, account: account, sessionDir: scriptSessionDir(providerId: provider.id))
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
        // EPG: same fire-and-poll shape as channels/events, but the reply isn't
        // imported as streams — it's guide data, so it's stored verbatim next to
        // the provider's session and served back from there. XMLTV or the JSON
        // form both go through untouched; ReStreamAir doesn't parse either, it
        // just holds onto what the script produced for a player to fetch.
        if request.method == "POST", let match = match(request.path, #"^/api/providers/([^/]+)/script/epg$"#) {
            guard let providerIndex = state.providers.firstIndex(where: { $0.id == match[0] }) else {
                throw PanelError.notFound("Provider not found.")
            }
            let provider = state.providers[providerIndex]
            guard !provider.scriptPath.trimmingCharacters(in: .whitespaces).isEmpty else {
                throw PanelError.badRequest("This provider has no script configured.")
            }
            guard scriptAllows(.epg, provider: provider) else {
                throw PanelError.badRequest("This provider's script doesn't declare the 'epg' action — enable it in Provider settings first.")
            }
            guard let account = try resolveScriptAccount(providerIndex: providerIndex, state: &state) else {
                throw PanelError.badRequest("Add or enable an account before running epg.")
            }
            let scriptPath = provider.scriptPath
            let logStreamId = "script:\(provider.id)"
            let destination = scriptSessionDir(providerId: provider.id) + "/epg.xml"
            let args = ScriptRunner.commonArgs(action: "epg", bind: provider.scriptBind, proxy: provider.proxy, doh: provider.scriptDoh, worker: provider.scriptWorker, account: account, sessionDir: scriptSessionDir(providerId: provider.id))
            logStore.record(streamId: logStreamId, level: "info", event: "scriptStart", url: nil, message: "epg · \(scriptPath) \(args.joined(separator: " "))")
            let logStore = self.logStore
            DispatchQueue.global(qos: .utility).async {
                do {
                    let (stdout, _) = try ScriptRunner.runSync(scriptPath: scriptPath, args: args, timeout: 120)
                    let guide = ScriptRunner.decodeValue(stdout.trimmingCharacters(in: .whitespacesAndNewlines))
                    guard !guide.isEmpty else {
                        logStore.record(streamId: logStreamId, level: "warn", event: "scriptExit", url: nil, message: "epg returned nothing")
                        return
                    }
                    try Data(guide.utf8).write(to: URL(fileURLWithPath: destination), options: .atomic)
                    logStore.record(streamId: logStreamId, level: "info", event: "scriptExit", url: nil, message: "epg stored · \(guide.utf8.count) bytes")
                } catch {
                    logStore.record(streamId: logStreamId, level: "error", event: "scriptExit", url: nil, message: "\(error)")
                }
            }
            return jsonResponse(status: 202, ["started": true, "logStreamId": logStreamId])
        }

        // Serves whatever the last `epg` run stored. Admin-gated like the rest
        // of /api/*; players that need it can be pointed here with a key.
        if request.method == "GET", let match = match(request.path, #"^/api/providers/([^/]+)/epg$"#) {
            let path = scriptSessionDir(providerId: match[0], create: false) + "/epg.xml"
            guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)) else {
                throw PanelError.notFound("No EPG stored for this provider yet — run the epg action first.")
            }
            let isXML = data.first == UInt8(ascii: "<")
            return response(status: 200, body: data, type: isXML ? "application/xml; charset=utf-8" : "application/json; charset=utf-8", noStore: true)
        }

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
            guard let known = ScriptAction(rawValue: action), scriptAllows(known, provider: provider) else {
                throw PanelError.badRequest("This provider's script doesn't declare the '\(action)' action — enable it in Provider settings first.")
            }
            let providerId = provider.id
            let scriptPath = provider.scriptPath
            let logStreamId = "script:\(provider.id)"
            let args = ScriptRunner.commonArgs(action: action, bind: provider.scriptBind, proxy: provider.proxy, doh: provider.scriptDoh, worker: provider.scriptWorker, account: account, sessionDir: scriptSessionDir(providerId: provider.id))
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

        if request.method == "POST", let match = match(request.path, #"^/api/providers/([^/]+)/script/run$"#) {
            guard let providerIndex = state.providers.firstIndex(where: { $0.id == match[0] }) else {
                throw PanelError.notFound("Provider not found.")
            }
            let provider = state.providers[providerIndex]
            guard !provider.scriptPath.trimmingCharacters(in: .whitespaces).isEmpty else {
                throw PanelError.badRequest("This provider has no script configured.")
            }
            guard let account = try resolveScriptAccount(providerIndex: providerIndex, state: &state) else {
                throw PanelError.badRequest("Add or enable an account before running a script action.")
            }
            
            let input = try decodeJSON([String: JSONValue].self, request.body)
            let action = input["action"]?.string ?? "run"
            
            var args = ScriptRunner.commonArgs(action: action, bind: provider.scriptBind, proxy: provider.proxy, doh: provider.scriptDoh, worker: provider.scriptWorker, account: account, sessionDir: scriptSessionDir(providerId: provider.id))
            
            for (k, v) in input {
                if k != "action" {
                    args.append(ScriptRunner.arg(k, v.string ?? ""))
                }
            }
            
            let scriptPath = provider.scriptPath
            let (stdout, stderr) = try ScriptRunner.runSync(scriptPath: scriptPath, args: args, timeout: 60)
            return jsonResponse(status: 200, ["stdout": stdout, "stderr": stderr])
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

        if request.method == "DELETE", request.path == "/api/logs" {
            // Clear the in-memory entries for one stream (or every stream when no
            // filter). The persisted per-day files are left alone.
            if let streamId = request.query["streamId"], !streamId.isEmpty {
                logStore.clearStream(streamId)
            } else {
                logStore.clearAll()
            }
            return jsonResponse(status: 200, ["ok": true])
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
                logPanel("warn", "auth", "failed sign-in for '\(input["username"] ?? "?")'")
                throw PanelError.unauthorized("Invalid username or password.")
            }
            let remember = input["remember"] == "true"
            let token = authStore.createSession(username: username, remember: remember)
            logPanel("info", "auth", "'\(username)' signed in")
            return response(status: 200, body: Data("{\"ok\":true}".utf8), type: "application/json; charset=utf-8", noStore: true, extraHeaders: ["Set-Cookie": authStore.setCookieHeader(token: token, remember: remember)])
        }

        if request.method == "POST", request.path == "/api/auth/logout" {
            logPanel("info", "auth", "signed out")
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
                // sessionManifest / useCdm / cdmType / cdmMode / scriptOverride /
                // scriptParams / heartbeatSeconds are set by the editor's
                // Scripting & DRM section, so they're read from the body
                // (streamFromBody) rather than carried over from the existing.
                let rawInput = try? decodeJSON([String: JSONValue].self, request.body)
                if existing.sourceType != "" && updated.scriptOverride.trimmingCharacters(in: .whitespaces).isEmpty {
                    if rawInput?["sessionManifest"] == nil { updated.sessionManifest = existing.sessionManifest }
                    if rawInput?["useCdm"] == nil { updated.useCdm = existing.useCdm }
                    if rawInput?["heartbeatEnabled"] == nil { updated.heartbeatEnabled = existing.heartbeatEnabled }
                    if rawInput?["scriptParams"] == nil { updated.scriptParams = existing.scriptParams }
                    if rawInput?["cdmType"] == nil { updated.cdmType = existing.cdmType }
                    if rawInput?["cdmMode"] == nil { updated.cdmMode = existing.cdmMode }
                }
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
                let streamName = state.providers[location.provider].streams[location.stream].name
                stop(stream: state.providers[location.provider].streams[location.stream])
                state.providers[location.provider].streams.remove(at: location.stream)
                try writeState(state)
                logPanel("info", "streamControl", "deleted '\(streamName)'")
                return jsonResponse(status: 200, viewState(state, host: request.headers["host"] ?? "127.0.0.1:\(port)"))
            }
        }

        if request.method == "POST", let match = match(request.path, #"^/api/streams/([^/]+)/(start|stop)$"#) {
            guard let location = findStream(match[0], in: state) else { throw PanelError.notFound("Stream not found.") }
            let streamName = state.providers[location.provider].streams[location.stream].name
            if match[1] == "start" {
                try start(provider: state.providers[location.provider], stream: &state.providers[location.provider].streams[location.stream])
                logPanel("info", "streamControl", "started '\(streamName)'")
            } else {
                stop(stream: state.providers[location.provider].streams[location.stream])
                state.providers[location.provider].streams[location.stream].status = "stopped"
                logPanel("info", "streamControl", "stopped '\(streamName)'")
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

    /// Record a panel-level (not stream-specific) event — server lifecycle,
    /// auth, stream control. Shows under the Logs tab's "Panel" filter.
    func logPanel(_ level: String, _ event: String, _ message: String) {
        logStore.record(streamId: "__panel__", level: level, event: event, url: nil, message: message)
    }

    /// When a stream is CENC-protected and the operator turned on CDM auto-keys
    /// (useCdm) with a provider script configured but entered no keys of their
    /// own, fetch the manifest, scrape the DRM challenge (PSSH + KIDs for DASH,
    /// EXT-X-KEY for HLS), and let the script hand back clear KID:KEY pairs. We
    /// don't run a real CDM — the script owns the license exchange; we just feed
    /// it what the manifest advertises. Best-effort: any failure logs and falls
    /// through to whatever the operator typed (possibly nothing), so a start
    /// never throws just because key acquisition failed. Runs synchronously
    /// before the worker spawns because the worker needs the keys up front;
    /// timeouts are bounded so a hung script can't wedge the start forever.
    func resolveDecryptionKeys(provider: Provider, stream: StreamConfig) -> String {
        let entered = joinLines(stream.decryptionKeys)
        guard entered.isEmpty, stream.useCdm, scriptAllows(.cdm, provider: provider, stream: stream) else { return entered }
        let scriptPath = effectiveScriptPath(provider: provider, stream: stream).trimmingCharacters(in: .whitespaces)
        guard !scriptPath.isEmpty, ScriptRunner.scriptExists(ScriptRunner.normalizePath(scriptPath)) else {
            logStore.record(streamId: stream.id, level: "warn", event: "cdm", url: nil, message: "CDM auto-keys on, but no readable script is configured for this stream")
            return entered
        }
        let account = activeScriptAccount(for: provider)
        // Script protocol (see allenteo.py): the common user=/password=/bind=/
        // proxy=/doh=/worker= prefix, cdm=external (the script returns clear
        // keys), then this stream's params. We don't pick a DRM system — we
        // parse the manifest for ALL of it and forward it.
        var args = ScriptRunner.commonArgs(action: "cdm", bind: provider.scriptBind, proxy: scriptProxy(provider: provider, stream: stream), doh: provider.scriptDoh, worker: provider.scriptWorker, account: account, sessionDir: scriptSessionDir(providerId: provider.id))
        args.append("cdm=external")
        args += ScriptRunner.splitParams(stream.scriptParams)
        // Parse the manifest for every scrap of DRM info and hand it all to the
        // script (KIDs, Widevine + PlayReady PSSH, the KID from the init
        // segment). Scripts that do their own extraction ignore what they don't
        // need; simpler scripts get everything without having to parse anything.
        args += drmArgsFromManifest(provider: provider, stream: stream)
        do {
            logStore.record(streamId: stream.id, level: "info", event: "cdm", url: nil, message: "requesting keys via \((scriptPath as NSString).lastPathComponent)")
            let (stdout, _) = try ScriptRunner.runSync(scriptPath: scriptPath, args: args, timeout: 60)
            let pairs = CDMKeyResolver.parseKeyOutput(stdout)
            let joined = joinLines(pairs.map { "\($0.kid):\($0.key)" }.joined(separator: "\n"))
            if joined.isEmpty {
                logStore.record(streamId: stream.id, level: "warn", event: "cdm", url: nil, message: "script returned no usable KID:KEY pairs")
                return entered
            }
            logStore.record(streamId: stream.id, level: "info", event: "cdm", url: nil, message: "acquired \(pairs.count) clear key(s) from script")
            return joined
        } catch {
            logStore.record(streamId: stream.id, level: "error", event: "cdm", url: nil, message: "CDM key fetch failed: \(error)")
            return entered
        }
    }

    /// Proactive session manifest: for script-driven providers whose source URL
    /// is a short-lived per-session URL, ask the script for a fresh one (plus
    /// CDNs / per-category headers / heartbeat cadence) on every start, so
    /// playback never begins from an already-expired URL. Mutates the inout
    /// stream so the caller's writeState persists it. THROWS on failure so the
    /// caller aborts the start — there's no point spawning a worker (or asking
    /// for keys) against a URL we couldn't refresh, and letting it fail later
    /// just makes the supervisor restart-loop the failing script.
    func refreshSessionManifest(provider: Provider, stream: inout StreamConfig) throws {
        guard stream.sessionManifest, scriptAllows(.manifest, provider: provider, stream: stream) else { return }
        let scriptPath = effectiveScriptPath(provider: provider, stream: stream).trimmingCharacters(in: .whitespaces)
        guard !scriptPath.isEmpty, ScriptRunner.scriptExists(ScriptRunner.normalizePath(scriptPath)) else {
            throw PanelError.server("Session manifest is on, but no readable script is configured for this stream.")
        }
        do {
            let result = try runManifestScript(provider: provider, stream: stream, account: activeScriptAccount(for: provider))
            stream.url = result.manifestUrl
            stream.cdnUrls = result.cdnUrls
            if !result.manifestHeaders.isEmpty { stream.manifestHeaders = result.manifestHeaders }
            if !result.mediaHeaders.isEmpty { stream.mediaHeaders = result.mediaHeaders }
            if result.heartbeatSeconds > 0 { stream.heartbeatSeconds = result.heartbeatSeconds }
            logStore.record(streamId: stream.id, level: "info", event: "scriptManifest", url: result.manifestUrl, message: "session manifest: fresh source (\(result.cdnUrls.count) CDN mirror(s)\(result.heartbeatSeconds > 0 ? ", heartbeat \(result.heartbeatSeconds)s" : ""))")
        } catch {
            logStore.record(streamId: stream.id, level: "error", event: "scriptManifest", url: nil, message: "session manifest fetch failed: \(error)")
            throw error
        }
    }

    /// Parse the stream's manifest for every DRM detail and return them as
    /// script args: all KIDs (`kid=`), the first + all PSSH boxes (`pssh=`,
    /// `psshAll=`), the Widevine/PlayReady boxes split out (`psshWidevine=`,
    /// `psshPlayReady=`), and HLS key URIs (`keyUri=`). We forward whatever we
    /// find; the script decides what it needs. Best-effort — [] on any failure.
    func drmArgsFromManifest(provider: Provider, stream: StreamConfig) -> [String] {
        guard let url = URL(string: stream.url), url.scheme != nil else { return [] }
        let proxy = manifestProxy(provider: provider, stream: stream)
        let headers = effectiveHeaders(provider: provider, stream: stream, category: .manifest)
        guard let data = try? HTTPClient(options: HTTPRequestOptions(timeout: 12, headers: headers, proxy: proxy.isEmpty ? nil : proxy)).get(url),
              let text = String(data: data, encoding: .utf8) else { return [] }
        let challenge = stream.kind == "m3u8" ? CDMKeyResolver.challengeFromHLS(text) : CDMKeyResolver.challengeFromMPD(text)
        var args: [String] = []
        if !challenge.kids.isEmpty { args.append(ScriptRunner.arg("kid", challenge.kids.joined(separator: ","))) }
        // `pssh` hook: the script gets first refusal on the box we extracted and
        // may hand back a different one to license against.
        if let first = challenge.psshAll.first {
            let processed = scriptProcessedPssh(first, url: url.absoluteString, provider: provider, stream: stream) ?? first
            args.append(ScriptRunner.arg("pssh", processed))
        }
        if !challenge.psshAll.isEmpty { args.append(ScriptRunner.arg("psshAll", challenge.psshAll.joined(separator: ","))) }
        if !challenge.psshWidevine.isEmpty { args.append(ScriptRunner.arg("psshWidevine", challenge.psshWidevine)) }
        if !challenge.psshPlayReady.isEmpty { args.append(ScriptRunner.arg("psshPlayReady", challenge.psshPlayReady)) }
        if !challenge.keyURIs.isEmpty { args.append(ScriptRunner.arg("keyUri", challenge.keyURIs.joined(separator: ","))) }
        if !challenge.isEmpty {
            logStore.record(streamId: stream.id, level: "info", event: "cdm", url: nil, message: "parsed DRM from manifest — KID(s) [\(challenge.kids.joined(separator: ", "))], \(challenge.psshAll.count) PSSH box(es)")
        }
        // `initparse` hook: some DRM details only live in the init segment, and
        // a script may recognise ones this parser doesn't.
        if scriptAllows(.initparse, provider: provider, stream: stream),
           let (initURL, initData) = firstInitSegment(manifest: text, manifestURL: url, headers: headers, proxy: proxy) {
            args += scriptParsedInit(initData, url: initURL, provider: provider, stream: stream)
        }
        return args
    }

    /// Fetches the first video init segment a manifest points at, the same way
    /// the stream editor's auto-detect already does (DashSegmentsCLI.probe) —
    /// via the MPD's initialization template for DASH, or `#EXT-X-MAP` for HLS.
    /// Best-effort: nil whenever the manifest has no init to speak of.
    func firstInitSegment(manifest text: String, manifestURL url: URL,
                          headers: [(String, String)], proxy: String) -> (url: String, data: Data)? {
        let client = HTTPClient(options: HTTPRequestOptions(timeout: 12, headers: headers, proxy: proxy.isEmpty ? nil : proxy))
        if text.contains("#EXTM3U") {
            // HLS: the init is whatever #EXT-X-MAP names, resolved against the
            // playlist it appeared in.
            for line in text.split(separator: "\n") where line.hasPrefix("#EXT-X-MAP") {
                guard let range = line.range(of: #"URI="([^"]+)""#, options: .regularExpression) else { continue }
                let uri = String(line[range]).dropFirst(5).dropLast()
                guard let initURL = URL(string: String(uri), relativeTo: url)?.absoluteURL,
                      let data = try? client.get(initURL) else { continue }
                return (initURL.absoluteString, data)
            }
            return nil
        }
        guard let mpd = try? MPDParser(sourceURL: url).parse(data: Data(text.utf8)) else { return nil }
        for period in mpd.periods {
            for adaptation in period.adaptationSets where DashSegmentsCLI.adaptationType(adaptation) == "video" {
                for representation in adaptation.representations {
                    var options = Options(mode: .list)
                    options.mpdURL = url
                    options.count = 1
                    options.includeInitialization = true
                    options.headers = headers
                    options.proxy = proxy.isEmpty ? nil : proxy
                    options.representationID = representation.id
                    guard let segments = try? DashSegmentsCLI.expandSegments(mpd: mpd, options: options),
                          let initSegment = segments.first(where: { $0.isInitialization }),
                          let data = try? client.get(initSegment.url) else { continue }
                    return (initSegment.url.absoluteString, data)
                }
            }
        }
        return nil
    }

    func start(provider: Provider, stream: inout StreamConfig) throws {
        // Clear old logs up front (not just before a worker spawns) so the
        // session-manifest / cdm / heartbeat lines logged just below survive
        // into this run rather than being wiped by a later clear.
        // Session manifest first: refresh the source URL (+ CDNs/headers/
        // heartbeat) from the script before anything reads stream.url, so every
        // pipeline — passthrough, internal workers, ffmpeg — starts from a live
        // session URL rather than a stale one.
        try refreshSessionManifest(provider: provider, stream: &stream)
        startHeartbeat(provider: provider, stream: stream)
        // Script lifecycle hooks (best-effort, async): `init` lets the script do
        // one-time session setup, `start` marks the stream going live (e.g. to
        // claim a concurrent-stream slot). No-ops for non-script streams.
        runScriptAction("init", provider: provider, stream: stream)
        runScriptAction("start", provider: provider, stream: stream)

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

        if stream.inputMode == "nm3u8dlre" {
            try startNM3U8DLREResident(provider: provider, stream: &stream)
            return
        }

        if stream.inputMode != "internal" {
            try startFfmpegResident(provider: provider, stream: &stream)
            return
        }

        guard FileManager.default.isExecutableFile(atPath: selfBinary.path) else {
            throw PanelError.server("Could not locate the restreamair binary at \(selfBinary.path) to spawn the live worker.")
        }

        let representationIds = effectiveRepresentationIds(for: stream)
        let multi = representationIds.count > 1
        // Split proxy: the worker's MPD fetch vs its segment/init fetches can use
        // the proxy independently (per the stream's proxy scope).
        let proxy = manifestProxy(provider: provider, stream: stream)
        let mediaProxyValue = mediaProxy(provider: provider, stream: stream)
        let manifestHeaders = swiftHeader(headerText(effectiveHeaders(provider: provider, stream: stream, category: .manifest)))
        let mediaHeaders = swiftHeader(headerText(effectiveHeaders(provider: provider, stream: stream, category: .media)))
        // Keys entered by hand win; otherwise, if CDM auto-keys is on, ask the
        // provider script to acquire clear keys from the manifest's DRM info.
        let keys = resolveDecryptionKeys(provider: provider, stream: stream)
        // `inheritUrlParams` can't be resolved here: some CDNs only attach the
        // auth query string on the redirect target, not the configured source
        // URL (effectiveSegmentUrlParams would read the latter and come back
        // empty). The worker follows the redirect itself, so it re-derives this
        // from whatever URL actually answers each poll — see LiveMPDToM3U8.
        let segmentParams = provider.inheritUrlParams ? "" : effectiveSegmentUrlParams(provider: provider, stream: stream).trimmingCharacters(in: .whitespaces)

        // Key rotation: give the worker the CDM script + args so it can
        // re-acquire a key on its own when a segment's KID changes mid-stream
        // (kid=<hex> is appended per fetch). Same call as resolveDecryptionKeys,
        // minus the one-shot manifest scrape.
        var cdmScriptArg = ""
        var cdmFetchArgs = ""
        if stream.useCdm, scriptAllows(.cdm, provider: provider, stream: stream) {
            let scriptPath = effectiveScriptPath(provider: provider, stream: stream).trimmingCharacters(in: .whitespaces)
            if !scriptPath.isEmpty, ScriptRunner.scriptExists(ScriptRunner.normalizePath(scriptPath)) {
                cdmScriptArg = scriptPath
                var a = ScriptRunner.commonArgs(action: "cdm", bind: provider.scriptBind, proxy: scriptProxy(provider: provider, stream: stream), doh: provider.scriptDoh, worker: provider.scriptWorker, account: activeScriptAccount(for: provider), sessionDir: scriptSessionDir(providerId: provider.id))
                a.append("cdm=\(stream.cdmMode.isEmpty ? "external" : stream.cdmMode)")
                if !stream.cdmType.isEmpty { a.append("cdmType=\(stream.cdmType)") }
                a += ScriptRunner.splitParams(stream.scriptParams)
                cdmFetchArgs = a.joined(separator: "\n")
            }
        }

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
            if !mediaProxyValue.isEmpty { args.append("mediaProxy=\(mediaProxyValue)") }
            if stream.forceOffline { args.append("forceOffline=true") }
            if !manifestHeaders.isEmpty { args.append("manifestHeader=\(manifestHeaders)") }
            if !mediaHeaders.isEmpty { args.append("header=\(mediaHeaders)") }
            if !keys.isEmpty { args.append("decryptionKeys=\(keys)") }
            if !cdmScriptArg.isEmpty {
                args.append("cdmScript=\(cdmScriptArg)")
                args.append("cdmFetchArgs=\(cdmFetchArgs)")
            }
            if !segmentParams.isEmpty { args.append("segmentUrlParams=\(segmentParams)") }
            if provider.inheritUrlParams { args.append("inheritUrlParams=true") }
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
                self?.persistWorkerPids()
            }
            try process.run()
            spawned.append(job)
        }

        lock.lock(); jobs[stream.id] = spawned; lock.unlock()
        persistWorkerPids()
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
            // Resolved keys: hand-entered OR CDM auto-keys (firstClearKey splits
            // on '|', '\n' and '\r', so either separator is fine).
            decryptionKeys: resolveDecryptionKeys(provider: provider, stream: stream),
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
            self?.persistWorkerPids()
        }
        try process.run()

        lock.lock(); jobs[stream.id] = [job]; lock.unlock()
        persistWorkerPids()
        stream.status = "running"
        stream.lastError = nil
    }

    /// Starts N_m3u8DL-RE as a download-mixer for a DASH/HLS source. It runs
    /// N_m3u8DL-RE with --live-real-time-merge which continuously downloads
    /// segments and muxes them into an HLS playlist on disk under runtimeDir/<id>/.
    /// The server's /play route is then rewritten to serve that on-disk playlist
    /// directly — i.e. it acts as a drop-in mixer just like the internal remuxer
    /// but delegates all the DASH→HLS conversion to N_m3u8DL-RE.
    func startNM3U8DLREResident(provider: Provider, stream: inout StreamConfig) throws {
        guard let binaryPath = NM3U8DLRELocator.resolve() else {
            throw PanelError.server("N_m3u8DL-RE isn't installed. Install it via the stream editor's Install button, or switch the stream back to the Internal remuxer.")
        }
        // Work dir: a fresh subdirectory per stream under runtimeDir.
        // N_m3u8DL-RE writes the output playlist + segments here.
        let workDir = runtimeDir.appendingPathComponent(stream.id, isDirectory: true)
        try? FileManager.default.removeItem(at: workDir)
        try FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)

        let sources = [stream.url] + stream.cdnUrls
        lock.lock(); let rotation = cdnRotation[stream.id, default: 0]; cdnRotation[stream.id] = rotation + 1; lock.unlock()
        let sourceURL = sources[rotation % sources.count]
        if sources.count > 1 {
            logStore.recordWorkerLine(#"{"event":"cdnFailover","status":"info","message":\#(jsonString("N_m3u8DL-RE source: " + sourceURL))}"#, streamId: stream.id)
        }

        // Core flags:
        //   --live-real-time-merge  — process live DASH/HLS at real-time speed
        //   --save-dir              — write playlist + segments here
        //   --save-name             — output basename (we use the stream id)
        //   --mux-after-done hls    — produce an HLS .m3u8 output file
        var arguments = [
            sourceURL,
            "--live-real-time-merge",
            "--save-dir", workDir.path,
            "--save-name", stream.id,
            "--mux-after-done", "hls"
        ]

        // Pass manifest + media headers (N_m3u8DL-RE accepts -H "Name: value")
        let manifestHeaders = effectiveHeaders(provider: provider, stream: stream, category: .manifest)
        for (k, v) in manifestHeaders {
            arguments += ["-H", "\(k): \(v)"]
        }

        // Proxy
        let proxy = effectiveProxy(provider: provider, stream: stream)
        if !proxy.isEmpty {
            arguments += ["--custom-proxy", proxy]
        }

        // CENC decryption keys (KID:KEY format → --key KID:KEY per key). Use the
        // resolved keys so CDM auto-keys (useCdm) reach N_m3u8DL-RE too, not just
        // hand-entered ones. resolveDecryptionKeys joins with '|'; the editor
        // field uses newlines — split on both.
        let keys = resolveDecryptionKeys(provider: provider, stream: stream)
        for key in keys.split(whereSeparator: { $0 == "|" || $0 == "\n" || $0 == "\r" }) {
            let trimmed = key.trimmingCharacters(in: .whitespaces)
            guard !trimmed.isEmpty else { continue }
            arguments += ["--key", trimmed]
        }

        // Any extra user-supplied CLI params (free-form key=value or flags)
        let extraParams = ScriptRunner.splitParams(stream.nm3u8dlreParams)
        arguments.append(contentsOf: extraParams)

        let process = Process()
        process.executableURL = URL(fileURLWithPath: binaryPath)
        process.arguments = arguments
        process.currentDirectoryURL = workDir
        logStore.recordWorkerLine(#"{"event":"nm3u8dlreStart","status":"start","message":\#(jsonString("N_m3u8DL-RE " + arguments.joined(separator: " ")))}"#, streamId: stream.id)

        let stderrPipe = Pipe()
        process.standardOutput = Pipe() // suppress direct stdout — output goes to save-dir files
        process.standardError = stderrPipe

        let job = RestreamJob(process: process, representationId: "")
        let streamId = stream.id

        stderrPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            job.stderr.append(data)
            if let text = String(data: data, encoding: .utf8) {
                for line in text.split(whereSeparator: { $0 == "\n" || $0 == "\r" }) {
                    let trimmed = line.trimmingCharacters(in: .whitespaces)
                    guard !trimmed.isEmpty else { continue }
                    self?.logStore.recordWorkerLine(#"{"event":"nm3u8dlre","status":"info","message":\#(self?.jsonString(trimmed) ?? "\"\"")}"#, streamId: streamId)
                }
            }
        }

        process.terminationHandler = { [weak self] proc in
            stderrPipe.fileHandleForReading.readabilityHandler = nil
            try? stderrPipe.fileHandleForReading.close()
            self?.logStore.recordWorkerLine(#"{"event":"nm3u8dlreExit","status":"\#(proc.terminationStatus == 0 ? "success" : "error")","message":"exit \#(proc.terminationStatus)"}"#, streamId: streamId)
            self?.lock.lock()
            self?.jobs[streamId]?.removeAll { $0.process === job.process }
            self?.lock.unlock()
            self?.persistWorkerPids()
        }
        try process.run()

        lock.lock(); jobs[stream.id] = [job]; lock.unlock()
        persistWorkerPids()
        stream.status = "running"
        stream.lastError = nil
    }


    /// (Re)start the per-stream heartbeat loop if the stream asks for one. Fires
    /// the provider script's `heartbeat` action every `heartbeatSeconds` (from
    /// the manifest's Heartbeat block or the editor) so providers that expire a
    /// session without periodic pings stay alive. No-op when heartbeat is off.
    /// A stream that runs through its provider script (session manifest / CDM
    /// keys / heartbeat / params / override). The start/stop/init lifecycle
    /// hooks only fire for these, so plain streams that happen to sit under a
    /// script provider don't invoke it.
    func isScriptDriven(_ stream: StreamConfig) -> Bool {
        stream.sessionManifest || stream.useCdm
            || !stream.scriptParams.trimmingCharacters(in: .whitespaces).isEmpty
            || !stream.scriptOverride.trimmingCharacters(in: .whitespaces).isEmpty
            || (stream.heartbeatEnabled && stream.heartbeatSeconds > 0)
    }

    /// The action set in effect for a stream: its own override when it has one,
    /// otherwise its provider's.
    func effectiveScriptActions(provider: Provider, stream: StreamConfig) -> Set<String> {
        Set(stream.scriptActionsOverride ?? provider.scriptActions)
    }

    /// Whether `action` may run for this stream. Everything that spawns a
    /// provider script goes through here, so an action a provider hasn't
    /// declared is never invoked — and an action ReStreamAir doesn't call yet
    /// (`isWired == false`) is never invoked either, whatever the config says.
    func scriptAllows(_ action: ScriptAction, provider: Provider, stream: StreamConfig) -> Bool {
        guard action.isWired else { return false }
        return effectiveScriptActions(provider: provider, stream: stream).contains(action.rawValue)
    }

    /// Provider-level actions (login/pair/channels/events/epg) have no stream
    /// to inherit from, so they consult the provider's own set directly.
    func scriptAllows(_ action: ScriptAction, provider: Provider) -> Bool {
        guard action.isWired else { return false }
        return provider.scriptActions.contains(action.rawValue)
    }

    /// The durable per-provider directory a script keeps its session and
    /// cookies in, created on demand. Passed as `sessiondir=`/`cookies=` on
    /// every invocation; ReStreamAir never reads what ends up in it. Lives
    /// under runtime/, which is already gitignored and never served statically.
    func scriptSessionDir(providerId: String, create: Bool = true) -> String {
        let path = runtimeDir.appendingPathComponent("scripts").appendingPathComponent(providerId)
        if create {
            try? FileManager.default.createDirectory(at: path, withIntermediateDirectories: true)
        }
        return path.path
    }

    /// Fire a one-shot provider-script lifecycle action (`init` / `start` /
    /// `stop`) for a script-driven stream. Best-effort and async: it runs on a
    /// background queue with a short timeout, logs the result under
    /// `script:<action>`, and never blocks or fails the caller. Scripts that
    /// don't implement the action just log a benign "invalid action" line.
    func runScriptAction(_ action: String, provider: Provider, stream: StreamConfig) {
        // The provider (or stream) has to have declared this action. Replaces
        // the old isScriptDriven heuristic, which fired these at any
        // script-shaped stream whether or not its script implemented them.
        guard let known = ScriptAction(rawValue: action),
              scriptAllows(known, provider: provider, stream: stream) else { return }
        let scriptPath = effectiveScriptPath(provider: provider, stream: stream).trimmingCharacters(in: .whitespaces)
        guard !scriptPath.isEmpty, ScriptRunner.scriptExists(ScriptRunner.normalizePath(scriptPath)) else { return }
        var args = ScriptRunner.commonArgs(action: action, bind: provider.scriptBind, proxy: scriptProxy(provider: provider, stream: stream), doh: provider.scriptDoh, worker: provider.scriptWorker, account: activeScriptAccount(for: provider), sessionDir: scriptSessionDir(providerId: provider.id))
        args += ScriptRunner.splitParams(stream.scriptParams)
        let streamId = stream.id
        DispatchQueue.global(qos: .utility).async { [weak self] in
            do {
                let (stdout, _) = try ScriptRunner.runSync(scriptPath: scriptPath, args: args, timeout: 20)
                let trimmed = stdout.trimmingCharacters(in: .whitespacesAndNewlines)
                self?.logStore.record(streamId: streamId, level: "info", event: "script:\(action)", url: nil, message: trimmed.isEmpty ? "ok" : String(trimmed.prefix(200)))
            } catch {
                self?.logStore.record(streamId: streamId, level: "warn", event: "script:\(action)", url: nil, message: "\(error)")
            }
        }
    }

    func startHeartbeat(provider: Provider, stream: StreamConfig) {
        stopHeartbeat(streamId: stream.id)
        guard stream.heartbeatSeconds > 0, scriptAllows(.heartbeat, provider: provider, stream: stream) else { return }
        let scriptPath = effectiveScriptPath(provider: provider, stream: stream).trimmingCharacters(in: .whitespaces)
        guard !scriptPath.isEmpty, ScriptRunner.scriptExists(ScriptRunner.normalizePath(scriptPath)) else { return }
        let interval = max(5, stream.heartbeatSeconds)
        let account = activeScriptAccount(for: provider)
        var args = ScriptRunner.commonArgs(action: "heartbeat", bind: provider.scriptBind, proxy: scriptProxy(provider: provider, stream: stream), doh: provider.scriptDoh, worker: provider.scriptWorker, account: account, sessionDir: scriptSessionDir(providerId: provider.id))
        args += ScriptRunner.splitParams(stream.scriptParams)
        let streamId = stream.id
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
        timer.schedule(deadline: .now() + .seconds(interval), repeating: .seconds(interval))
        timer.setEventHandler { [weak self] in
            do {
                _ = try ScriptRunner.runSync(scriptPath: scriptPath, args: args, timeout: 20)
                self?.logStore.record(streamId: streamId, level: "info", event: "heartbeat", url: nil, message: "ok")
            } catch {
                self?.logStore.record(streamId: streamId, level: "warn", event: "heartbeat", url: nil, message: "\(error)")
            }
        }
        lock.lock(); heartbeatTimers[streamId] = timer; lock.unlock()
        timer.resume()
        logStore.record(streamId: streamId, level: "info", event: "heartbeat", url: nil, message: "started (every \(interval)s)")
    }

    func stopHeartbeat(streamId: String) {
        lock.lock(); let timer = heartbeatTimers.removeValue(forKey: streamId); lock.unlock()
        timer?.cancel()
    }

    func stop(stream: StreamConfig) {
        // Script `stop` hook (best-effort) before we tear anything down, so a
        // script can end its session / release a slot. Resolve the provider from
        // state since callers only hand us the stream.
        if let state = try? readState(), let location = findStream(stream.id, in: state) {
            runScriptAction("stop", provider: state.providers[location.provider], stream: stream)
        }
        stopHeartbeat(streamId: stream.id)
        lock.lock()
        let jobsForStream = jobs.removeValue(forKey: stream.id) ?? []
        lock.unlock()
        for job in jobsForStream {
            let pid = job.process.processIdentifier
            job.process.terminate() // SIGTERM — the clean path for a well-behaved worker.
            // Worker subprocesses inherit the panel's blocked-signal mask
            // (libdispatch blocks most signals on Linux, SIGTERM among them), so
            // the SIGTERM from terminate() is never delivered: the worker keeps
            // running — orphaned, still fetching the manifest and writing log
            // lines long after the stream is marked "stopped" (the "logs still
            // ongoing after stop" bug). SIGKILL can't be blocked or caught, so
            // escalate to guarantee the worker actually dies. The worker is
            // stateless (it only writes segment files + an atomically-replaced
            // playlist), so there's nothing to flush.
            #if !os(Windows)
            if pid > 0 { kill(pid, SIGKILL) }
            #endif
        }
        persistWorkerPids()
    }

    // MARK: - Worker PID bookkeeping (restart hygiene)

    /// PIDs of every live worker/ffmpeg child, persisted so a *new* panel
    /// process (after a crash and supervisor restart) can clean up workers
    /// the previous process left behind. Without this, every restart doubles
    /// the downloader/ffmpeg population — the old ones keep fetching forever
    /// with nobody supervising them.
    var workerPidsFile: URL { runtimeDir.appendingPathComponent("worker-pids.json") }

    func persistWorkerPids() {
        lock.lock()
        let pids = jobs.values.flatMap { $0 }.map { Int($0.process.processIdentifier) }.filter { $0 > 0 }
        lock.unlock()
        let data = (try? JSONSerialization.data(withJSONObject: pids)) ?? Data("[]".utf8)
        try? data.write(to: workerPidsFile, options: [.atomic])
    }

    func reapOrphanWorkers() {
        guard let data = try? Data(contentsOf: workerPidsFile),
              let pids = (try? JSONSerialization.jsonObject(with: data)) as? [Int] else { return }
        for pidValue in pids {
            let pid = pid_t(pidValue)
            guard pid > 1, pid != ProcessInfo.processInfo.processIdentifier else { continue }
            // PIDs get reused by the OS — only kill if the process actually
            // looks like one of ours, never blindly.
            guard commandLooksLikeWorker(pid) else { continue }
            #if !os(Windows)
            kill(pid, SIGTERM)
            // Workers inherit a blocked SIGTERM mask (see stop()) — escalate.
            kill(pid, SIGKILL)
            #endif
        }
        try? FileManager.default.removeItem(at: workerPidsFile)
    }

    private func commandLooksLikeWorker(_ pid: pid_t) -> Bool {
        #if os(Linux)
        guard let raw = try? Data(contentsOf: URL(fileURLWithPath: "/proc/\(pid)/cmdline")) else { return false }
        let command = String(decoding: raw, as: UTF8.self).replacingOccurrences(of: "\0", with: " ")
        #elseif os(Windows)
        let command = ""
        #else
        let ps = Process()
        ps.executableURL = URL(fileURLWithPath: "/bin/ps")
        ps.arguments = ["-o", "command=", "-p", "\(pid)"]
        let pipe = Pipe()
        ps.standardOutput = pipe
        guard (try? ps.run()) != nil else { return false }
        ps.waitUntilExit()
        let command = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        #endif
        return command.contains(selfBinary.lastPathComponent) || command.contains("ffmpeg")
    }

    // MARK: - Script manifest refresh (source recovery)
    //
    // The README's script protocol defines an `action=manifest` call that
    // returns a fresh ManifestUrl (+ CDN mirrors + headers) for a stream.
    // This wires it up as the last-resort recovery step: when a stream's
    // manifest fails on its primary URL *and* every CDN mirror, and its
    // provider has a script configured, ask the script for a new manifest,
    // persist it, and retry — instead of failing until someone re-imports
    // the channel by hand.

    struct ScriptManifestResult {
        var manifestUrl: String
        var cdnUrls: [String]
        var manifestHeaders: String
        var mediaHeaders: String
        /// From the script's Heartbeat block — how often to ping to keep the
        /// session alive (0 = none). Url/Params are informational for now; the
        /// heartbeat action re-runs the script with the stream's params.
        var heartbeatSeconds: Int = 0
    }

    /// The script that owns this stream's manifest/cdm/heartbeat actions: the
    /// per-stream override if set, otherwise the provider's script.
    func effectiveScriptPath(provider: Provider, stream: StreamConfig) -> String {
        let override = stream.scriptOverride.trimmingCharacters(in: .whitespaces)
        return override.isEmpty ? provider.scriptPath : override
    }

    /// Non-mutating active-account pick for start-time script calls (no rotation
    /// side effects — that's resolveScriptAccount's job on the request path).
    func activeScriptAccount(for provider: Provider) -> ScriptAccount {
        let enabled = provider.scriptAccounts.filter(\.enabled)
        return enabled.first(where: { $0.id == provider.activeScriptAccountId })
            ?? enabled.first
            ?? provider.scriptAccounts.first
            ?? ScriptAccount(id: "", name: "", username: "", password: "", params: "", enabled: true)
    }

    func canScriptRefresh(provider: Provider) -> Bool {
        let path = provider.scriptPath.trimmingCharacters(in: .whitespaces)
        return !path.isEmpty && ScriptRunner.scriptExists(ScriptRunner.normalizePath(path))
    }

    /// Throttle + single-flight gate: one attempt per stream per cooldown,
    /// never two at once, no matter how many players/timers hit the failure.
    private func reserveScriptRefresh(streamId: String) -> Bool {
        lock.lock(); defer { lock.unlock() }
        if scriptRefreshInFlight.contains(streamId) { return false }
        if let last = scriptRefreshLastAttempt[streamId],
           Date().timeIntervalSince(last) < PanelServer.scriptRefreshCooldown { return false }
        scriptRefreshLastAttempt[streamId] = Date()
        scriptRefreshInFlight.insert(streamId)
        return true
    }

    private func releaseScriptRefresh(streamId: String) {
        lock.lock(); scriptRefreshInFlight.remove(streamId); lock.unlock()
    }

    /// Runs `action=manifest` (per the script protocol) and parses its JSON
    /// reply. Blocking — never call while holding stateQueue.
    func runManifestScript(provider: Provider, stream: StreamConfig, account: ScriptAccount) throws -> ScriptManifestResult {
        var args = ScriptRunner.commonArgs(action: "manifest", bind: provider.scriptBind, proxy: scriptProxy(provider: provider, stream: stream), doh: provider.scriptDoh, worker: provider.scriptWorker, account: account, sessionDir: scriptSessionDir(providerId: provider.id))
        args += ScriptRunner.splitParams(stream.scriptParams)
        let (stdout, _) = try ScriptRunner.runSync(scriptPath: effectiveScriptPath(provider: provider, stream: stream), args: args, timeout: 45)
        // Tolerate scripts that log a line or two before the JSON blob.
        guard let braceIndex = stdout.firstIndex(of: "{"),
              let object = (try? JSONSerialization.jsonObject(with: Data(stdout[braceIndex...].utf8))) as? [String: Any],
              let manifestUrl = object["ManifestUrl"] as? String,
              URL(string: manifestUrl)?.scheme?.hasPrefix("http") == true else {
            throw PanelError.server("manifest script returned no usable ManifestUrl")
        }
        let cdns = (object["Cdn"] as? [[String: Any]] ?? [])
            .compactMap { $0["ManifestUrl"] as? String }
            .filter { URL(string: $0)?.scheme?.hasPrefix("http") == true && $0 != manifestUrl }
        func headerText(_ value: Any?) -> String {
            ((value as? [String: Any]) ?? [:])
                .compactMap { key, val in (val as? String).map { "\(key): \($0)" } }
                .sorted()
                .joined(separator: "\n")
        }
        // Headers block keys are case-insensitive across scripts ("Manifest"
        // vs "manifest") — accept either.
        let headers = object["Headers"] as? [String: Any]
        func headerFor(_ name: String) -> String {
            headerText(headers?[name] ?? headers?[name.capitalized] ?? headers?[name.lowercased()])
        }
        // Heartbeat.PeriodMs (ms) -> seconds; a script can drive the ping cadence.
        var heartbeatSeconds = 0
        if let hb = object["Heartbeat"] as? [String: Any] {
            if let ms = (hb["PeriodMs"] as? Int) ?? (hb["PeriodMs"] as? Double).map({ Int($0) }), ms > 0 {
                heartbeatSeconds = max(1, ms / 1000)
            }
        }
        return ScriptManifestResult(
            manifestUrl: manifestUrl,
            cdnUrls: cdns,
            manifestHeaders: headerFor("manifest"),
            mediaHeaders: headerFor("media"),
            heartbeatSeconds: heartbeatSeconds
        )
    }

    /// Persists a refresh so every pipeline — m3u8 passthrough, internal
    /// workers, resident ffmpeg — picks the new source up on its next
    /// fetch or supervisor restart.
    func applyManifestRefresh(_ result: ScriptManifestResult, streamId: String) {
        stateQueue.sync {
            guard var state = try? readState(), let location = findStream(streamId, in: state) else { return }
            state.providers[location.provider].streams[location.stream].url = result.manifestUrl
            state.providers[location.provider].streams[location.stream].cdnUrls = result.cdnUrls
            if !result.manifestHeaders.isEmpty { state.providers[location.provider].streams[location.stream].manifestHeaders = result.manifestHeaders }
            if !result.mediaHeaders.isEmpty { state.providers[location.provider].streams[location.stream].mediaHeaders = result.mediaHeaders }
            try? writeState(state)
        }
        lock.lock(); cdnRotation[streamId] = 0; lock.unlock()
    }

    /// Blocking refresh for request paths (runs the script inline, outside
    /// stateQueue). Returns the refreshed provider+stream on success, nil when
    /// refresh isn't configured, is throttled, or the script failed.
    func refreshManifestViaScript(providerId: String, streamId: String, trigger: String) -> (Provider, StreamConfig)? {
        guard reserveScriptRefresh(streamId: streamId) else { return nil }
        defer { releaseScriptRefresh(streamId: streamId) }
        // Snapshot provider/stream and resolve the account under stateQueue
        // (account rotation writes state), then run the script lock-free.
        var snapshot: (Provider, StreamConfig, ScriptAccount)?
        stateQueue.sync {
            guard var state = try? readState(),
                  let providerIndex = state.providers.firstIndex(where: { $0.id == providerId }),
                  let location = findStream(streamId, in: state) else { return }
            let provider = state.providers[providerIndex]
            guard canScriptRefresh(provider: provider) else { return }
            guard let account = (try? resolveScriptAccount(providerIndex: providerIndex, state: &state)) ?? nil else { return }
            snapshot = (provider, state.providers[location.provider].streams[location.stream], account)
        }
        guard let (provider, stream, account) = snapshot else { return nil }
        logStore.record(streamId: streamId, level: "info", event: "scriptManifest", url: stream.url, message: "source failing (\(trigger)) — asking \((provider.scriptPath as NSString).lastPathComponent) for a fresh manifest")
        do {
            let result = try runManifestScript(provider: provider, stream: stream, account: account)
            applyManifestRefresh(result, streamId: streamId)
            logStore.record(streamId: streamId, level: "info", event: "scriptManifest", url: result.manifestUrl, message: "manifest refreshed (\(result.cdnUrls.count) CDN mirrors)")
            var refreshed: (Provider, StreamConfig)?
            stateQueue.sync {
                guard let state = try? readState(), let location = findStream(streamId, in: state) else { return }
                refreshed = (state.providers[location.provider], state.providers[location.provider].streams[location.stream])
            }
            return refreshed
        } catch {
            logStore.record(streamId: streamId, level: "error", event: "scriptManifest", url: nil, message: "manifest refresh failed: \(error)")
            return nil
        }
    }

    /// Non-blocking variant for the worker supervisor, which runs *on* the
    /// state queue: fire the refresh in the background; the next supervision
    /// tick restarts the worker against the refreshed URL.
    func scheduleManifestRefresh(providerId: String, streamId: String, trigger: String) {
        DispatchQueue.global(qos: .utility).async { [weak self] in
            _ = self?.refreshManifestViaScript(providerId: providerId, streamId: streamId, trigger: trigger)
        }
    }

    /// Manifest fetch that walks the primary URL and then every CDN mirror in
    /// order before giving up — the m3u8 passthrough previously only ever
    /// tried the primary, so configured mirrors never helped it.
    func fetchManifestWithFailover(provider: Provider, stream: StreamConfig, primary: URL) throws -> Data {
        var candidates = [primary]
        for mirror in stream.cdnUrls {
            if let url = URL(string: mirror), url != primary { candidates.append(url) }
        }
        var lastError: Error = PanelError.server("Stream has no source URL.")
        for (index, url) in candidates.enumerated() {
            do {
                let data = try fetchRemote(provider: provider, stream: stream, url: url, category: .manifest)
                let mirrorNote = index > 0 ? " · CDN mirror #\(index)" : ""
                logStore.record(streamId: stream.id, level: "info", event: "fetchManifest", url: url.absoluteString, message: "success · \(data.count) bytes\(mirrorNote)")
                return data
            } catch {
                logStore.record(streamId: stream.id, level: "error", event: "fetchManifest", url: url.absoluteString, message: "\(error)")
                lastError = error
            }
        }
        throw lastError
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

                    // Two failed restarts means the worker has already been
                    // through its own retries and (for ffmpeg) the CDN
                    // rotation — the source itself has likely expired. Ask
                    // the provider script for a fresh manifest in the
                    // background (throttled + single-flight); the next
                    // restart picks up whatever it persists.
                    if streak >= 2, canScriptRefresh(provider: provider) {
                        scheduleManifestRefresh(providerId: provider.id, streamId: streamSnapshot.id, trigger: "worker restart streak \(streak)")
                    }

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
        var request = request
        // The copyable play URL uses a friendly name-slug; resolve it to the real
        // id here so every branch below (and the relative segment requests that
        // follow) work id-based exactly as before. No-op when it's already an id.
        if let segment = match(request.path, #"^/play/([^/]+)/"#)?.first, providerAndStream(segment, in: try readState()) == nil,
           let realId = resolvePlaySegment(segment, in: try readState()) {
            request.path = request.path.replacingOccurrences(of: "/play/\(segment)/", with: "/play/\(realId)/")
        }
        // Direct-source streams answer every playlist/manifest request with a
        // 302 to the origin — the player talks to the source directly and the
        // panel never proxies a byte. Segment/runtime paths deliberately fall
        // through: a redirected player should never request them anyway.
        if let id = match(request.path, #"^/play/([^/]+)/index\.(?:m3u8|mpd)$"#)?.first {
            let state = try readState()
            if let (_, stream) = providerAndStream(id, in: state), stream.directSource {
                return try sourceRedirect(stream: stream)
            }
        }
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
            } else if let configured = URL(string: stream.url) ?? URL(string: stream.url.trimmingCharacters(in: .whitespacesAndNewlines)) {
                // Never force-unwrap a user-entered URL: a stream saved with a
                // stray space used to trap here and kill the whole panel the
                // moment any player requested the playlist.
                manifestURL = configured
            } else {
                logStore.record(streamId: stream.id, level: "error", event: "fetchManifest", url: stream.url, message: "stream URL is not a valid URL")
                throw PanelError.badRequest("Stream URL is not a valid URL — fix it in the stream editor.")
            }
            let isVariant = request.query["variant"] != nil
            var effectiveStream = stream
            var effectiveManifestURL = manifestURL
            let data: Data
            if isVariant {
                // A variant URL is an absolute link from an already-rewritten
                // master — tied to whichever host served that master, so CDN
                // failover / script refresh don't apply here.
                do {
                    data = try fetchRemote(provider: provider, stream: stream, url: manifestURL, category: .manifest)
                    logStore.record(streamId: stream.id, level: "info", event: "fetchManifest", url: manifestURL.absoluteString, message: "success · \(data.count) bytes")
                } catch {
                    logStore.record(streamId: stream.id, level: "error", event: "fetchManifest", url: manifestURL.absoluteString, message: "\(error)")
                    throw error
                }
            } else {
                do {
                    data = try fetchManifestWithFailover(provider: provider, stream: stream, primary: manifestURL)
                } catch {
                    // Primary + every CDN mirror failed. Last resort: ask the
                    // provider script (when one is configured) for a brand-new
                    // manifest and try the refreshed source once.
                    guard let (freshProvider, freshStream) = refreshManifestViaScript(providerId: provider.id, streamId: stream.id, trigger: "manifest and all CDNs failed"),
                          let freshURL = URL(string: freshStream.url) else { throw error }
                    effectiveStream = freshStream
                    effectiveManifestURL = freshURL
                    data = try fetchManifestWithFailover(provider: freshProvider, stream: freshStream, primary: freshURL)
                }
            }
            let text = String(data: data, encoding: .utf8) ?? ""
            if M3U8Rewriter.isMasterPlaylist(text) {
                let rewritten = rewriteMasterM3U8(text, base: effectiveManifestURL, streamId: stream.id)
                return response(status: 200, body: Data(rewritten.utf8), type: "application/vnd.apple.mpegurl; charset=utf-8", noStore: true)
            }
            let rewritten = rewriteM3U8(text, base: effectiveManifestURL, streamId: stream.id, decrypt: !effectiveStream.hlsKey.isEmpty)
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
        guard stream.status == "running" else { throw PanelError.notFound("Stream is stopped.") }
        let kind = request.query["kind"] ?? "segment"
        let category: HeaderCategory = kind == "key" ? .hlsKey : .media
        let url = kind == "key" ? requestedUrl : appendingQueryParams(requestedUrl, effectiveSegmentUrlParams(provider: provider, stream: stream))
        var data: Data
        do {
            data = try fetchRemote(provider: provider, stream: stream, url: url, category: category)
            logStore.record(streamId: stream.id, level: "info", event: kind == "map" ? "accessInit" : "accessSegment", url: url.absoluteString, message: "Served \(kind) (\(data.count) bytes)")
        } catch {
            logStore.record(streamId: stream.id, level: "error", event: kind == "map" ? "accessInit" : "accessSegment", url: url.absoluteString, message: "\(error)")
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
        if let state = try? readState(), let (_, stream) = providerAndStream(streamId, in: state), stream.status != "running" {
            throw PanelError.notFound("Stream is stopped.")
        }
        let base = runtimeDir.appendingPathComponent(streamId, isDirectory: true).standardizedFileURL
        let url = base.appendingPathComponent(file).standardizedFileURL
        guard url.path.hasPrefix(base.path) else { throw PanelError.badRequest("Invalid path.") }
        guard FileManager.default.fileExists(atPath: url.path) else {
            throw PanelError.notFound("Runtime file is not ready.")
        }
        let data = try Data(contentsOf: url)
        if !file.hasSuffix(".m3u8") && !file.hasSuffix(".mpd") {
            logStore.record(streamId: streamId, level: "info", event: "accessSegment", url: file, message: "Served segment (\(data.count) bytes)")
        }
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
        // `url` and `downloadmanifest` hooks: the script may rewrite the URL
        // before it's fetched, and may do the manifest fetch itself. Both are
        // no-ops unless the provider (or stream) declared the action, so an
        // ordinary stream pays nothing but a Set lookup.
        let target = scriptProcessedURL(url, provider: provider, stream: stream) ?? url
        if category == .manifest, let text = scriptDownloadedManifest(target, provider: provider, stream: stream) {
            return Data(text.utf8)
        }
        let proxy = effectiveProxy(provider: provider, stream: stream)
        let headers = effectiveHeaders(provider: provider, stream: stream, category: category)
        return try HTTPClient(options: HTTPRequestOptions(timeout: 30, headers: headers, proxy: proxy.isEmpty ? nil : proxy)).get(target)
    }

    // MARK: - Script pipeline hooks
    //
    // Each of these runs the provider script for one step of the fetch/decrypt
    // pipeline, and each is inert unless its ScriptAction is enabled. They all
    // fail soft: a script that errors, times out or returns nothing leaves the
    // pipeline exactly as it was, because a broken hook should degrade to the
    // built-in behaviour rather than take the stream down with it.

    /// Runs one pipeline action and returns its trimmed stdout, or nil when the
    /// action is disabled, no script is configured, or the run failed.
    private func runPipelineScript(_ action: ScriptAction, provider: Provider, stream: StreamConfig,
                                   extra: [String], timeout: Double = 20) -> String? {
        guard scriptAllows(action, provider: provider, stream: stream) else { return nil }
        let scriptPath = effectiveScriptPath(provider: provider, stream: stream).trimmingCharacters(in: .whitespaces)
        guard !scriptPath.isEmpty, ScriptRunner.scriptExists(ScriptRunner.normalizePath(scriptPath)) else { return nil }
        var args = ScriptRunner.commonArgs(
            action: action.rawValue, bind: provider.scriptBind,
            proxy: scriptProxy(provider: provider, stream: stream), doh: provider.scriptDoh,
            worker: provider.scriptWorker, account: activeScriptAccount(for: provider),
            sessionDir: scriptSessionDir(providerId: provider.id))
        args += ScriptRunner.splitParams(stream.scriptParams)
        args += extra
        do {
            let (stdout, _) = try ScriptRunner.runSync(scriptPath: scriptPath, args: args, timeout: timeout)
            let trimmed = stdout.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? nil : trimmed
        } catch {
            logStore.record(streamId: stream.id, level: "warn", event: "script:\(action.rawValue)", url: nil, message: "\(error)")
            return nil
        }
    }

    /// `url` — lets the script rewrite a URL (sign it, swap a CDN host, ...)
    /// before ReStreamAir fetches it. Returns nil to leave the URL alone.
    func scriptProcessedURL(_ url: URL, provider: Provider, stream: StreamConfig) -> URL? {
        guard let output = runPipelineScript(.url, provider: provider, stream: stream,
                                             extra: [ScriptRunner.arg("url", url.absoluteString)], timeout: 15)
        else { return nil }
        let candidate = ScriptRunner.decodeValue(scriptJSONField(output, "Url") ?? output)
        guard let rewritten = URL(string: candidate), rewritten.scheme?.hasPrefix("http") == true,
              rewritten != url else { return nil }
        logStore.record(streamId: stream.id, level: "info", event: "script:url", url: rewritten.absoluteString, message: "url rewritten by script")
        return rewritten
    }

    /// `downloadmanifest` — the script fetches the manifest itself and prints
    /// it, for origins that need a request ReStreamAir can't reproduce.
    func scriptDownloadedManifest(_ url: URL, provider: Provider, stream: StreamConfig) -> String? {
        guard let output = runPipelineScript(.downloadmanifest, provider: provider, stream: stream,
                                             extra: [ScriptRunner.arg("url", url.absoluteString)], timeout: 45)
        else { return nil }
        let text = ScriptRunner.decodeValue(scriptJSONField(output, "ManifestContent") ?? output)
        guard !text.isEmpty else { return nil }
        logStore.record(streamId: stream.id, level: "info", event: "script:downloadmanifest", url: url.absoluteString, message: "manifest supplied by script · \(text.utf8.count) bytes")
        return text
    }

    /// `pssh` — hands the PSSH boxes parsed out of the manifest/init to the
    /// script, which may return a replacement to use for the licence exchange.
    func scriptProcessedPssh(_ pssh: String, url: String, provider: Provider, stream: StreamConfig) -> String? {
        guard !pssh.isEmpty,
              let output = runPipelineScript(.pssh, provider: provider, stream: stream,
                                             extra: [ScriptRunner.arg("pssh", pssh), ScriptRunner.arg("url", url)])
        else { return nil }
        let processed = ScriptRunner.decodeValue(scriptJSONField(output, "ProcessedPssh") ?? output)
        guard !processed.isEmpty, processed != pssh else { return nil }
        logStore.record(streamId: stream.id, level: "info", event: "script:pssh", url: nil, message: "pssh rewritten by script")
        return processed
    }

    /// `initparse` — shows the script a fetched init segment so it can report
    /// DRM details ReStreamAir's own parser missed. Returns extra `key=value`
    /// argv tokens (`kid=`, `pssh=`, ...) to fold into the `cdm` call.
    func scriptParsedInit(_ initData: Data, url: String, provider: Provider, stream: StreamConfig) -> [String] {
        guard let output = runPipelineScript(.initparse, provider: provider, stream: stream,
                                             extra: [ScriptRunner.arg("url", url),
                                                     ScriptRunner.arg("init", initData.base64EncodedString())])
        else { return [] }
        var extras: [String] = []
        for key in ["kid", "pssh", "psshWidevine", "psshPlayReady"] {
            if let value = scriptJSONField(output, key.prefix(1).uppercased() + key.dropFirst()) ?? scriptJSONField(output, key) {
                extras.append(ScriptRunner.arg(key, ScriptRunner.decodeValue(value)))
            }
        }
        if !extras.isEmpty {
            logStore.record(streamId: stream.id, level: "info", event: "script:initparse", url: nil, message: "init parsed by script · \(extras.count) field(s)")
        }
        return extras
    }

    /// Pulls one string field out of a script's JSON reply, tolerating the
    /// log lines scripts habitually print before the blob. Returns nil when the
    /// output isn't JSON at all, so callers can fall back to treating it as a
    /// bare value.
    func scriptJSONField(_ output: String, _ field: String) -> String? {
        guard let brace = output.firstIndex(of: "{"),
              let object = (try? JSONSerialization.jsonObject(with: Data(output[brace...].utf8))) as? [String: Any]
        else { return nil }
        if let value = object[field] as? String { return value }
        // Accept a lowercase spelling too — scripts are inconsistent about it.
        let lowered = field.lowercased()
        for (key, value) in object where key.lowercased() == lowered {
            if let text = value as? String { return text }
        }
        return nil
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
            "scriptActions": provider.scriptActions,
            "scriptSessionDir": scriptSessionDir(providerId: provider.id, create: false),
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
        // Friendly name-slug for the copyable play URL (falls back to the id).
        let slug = (try? readState()).map { playSlug(stream, in: $0) } ?? stream.id
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
            "playUrl": "http://\(host)/play/\(slug)/index.m3u8",
            "directSource": stream.directSource,
            "sourceUrl": "http://\(host)/source/\(stream.id)",
            "directUrl": stream.kind == "m3u8" ? "http://\(host)/play/\(slug)/index.m3u8" : "http://\(host)/restream/\(stream.id)/live.m3u8",
            "directStreamUrls": directUrls,
            "downloadUrls": downloadUrls,
            "sourceType": stream.sourceType,
            "mode": stream.mode,
            "sessionManifest": stream.sessionManifest,
            "scriptActionsOverride": stream.scriptActionsOverride as Any,
            "scriptParams": stream.scriptParams,
            "cdmType": stream.cdmType,
            "useCdm": stream.useCdm,
            "scriptOverride": stream.scriptOverride,
            "proxyScript": stream.proxyScript,
            "proxyManifest": stream.proxyManifest,
            "proxyMedia": stream.proxyMedia,
            "heartbeatEnabled": stream.heartbeatEnabled,
            "heartbeatSeconds": stream.heartbeatSeconds,
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
                stream.mode = (dict["Mode"] as? String) ?? (dict["mode"] as? String) ?? "live"
                stream.sessionManifest = (dict["SessionManifest"] as? Bool) ?? (dict["sessionManifest"] as? Bool) ?? false
                stream.scriptParams = (dict["ScriptParams"] as? String) ?? (dict["scriptParams"] as? String) ?? ""
                stream.cdmType = (dict["CdmType"] as? String) ?? (dict["cdmType"] as? String) ?? ""
                stream.useCdm = (dict["UseCdm"] as? Bool) ?? (dict["useCdm"] as? Bool) ?? false
                stream.scriptVideoSelector = (dict["Video"] as? String) ?? (dict["video"] as? String) ?? ""
                stream.scriptAudioSelector = (dict["Audio"] as? String) ?? (dict["audio"] as? String) ?? ""
                stream.onDemand = (dict["OnDemand"] as? Bool) ?? (dict["onDemand"] as? Bool) ?? false
                stream.speedUp = (dict["SpeedUp"] as? Bool) ?? (dict["speedUp"] as? Bool) ?? false
                stream.autostart = (dict["Autostart"] as? Bool) ?? (dict["autostart"] as? Bool) ?? false
                stream.scriptStart = (dict["Start"] as? NSNumber)?.intValue ?? (dict["start"] as? NSNumber)?.intValue
                stream.scriptEnd = (dict["End"] as? NSNumber)?.intValue ?? (dict["end"] as? NSNumber)?.intValue
                stream.recordEvent = (dict["RecordEvent"] as? Bool) ?? (dict["recordEvent"] as? Bool) ?? false
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
        let validInputModes = ["internal", "ffmpegResident", "ffmpegTsHls", "ffmpegMultiTsHls", "ffmpegFmp4Hls", "nm3u8dlre"]
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
        stream.directSource = input["directSource"]?.bool ?? false
        stream.nm3u8dlreParams = input["nm3u8dlreParams"]?.string ?? ""
        // Scripting & DRM (editor-controlled — see the update merge, which no
        // longer carries these over from the existing stream).
        stream.useCdm = input["useCdm"]?.bool ?? false
        stream.scriptParams = input["scriptParams"]?.string ?? ""
        stream.heartbeatEnabled = input["heartbeatEnabled"]?.bool ?? true
        stream.heartbeatSeconds = max(0, input["heartbeatSeconds"]?.int ?? 0)
        stream.scriptOverride = input["scriptOverride"]?.string ?? ""
        stream.sessionManifest = input["sessionManifest"]?.bool ?? false
        // Absent (or explicitly null) means inherit the provider's set; an
        // array overrides it wholesale, including an empty one — "run nothing
        // for this stream" has to be expressible.
        if let requestedActions = raw?["scriptActionsOverride"] as? [Any] {
            stream.scriptActionsOverride = requestedActions
                .compactMap { $0 as? String }
                .filter { ScriptAction(rawValue: $0) != nil }
        } else {
            stream.scriptActionsOverride = nil
        }
        stream.cdmMode = "external"   // internal mode removed
        stream.cdmType = ""            // no longer user-selected — DRM info is auto-parsed
        stream.proxyScript = input["proxyScript"]?.bool ?? true
        stream.proxyManifest = input["proxyManifest"]?.bool ?? true
        stream.proxyMedia = input["proxyMedia"]?.bool ?? true
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

    /// A clean, URL-friendly slug from a stream name for the copyable play URL,
    /// instead of the internal `stream_…` id. Keeps Unicode letters/digits (so
    /// Arabic names survive), collapses every other run into a single '-',
    /// trims dashes, lowercases. Empty when the name has nothing usable.
    func slugify(_ name: String) -> String {
        var out = ""
        var pendingDash = false
        for scalar in name.lowercased().unicodeScalars {
            if CharacterSet.alphanumerics.contains(scalar) {
                if pendingDash { out.append("-"); pendingDash = false }
                out.unicodeScalars.append(scalar)
            } else if !out.isEmpty {
                pendingDash = true
            }
        }
        return out
    }

    /// The path segment used in this stream's play URL: its name-slug when that's
    /// unambiguous, otherwise the id. Ambiguous (two streams slugging the same)
    /// falls back to the id so the URL always resolves to the right stream.
    func playSlug(_ stream: StreamConfig, in state: PanelState) -> String {
        let slug = slugify(stream.name)
        guard !slug.isEmpty else { return stream.id }
        let matches = state.providers.flatMap(\.streams).filter { slugify($0.name) == slug }
        return matches.count == 1 ? slug : stream.id
    }

    /// Resolve a play-URL first path segment (an id or a name-slug) to the real
    /// stream id, so `/play/<slug>/…` and its relative segment requests all work.
    func resolvePlaySegment(_ segment: String, in state: PanelState) -> String? {
        if providerAndStream(segment, in: state) != nil { return segment }
        let matches = state.providers.flatMap(\.streams).filter { slugify($0.name) == segment && !slugify($0.name).isEmpty }
        return matches.count == 1 ? matches[0].id : nil
    }

    func effectiveProxy(provider: Provider, stream: StreamConfig) -> String {
        stream.proxy.isEmpty ? provider.proxy : stream.proxy
    }

    /// Per-category proxy: the base proxy, but only when that category's scope
    /// flag is on — so the proxy can cover, say, just the script/session-manifest
    /// calls while the MPD + media go direct.
    func scriptProxy(provider: Provider, stream: StreamConfig) -> String {
        stream.proxyScript ? effectiveProxy(provider: provider, stream: stream) : ""
    }
    func manifestProxy(provider: Provider, stream: StreamConfig) -> String {
        stream.proxyManifest ? effectiveProxy(provider: provider, stream: stream) : ""
    }
    func mediaProxy(provider: Provider, stream: StreamConfig) -> String {
        stream.proxyMedia ? effectiveProxy(provider: provider, stream: stream) : ""
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
        // Ignore SIGPIPE: writing to a socket whose peer has already closed
        // (a browser tab closing, an SSE client dropping, a stream consumer
        // disconnecting mid-write) delivers SIGPIPE, whose default action is
        // to terminate the whole process. Ignoring it turns those writes into
        // an ordinary EPIPE error on that one call, which the I/O paths handle,
        // instead of silently killing the server (exit code 141 = 128 + 13).
        #if !os(Windows)
        signal(SIGPIPE, SIG_IGN)
        #endif
        let arguments = Array(CommandLine.arguments.dropFirst())
        switch arguments.first {
        case "dash":
            DashSegmentsCLI.main(Array(arguments.dropFirst()))
        case "live":
            LiveM3U8ActionRuntime.run(Array(arguments.dropFirst()))
        case "serve":
            serveEntry(Array(arguments.dropFirst()))
        case "cdmprobe":
            // Debug helper: fetch a DASH/HLS manifest and print the DRM
            // challenge (KIDs + PSSH) we'd hand a CDM script — and, if a
            // script= is given, run it and print the clear keys it returns.
            //   restreamair cdmprobe mpd=<url> [script=/path] [proxy=..] [cdmType=..]
            cdmProbe(Array(arguments.dropFirst()))
        case "selftest":
            // Known-answer crypto vectors — verifies the pure-Swift SHA/PBKDF2/
            // AES produce correct bytes on this platform (used by Linux CI).
            let cdmFailures = CDMKeyResolver.selfTestFailures()
            if !cdmFailures.isEmpty {
                FileHandle.standardError.write(Data("CDM key-resolver self-test FAILED: \(cdmFailures.joined(separator: ", "))\n".utf8))
                exit(1)
            }
            CryptoSelfTest.runOrExit()
            // The same comparison for the C core in core/, which nothing calls
            // in production yet — this is what proves each port before its call
            // sites switch over.
            CoreParitySelfTest.runOrExit()
        default:
            serveEntry(arguments)
        }
    }

    // MARK: - Serve supervision
    //
    // `serve` runs as two processes: a tiny supervisor parent that does
    // nothing but watch, and a child that actually binds the port and serves.
    // Stream downloads were already isolated in their own worker processes;
    // this closes the remaining gap — if anything in the serving process
    // still manages to die (a Swift runtime trap can't be caught in-process),
    // the supervisor restarts it within seconds instead of the whole service
    // staying down. Set RESTREAMAIR_NO_SUPERVISOR=1 (or pass --no-supervisor)
    // to run single-process, e.g. under systemd with Restart=always.

    /// Set on the parent while a serve child is running, so the C signal
    /// handlers (which can't capture context) can forward SIGTERM/SIGINT.
    static var supervisedChildPid: pid_t = -1
    /// Set by the signal handlers so the wait loop knows an exit was a
    /// requested stop, not a crash to restart.
    static var supervisorStopping = false

    static func serveEntry(_ arguments: [String]) {
        // A dropped SSH session sends SIGHUP to the foreground process group;
        // a panel that's meant to keep serving shouldn't die with the terminal.
        #if !os(Windows)
        signal(SIGHUP, SIG_IGN)
        #endif
        let serveArguments = arguments.filter { $0 != "--no-supervisor" }
        #if os(Windows)
        // The supervisor is POSIX signal/fork based; Windows runs single-process.
        runServe(serveArguments)
        #else
        let environment = ProcessInfo.processInfo.environment
        let supervised = environment["RESTREAMAIR_SUPERVISED"] == "1"
        let optOut = environment["RESTREAMAIR_NO_SUPERVISOR"] == "1" || arguments.contains("--no-supervisor")
        if supervised || optOut {
            runServe(serveArguments)
        } else {
            superviseServe(serveArguments)
        }
        #endif
    }

    /// The watcher: spawn the serving child, wait, restart on abnormal death
    /// with capped exponential backoff. Deliberately allocates nothing per
    /// iteration and handles no requests — the less it does, the less can
    /// kill it.
    #if !os(Windows)
    static func superviseServe(_ arguments: [String]) {
        signal(SIGTERM, { _ in
            RestreamAirMain.supervisorStopping = true
            if RestreamAirMain.supervisedChildPid > 0 { kill(RestreamAirMain.supervisedChildPid, SIGTERM) }
        })
        signal(SIGINT, { _ in
            RestreamAirMain.supervisorStopping = true
            if RestreamAirMain.supervisedChildPid > 0 { kill(RestreamAirMain.supervisedChildPid, SIGINT) }
        })
        let selfBinary = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
        var fastFailures = 0
        var firstAttempt = true
        while true {
            let child = Process()
            child.executableURL = selfBinary
            child.arguments = ["serve"] + arguments
            var environment = ProcessInfo.processInfo.environment
            environment["RESTREAMAIR_SUPERVISED"] = "1"
            child.environment = environment
            let startedAt = Date()
            do {
                try child.run()
            } catch {
                fputs("error: could not start panel process: \(error)\n", stderr)
                exit(1)
            }
            supervisedChildPid = child.processIdentifier
            child.waitUntilExit()
            supervisedChildPid = -1
            let uptime = Date().timeIntervalSince(startedAt)
            let signaled = child.terminationReason == .uncaughtSignal
            let status = child.terminationStatus
            if supervisorStopping || (!signaled && status == 0) { exit(0) }
            // An immediate nonzero exit on the very first attempt is a
            // configuration error (port in use, bad flags) — the child
            // already printed why. Propagate instead of crash-looping on it.
            if firstAttempt && !signaled && uptime < 5 { exit(status) }
            firstAttempt = false
            fastFailures = uptime >= 60 ? 0 : fastFailures + 1
            let delay = min(30.0, pow(2.0, Double(min(fastFailures, 5))))
            let cause = signaled ? "signal \(status)" : "exit code \(status)"
            fputs("panel process died (\(cause)) after \(Int(uptime))s — restarting in \(Int(delay))s\n", stderr)
            Thread.sleep(forTimeInterval: delay)
        }
    }
    #endif

    static func cdmProbe(_ arguments: [String]) {
        var args: [String: String] = [:]
        for token in arguments {
            guard let eq = token.firstIndex(of: "=") else { continue }
            args[String(token[..<eq])] = String(token[token.index(after: eq)...])
        }
        guard let manifest = args["mpd"] ?? args["url"] ?? args["m3u8"], let url = URL(string: manifest) else {
            fputs("usage: restreamair cdmprobe mpd=<url> [script=/path] [proxy=..] [cdmType=..]\n", stderr); exit(2)
        }
        let isHLS = manifest.contains(".m3u8") || args["m3u8"] != nil
        do {
            let opts = HTTPRequestOptions(timeout: 20, headers: [], proxy: args["proxy"])
            let text = String(data: try HTTPClient(options: opts).get(url), encoding: .utf8) ?? ""
            let challenge = isHLS ? CDMKeyResolver.challengeFromHLS(text) : CDMKeyResolver.challengeFromMPD(text)
            print("KIDs:        \(challenge.kids.isEmpty ? "(none)" : challenge.kids.joined(separator: ", "))")
            print("Widevine:    \(challenge.psshWidevine.isEmpty ? "(none)" : challenge.psshWidevine.prefix(48) + "…")")
            print("PlayReady:   \(challenge.psshPlayReady.isEmpty ? "(none)" : challenge.psshPlayReady.prefix(48) + "…")")
            print("FairPlay:    \(challenge.psshFairPlay.isEmpty ? "(none)" : challenge.psshFairPlay)")
            print("PSSH boxes:  \(challenge.psshAll.count)")
            if !challenge.keyURIs.isEmpty { print("Key URIs:    \(challenge.keyURIs.joined(separator: ", "))") }
            if let script = args["script"], !script.isEmpty {
                var extra: [String] = []
                if let p = args["proxy"] { extra.append("proxy=\(p)") }
                let keys = try CDMKeyResolver.resolveKeys(scriptPath: script, cdmType: args["cdmType"] ?? "", challenge: challenge, extraArgs: extra)
                print("Clear keys:  \(keys.isEmpty ? "(script returned none)" : "\n" + keys)")
            }
        } catch {
            fputs("cdmprobe error: \(error)\n", stderr); exit(1)
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
