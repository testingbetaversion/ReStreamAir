import Foundation

/// Auto-acquires **clear** CENC keys via a provider "CDM" script when a
/// DRM-protected stream has no keys entered but a script is configured.
///
/// ReStreamAir is an IPTV restreamer, not a DRM client — it does not run a real
/// Widevine/PlayReady/FairPlay CDM or talk to a license server itself. Instead,
/// when a stream is protected and the operator has pointed the provider at a
/// script, we hand that script whatever DRM material we can scrape from the
/// manifest (and init segment) — the KIDs and the raw PSSH boxes for each DRM
/// system — and the script returns raw `KID:KEY` hex pairs (clear keys). Those
/// go to the mixer exactly as if they'd been typed into the decryption-keys box
/// by hand, and AES-CTR/CBC decryption proceeds normally.
///
/// This file is pure parsing + script invocation with no panel dependencies, so
/// it can be exercised directly from `selftest`.
enum CDMKeyResolver {
    /// Well-known DRM system IDs (lowercased, dashless) used to label PSSH boxes
    /// and ContentProtection entries so the script knows which system each blob
    /// belongs to.
    static let widevineSystemID  = "edef8ba979d64acea3c827dcd51d21ed"
    static let playreadySystemID = "9a04f07998404286ab92e65be0885f95"
    static let commonSystemID    = "1077efecc0b24d02ace33c1e52e2fb4b" // W3C Common PSSH (carries KIDs)
    static let fairplaySystemID  = "94ce86fb07ff4f43adb893d2fa968ca2"

    /// Everything we could scrape to identify what needs keys. Passed to the
    /// script as `kid=`, `pssh=`, `psshWidevine=`, `psshPlayReady=`, etc.
    struct DRMChallenge: Equatable {
        var kids: [String] = []            // hex KIDs (32 hex chars each), dashless, lowercased
        var psshWidevine: String = ""      // base64 pssh box for Widevine, if found
        var psshPlayReady: String = ""     // base64 pssh box for PlayReady, if found
        var psshFairPlay: String = ""      // base64 / key URI for FairPlay, if found
        var psshAll: [String] = []         // every base64 pssh box found, in order
        /// HLS only: the EXT-X-KEY / EXT-X-SESSION-KEY license/key URIs, so a
        /// script that fetches from them has what it needs.
        var keyURIs: [String] = []

        var isEmpty: Bool {
            kids.isEmpty && psshAll.isEmpty && keyURIs.isEmpty
        }
    }

    // MARK: - DASH (MPD) extraction

    /// Scrape ContentProtection PSSH boxes and default_KIDs from a DASH MPD.
    /// Handles both `<cenc:pssh>` and namespace-prefixed variants, and
    /// `cenc:default_KID` / `default_KID` on any ContentProtection element.
    static func challengeFromMPD(_ xml: String) -> DRMChallenge {
        var challenge = DRMChallenge()

        // <cenc:pssh> ... </cenc:pssh> (any namespace prefix). Base64 may be
        // split across whitespace/newlines in pretty-printed manifests.
        for raw in captures(#"<(?:[A-Za-z0-9]+:)?pssh[^>]*>([\s\S]*?)</(?:[A-Za-z0-9]+:)?pssh>"#, in: xml) {
            let b64 = raw.replacingOccurrences(of: "[^A-Za-z0-9+/=]", with: "", options: .regularExpression)
            guard !b64.isEmpty else { continue }
            challenge.psshAll.append(b64)
            classifyPSSH(base64: b64, into: &challenge)
        }

        // default_KID="26d9…" or cenc:default_KID='…', with or without dashes.
        for raw in captures(#"(?:cenc:)?default_KID\s*=\s*["']([0-9a-fA-F-]+)["']"#, in: xml) {
            addKID(raw, to: &challenge)
        }
        return challenge
    }

    /// Inspect a base64 PSSH box's 16-byte SystemID (bytes 12..28 of the box)
    /// and route it to the matching per-system slot. Also pulls the KIDs a
    /// v1 PSSH box lists after the SystemID.
    static func classifyPSSH(base64: String, into challenge: inout DRMChallenge) {
        guard let data = Data(base64Encoded: base64) else { return }
        let bytes = [UInt8](data)
        // box: [size:4][type "pssh":4][version+flags:4][systemID:16]...
        guard bytes.count >= 28, bytes[4] == 0x70, bytes[5] == 0x73, bytes[6] == 0x73, bytes[7] == 0x68 else { return }
        let systemID = hex(Array(bytes[12..<28]))
        switch systemID {
        case widevineSystemID where challenge.psshWidevine.isEmpty:   challenge.psshWidevine = base64
        case playreadySystemID where challenge.psshPlayReady.isEmpty: challenge.psshPlayReady = base64
        default: break
        }
        // v1 PSSH (version byte at [8] == 1): [version:1][flags:3][systemID:16][KID_count:4][KID*16...]
        if bytes[8] == 1, bytes.count >= 32 {
            let kidCount = Int(bytes[28]) << 24 | Int(bytes[29]) << 16 | Int(bytes[30]) << 8 | Int(bytes[31])
            var offset = 32
            for _ in 0..<min(kidCount, 64) where offset + 16 <= bytes.count {
                addKID(hex(Array(bytes[offset..<offset + 16])), to: &challenge)
                offset += 16
            }
        }
    }

    // MARK: - HLS (m3u8) extraction

    /// Scrape EXT-X-KEY / EXT-X-SESSION-KEY tags from an HLS playlist. Captures
    /// the key/license URI and, for FairPlay/Widevine/PlayReady, the KEYFORMAT
    /// so the script can tell systems apart, plus any KEYID hint.
    static func challengeFromHLS(_ playlist: String) -> DRMChallenge {
        var challenge = DRMChallenge()
        for line in playlist.split(whereSeparator: { $0 == "\n" || $0 == "\r" }) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            guard trimmed.hasPrefix("#EXT-X-KEY:") || trimmed.hasPrefix("#EXT-X-SESSION-KEY:") else { continue }
            let attrs = trimmed.drop(while: { $0 != ":" }).dropFirst()
            let method = attribute("METHOD", in: String(attrs)) ?? ""
            if method.uppercased() == "NONE" { continue }
            if let uri = attribute("URI", in: String(attrs)), !uri.isEmpty {
                let keyFormat = attribute("KEYFORMAT", in: String(attrs)) ?? ""
                if !challenge.keyURIs.contains(uri) { challenge.keyURIs.append(uri) }
                // FairPlay identifies itself with com.apple.streamingkeydelivery.
                if keyFormat.contains("streamingkeydelivery"), challenge.psshFairPlay.isEmpty {
                    challenge.psshFairPlay = uri
                }
                // Widevine HLS carries a base64 PSSH in the URI (data:...;base64,)
                if keyFormat.lowercased().contains("urn:uuid:\(widevineSystemID.inserting(dashesForUUID: true))") || uri.hasPrefix("data:") {
                    if let comma = uri.range(of: "base64,") {
                        let b64 = String(uri[comma.upperBound...])
                        challenge.psshAll.append(b64)
                        classifyPSSH(base64: b64, into: &challenge)
                    }
                }
            }
            if let keyID = attribute("KEYID", in: String(attrs)) {
                addKID(keyID.replacingOccurrences(of: "0x", with: "", options: .caseInsensitive), to: &challenge)
            }
        }
        return challenge
    }

    // MARK: - Script invocation

    /// Invoke the provider's CDM script with the challenge and parse the clear
    /// keys it returns. `extraArgs` typically carries proxy=/header=/params so
    /// the script can reach the same license endpoint the stream uses. Returns a
    /// newline-joined `KID:KEY` string ready for the mixer's decryptionKeys arg,
    /// or throws with the script's stderr on failure.
    static func resolveKeys(scriptPath: String, cdmType: String, challenge: DRMChallenge,
                            extraArgs: [String], timeout: Double = 45) throws -> String {
        var args = ["action=cdm", "cdmType=\(cdmType)"]
        if !challenge.kids.isEmpty { args.append("kid=\(challenge.kids.joined(separator: ","))") }
        if !challenge.psshWidevine.isEmpty { args.append("psshWidevine=\(challenge.psshWidevine)") }
        if !challenge.psshPlayReady.isEmpty { args.append("psshPlayReady=\(challenge.psshPlayReady)") }
        if !challenge.psshFairPlay.isEmpty { args.append("psshFairPlay=\(challenge.psshFairPlay)") }
        // A single canonical pssh= (first found) plus every box as psshAll= —
        // scripts that only read one, and scripts that want them all, both work.
        if let first = challenge.psshAll.first { args.append("pssh=\(first)") }
        if !challenge.psshAll.isEmpty { args.append("psshAll=\(challenge.psshAll.joined(separator: ","))") }
        if !challenge.keyURIs.isEmpty { args.append("keyUri=\(challenge.keyURIs.joined(separator: ","))") }
        args.append(contentsOf: extraArgs)

        let (stdout, _) = try ScriptRunner.runSync(scriptPath: scriptPath, args: args, timeout: timeout)
        let keys = parseKeyOutput(stdout)
        return keys.map { "\($0.kid):\($0.key)" }.joined(separator: "\n")
    }

    /// Parse a CDM script's stdout into KID:KEY pairs. Accepts three shapes so
    /// existing DRM tooling drops in unmodified:
    ///  1. bare `KID:KEY` lines (one per line),
    ///  2. pywidevine's `--> KID:KEY` log lines,
    ///  3. JSON `{"keys":[{"kid":"…","key":"…"}, …]}` (or a bare array).
    static func parseKeyOutput(_ stdout: String) -> [(kid: String, key: String)] {
        var pairs: [(kid: String, key: String)] = []
        var seen = Set<String>()

        func add(_ rawKid: String, _ rawKey: String) {
            let kid = rawKid.replacingOccurrences(of: "-", with: "").lowercased()
            let key = rawKey.lowercased()
            guard kid.count == 32, key.count == 32,
                  kid.allSatisfy(\.isHexDigit), key.allSatisfy(\.isHexDigit),
                  !seen.contains(kid) else { return }
            seen.insert(kid)
            pairs.append((kid, key))
        }

        // JSON first — if the whole output parses as JSON with a keys array.
        if let data = stdout.data(using: .utf8),
           let json = try? JSONSerialization.jsonObject(with: data) {
            let list = (json as? [String: Any])?["keys"] as? [[String: Any]] ?? (json as? [[String: Any]])
            for entry in list ?? [] {
                if let kid = entry["kid"] as? String, let key = entry["key"] as? String { add(kid, key) }
            }
            if !pairs.isEmpty { return pairs }
        }

        // Line-based `KID:KEY` (optionally prefixed with "-->", whitespace, etc.)
        for raw in captures(#"([0-9a-fA-F]{32})\s*:\s*([0-9a-fA-F]{32})"#, in: stdout, groups: 2) {
            let parts = raw.split(separator: "\u{1}") // captures(groups:2) joins groups with \u{1}
            if parts.count == 2 { add(String(parts[0]), String(parts[1])) }
        }
        return pairs
    }

    // MARK: - helpers

    private static func addKID(_ raw: String, to challenge: inout DRMChallenge) {
        let kid = raw.replacingOccurrences(of: "-", with: "").lowercased()
        guard kid.count == 32, kid.allSatisfy(\.isHexDigit), !challenge.kids.contains(kid) else { return }
        challenge.kids.append(kid)
    }

    private static func hex(_ bytes: [UInt8]) -> String {
        bytes.map { String(format: "%02x", $0) }.joined()
    }

    private static func attribute(_ name: String, in attrs: String) -> String? {
        // NAME="value" or NAME=value (unquoted, comma/space-terminated).
        if let q = captures("\(name)=\"([^\"]*)\"", in: attrs).first { return q }
        return captures("\(name)=([^,\\s]+)", in: attrs).first
    }

    /// Regex capture helper: returns capture group 1 of each match. When
    /// `groups == 2`, returns group1 and group2 joined by \u{1} (a separator
    /// that can't appear in the hex/base64 payloads).
    private static func captures(_ pattern: String, in text: String, groups: Int = 1) -> [String] {
        guard let regex = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]) else { return [] }
        let range = NSRange(text.startIndex..., in: text)
        var out: [String] = []
        for match in regex.matches(in: text, range: range) {
            if groups == 2, match.numberOfRanges >= 3,
               let r1 = Range(match.range(at: 1), in: text), let r2 = Range(match.range(at: 2), in: text) {
                out.append("\(text[r1])\u{1}\(text[r2])")
            } else if match.numberOfRanges >= 2, let r1 = Range(match.range(at: 1), in: text) {
                out.append(String(text[r1]))
            }
        }
        return out
    }
}

extension CDMKeyResolver {
    /// Known-answer tests for the pure parsing paths (no script execution),
    /// run from the `selftest` command so a Linux CI build proves them too.
    static func selfTestFailures() -> [String] {
        var failures: [String] = []
        func expect(_ name: String, _ condition: Bool) { if !condition { failures.append(name) } }

        // 1. bare KID:KEY line
        let a = parseKeyOutput("26d939a4f72a4b6c8d0e1f2a3b4c5d6e:0123456789abcdef0123456789abcdef")
        expect("parse bare line", a.count == 1 && a[0].kid == "26d939a4f72a4b6c8d0e1f2a3b4c5d6e" && a[0].key == "0123456789abcdef0123456789abcdef")

        // 2. pywidevine-style "--> KID:KEY" with surrounding noise
        let b = parseKeyOutput("[INFO] got key\n--> AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA:BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\n[done]")
        expect("parse pywidevine line", b.count == 1 && b[0].kid == "a".repeated(32) && b[0].key == "b".repeated(32))

        // 3. JSON {"keys":[{kid,key}]}
        let c = parseKeyOutput(#"{"keys":[{"kid":"11111111111111111111111111111111","key":"22222222222222222222222222222222"}]}"#)
        expect("parse json keys", c.count == 1 && c[0].kid == "1".repeated(32) && c[0].key == "2".repeated(32))

        // 4. dedup + reject wrong-length
        let d = parseKeyOutput("11111111111111111111111111111111:22222222222222222222222222222222\n11111111111111111111111111111111:33333333333333333333333333333333\nZZZ:22")
        expect("dedup by kid", d.count == 1)

        // 5. MPD default_KID (with dashes) + cenc:pssh classification
        let widevinePSSH = makeTestPSSH(systemID: widevineSystemID)
        let mpd = "<ContentProtection cenc:default_KID=\"26d939a4-f72a-4b6c-8d0e-1f2a3b4c5d6e\"><cenc:pssh>\(widevinePSSH)</cenc:pssh></ContentProtection>"
        let e = challengeFromMPD(mpd)
        expect("mpd default_KID", e.kids.contains("26d939a4f72a4b6c8d0e1f2a3b4c5d6e"))
        expect("mpd widevine pssh", e.psshWidevine == widevinePSSH)

        // 6. HLS EXT-X-KEY URI capture
        let hls = "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://abc\",KEYFORMAT=\"com.apple.streamingkeydelivery\"\n"
        let f = challengeFromHLS(hls)
        expect("hls key uri", f.keyURIs == ["skd://abc"] && f.psshFairPlay == "skd://abc")

        return failures
    }

    /// Build a minimal valid v0 PSSH box (base64) for a given system id — used
    /// only by the self-test.
    static func makeTestPSSH(systemID: String) -> String {
        var bytes: [UInt8] = []
        let sysBytes = stride(from: 0, to: 32, by: 2).compactMap { UInt8(systemID.dropFirst($0).prefix(2), radix: 16) }
        let size = UInt32(12 + 16 + 4)
        bytes.append(contentsOf: [UInt8(size >> 24 & 0xff), UInt8(size >> 16 & 0xff), UInt8(size >> 8 & 0xff), UInt8(size & 0xff)])
        bytes.append(contentsOf: Array("pssh".utf8))
        bytes.append(contentsOf: [0, 0, 0, 0])   // version 0 + flags
        bytes.append(contentsOf: sysBytes)         // 16-byte system id
        bytes.append(contentsOf: [0, 0, 0, 0])   // data size 0
        return Data(bytes).base64EncodedString()
    }
}

private extension String {
    func repeated(_ n: Int) -> String { String(repeating: self, count: n) }
}

private extension String {
    /// Insert dashes into a 32-char hex string to make a UUID (8-4-4-4-12).
    func inserting(dashesForUUID: Bool) -> String {
        guard dashesForUUID, count == 32 else { return self }
        let c = Array(self)
        return "\(String(c[0..<8]))-\(String(c[8..<12]))-\(String(c[12..<16]))-\(String(c[16..<20]))-\(String(c[20..<32]))"
    }
}
