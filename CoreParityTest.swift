import Foundation
import restream_core

/// Cross-checks the C core in `core/` against the Swift implementations it was
/// ported from. Nothing in the app calls the C code yet — the migration keeps
/// the Swift paths in place until each port has proven itself — so this is what
/// exercises it: `restreamair selftest` runs both implementations over the same
/// inputs and fails if a single byte differs.
///
/// The playlist and ffmpeg inputs come from CoreParityFixtures.swift, which is
/// generated together with the C self-test's copy so the two sides cannot be
/// fed subtly different data. The crypto inputs are generated here, from a
/// seeded sequence, so the comparison covers far more than a fixed vector list.
enum CoreParitySelfTest {

    // MARK: Support

    /// Deterministic byte source — a plain linear congruential generator, so
    /// every run compares the same inputs and a failure can be reproduced.
    private struct SeededBytes {
        private var state: UInt64

        init(seed: UInt64) { state = seed }

        mutating func next() -> UInt8 {
            state = state &* 6364136223846793005 &+ 1442695040888963407
            return UInt8((state >> 33) & 0xff)
        }

        mutating func bytes(_ count: Int) -> [UInt8] {
            return (0..<count).map { _ in next() }
        }
    }

    private static func hex(_ bytes: [UInt8]) -> String {
        return bytes.map { String(format: "%02x", $0) }.joined()
    }

    /// Owns the C strings handed across the boundary for the duration of a
    /// call, so the fixtures can be described in Swift without leaking.
    private final class CStringBag {
        private var strings: [UnsafeMutablePointer<CChar>] = []
        private var arrays: [UnsafeMutableRawPointer] = []

        func dup(_ value: String) -> UnsafePointer<CChar> {
            let pointer = strdup(value)!
            strings.append(pointer)
            return UnsafePointer(pointer)
        }

        func array(_ values: [String]) -> UnsafeMutablePointer<UnsafePointer<CChar>?>? {
            guard !values.isEmpty else { return nil }
            let buffer = UnsafeMutablePointer<UnsafePointer<CChar>?>.allocate(capacity: values.count)
            for (index, value) in values.enumerated() { buffer[index] = dup(value) }
            arrays.append(UnsafeMutableRawPointer(buffer))
            return buffer
        }

        deinit {
            for pointer in strings { free(pointer) }
            for pointer in arrays { pointer.deallocate() }
        }
    }

    /// Takes ownership of a string the C core allocated.
    private static func take(_ pointer: UnsafeMutablePointer<CChar>?) -> String? {
        guard let pointer = pointer else { return nil }
        defer { rs_free(pointer) }
        return String(cString: pointer)
    }

    // MARK: Crypto

    private static func checkCrypto(_ report: (String) -> Void) {
        var random = SeededBytes(seed: 0x5265537472656100)

        for length in [0, 1, 55, 56, 57, 63, 64, 65, 127, 128, 1000] {
            let message = random.bytes(length)
            var digest = [UInt8](repeating: 0, count: Int(RS_SHA256_DIGEST_LEN))
            message.withUnsafeBufferPointer { input in
                rs_sha256(input.baseAddress, message.count, &digest)
            }
            if digest != PureCrypto.sha256(message) {
                report("sha256 differs at length \(length): C \(hex(digest)) vs Swift \(hex(PureCrypto.sha256(message)))")
            }
        }

        // Chunked updates must land on the same digest as one call.
        let streamed = random.bytes(300)
        var context = rs_sha256_ctx()
        rs_sha256_init(&context)
        var offset = 0
        for chunk in [1, 7, 64, 65, 100, 63] where offset < streamed.count {
            let take = min(chunk, streamed.count - offset)
            streamed[offset..<(offset + take)].withUnsafeBufferPointer { slice in
                rs_sha256_update(&context, slice.baseAddress, take)
            }
            offset += take
        }
        if offset < streamed.count {
            streamed[offset...].withUnsafeBufferPointer { slice in
                rs_sha256_update(&context, slice.baseAddress, streamed.count - offset)
            }
        }
        var streamedDigest = [UInt8](repeating: 0, count: Int(RS_SHA256_DIGEST_LEN))
        rs_sha256_final(&context, &streamedDigest)
        if streamedDigest != PureCrypto.sha256(streamed) {
            report("sha256 streaming differs from Swift one-shot")
        }

        // Key lengths either side of the block size, where HMAC either pads the
        // key or hashes it down first.
        for keyLength in [0, 1, 32, 63, 64, 65, 200] {
            let key = random.bytes(keyLength)
            let message = random.bytes(37)
            var digest = [UInt8](repeating: 0, count: Int(RS_SHA256_DIGEST_LEN))
            key.withUnsafeBufferPointer { keyBuffer in
                message.withUnsafeBufferPointer { messageBuffer in
                    rs_hmac_sha256(keyBuffer.baseAddress, key.count,
                                   messageBuffer.baseAddress, message.count, &digest)
                }
            }
            if digest != PureCrypto.hmacSHA256(key: key, message: message) {
                report("hmac differs at key length \(keyLength)")
            }
        }

        for (iterations, keyLength) in [(1, 16), (2, 32), (3, 40), (17, 64), (1000, 32)] {
            let password = random.bytes(12)
            let salt = random.bytes(16)
            var derived = [UInt8](repeating: 0, count: keyLength)
            password.withUnsafeBufferPointer { passwordBuffer in
                salt.withUnsafeBufferPointer { saltBuffer in
                    _ = rs_pbkdf2_sha256(passwordBuffer.baseAddress, password.count,
                                         saltBuffer.baseAddress, salt.count,
                                         UInt32(iterations), &derived, keyLength)
                }
            }
            let expected = PureCrypto.pbkdf2SHA256(password: password, salt: salt,
                                                   iterations: iterations, keyLength: keyLength)
            if derived != expected {
                report("pbkdf2 differs at \(iterations) iterations, \(keyLength) bytes")
            }
        }

        // The parameters AuthStore actually hashes passwords with — the pair
        // that has to stay interchangeable for existing logins to keep working.
        let password = Array("correct horse battery staple".utf8)
        let salt = random.bytes(16)
        var derived = [UInt8](repeating: 0, count: 32)
        password.withUnsafeBufferPointer { passwordBuffer in
            salt.withUnsafeBufferPointer { saltBuffer in
                _ = rs_pbkdf2_sha256(passwordBuffer.baseAddress, password.count,
                                     saltBuffer.baseAddress, salt.count, 100_000, &derived, 32)
            }
        }
        if derived != PureCrypto.pbkdf2SHA256(password: password, salt: salt,
                                              iterations: 100_000, keyLength: 32) {
            report("pbkdf2 differs at the AuthStore parameters (100000 iterations, 32 bytes)")
        }
    }

    // MARK: AES

    private static func checkAES(_ report: (String) -> Void) {
        var random = SeededBytes(seed: 0x4145535f70617269)

        for keyLength in [16, 24, 32] {
            let key = random.bytes(keyLength)
            guard let swiftAES = AES(key: key) else {
                report("Swift AES rejected a \(keyLength)-byte key")
                continue
            }
            var c = rs_aes()
            if key.withUnsafeBufferPointer({ rs_aes_init(&c, $0.baseAddress, key.count) }) != 0 {
                report("C AES rejected a \(keyLength)-byte key")
                continue
            }
            for _ in 0..<8 {
                let block = random.bytes(16)
                var encrypted = [UInt8](repeating: 0, count: 16)
                block.withUnsafeBufferPointer { rs_aes_encrypt_block(&c, $0.baseAddress, &encrypted) }
                if encrypted != swiftAES.encryptBlock(block) {
                    report("AES-\(keyLength * 8) encrypt differs")
                }
                var decrypted = [UInt8](repeating: 0, count: 16)
                encrypted.withUnsafeBufferPointer { rs_aes_decrypt_block(&c, $0.baseAddress, &decrypted) }
                if decrypted != block {
                    report("AES-\(keyLength * 8) decrypt does not round-trip")
                }
            }
        }

        // CTR, in one call and split across ragged boundaries — the keystream
        // continuity CENC depends on.
        for ivLength in [0, 4, 8, 16] {
            let key = random.bytes(16)
            let iv = random.bytes(ivLength)
            let plaintext = random.bytes(200)

            var whole = plaintext
            var ctr = rs_aes_ctr()
            let initialised = key.withUnsafeBufferPointer { keyBuffer in
                iv.withUnsafeBufferPointer { ivBuffer in
                    rs_aes_ctr_init(&ctr, keyBuffer.baseAddress, key.count, ivBuffer.baseAddress, iv.count)
                }
            }
            if initialised != 0 {
                report("C AES-CTR rejected a \(ivLength)-byte IV")
                continue
            }
            whole.withUnsafeMutableBufferPointer { rs_aes_ctr_process(&ctr, $0.baseAddress, 200) }

            guard let swiftCTR = AESCTR(key: key, iv: iv) else {
                report("Swift AESCTR rejected a \(ivLength)-byte IV")
                continue
            }
            var swiftWhole = plaintext
            swiftCTR.process(&swiftWhole, start: 0, length: 200)
            if whole != swiftWhole {
                report("AES-CTR differs with a \(ivLength)-byte IV")
            }

            var split = plaintext
            var splitCTR = rs_aes_ctr()
            _ = key.withUnsafeBufferPointer { keyBuffer in
                iv.withUnsafeBufferPointer { ivBuffer in
                    rs_aes_ctr_init(&splitCTR, keyBuffer.baseAddress, key.count,
                                    ivBuffer.baseAddress, iv.count)
                }
            }
            var position = 0
            for chunk in [1, 15, 33, 64, 87] {
                let take = min(chunk, 200 - position)
                if take <= 0 { break }
                split.withUnsafeMutableBufferPointer {
                    rs_aes_ctr_process(&splitCTR, $0.baseAddress! + position, take)
                }
                position += take
            }
            if split != whole {
                report("AES-CTR split processing differs from a single call")
            }
        }

        // CBC: a valid pad, an invalid pad (which must return everything), and
        // the inputs both implementations have to reject.
        for keyLength in [16, 32] {
            let key = random.bytes(keyLength)
            let iv = random.bytes(16)
            guard let aes = AES(key: key) else { continue }

            for finalByte in [UInt8(16), UInt8(0), UInt8(4), UInt8(200)] {
                var plaintext = random.bytes(31)
                plaintext.append(finalByte)
                var ciphertext: [UInt8] = []
                var previous = iv
                for offset in stride(from: 0, to: plaintext.count, by: 16) {
                    var block = Array(plaintext[offset..<(offset + 16)])
                    for i in 0..<16 { block[i] ^= previous[i] }
                    let encrypted = aes.encryptBlock(block)
                    ciphertext += encrypted
                    previous = encrypted
                }

                var out = [UInt8](repeating: 0, count: ciphertext.count)
                var outLength = 0
                let status = key.withUnsafeBufferPointer { keyBuffer in
                    iv.withUnsafeBufferPointer { ivBuffer in
                        ciphertext.withUnsafeBufferPointer { cipherBuffer in
                            rs_aes_cbc_decrypt(keyBuffer.baseAddress, key.count,
                                               ivBuffer.baseAddress, iv.count,
                                               cipherBuffer.baseAddress, ciphertext.count,
                                               &out, &outLength)
                        }
                    }
                }
                let expected = AESCBC.decrypt(key: key, iv: iv, ciphertext: ciphertext)
                if status != 0 {
                    report("C AES-CBC failed where Swift succeeded (final byte \(finalByte))")
                } else if Array(out.prefix(outLength)) != expected {
                    report("AES-CBC differs with final plaintext byte \(finalByte)")
                }
            }

            // Rejections must match too.
            var out = [UInt8](repeating: 0, count: 64)
            var outLength = 0
            let misaligned = random.bytes(31)
            let rejected = key.withUnsafeBufferPointer { keyBuffer in
                iv.withUnsafeBufferPointer { ivBuffer in
                    misaligned.withUnsafeBufferPointer { cipherBuffer in
                        rs_aes_cbc_decrypt(keyBuffer.baseAddress, key.count,
                                           ivBuffer.baseAddress, iv.count,
                                           cipherBuffer.baseAddress, misaligned.count,
                                           &out, &outLength)
                    }
                }
            }
            let swiftRejected = AESCBC.decrypt(key: key, iv: iv, ciphertext: misaligned) == nil
            if (rejected != 0) != swiftRejected {
                report("AES-CBC disagrees about rejecting a misaligned ciphertext")
            }
        }
    }

    // MARK: Playlists

    private static func kindName(_ kind: M3U8Rewriter.LineKind) -> String {
        switch kind {
        case .key: return "key"
        case .map: return "map"
        case .segment: return "seg"
        }
    }

    private static func swiftMediaTransform(_ uri: String, _ kind: M3U8Rewriter.LineKind, _ seq: Int?) -> String {
        return "/p/\(kindName(kind))?u=\(uri)" + (seq.map { "&s=\($0)" } ?? "")
    }

    private static func checkPlaylists(_ report: (String) -> Void) {
        // C callbacks cannot capture, and these do not need to: they are pure
        // functions of their arguments, matching swiftMediaTransform above.
        let cMediaTransform: rs_m3u8_transform_fn = { _, uri, kind, seq in
            let name: String
            switch kind {
            case RS_M3U8_LINE_KEY: name = "key"
            case RS_M3U8_LINE_MAP: name = "map"
            default: name = "seg"
            }
            let text = "/p/\(name)?u=\(String(cString: uri!))" + (seq >= 0 ? "&s=\(seq)" : "")
            return strdup(text)
        }
        let cMasterTransform: rs_m3u8_master_transform_fn = { _, uri in
            return strdup("/m?u=\(String(cString: uri!))")
        }

        for (name, base, text) in CoreParityFixtures.playlists {
            guard let baseURL = URL(string: base) else {
                report("fixture \(name) has an unparseable base URL")
                continue
            }

            let cSequence = rs_m3u8_media_sequence(text)
            if cSequence != Int64(M3U8Rewriter.mediaSequence(text)) {
                report("\(name): media sequence differs (C \(cSequence))")
            }
            if rs_m3u8_is_master(text) != M3U8Rewriter.isMasterPlaylist(text) {
                report("\(name): master detection differs")
            }

            for dropKey in [false, true] {
                let expected = M3U8Rewriter.rewrite(text, base: baseURL, dropKey: dropKey,
                                                   transform: swiftMediaTransform)
                let actual = take(rs_m3u8_rewrite(text, base, dropKey, cMediaTransform, nil))
                if actual != expected {
                    report("\(name): rewrite(dropKey: \(dropKey)) differs\n"
                           + firstDifference(expected: expected, actual: actual))
                }
            }

            let expectedMaster = M3U8Rewriter.rewriteMaster(text, base: baseURL) { "/m?u=\($0)" }
            let actualMaster = take(rs_m3u8_rewrite_master(text, base, cMasterTransform, nil))
            if actualMaster != expectedMaster {
                report("\(name): rewriteMaster differs\n"
                       + firstDifference(expected: expectedMaster, actual: actualMaster))
            }

            checkProbe(name: name, text: text, base: base, baseURL: baseURL, report: report)
        }
    }

    private static func checkProbe(name: String, text: String, base: String, baseURL: URL,
                                   report: (String) -> Void) {
        var probe = rs_m3u8_probe()
        guard rs_m3u8_probe_master(text, base, &probe) == 0 else {
            report("\(name): C probeMaster failed")
            return
        }
        defer { rs_m3u8_probe_dispose(&probe) }

        let expected = M3U8Rewriter.probeMaster(text, base: baseURL)
        if probe.variant_count != expected.variants.count {
            report("\(name): variant count differs (C \(probe.variant_count), Swift \(expected.variants.count))")
            return
        }
        if probe.audio_count != expected.audioTracks.count {
            report("\(name): audio track count differs (C \(probe.audio_count), Swift \(expected.audioTracks.count))")
            return
        }

        for index in 0..<expected.variants.count {
            let c = probe.variants[index]
            let swift = expected.variants[index]
            if String(cString: c.uri) != swift.uri {
                report("\(name): variant \(index) URI differs")
            }
            if c.bandwidth != Int64(swift.bandwidth ?? -1) {
                report("\(name): variant \(index) bandwidth differs")
            }
            if c.width != Int64(swift.width ?? -1) || c.height != Int64(swift.height ?? -1) {
                report("\(name): variant \(index) resolution differs")
            }
            let codecs = c.codecs.map { String(cString: $0) }
            if codecs != swift.codecs {
                report("\(name): variant \(index) codecs differ")
            }
        }

        for index in 0..<expected.audioTracks.count {
            let c = probe.audio_tracks[index]
            let swift = expected.audioTracks[index]
            if String(cString: c.group_id) != swift.groupId || String(cString: c.name) != swift.name {
                report("\(name): audio track \(index) identity differs")
            }
            if c.language.map({ String(cString: $0) }) != swift.language {
                report("\(name): audio track \(index) language differs")
            }
            if c.uri.map({ String(cString: $0) }) != swift.uri {
                report("\(name): audio track \(index) URI differs")
            }
        }
    }

    /// Points at the first differing line, since a whole rewritten playlist is
    /// unreadable in a failure message.
    private static func firstDifference(expected: String, actual: String?) -> String {
        guard let actual = actual else { return "      C returned nothing" }
        let expectedLines = expected.split(separator: "\n", omittingEmptySubsequences: false)
        let actualLines = actual.split(separator: "\n", omittingEmptySubsequences: false)
        for index in 0..<max(expectedLines.count, actualLines.count) {
            let left = index < expectedLines.count ? String(expectedLines[index]) : "<missing>"
            let right = index < actualLines.count ? String(actualLines[index]) : "<missing>"
            if left != right {
                return "      line \(index + 1)\n        Swift: \(left)\n        C:     \(right)"
            }
        }
        return "      lines match; the difference is in the trailing bytes"
    }

    // MARK: ffmpeg arguments

    private static func checkFFmpegArguments(_ report: (String) -> Void) {
        for (name, swiftInputs) in CoreParityFixtures.ffmpegCases {
            let bag = CStringBag()
            var inputs = rs_ffargs_inputs()
            inputs.source_url = bag.dup(swiftInputs.sourceURL)
            inputs.kind = bag.dup(swiftInputs.kind)
            inputs.input_mode = bag.dup(swiftInputs.inputMode)
            inputs.output_mode = bag.dup(swiftInputs.outputMode)
            inputs.output_target = bag.dup(swiftInputs.outputTarget)
            inputs.decryption_keys = bag.dup(swiftInputs.decryptionKeys)
            inputs.headers = bag.dup(swiftInputs.headers)
            inputs.proxy = bag.dup(swiftInputs.proxy)
            inputs.segment_url_params = bag.dup(swiftInputs.segmentUrlParams)

            let ids = swiftInputs.representationIds
            inputs.representation_ids = UnsafePointer(bag.array(ids))
            inputs.representation_id_count = ids.count
            // The Swift dictionary becomes parallel key and value arrays, in a
            // fixed order so the C lookup sees the same typing.
            let typedIds = swiftInputs.representationTypes.keys.sorted()
            inputs.rep_type_ids = UnsafePointer(bag.array(typedIds))
            inputs.rep_type_values = UnsafePointer(bag.array(typedIds.map { swiftInputs.representationTypes[$0]! }))
            inputs.rep_type_count = typedIds.count

            inputs.temp_dir = bag.dup(swiftInputs.tempDir.path)
            inputs.output_playlist = bag.dup(swiftInputs.outputPlaylist.path)
            inputs.playlist_segments = Int32(swiftInputs.playlistSegments)
            inputs.segment_seconds = Int32(swiftInputs.segmentSeconds)

            var command = rs_ffargs_command()
            guard rs_ffargs_build(&inputs, &command) == 0 else {
                report("\(name): C rs_ffargs_build failed")
                continue
            }
            defer { rs_ffargs_command_dispose(&command) }

            let expected = FFmpegResident.command(swiftInputs)
            var actualArguments: [String] = []
            for index in 0..<command.argc {
                actualArguments.append(String(cString: command.argv[index]!))
            }
            if actualArguments != expected.arguments {
                report("\(name): arguments differ\n" + argumentDifference(expected.arguments, actualArguments))
            }

            var actualEnvironment: [String: String] = [:]
            for index in 0..<command.env_count {
                actualEnvironment[String(cString: command.env_keys[index]!)] =
                    String(cString: command.env_values[index]!)
            }
            if actualEnvironment != expected.environment {
                report("\(name): environment differs (C \(actualEnvironment), Swift \(expected.environment))")
            }
        }
    }

    private static func argumentDifference(_ expected: [String], _ actual: [String]) -> String {
        for index in 0..<max(expected.count, actual.count) {
            let left = index < expected.count ? expected[index] : "<missing>"
            let right = index < actual.count ? actual[index] : "<missing>"
            if left != right {
                return "      argument \(index)\n        Swift: \(left)\n        C:     \(right)"
            }
        }
        return "      argument lists match"
    }

    // MARK: Call sites backed by the C core

    private static func cbcEncrypt(_ plaintext: [UInt8], key: [UInt8], iv: [UInt8]) -> [UInt8]? {
        guard let aes = AES(key: key), plaintext.count % 16 == 0 else { return nil }
        var previous = iv
        var out: [UInt8] = []
        for offset in stride(from: 0, to: plaintext.count, by: 16) {
            var block = Array(plaintext[offset..<(offset + 16)])
            for i in 0..<16 { block[i] ^= previous[i] }
            let encrypted = aes.encryptBlock(block)
            out += encrypted
            previous = encrypted
        }
        return out
    }

    /// HLSDecryptor now decrypts through the C core rather than CommonCrypto or
    /// the pure-Swift fallback, so check the whole call site end to end: a real
    /// PKCS7-padded segment must come back byte-identical, the RFC 8216
    /// sequence-derived IV must work, and a segment whose padding is wrong must
    /// still be rejected rather than served.
    private static func checkHLSDecryptor(_ report: (String) -> Void) {
        var random = SeededBytes(seed: 0x484c5344454300)
        let key = random.bytes(16)
        let iv = random.bytes(16)

        for payloadLength in [1, 15, 16, 17, 100, 4096] {
            let plaintext = random.bytes(payloadLength)
            let padding = 16 - (plaintext.count % 16)
            let padded = plaintext + [UInt8](repeating: UInt8(padding), count: padding)
            guard let ciphertext = cbcEncrypt(padded, key: key, iv: iv) else {
                report("HLSDecryptor: could not build a test segment")
                continue
            }
            do {
                let clear = try HLSDecryptor.decryptSegment(Data(ciphertext), keyHex: hex(key),
                                                            ivHex: hex(iv), sequence: 0)
                if Array(clear) != plaintext {
                    report("HLSDecryptor: \(payloadLength)-byte segment did not round-trip")
                }
            } catch {
                report("HLSDecryptor: \(payloadLength)-byte segment failed to decrypt (\(error))")
            }
        }

        // No IV attribute: the IV is the media sequence number, big-endian.
        var sequenceIV = [UInt8](repeating: 0, count: 16)
        sequenceIV[15] = 7
        let plaintext = random.bytes(32)
        let padded = plaintext + [UInt8](repeating: 16, count: 16)
        if let ciphertext = cbcEncrypt(padded, key: key, iv: sequenceIV) {
            do {
                let clear = try HLSDecryptor.decryptSegment(Data(ciphertext), keyHex: hex(key),
                                                            ivHex: "", sequence: 7)
                if Array(clear) != plaintext {
                    report("HLSDecryptor: sequence-derived IV did not round-trip")
                }
            } catch {
                report("HLSDecryptor: sequence-derived IV failed (\(error))")
            }
        }

        // A final byte outside 1...16 is not a valid pad. The C decrypt is
        // deliberately lenient there, so the call site has to be the strict one.
        var corrupt = random.bytes(31)
        corrupt.append(0)
        if let ciphertext = cbcEncrypt(corrupt, key: key, iv: iv) {
            do {
                _ = try HLSDecryptor.decryptSegment(Data(ciphertext), keyHex: hex(key),
                                                    ivHex: hex(iv), sequence: 0)
                report("HLSDecryptor: accepted a segment with invalid PKCS7 padding")
            } catch {
                // Expected.
            }
        }

        // Malformed inputs must still be refused.
        do {
            _ = try HLSDecryptor.decryptSegment(Data(repeating: 0, count: 31), keyHex: hex(key),
                                                ivHex: hex(iv), sequence: 0)
            report("HLSDecryptor: accepted a ciphertext that is not a multiple of the block size")
        } catch {
            // Expected.
        }
        do {
            _ = try HLSDecryptor.decryptSegment(Data(repeating: 0, count: 32), keyHex: "abcd",
                                                ivHex: hex(iv), sequence: 0)
            report("HLSDecryptor: accepted a malformed key")
        } catch {
            // Expected.
        }
    }

    // MARK: CENC decryptor
    //
    // One segment file is not one fragment: CMAF sources ship several
    // moof/mdat pairs per segment, and both decryptors used to stop after the
    // first moof — so half of every segment reached the player as ciphertext.
    // It was invisible to every existing check because the container still
    // parsed and the timestamps were still right; only the decoder saw it, as
    // h264 failing at "MB 0 0" and AAC decoding to noise.
    //
    // Builds a two-fragment segment (fragment 0 subsample-encrypted the way
    // video is, fragment 1 full-sample the way audio is) and requires both
    // implementations to return the original plaintext, and to agree.

    /// Assembles one moof+mdat pair with its samples already encrypted, and
    /// returns it alongside the plaintext the decryptors have to recover.
    private static func cencFragment(sequence: Int, samples: [[UInt8]], ivs: [[UInt8]],
                                     clearPrefix: Int?, key: [UInt8]) -> (bytes: [UInt8], plaintext: [UInt8])? {
        func box(_ type: String, _ payload: [UInt8]) -> [UInt8] {
            var out = [UInt8](repeating: 0, count: 4)
            writeU32(&out, 0, UInt32(8 + payload.count))
            return out + Array(type.utf8) + payload
        }
        func u32(_ value: UInt32) -> [UInt8] {
            var out = [UInt8](repeating: 0, count: 4); writeU32(&out, 0, value); return out
        }
        let subsampled = clearPrefix != nil
        let sampleLength = samples[0].count

        let mfhd = box("mfhd", u32(0) + u32(UInt32(sequence)))
        let tfhd = box("tfhd", u32(0x020000) + u32(1))   // default-base-is-moof, no optional fields
        var tfdtPayload = u32(0x0100_0000) + [UInt8](repeating: 0, count: 8)
        writeU64(&tfdtPayload, 4, UInt64(sequence) * 10000)
        let tfdt = box("tfdt", tfdtPayload)

        // trun: data-offset-present | sample-size-present. The offset itself is
        // moof-relative, so it can only be filled in once the moof is sized.
        var trunPayload = u32(0x000201) + u32(UInt32(samples.count)) + u32(0)
        for sample in samples { trunPayload += u32(UInt32(sample.count)) }
        let trun = box("trun", trunPayload)

        let auxSize = UInt8(ivs[0].count + (subsampled ? 8 : 0))
        let saiz = box("saiz", u32(0) + [auxSize] + u32(UInt32(samples.count)))
        let saio = box("saio", u32(0) + u32(1) + u32(0))

        var sencPayload = u32(subsampled ? 0x000002 : 0) + u32(UInt32(samples.count))
        for (index, _) in samples.enumerated() {
            sencPayload += ivs[index]
            if let clear = clearPrefix {
                sencPayload += [0, 1]                                        // subsample_count
                sencPayload += [UInt8(clear >> 8), UInt8(clear & 0xFF)]      // clear bytes
                sencPayload += u32(UInt32(sampleLength - clear))             // protected bytes
            }
        }
        let senc = box("senc", sencPayload)

        var moof = box("moof", mfhd + box("traf", tfhd + tfdt + trun + saiz + saio + senc))
        // The trun payload starts 8 bytes into the trun box; data_offset is its
        // third field. Locate the box rather than hard-coding an offset.
        guard let trunStart = (0..<(moof.count - 8)).first(where: {
            Array(moof[($0 + 4)..<($0 + 8)]) == Array("trun".utf8)
        }) else { return nil }
        writeU32(&moof, trunStart + 8 + 8, UInt32(moof.count + 8 - 0))

        var plaintext: [UInt8] = []
        var payload: [UInt8] = []
        for (index, sample) in samples.enumerated() {
            plaintext += sample
            let clear = clearPrefix ?? 0
            var encrypted = Array(sample[clear...])
            var context = rs_aes_ctr()
            let status = key.withUnsafeBufferPointer { keyPtr in
                ivs[index].withUnsafeBufferPointer { ivPtr in
                    rs_aes_ctr_init(&context, keyPtr.baseAddress, key.count, ivPtr.baseAddress, ivs[index].count)
                }
            }
            guard status == 0 else { return nil }
            encrypted.withUnsafeMutableBufferPointer { buffer in
                rs_aes_ctr_process(&context, buffer.baseAddress, buffer.count)
            }
            payload += Array(sample[..<clear]) + encrypted
        }
        return (moof + box("mdat", payload), plaintext)
    }

    private static func checkCENCDecryptor(_ report: (String) -> Void) {
        var random = SeededBytes(seed: 0x43454e4300)
        let key = random.bytes(16)

        var segment: [UInt8] = []
        var plaintext: [UInt8] = []
        var payloadOffsets: [Int] = []
        for fragment in 0..<2 {
            let subsampled = fragment == 0
            let samples = (0..<3).map { _ in random.bytes(96) }
            let ivs = (0..<samples.count).map { [UInt8](repeating: UInt8(fragment * 16 + $0), count: 8) }
            guard let built = cencFragment(sequence: fragment + 1, samples: samples, ivs: ivs,
                                           clearPrefix: subsampled ? 16 : nil, key: key) else {
                report("CENCDecryptor: could not build fragment \(fragment)")
                return
            }
            // The mdat payload is the last `plaintext.count` bytes of the fragment.
            payloadOffsets.append(segment.count + built.bytes.count - built.plaintext.count)
            segment += built.bytes
            plaintext += built.plaintext
        }
        guard segment != plaintext else {
            report("CENCDecryptor: test segment was not actually encrypted")
            return
        }

        let decryptor = CENCDecryptor(keys: ["00": Data(key)])
        let swiftOutput: [UInt8]
        do {
            swiftOutput = [UInt8](try decryptor.decryptMediaSegment(Data(segment)))
        } catch {
            report("CENCDecryptor: Swift decrypt failed (\(error))")
            return
        }

        var cOutputLength = 0
        let cOutput: [UInt8]? = segment.withUnsafeBufferPointer { segmentPtr in
            key.withUnsafeBufferPointer { keyPtr in
                guard let raw = rs_cenc_decrypt_segment(segmentPtr.baseAddress, segment.count,
                                                        &cOutputLength, keyPtr.baseAddress, 8) else { return nil }
                defer { rs_free(raw) }
                return Array(UnsafeBufferPointer(start: raw, count: cOutputLength))
            }
        }
        guard let cOutput else {
            report("CENCDecryptor: C decrypt returned nothing")
            return
        }

        if swiftOutput != cOutput {
            report("CENCDecryptor: C and Swift disagree on a two-fragment segment")
        }

        // Every fragment's samples must come back clear — not just the first.
        var recovered: [UInt8] = []
        for (fragment, offset) in payloadOffsets.enumerated() {
            let length = plaintext.count / payloadOffsets.count
            guard offset + length <= swiftOutput.count else {
                report("CENCDecryptor: fragment \(fragment) payload is out of range")
                return
            }
            recovered += Array(swiftOutput[offset..<(offset + length)])
        }
        if recovered != plaintext {
            report("CENCDecryptor: a fragment was left encrypted (only the first moof is decrypted)")
        }

        // Signalling left behind on decrypted samples is self-contradictory,
        // and Chrome rejects the fragment outright.
        for (fragment, moof) in parseBoxes(swiftOutput, 0, swiftOutput.count)
            .filter({ $0.type == "moof" }).enumerated() {
            for traf in moof.children where traf.type == "traf" {
                for child in traf.children where ["senc", "saiz", "saio"].contains(child.type) {
                    report("CENCDecryptor: fragment \(fragment) still carries a \(child.type) box")
                }
            }
        }
    }

    // MARK: Provider-script value encoding
    //
    // Not a C-vs-Swift comparison like the rest of this file — it lives here
    // because `restreamair selftest` is the one harness that runs on every
    // build, and getting this wrong is silent and nasty: a mis-detected value
    // reaches a provider script as binary garbage, or a password leaks into the
    // process list unencoded.
    private static func checkScriptValueEncoding(_ report: (String) -> Void) {
        // Values safe to pass literally must be left completely alone, or every
        // existing script breaks.
        // URLs and shell metacharacters stay readable: there's no shell between
        // us and the interpreter, so encoding them would buy nothing.
        for plain in ["login", "manifest", "id=123", "me@example.com", "9100", "abc-def_123",
                      "https://cdn.example.com/a/b.mpd?token=xyz&region=eu",
                      "semi;colon", "pipe|char", "dollar$sign", "star*glob", ""] {
            let encoded = ScriptRunner.encodeValue(plain)
            if encoded != plain {
                report("script encoding: '\(plain)' should pass through untouched, got '\(encoded)'")
            }
        }

        // Values that can't survive argv must be encoded, and must come back
        // exactly as they went in.
        for awkward in ["p@ss w0rd!", "a b", "quote\"inside", "back\\slash", "it's",
                        "new\nline", "tab\there", "café", "b64:notreally"] {
            let encoded = ScriptRunner.encodeValue(awkward)
            if !encoded.hasPrefix(ScriptRunner.base64Prefix) {
                report("script encoding: '\(awkward)' should have been encoded")
            }
            if ScriptRunner.decodeValue(encoded) != awkward {
                report("script encoding: '\(awkward)' did not round-trip")
            }
        }

        // A password is encoded whatever it contains — it must never be
        // readable in `ps` output.
        let simplePassword = "hunter2"
        let forced = ScriptRunner.encodeValue(simplePassword, force: true)
        if !forced.hasPrefix(ScriptRunner.base64Prefix) || ScriptRunner.decodeValue(forced) != simplePassword {
            report("script encoding: a forced value must be encoded and round-trip")
        }

        // The ambiguous case: short words that are coincidentally valid base64
        // must NOT be decoded, or a plain value silently turns into garbage.
        for coincidence in ["test", "abcd", "ABCD", "1234", "true", "live"] {
            if ScriptRunner.decodeValue(coincidence) != coincidence {
                report("script encoding: '\(coincidence)' is valid base64 by accident and must stay plain, "
                       + "got '\(ScriptRunner.decodeValue(coincidence))'")
            }
        }

        // A script that base64s its output without the marker is still
        // understood, as long as the reading is unambiguous.
        let bare = Data("a value with spaces".utf8).base64EncodedString()
        if ScriptRunner.decodeValue(bare) != "a value with spaces" {
            report("script encoding: bare base64 from a script should decode")
        }

        // splitParams encodes each token's value, so a token can never reach the
        // script carrying a space.
        for token in ScriptRunner.splitParams("id=123 region=us title=b64:TXkgU2hvdw==") {
            if token.contains(" ") {
                report("script encoding: split param '\(token)' contains a space")
            }
        }
        if ScriptRunner.splitParams("id=123") != ["id=123"] {
            report("script encoding: a simple param should be untouched by splitParams")
        }
    }

    // MARK: Entry points

    /// Returns every difference found, or an empty array when the two
    /// implementations agree.
    static func differences() -> [String] {
        var found: [String] = []
        let report: (String) -> Void = { found.append($0) }
        checkCrypto(report)
        checkAES(report)
        checkPlaylists(report)
        checkFFmpegArguments(report)
        checkHLSDecryptor(report)
        checkCENCDecryptor(report)
        checkScriptValueEncoding(report)
        return found
    }

    /// Prints the outcome and exits non-zero on any difference.
    static func runOrExit() {
        let found = differences()
        guard found.isEmpty else {
            FileHandle.standardError.write(Data("C core parity test FAILED:\n".utf8))
            for difference in found {
                FileHandle.standardError.write(Data("  - \(difference)\n".utf8))
            }
            exit(1)
        }
        print("C core parity test: C and Swift agree")
    }
}
