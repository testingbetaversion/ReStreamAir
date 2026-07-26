import Foundation

/// Prints what the Swift implementations produce for every parity fixture, in a
/// line-oriented form scripts/gen-goldens.py turns into core/tests/goldens.h.
/// Not part of any build target — it lives in scripts/, which SwiftPM excludes.
///
///     swiftc M3U8Rewriter.swift FFmpegResident.swift CoreParityFixtures.swift \
///         scripts/dump-goldens.swift -o /tmp/dump-goldens
///     /tmp/dump-goldens | python3 scripts/gen-goldens.py
@main
struct DumpGoldens {
    static let unitSep = "\u{1f}"
    static let recordSep = "\u{1e}"
    static let groupSep = "\u{1d}"

    /// Escapes over unicode scalars rather than characters: Swift treats CRLF
    /// as a single Character, so a character-by-character switch would let the
    /// "\r\n" inside an ffmpeg -headers argument through unescaped and split the
    /// record in two.
    static func escaped(_ s: String) -> String {
        var out = ""
        for scalar in s.unicodeScalars {
            switch scalar {
            case "\\": out += "\\\\"
            case "\n": out += "\\n"
            case "\r": out += "\\r"
            case "\t": out += "\\t"
            default: out.unicodeScalars.append(scalar)
            }
        }
        return out
    }

    static func emit(_ key: String, _ value: String) {
        print("\(key)\t\(escaped(value))")
    }

    static func kindName(_ kind: M3U8Rewriter.LineKind) -> String {
        switch kind {
        case .key: return "key"
        case .map: return "map"
        case .segment: return "seg"
        }
    }

    static func mediaTransform(_ uri: String, _ kind: M3U8Rewriter.LineKind, _ seq: Int?) -> String {
        return "/p/\(kindName(kind))?u=\(uri)" + (seq.map { "&s=\($0)" } ?? "")
    }

    static func masterTransform(_ uri: String) -> String {
        return "/m?u=\(uri)"
    }

    /// URL shapes the playlists do not cover, checked so the C resolver is
    /// measured against Foundation rather than against a reading of RFC 3986.
    /// The first block is the RFC's own section 5.4 example table.
    static let resolveCases: [(String, String)] = [
        ("http://a/b/c/d;p?q", "g:h"), ("http://a/b/c/d;p?q", "g"), ("http://a/b/c/d;p?q", "./g"),
        ("http://a/b/c/d;p?q", "g/"), ("http://a/b/c/d;p?q", "/g"), ("http://a/b/c/d;p?q", "//g"),
        ("http://a/b/c/d;p?q", "?y"), ("http://a/b/c/d;p?q", "g?y"), ("http://a/b/c/d;p?q", "#s"),
        ("http://a/b/c/d;p?q", "g#s"), ("http://a/b/c/d;p?q", "g?y#s"), ("http://a/b/c/d;p?q", ";x"),
        ("http://a/b/c/d;p?q", "g;x"), ("http://a/b/c/d;p?q", ""), ("http://a/b/c/d;p?q", "."),
        ("http://a/b/c/d;p?q", "./"), ("http://a/b/c/d;p?q", ".."), ("http://a/b/c/d;p?q", "../"),
        ("http://a/b/c/d;p?q", "../g"), ("http://a/b/c/d;p?q", "../.."), ("http://a/b/c/d;p?q", "../../"),
        ("http://a/b/c/d;p?q", "../../g"), ("http://a/b/c/d;p?q", "../../../g"),
        ("http://a/b/c/d;p?q", "/./g"), ("http://a/b/c/d;p?q", "g."), ("http://a/b/c/d;p?q", ".g"),
        ("http://a/b/c/d;p?q", "g.."), ("http://a/b/c/d;p?q", "..g"), ("http://a/b/c/d;p?q", "./../g"),
        ("http://a/b/c/d;p?q", "g/./h"), ("http://a/b/c/d;p?q", "g/../h"),

        ("https://h.example.com", "seg.ts"), ("https://h.example.com?x=1", "seg.ts"),
        ("https://h.example.com/a/b.m3u8?t=1", "c.ts"), ("https://h.example.com/a/b.m3u8", "?t=2"),
        ("https://h.example.com/a/b.m3u8", "http://other/x.ts"),
        ("https://h.example.com/a/b.m3u8", "//cdn.example.com/x.ts"),
        ("https://h.example.com/a/b/../c.m3u8", "d.ts"),
        ("https://user:pw@h.example.com:8443/a/b.m3u8", "d.ts"),
        ("https://h.example.com/a/b.m3u8", "?"), ("https://h.example.com/a/b.m3u8", "#"),
        ("https://h.example.com/a/b.m3u8", "/"), ("https://h.example.com/a/b.m3u8", "//"),

        // Percent-encoding: characters that must be escaped, escapes that must
        // be preserved, and a stray % that must not corrupt the rest.
        ("https://h.example.com/a/b.m3u8", "has space.ts"),
        ("https://h.example.com/a/b.m3u8", "quo\"te.ts"),
        ("https://h.example.com/a/b.m3u8", "br[ack]et^back`tick{brace}pipe|.ts"),
        ("https://h.example.com/a/b.m3u8", "back\\slash.ts"),
        ("https://h.example.com/a/b.m3u8", "less<greater>.ts"),
        ("https://h.example.com/a/b.m3u8", "x%20y.ts"),
        ("https://h.example.com/a/b.m3u8", "x%2Gy.ts"),
        ("https://h.example.com/a/b.m3u8", "x%y.ts"),
        ("https://h.example.com/a/b.m3u8", "x%2.ts"),
        ("https://h.example.com/a/b.m3u8", "seg.ts?t=a%3Db&u=1"),
        ("https://h.example.com/a b/c.m3u8", "d.ts"),
        ("https://h.example.com/a/b.m3u8", "caf\u{e9}.ts"),
        ("https://h.example.com/a/b.m3u8", "http://other/x y.ts"),
        ("https://h.example.com/a/b.m3u8", "http://other/b/../c.ts"),
        ("https://h.example.com/a/b.m3u8", "#frag ment"),
        ("HTTPS://H.Example.COM/a/b.m3u8", "d.ts"),
    ]

    static func hex(_ bytes: [UInt8]) -> String {
        return bytes.map { String(format: "%02x", $0) }.joined()
    }

    static func bytes(_ hex: String) -> [UInt8] {
        var out: [UInt8] = []
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            out.append(UInt8(hex[index..<next], radix: 16)!)
            index = next
        }
        return out
    }

    /// AES output for the cases the published vectors in AES.swift do not
    /// already cover: AES-192, a multi-block CTR run, and CBC in both its
    /// valid-padding and invalid-padding forms. Taking these from the Swift
    /// implementation — which is itself checked against CommonCrypto and the
    /// FIPS vectors — beats inventing expected values.
    static func dumpAES() {
        let key192 = bytes("000102030405060708090a0b0c0d0e0f1011121314151617")
        let block = bytes("00112233445566778899aabbccddeeff")
        let aes192 = AES(key: key192)!
        let encrypted192 = aes192.encryptBlock(block)
        emit("aes:192-encrypt", hex(encrypted192))
        emit("aes:192-decrypt", hex(aes192.decryptBlock(encrypted192)))

        let ctrKey = bytes("2b7e151628aed2a6abf7158809cf4f3c")
        let ctrIV = bytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")
        var data = (0..<64).map { UInt8($0) }
        let ctr = AESCTR(key: ctrKey, iv: ctrIV)!
        ctr.process(&data, start: 0, length: data.count)
        emit("aes:ctr-64", hex(data))

        // The same key with a four-byte IV, which AESCTR zero-pads to 16.
        var shortIVData = (0..<32).map { UInt8($0) }
        let shortIVCTR = AESCTR(key: ctrKey, iv: bytes("f0f1f2f3"))!
        shortIVCTR.process(&shortIVData, start: 0, length: shortIVData.count)
        emit("aes:ctr-short-iv", hex(shortIVData))

        func cbcEncrypt(_ plain: [UInt8], key: [UInt8], iv: [UInt8]) -> [UInt8] {
            let aes = AES(key: key)!
            var previous = iv
            var out: [UInt8] = []
            for offset in stride(from: 0, to: plain.count, by: 16) {
                var block = Array(plain[offset..<offset + 16])
                for i in 0..<16 { block[i] ^= previous[i] }
                let encrypted = aes.encryptBlock(block)
                out += encrypted
                previous = encrypted
            }
            return out
        }

        let cbcIV = bytes("000102030405060708090a0b0c0d0e0f")
        // 48 bytes of content plus a full block of PKCS7 padding: decrypt must
        // hand back 48.
        let padded = (0..<48).map { UInt8($0) } + [UInt8](repeating: 16, count: 16)
        let paddedCipher = cbcEncrypt(padded, key: ctrKey, iv: cbcIV)
        emit("aes:cbc-padded-cipher", hex(paddedCipher))
        emit("aes:cbc-padded-plain", hex(AESCBC.decrypt(key: ctrKey, iv: cbcIV, ciphertext: paddedCipher)!))

        // A final byte of 0x00 is not a valid pad, and the whole plaintext must
        // come back rather than an error.
        let unpadded = [UInt8](repeating: 0x41, count: 31) + [0x00]
        let unpaddedCipher = cbcEncrypt(unpadded, key: ctrKey, iv: cbcIV)
        emit("aes:cbc-unpadded-cipher", hex(unpaddedCipher))
        emit("aes:cbc-unpadded-plain",
             hex(AESCBC.decrypt(key: ctrKey, iv: cbcIV, ciphertext: unpaddedCipher)!))
    }

    static func main() {
        dumpAES()

        for (name, base, text) in CoreParityFixtures.playlists {
            guard let baseURL = URL(string: base) else { fatalError("unparseable base \(base)") }
            emit("mediaseq:\(name)", String(M3U8Rewriter.mediaSequence(text)))
            emit("ismaster:\(name)", M3U8Rewriter.isMasterPlaylist(text) ? "1" : "0")
            emit("rewrite:\(name):0",
                 M3U8Rewriter.rewrite(text, base: baseURL, dropKey: false, transform: mediaTransform))
            emit("rewrite:\(name):1",
                 M3U8Rewriter.rewrite(text, base: baseURL, dropKey: true, transform: mediaTransform))
            emit("master:\(name)", M3U8Rewriter.rewriteMaster(text, base: baseURL, transform: masterTransform))

            let probe = M3U8Rewriter.probeMaster(text, base: baseURL)
            let variants = probe.variants.map { v in
                [v.uri,
                 v.bandwidth.map(String.init) ?? "<nil>",
                 v.width.map(String.init) ?? "<nil>",
                 v.height.map(String.init) ?? "<nil>",
                 v.codecs ?? "<nil>"].joined(separator: unitSep)
            }
            let tracks = probe.audioTracks.map { t in
                [t.groupId, t.name, t.language ?? "<nil>", t.uri ?? "<nil>"].joined(separator: unitSep)
            }
            emit("probe:\(name)",
                 variants.joined(separator: recordSep) + groupSep + tracks.joined(separator: recordSep))
        }

        for (name, inputs) in CoreParityFixtures.ffmpegCases {
            let command = FFmpegResident.command(inputs)
            let env = command.environment.keys.sorted().map { "\($0)=\(command.environment[$0]!)" }
            emit("ffargs:\(name)",
                 command.arguments.joined(separator: unitSep) + groupSep + env.joined(separator: unitSep))
        }

        for (base, ref) in resolveCases {
            let resolved = URL(string: base).flatMap {
                URL(string: ref, relativeTo: $0)?.absoluteURL.absoluteString
            }
            // A unit separator, not a tab: a tab is what separates a key from
            // its value, so the key itself must not contain one.
            emit("resolve:\(base)\(unitSep)\(ref)", resolved ?? "<nil>")
        }
    }
}
