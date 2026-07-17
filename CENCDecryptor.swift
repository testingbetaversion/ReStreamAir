import Foundation
#if canImport(CommonCrypto)
import CommonCrypto
#endif

// MARK: - Minimal MP4 box parser (shared by init-segment patching and media-segment decryption)

let containerBoxTypes: Set<String> = ["moov", "trak", "mdia", "minf", "stbl", "moof", "traf", "mvex", "edts", "sinf", "schi", "udta", "meta"]

struct MP4Box {
    let type: String
    let start: Int
    let headerSize: Int
    let payloadStart: Int
    let end: Int
    let children: [MP4Box]
}

func parseBoxes(_ bytes: [UInt8], _ start: Int, _ end: Int) -> [MP4Box] {
    var boxes: [MP4Box] = []
    var offset = start
    while offset + 8 <= end {
        let size32 = readU32(bytes, offset)
        let type = readFourCC(bytes, offset + 4)
        var headerSize = 8
        var boxSize = Int(size32)
        if size32 == 1 {
            guard offset + 16 <= end else { break }
            boxSize = Int(readU64(bytes, offset + 8))
            headerSize = 16
        } else if size32 == 0 {
            boxSize = end - offset
        }
        guard boxSize >= headerSize, boxSize > 0, offset + boxSize <= end else { break }
        let payloadStart = offset + headerSize
        let boxEnd = offset + boxSize
        let children = containerBoxTypes.contains(type) ? parseBoxes(bytes, payloadStart, boxEnd) : []
        boxes.append(MP4Box(type: type, start: offset, headerSize: headerSize, payloadStart: payloadStart, end: boxEnd, children: children))
        offset = boxEnd
    }
    return boxes
}

func readU32(_ bytes: [UInt8], _ offset: Int) -> UInt32 {
    (UInt32(bytes[offset]) << 24) | (UInt32(bytes[offset + 1]) << 16) | (UInt32(bytes[offset + 2]) << 8) | UInt32(bytes[offset + 3])
}

func readU64(_ bytes: [UInt8], _ offset: Int) -> UInt64 {
    var value: UInt64 = 0
    for i in 0..<8 { value = (value << 8) | UInt64(bytes[offset + i]) }
    return value
}

func readFourCC(_ bytes: [UInt8], _ offset: Int) -> String {
    String(bytes[offset..<offset + 4].map { Character(UnicodeScalar($0)) })
}

func writeU32(_ bytes: inout [UInt8], _ offset: Int, _ value: UInt32) {
    bytes[offset] = UInt8((value >> 24) & 0xFF)
    bytes[offset + 1] = UInt8((value >> 16) & 0xFF)
    bytes[offset + 2] = UInt8((value >> 8) & 0xFF)
    bytes[offset + 3] = UInt8(value & 0xFF)
}

func writeU64(_ bytes: inout [UInt8], _ offset: Int, _ value: UInt64) {
    for i in 0..<8 { bytes[offset + i] = UInt8((value >> UInt64(8 * (7 - i))) & 0xFF) }
}

private func hexString(_ bytes: [UInt8]) -> String {
    bytes.map { String(format: "%02x", $0) }.joined()
}

enum CENCError: Error, CustomStringConvertible {
    case noKey(String)
    case parseFailed(String)
    case cryptoFailed(String)
    case unsupportedPlatform(String)

    var description: String {
        switch self {
        case .noKey(let m), .parseFailed(let m), .cryptoFailed(let m), .unsupportedPlatform(let m): return m
        }
    }
}

/// Decrypts CENC clearkey (AES-CTR, full-sample 'cenc' scheme) DASH segments,
/// and patches init segments so `encv`/`enca` sample entries play as plain
/// video/audio. Widevine/PlayReady and 'cbcs' pattern encryption are not
/// supported (see README).
final class CENCDecryptor {
    private let keys: [String: Data]
    private let singleFallbackKey: Data?
    private var defaultKIDHex: String?
    private var defaultIVSize: Int = 8

    init(keys: [String: Data]) {
        self.keys = keys
        self.singleFallbackKey = keys.count == 1 ? keys.values.first : nil
    }

    static func parseCENCKeys(_ text: String) -> [String: Data] {
        var result: [String: Data] = [:]
        let entries = text.split(whereSeparator: { $0 == "|" || $0 == "\n" || $0 == "\r" })
        for entry in entries {
            let parts = entry.split(separator: ":", maxSplits: 1)
            guard parts.count == 2, let key = dataFromHex(String(parts[1])) else { continue }
            let kid = String(parts[0]).trimmingCharacters(in: .whitespaces).lowercased()
            guard !kid.isEmpty else { continue }
            result[kid] = key
        }
        return result
    }

    static func dataFromHex(_ hex: String) -> Data? {
        let clean = hex.trimmingCharacters(in: .whitespaces)
        guard !clean.isEmpty, clean.count % 2 == 0 else { return nil }
        var data = Data(capacity: clean.count / 2)
        var index = clean.startIndex
        while index < clean.endIndex {
            let next = clean.index(index, offsetBy: 2)
            guard let byte = UInt8(clean[index..<next], radix: 16) else { return nil }
            data.append(byte)
            index = next
        }
        return data
    }

    /// Read-only scan for the probe endpoint: reports every default_KID found
    /// in `sinf/schi/tenc` boxes without patching anything, so the stream
    /// editor can tell the user which KID(s) they need a clearkey for.
    static func detectProtection(_ data: Data) -> [String] {
        let bytes = [UInt8](data)
        let top = parseBoxes(bytes, 0, bytes.count)
        guard let moov = top.first(where: { $0.type == "moov" }) else { return [] }
        var kids: [String] = []
        for trak in moov.children.filter({ $0.type == "trak" }) {
            guard let mdia = trak.children.first(where: { $0.type == "mdia" }),
                  let minf = mdia.children.first(where: { $0.type == "minf" }),
                  let stbl = minf.children.first(where: { $0.type == "stbl" }),
                  let stsd = stbl.children.first(where: { $0.type == "stsd" }),
                  stsd.payloadStart + 8 <= stsd.end else { continue }
            let sampleEntries = parseBoxes(bytes, stsd.payloadStart + 8, stsd.end)
            for entry in sampleEntries where entry.type == "encv" || entry.type == "enca" {
                let fixedSize = entry.type == "encv" ? 86 : 36
                let nestedStart = entry.start + fixedSize
                guard nestedStart < entry.end else { continue }
                let nested = parseBoxes(bytes, nestedStart, entry.end)
                guard let sinf = nested.first(where: { $0.type == "sinf" }) else { continue }
                if let schi = sinf.children.first(where: { $0.type == "schi" }),
                   let tenc = schi.children.first(where: { $0.type == "tenc" }),
                   tenc.payloadStart + 24 <= tenc.end {
                    let hex = hexString(Array(bytes[(tenc.payloadStart + 8)..<(tenc.payloadStart + 24)]))
                    if !kids.contains(hex) { kids.append(hex) }
                }
            }
        }
        return kids
    }

    // MARK: - Init segment patching (encv/enca -> original codec, strip sinf)

    func patchInitSegment(_ data: Data) -> Data {
        var bytes = [UInt8](data)
        while let found = findNextProtectedSampleEntry(bytes) {
            if defaultKIDHex == nil, let kid = found.kid {
                defaultKIDHex = hexString(kid)
            }
            if let ivSize = found.ivSize, ivSize > 0 {
                defaultIVSize = ivSize
            }
            applyPatch(&bytes, found)
        }
        // Re-parse rather than reusing offsets from above: applyPatch excises
        // each sinf, shifting everything after it.
        let top = parseBoxes(bytes, 0, bytes.count)
        if let moov = top.first(where: { $0.type == "moov" }) {
            neutralizePSSH(&bytes, in: moov.children)
        }
        return Data(bytes)
    }

    private struct FoundProtection {
        var ancestors: [(start: Int, end: Int, headerSize: Int)]
        var sinfRange: (start: Int, end: Int)
        var entryTypeOffset: Int
        var newType: [UInt8]
        var kid: [UInt8]?
        var ivSize: Int?
    }

    private func findNextProtectedSampleEntry(_ bytes: [UInt8]) -> FoundProtection? {
        let top = parseBoxes(bytes, 0, bytes.count)
        guard let moov = top.first(where: { $0.type == "moov" }) else { return nil }
        for trak in moov.children.filter({ $0.type == "trak" }) {
            guard let mdia = trak.children.first(where: { $0.type == "mdia" }),
                  let minf = mdia.children.first(where: { $0.type == "minf" }),
                  let stbl = minf.children.first(where: { $0.type == "stbl" }),
                  let stsd = stbl.children.first(where: { $0.type == "stsd" }),
                  stsd.payloadStart + 8 <= stsd.end else { continue }

            let sampleEntries = parseBoxes(bytes, stsd.payloadStart + 8, stsd.end)
            for entry in sampleEntries where entry.type == "encv" || entry.type == "enca" {
                let fixedSize = entry.type == "encv" ? 86 : 36
                let nestedStart = entry.start + fixedSize
                guard nestedStart < entry.end else { continue }
                let nested = parseBoxes(bytes, nestedStart, entry.end)
                guard let sinf = nested.first(where: { $0.type == "sinf" }),
                      let frma = sinf.children.first(where: { $0.type == "frma" }),
                      frma.payloadStart + 4 <= frma.end else { continue }

                let newType = Array(bytes[frma.payloadStart..<frma.payloadStart + 4])
                var kid: [UInt8]?
                var ivSize: Int?
                if let schi = sinf.children.first(where: { $0.type == "schi" }),
                   let tenc = schi.children.first(where: { $0.type == "tenc" }),
                   tenc.payloadStart + 24 <= tenc.end {
                    ivSize = Int(bytes[tenc.payloadStart + 7])
                    kid = Array(bytes[(tenc.payloadStart + 8)..<(tenc.payloadStart + 24)])
                }

                let ancestors: [(start: Int, end: Int, headerSize: Int)] = [
                    (moov.start, moov.end, moov.headerSize),
                    (trak.start, trak.end, trak.headerSize),
                    (mdia.start, mdia.end, mdia.headerSize),
                    (minf.start, minf.end, minf.headerSize),
                    (stbl.start, stbl.end, stbl.headerSize),
                    (stsd.start, stsd.end, stsd.headerSize),
                    (entry.start, entry.end, entry.headerSize)
                ]
                return FoundProtection(ancestors: ancestors, sinfRange: (sinf.start, sinf.end), entryTypeOffset: entry.start + 4, newType: newType, kid: kid, ivSize: ivSize)
            }
        }
        return nil
    }

    private func applyPatch(_ bytes: inout [UInt8], _ patch: FoundProtection) {
        let removed = patch.sinfRange.end - patch.sinfRange.start
        for ancestor in patch.ancestors {
            let newSize = (ancestor.end - ancestor.start) - removed
            if ancestor.headerSize == 16 {
                writeU64(&bytes, ancestor.start + 8, UInt64(newSize))
            } else {
                writeU32(&bytes, ancestor.start, UInt32(newSize))
            }
        }
        for i in 0..<4 { bytes[patch.entryTypeOffset + i] = patch.newType[i] }
        bytes.removeSubrange(patch.sinfRange.start..<patch.sinfRange.end)
    }

    // MARK: - Media segment decryption (moof/traf: senc, or saiz+saio fallback)

    private func sampleAuxInfo(traf: MP4Box, moofStart: Int, bytes: [UInt8]) -> [(iv: [UInt8], subsamples: [(Int, Int)])]? {
        if let senc = traf.children.first(where: { $0.type == "senc" }) {
            return parseSenc(senc, bytes: bytes)
        }
        if let saiz = traf.children.first(where: { $0.type == "saiz" }), let saio = traf.children.first(where: { $0.type == "saio" }) {
            return parseSaizSaio(saiz: saiz, saio: saio, moofStart: moofStart, bytes: bytes)
        }
        return nil
    }

    private func parseSenc(_ senc: MP4Box, bytes: [UInt8]) -> [(iv: [UInt8], subsamples: [(Int, Int)])]? {
        var offset = senc.payloadStart
        guard offset + 4 <= senc.end else { return nil }
        let flags = readU32(bytes, offset) & 0x00FF_FFFF
        offset += 4
        guard offset + 4 <= senc.end else { return nil }
        let sampleCount = Int(readU32(bytes, offset))
        offset += 4
        let hasSubsamples = flags & 0x0000_0002 != 0
        let ivSize = defaultIVSize > 0 ? defaultIVSize : 8

        var result: [(iv: [UInt8], subsamples: [(Int, Int)])] = []
        result.reserveCapacity(sampleCount)
        for _ in 0..<sampleCount {
            guard offset + ivSize <= senc.end else { return nil }
            let iv = Array(bytes[offset..<offset + ivSize])
            offset += ivSize
            var subsamples: [(Int, Int)] = []
            if hasSubsamples {
                guard offset + 2 <= senc.end else { return nil }
                let subCount = Int(UInt16(bytes[offset]) << 8 | UInt16(bytes[offset + 1]))
                offset += 2
                for _ in 0..<subCount {
                    guard offset + 6 <= senc.end else { return nil }
                    let clear = Int(UInt16(bytes[offset]) << 8 | UInt16(bytes[offset + 1]))
                    let encrypted = Int(readU32(bytes, offset + 2))
                    subsamples.append((clear, encrypted))
                    offset += 6
                }
            }
            result.append((iv, subsamples))
        }
        return result
    }

    private func parseSaizSaio(saiz: MP4Box, saio: MP4Box, moofStart: Int, bytes: [UInt8]) -> [(iv: [UInt8], subsamples: [(Int, Int)])]? {
        var p = saiz.payloadStart
        guard p + 4 <= saiz.end else { return nil }
        let saizFlags = readU32(bytes, p) & 0x00FF_FFFF
        p += 4
        if saizFlags & 0x0000_0001 != 0 { p += 8 }
        guard p + 5 <= saiz.end else { return nil }
        let defaultSize = Int(bytes[p])
        p += 1
        let sampleCount = Int(readU32(bytes, p))
        p += 4
        var sizes = [Int](repeating: defaultSize, count: sampleCount)
        if defaultSize == 0 {
            guard p + sampleCount <= saiz.end else { return nil }
            for i in 0..<sampleCount { sizes[i] = Int(bytes[p + i]) }
            p += sampleCount
        }

        var q = saio.payloadStart
        guard q + 4 <= saio.end else { return nil }
        let saioVersion = bytes[saio.payloadStart]
        let saioFlags = readU32(bytes, q) & 0x00FF_FFFF
        q += 4
        if saioFlags & 0x0000_0001 != 0 { q += 8 }
        guard q + 4 <= saio.end else { return nil }
        let entryCount = Int(readU32(bytes, q))
        q += 4
        guard entryCount >= 1 else { return nil }
        let firstOffset: Int
        if saioVersion == 0 {
            guard q + 4 <= saio.end else { return nil }
            firstOffset = Int(readU32(bytes, q))
        } else {
            guard q + 8 <= saio.end else { return nil }
            firstOffset = Int(readU64(bytes, q))
        }

        let ivSize = defaultIVSize > 0 ? defaultIVSize : 8
        var offset = moofStart + firstOffset
        var result: [(iv: [UInt8], subsamples: [(Int, Int)])] = []
        for size in sizes {
            guard offset + ivSize <= bytes.count else { return nil }
            let iv = Array(bytes[offset..<offset + ivSize])
            var cursor = offset + ivSize
            var subsamples: [(Int, Int)] = []
            let remaining = size - ivSize
            if remaining >= 2 {
                guard cursor + 2 <= bytes.count else { return nil }
                let subCount = Int(UInt16(bytes[cursor]) << 8 | UInt16(bytes[cursor + 1]))
                cursor += 2
                for _ in 0..<subCount {
                    guard cursor + 6 <= bytes.count else { return nil }
                    let clear = Int(UInt16(bytes[cursor]) << 8 | UInt16(bytes[cursor + 1]))
                    let encrypted = Int(readU32(bytes, cursor + 2))
                    subsamples.append((clear, encrypted))
                    cursor += 6
                }
            }
            result.append((iv, subsamples))
            offset += size
        }
        return result
    }

    /// Assumes `default-base-is-moof` (mandated by CMAF, used by every DASH
    /// source this tool targets) — sample data offsets are relative to the
    /// start of the enclosing `moof` box.
    private func sampleByteRanges(trun: MP4Box, tfhd: MP4Box?, moofStart: Int, bytes: [UInt8]) -> [(start: Int, size: Int)]? {
        var defaultSampleSize = 0
        if let tfhd {
            var p = tfhd.payloadStart
            guard p + 8 <= tfhd.end else { return nil }
            let flags = readU32(bytes, p) & 0x00FF_FFFF
            p += 8 // full-box header (4) + track_ID (4)
            if flags & 0x0000_0001 != 0 { p += 8 }
            if flags & 0x0000_0002 != 0 { p += 4 }
            if flags & 0x0000_0008 != 0 { p += 4 }
            if flags & 0x0000_0010 != 0 {
                guard p + 4 <= tfhd.end else { return nil }
                defaultSampleSize = Int(readU32(bytes, p))
                p += 4
            }
        }

        var q = trun.payloadStart
        guard q + 4 <= trun.end else { return nil }
        let trunFlags = readU32(bytes, q) & 0x00FF_FFFF
        q += 4
        guard q + 4 <= trun.end else { return nil }
        let sampleCount = Int(readU32(bytes, q))
        q += 4
        var dataOffset = 0
        if trunFlags & 0x0000_0001 != 0 {
            guard q + 4 <= trun.end else { return nil }
            dataOffset = Int(Int32(bitPattern: readU32(bytes, q)))
            q += 4
        }
        if trunFlags & 0x0000_0004 != 0 { q += 4 }

        let hasDuration = trunFlags & 0x0000_0100 != 0
        let hasSize = trunFlags & 0x0000_0200 != 0
        let hasFlags = trunFlags & 0x0000_0400 != 0
        let hasCTS = trunFlags & 0x0000_0800 != 0

        var ranges: [(start: Int, size: Int)] = []
        var cursor = moofStart + dataOffset
        for _ in 0..<sampleCount {
            if hasDuration { q += 4 }
            var size = defaultSampleSize
            if hasSize {
                guard q + 4 <= trun.end else { return nil }
                size = Int(readU32(bytes, q))
                q += 4
            }
            if hasFlags { q += 4 }
            if hasCTS { q += 4 }
            ranges.append((cursor, size))
            cursor += size
        }
        return ranges
    }

    private func resolveKey() throws -> Data {
        if let singleFallbackKey { return singleFallbackKey }
        guard let defaultKIDHex, let key = keys[defaultKIDHex] else {
            throw CENCError.noKey("No decryption key matches this segment's KID, and more than one KID:KEY pair is configured.")
        }
        return key
    }

    /// Retype a box in place to "free" — the ISO-BMFF free-space box every
    /// parser skips. Used instead of excising the box because removal would
    /// shift every following byte and force a rewrite of the enclosing
    /// traf/moof sizes *and* each trun's moof-relative data_offset. Retyping
    /// keeps the size and every offset identical, so nothing needs fixing up.
    /// Pure box edit (no crypto) — patchInitSegment uses it on every platform,
    /// so it lives outside the CommonCrypto guard.
    private func retypeToFree(_ bytes: inout [UInt8], _ box: MP4Box) {
        let free = Array("free".utf8)
        // Box layout is [4-byte size][4-byte type], so the type starts at +4.
        for i in 0..<4 { bytes[box.start + 4 + i] = free[i] }
    }

    /// `pssh` carries DRM system headers (Widevine/PlayReady). On content we've
    /// already decrypted to clear they're worse than useless: the browser sees
    /// them, fires `encrypted` media events, and blocks waiting on a CDM/EME key
    /// that is never coming. Strip them from both the init moov and every moof.
    /// (Also runs on the init segment on Linux, where decryption itself is off.)
    private func neutralizePSSH(_ bytes: inout [UInt8], in boxes: [MP4Box]) {
        for box in boxes where box.type == "pssh" { retypeToFree(&bytes, box) }
    }

    func decryptMediaSegment(_ data: Data) throws -> Data {
        var bytes = [UInt8](data)
        let top = parseBoxes(bytes, 0, bytes.count)
        guard let moof = top.first(where: { $0.type == "moof" }) else { return data }
        let key = try resolveKey()

        for traf in moof.children.filter({ $0.type == "traf" }) {
            guard let info = sampleAuxInfo(traf: traf, moofStart: moof.start, bytes: bytes), !info.isEmpty else { continue }
            guard let trun = traf.children.first(where: { $0.type == "trun" }) else { continue }
            let tfhd = traf.children.first(where: { $0.type == "tfhd" })
            guard let ranges = sampleByteRanges(trun: trun, tfhd: tfhd, moofStart: moof.start, bytes: bytes), ranges.count == info.count else {
                throw CENCError.parseFailed("Could not align senc/saiz sample count with trun sample count.")
            }
            for (index, range) in ranges.enumerated() {
                try decryptSample(&bytes, sampleStart: range.start, sampleSize: range.size, iv: info[index].iv, subsamples: info[index].subsamples, key: key)
            }
            neutralizeCENCBoxes(&bytes, traf: traf)
        }
        // pssh sits at moof level, not inside traf — this source repeats the
        // full Widevine/PlayReady headers in every single fragment.
        neutralizePSSH(&bytes, in: moof.children)
        return Data(bytes)
    }

    /// Once a fragment's samples have been decrypted in place, every trace of
    /// encryption signalling has to go with them. The init segment already
    /// advertises a clear codec (enca->mp4a / encv->avc1, sinf stripped), so a
    /// fragment that still says "these samples are encrypted" is
    /// self-contradictory and the browser refuses it:
    ///
    ///  - senc/saiz/saio (the per-sample IVs and aux-info offsets). Left in
    ///    place, Chrome still treats the samples as encrypted and dies with
    ///    PIPELINE_ERROR_DECODE.
    ///  - sbgp/sgpd with grouping_type 'seig' — the CENC sample-encryption
    ///    *group*, an independent way of marking which samples are encrypted.
    ///    This one is easy to miss: with only senc/saiz/saio cleared, Chrome
    ///    still concludes the track is encrypted from the 'seig' group and then
    ///    fails with "Sample encryption info is not available" because the senc
    ///    it now wants is gone. Both signals have to be cleared together.
    ///
    /// ffmpeg is lenient about all of this and plays the segment either way,
    /// which is why it only ever reproduced in a browser.
    private func neutralizeCENCBoxes(_ bytes: inout [UInt8], traf: MP4Box) {
        for child in traf.children {
            switch child.type {
            case "senc", "saiz", "saio":
                retypeToFree(&bytes, child)
            case "sbgp", "sgpd":
                // Only the CENC group — any other sample grouping is unrelated
                // to encryption and must be left alone.
                guard child.payloadStart + 8 <= child.end else { continue }
                if readFourCC(bytes, child.payloadStart + 4) == "seig" {
                    retypeToFree(&bytes, child)
                }
            default:
                break
            }
        }
    }

    private func decryptSample(_ bytes: inout [UInt8], sampleStart: Int, sampleSize: Int, iv: [UInt8], subsamples: [(Int, Int)], key: Data) throws {
        guard sampleSize >= 0, sampleStart >= 0, sampleStart + sampleSize <= bytes.count else {
            throw CENCError.parseFailed("Sample byte range out of bounds.")
        }
        var counterBlock = [UInt8](repeating: 0, count: 16)
        if iv.count == 16 {
            counterBlock = iv
        } else {
            counterBlock.replaceSubrange(0..<min(iv.count, 16), with: iv.prefix(16))
        }

        // Each tuple is (clearBytes, encryptedBytes). An empty subsample list
        // means full-sample encryption — the *entire* sample is encrypted, with
        // no clear prefix. This is the norm for audio (and any track whose senc
        // box carries no per-sample subsample table). The fallback therefore
        // has to be (clear: 0, encrypted: sampleSize) — decrypt the whole
        // sample. It was previously (sampleSize, 0), i.e. "all clear, nothing
        // encrypted", which silently skipped decryption entirely: subsample-
        // encrypted video happened to work (it always has a real subsample
        // table), but full-sample-encrypted audio was left ciphertext, so it
        // decoded as garbage (e.g. "AAC: Number of bands exceeds limit") and
        // no player could play the audio track.
        let clearSubsamples = subsamples.isEmpty ? [(0, sampleSize)] : subsamples

        #if canImport(CommonCrypto)
        var cryptorRef: CCCryptorRef?
        let status = key.withUnsafeBytes { keyPtr -> CCCryptorStatus in
            counterBlock.withUnsafeBytes { ivPtr in
                CCCryptorCreateWithMode(
                    CCOperation(kCCEncrypt), CCMode(kCCModeCTR), CCAlgorithm(kCCAlgorithmAES),
                    CCPadding(ccNoPadding), ivPtr.baseAddress, keyPtr.baseAddress, keyPtr.count,
                    nil, 0, 0, CCModeOptions(kCCModeOptionCTR_BE), &cryptorRef
                )
            }
        }
        guard status == kCCSuccess, let cryptor = cryptorRef else {
            throw CENCError.cryptoFailed("Could not initialize AES-CTR cryptor (status \(status)).")
        }
        defer { CCCryptorRelease(cryptor) }
        var cursor = sampleStart
        for (clear, encrypted) in clearSubsamples {
            cursor += clear
            if encrypted > 0 {
                try decryptRange(&bytes, start: cursor, length: encrypted, cryptor: cryptor)
                cursor += encrypted
            }
        }
        #else
        // Linux: pure-Swift AES-CTR (AES.swift). The cryptor is created once per
        // sample and its keystream continues across subsamples, matching
        // CommonCrypto's kCCModeOptionCTR_BE.
        guard let ctr = AESCTR(key: Array(key), iv: counterBlock) else {
            throw CENCError.cryptoFailed("Could not initialize AES-CTR cryptor.")
        }
        var cursor = sampleStart
        for (clear, encrypted) in clearSubsamples {
            cursor += clear
            if encrypted > 0 {
                guard cursor >= 0, cursor + encrypted <= bytes.count else {
                    throw CENCError.parseFailed("Subsample range out of bounds.")
                }
                ctr.process(&bytes, start: cursor, length: encrypted)
                cursor += encrypted
            }
        }
        #endif
    }

    #if canImport(CommonCrypto)
    private func decryptRange(_ bytes: inout [UInt8], start: Int, length: Int, cryptor: CCCryptorRef) throws {
        guard length > 0 else { return }
        guard start >= 0, start + length <= bytes.count else { throw CENCError.parseFailed("Subsample range out of bounds.") }
        var moved = 0
        let status = bytes.withUnsafeMutableBufferPointer { buffer -> CCCryptorStatus in
            let base = buffer.baseAddress! + start
            return CCCryptorUpdate(cryptor, base, length, base, length, &moved)
        }
        guard status == kCCSuccess else { throw CENCError.cryptoFailed("AES-CTR decrypt failed (status \(status)).") }
    }
    #endif
}
