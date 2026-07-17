import Foundation

/// Parsing/rewriting for arbitrary HLS playlist text — the "m3u8 file"
/// counterpart to DashSegments.swift's MPD parsing. Used by the panel's HLS
/// proxy pipeline (rewriting a remote playlist's URIs into proxy paths) and
/// by the probe endpoint (listing master-playlist variants/audio tracks).
/// Deliberately has no knowledge of PanelServer's routing scheme — callers
/// supply a `transform` closure to turn a resolved URI into whatever path
/// they want; this file only owns parsing/classifying the playlist text.
enum M3U8Rewriter {
    enum LineKind {
        case key, map, segment
    }

    struct Variant {
        let uri: String
        let bandwidth: Int?
        let width: Int?
        let height: Int?
        let codecs: String?
    }

    struct AudioTrack {
        let groupId: String
        let name: String
        let language: String?
        let uri: String?
    }

    // Compiled once and reused. These patterns are constant, and the rewrite
    // paths run on every HLS playlist request (a live stream refreshes its
    // media playlist every few seconds, per viewer) — recompiling an
    // NSRegularExpression each time was pure, repeated waste.
    private static let mediaSequenceRegex = try? NSRegularExpression(pattern: #"#EXT-X-MEDIA-SEQUENCE:(\d+)"#)
    private static let uriAttributeRegex = try? NSRegularExpression(pattern: #"URI="([^"]+)""#)

    static func mediaSequence(_ text: String) -> Int {
        guard let regex = mediaSequenceRegex,
              let match = regex.firstMatch(in: text, range: NSRange(text.startIndex..<text.endIndex, in: text)),
              let range = Range(match.range(at: 1), in: text) else { return 0 }
        return Int(text[range]) ?? 0
    }

    static func isMasterPlaylist(_ text: String) -> Bool {
        text.contains("#EXT-X-STREAM-INF")
    }

    /// Rewrites every key/map/segment URI in a media playlist via `transform`,
    /// which also receives the segment's media sequence number when
    /// `dropKey` is set (needed to derive a default AES-128 IV when none is
    /// configured; `nil` for key/map lines and whenever `dropKey` is false).
    /// When `dropKey` is true, `#EXT-X-KEY` lines are removed entirely
    /// (server-side decryption case — the player must not also try to decrypt).
    static func rewrite(_ text: String, base: URL, dropKey: Bool, transform: (String, LineKind, Int?) -> String) -> String {
        var sequence = dropKey ? mediaSequence(text) : 0
        return text.split(separator: "\n", omittingEmptySubsequences: false).map { rawLine -> String in
            let line = String(rawLine).trimmingCharacters(in: .whitespacesAndNewlines)
            if line.isEmpty { return String(rawLine) }
            if line.hasPrefix("#EXT-X-KEY") {
                if dropKey { return "" }
                return rewriteURIAttribute(String(rawLine), base: base, kind: .key) { transform($0, $1, nil) }
            }
            if line.hasPrefix("#EXT-X-MAP") {
                return rewriteURIAttribute(String(rawLine), base: base, kind: .map) { transform($0, $1, nil) }
            }
            if line.hasPrefix("#") { return String(rawLine) }
            guard let url = URL(string: line, relativeTo: base)?.absoluteURL else { return String(rawLine) }
            let seq = dropKey ? sequence : nil
            if dropKey { sequence += 1 }
            return transform(url.absoluteString, .segment, seq)
        }.joined(separator: "\n") + "\n"
    }

    /// Rewrites a master playlist's variant/audio-track URIs via `transform`
    /// — everything else (BANDWIDTH/CODECS/etc. attributes) passes through
    /// untouched. Unlike `rewrite` (for media playlists, where every
    /// referenced URI is a segment/key/init to proxy byte-for-byte), a
    /// master playlist's URIs point at *other playlists* that themselves
    /// need parsing/rewriting — proxying one through the raw byte pipe
    /// would return its un-rewritten relative segment URIs, which resolve
    /// against the wrong base once the player has them and silently fail.
    static func rewriteMaster(_ text: String, base: URL, transform: (String) -> String) -> String {
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false).map { String($0) }
        var output: [String] = []
        var index = 0
        while index < lines.count {
            let trimmed = lines[index].trimmingCharacters(in: .whitespacesAndNewlines)
            if trimmed.hasPrefix("#EXT-X-STREAM-INF") {
                output.append(lines[index])
                if index + 1 < lines.count {
                    let uriLine = lines[index + 1].trimmingCharacters(in: .whitespacesAndNewlines)
                    if !uriLine.isEmpty, let url = URL(string: uriLine, relativeTo: base)?.absoluteURL {
                        output.append(transform(url.absoluteString))
                    } else {
                        output.append(lines[index + 1])
                    }
                    index += 2
                    continue
                }
                index += 1
                continue
            }
            if trimmed.hasPrefix("#EXT-X-MEDIA"), trimmed.contains("URI=") {
                output.append(rewriteURIAttribute(lines[index], base: base, kind: .segment) { url, _ in transform(url) })
                index += 1
                continue
            }
            output.append(lines[index])
            index += 1
        }
        return output.joined(separator: "\n") + "\n"
    }

    private static func rewriteURIAttribute(_ line: String, base: URL, kind: LineKind, transform: (String, LineKind) -> String) -> String {
        guard let regex = uriAttributeRegex else { return line }
        var output = line
        let matches = regex.matches(in: line, range: NSRange(line.startIndex..<line.endIndex, in: line)).reversed()
        for match in matches {
            guard let fullRange = Range(match.range(at: 0), in: output),
                  let uriRange = Range(match.range(at: 1), in: output),
                  let url = URL(string: String(output[uriRange]), relativeTo: base)?.absoluteURL else { continue }
            output.replaceSubrange(fullRange, with: #"URI="\#(transform(url.absoluteString, kind))""#)
        }
        return output
    }

    /// Lists variant streams (video qualities) and audio tracks from a
    /// master playlist, for the stream-editor probe UI.
    static func probeMaster(_ text: String, base: URL) -> (variants: [Variant], audioTracks: [AudioTrack]) {
        var variants: [Variant] = []
        var audioTracks: [AudioTrack] = []
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false).map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        var index = 0
        while index < lines.count {
            let line = lines[index]
            if line.hasPrefix("#EXT-X-STREAM-INF") {
                let attrs = parseAttributes(line)
                let uriLine = index + 1 < lines.count ? lines[index + 1] : ""
                if let url = URL(string: uriLine, relativeTo: base)?.absoluteURL {
                    variants.append(Variant(
                        uri: url.absoluteString,
                        bandwidth: attrs["BANDWIDTH"].flatMap { Int($0) },
                        width: attrs["RESOLUTION"]?.split(separator: "x").first.flatMap { Int($0) },
                        height: attrs["RESOLUTION"]?.split(separator: "x").last.flatMap { Int($0) },
                        codecs: attrs["CODECS"]
                    ))
                }
                index += 2
                continue
            }
            if line.hasPrefix("#EXT-X-MEDIA"), line.contains("TYPE=AUDIO") {
                let attrs = parseAttributes(line)
                let uri = attrs["URI"].flatMap { URL(string: $0, relativeTo: base)?.absoluteURL.absoluteString }
                audioTracks.append(AudioTrack(
                    groupId: attrs["GROUP-ID"] ?? "audio",
                    name: attrs["NAME"] ?? "Audio",
                    language: attrs["LANGUAGE"],
                    uri: uri
                ))
            }
            index += 1
        }
        return (variants, audioTracks)
    }

    private static func parseAttributes(_ line: String) -> [String: String] {
        guard let colon = line.firstIndex(of: ":") else { return [:] }
        let attrText = line[line.index(after: colon)...]
        var result: [String: String] = [:]
        var current = ""
        var inQuotes = false
        var pieces: [String] = []
        for char in attrText {
            if char == "\"" { inQuotes.toggle() }
            if char == "," && !inQuotes {
                pieces.append(current)
                current = ""
            } else {
                current.append(char)
            }
        }
        if !current.isEmpty { pieces.append(current) }
        for piece in pieces {
            guard let eq = piece.firstIndex(of: "=") else { continue }
            let key = String(piece[..<eq]).trimmingCharacters(in: .whitespaces)
            var value = String(piece[piece.index(after: eq)...]).trimmingCharacters(in: .whitespaces)
            if value.hasPrefix("\""), value.hasSuffix("\""), value.count >= 2 {
                value = String(value.dropFirst().dropLast())
            }
            result[key] = value
        }
        return result
    }
}
