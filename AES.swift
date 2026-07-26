import Foundation

/// Pure-Swift AES (no platform crypto): the block cipher plus the two modes the
/// CENC/HLS decryptors use — streaming CTR (CENC) and CBC decrypt (HLS AES-128).
///
/// The decryptors themselves now call the C core's rs_aes instead, on every
/// platform. This stays as the reference that port is measured against, and as
/// the home of the known-answer vectors below; CoreParitySelfTest compares the
/// two on every `restreamair selftest`. Verified byte-for-byte against
/// CommonCrypto, which is what the macOS build used before the port.
struct AES {
    private let roundKeys: [UInt8]
    private let rounds: Int

    /// key must be 16, 24, or 32 bytes (AES-128/192/256).
    init?(key: [UInt8]) {
        switch key.count {
        case 16: rounds = 10
        case 24: rounds = 12
        case 32: rounds = 14
        default: return nil
        }
        roundKeys = AES.expandKey(key, rounds: rounds)
    }

    // MARK: Tables

    private static let sbox: [UInt8] = [
        0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
        0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
        0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
        0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
        0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
        0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
        0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
        0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
        0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
        0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
        0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
        0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
        0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
        0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
        0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
        0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
    ]
    private static let invSbox: [UInt8] = {
        var inv = [UInt8](repeating: 0, count: 256)
        for i in 0..<256 { inv[Int(sbox[i])] = UInt8(i) }
        return inv
    }()
    private static let rcon: [UInt8] = [0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,0x6c,0xd8,0xab,0x4d]

    // Precomputed GF(2^8) multiplication tables so the hot path (MixColumns on
    // every block of every video segment) is table lookups, not bit loops.
    private static func mulTable(_ n: UInt8) -> [UInt8] { (0...255).map { gmul(UInt8($0), n) } }
    private static let mul2 = mulTable(2), mul3 = mulTable(3)
    private static let mul9 = mulTable(9), mul11 = mulTable(11), mul13 = mulTable(13), mul14 = mulTable(14)

    private static func gmul(_ a: UInt8, _ b: UInt8) -> UInt8 {
        var a = a, b = b, product: UInt8 = 0
        for _ in 0..<8 {
            if b & 1 != 0 { product ^= a }
            let highBit = a & 0x80
            a <<= 1
            if highBit != 0 { a ^= 0x1b }
            b >>= 1
        }
        return product
    }

    private static func expandKey(_ key: [UInt8], rounds: Int) -> [UInt8] {
        let keyWords = key.count / 4
        let totalWords = 4 * (rounds + 1)
        var words = [UInt8](repeating: 0, count: totalWords * 4)
        for i in 0..<key.count { words[i] = key[i] }
        var rconIndex = 0
        for i in keyWords..<totalWords {
            var temp = [words[(i - 1) * 4], words[(i - 1) * 4 + 1], words[(i - 1) * 4 + 2], words[(i - 1) * 4 + 3]]
            if i % keyWords == 0 {
                temp = [temp[1], temp[2], temp[3], temp[0]]           // RotWord
                temp = temp.map { sbox[Int($0)] }                     // SubWord
                temp[0] ^= rcon[rconIndex]; rconIndex += 1
            } else if keyWords > 6 && i % keyWords == 4 {
                temp = temp.map { sbox[Int($0)] }                     // AES-256 extra SubWord
            }
            for j in 0..<4 { words[i * 4 + j] = words[(i - keyWords) * 4 + j] ^ temp[j] }
        }
        return words
    }

    // MARK: Block cipher (state is column-major: index = col*4 + row)

    private func addRoundKey(_ state: inout [UInt8], round: Int) {
        for i in 0..<16 { state[i] ^= roundKeys[round * 16 + i] }
    }

    private func subBytes(_ s: inout [UInt8]) { for i in 0..<16 { s[i] = AES.sbox[Int(s[i])] } }
    private func invSubBytes(_ s: inout [UInt8]) { for i in 0..<16 { s[i] = AES.invSbox[Int(s[i])] } }

    func encryptBlock(_ input: [UInt8]) -> [UInt8] {
        var s = input
        addRoundKey(&s, round: 0)
        for round in 1..<rounds {
            subBytes(&s)
            shiftRows(&s)
            mixColumns(&s)
            addRoundKey(&s, round: round)
        }
        subBytes(&s)
        shiftRows(&s)
        addRoundKey(&s, round: rounds)
        return s
    }

    func decryptBlock(_ input: [UInt8]) -> [UInt8] {
        var s = input
        addRoundKey(&s, round: rounds)
        for round in stride(from: rounds - 1, through: 1, by: -1) {
            invShiftRows(&s)
            invSubBytes(&s)
            addRoundKey(&s, round: round)
            invMixColumns(&s)
        }
        invShiftRows(&s)
        invSubBytes(&s)
        addRoundKey(&s, round: 0)
        return s
    }

    private func shiftRows(_ s: inout [UInt8]) {
        let original = s
        for row in 1..<4 {
            for col in 0..<4 { s[col * 4 + row] = original[((col + row) % 4) * 4 + row] }
        }
    }
    private func invShiftRows(_ s: inout [UInt8]) {
        let original = s
        for row in 1..<4 {
            for col in 0..<4 { s[col * 4 + row] = original[((col + 4 - row) % 4) * 4 + row] }
        }
    }
    private func mixColumns(_ s: inout [UInt8]) {
        let m2 = AES.mul2, m3 = AES.mul3
        for c in 0..<4 {
            let a0 = s[c*4], a1 = s[c*4+1], a2 = s[c*4+2], a3 = s[c*4+3]
            s[c*4]   = m2[Int(a0)] ^ m3[Int(a1)] ^ a2 ^ a3
            s[c*4+1] = a0 ^ m2[Int(a1)] ^ m3[Int(a2)] ^ a3
            s[c*4+2] = a0 ^ a1 ^ m2[Int(a2)] ^ m3[Int(a3)]
            s[c*4+3] = m3[Int(a0)] ^ a1 ^ a2 ^ m2[Int(a3)]
        }
    }
    private func invMixColumns(_ s: inout [UInt8]) {
        let m9 = AES.mul9, m11 = AES.mul11, m13 = AES.mul13, m14 = AES.mul14
        for c in 0..<4 {
            let a0 = s[c*4], a1 = s[c*4+1], a2 = s[c*4+2], a3 = s[c*4+3]
            s[c*4]   = m14[Int(a0)] ^ m11[Int(a1)] ^ m13[Int(a2)] ^ m9[Int(a3)]
            s[c*4+1] = m9[Int(a0)]  ^ m14[Int(a1)] ^ m11[Int(a2)] ^ m13[Int(a3)]
            s[c*4+2] = m13[Int(a0)] ^ m9[Int(a1)]  ^ m14[Int(a2)] ^ m11[Int(a3)]
            s[c*4+3] = m11[Int(a0)] ^ m13[Int(a1)] ^ m9[Int(a2)]  ^ m14[Int(a3)]
        }
    }
}

/// Streaming AES-CTR, big-endian full-128-bit counter — matches CommonCrypto's
/// kCCModeCTR + kCCModeOptionCTR_BE, including keystream continuity across calls
/// (so CENC's per-subsample decrypt with a shared counter behaves identically).
final class AESCTR {
    private let aes: AES
    private var counter: [UInt8]
    private var keystream = [UInt8](repeating: 0, count: 16)
    private var keystreamPos = 16  // 16 = exhausted, generate a fresh block first

    init?(key: [UInt8], iv: [UInt8]) {
        guard let aes = AES(key: key) else { return nil }
        self.aes = aes
        var counter = [UInt8](repeating: 0, count: 16)
        for i in 0..<min(iv.count, 16) { counter[i] = iv[i] }
        self.counter = counter
    }

    /// XORs `bytes[start ..< start+length]` in place with the keystream.
    func process(_ bytes: inout [UInt8], start: Int, length: Int) {
        var i = start
        let end = start + length
        while i < end {
            if keystreamPos == 16 {
                keystream = aes.encryptBlock(counter)
                keystreamPos = 0
                incrementCounter()
            }
            bytes[i] ^= keystream[keystreamPos]
            keystreamPos += 1
            i += 1
        }
    }

    private func incrementCounter() {
        var index = 15
        while index >= 0 {
            counter[index] = counter[index] &+ 1
            if counter[index] != 0 { break }
            index -= 1
        }
    }
}

/// Known-answer self-test for the pure-Swift crypto, using published NIST/FIPS
/// vectors (no CommonCrypto involved). Run at startup on non-Apple platforms
/// and by CI, so a Linux build proves its SHA-256/PBKDF2/AES actually produce
/// the right bytes on that platform, not just that they compile.
enum CryptoSelfTest {
    private static func hex(_ b: [UInt8]) -> String { b.map { String(format: "%02x", $0) }.joined() }
    private static func bytes(_ h: String) -> [UInt8] {
        stride(from: 0, to: h.count, by: 2).map { UInt8(h[h.index(h.startIndex, offsetBy: $0)...h.index(h.startIndex, offsetBy: $0 + 1)], radix: 16)! }
    }

    /// Returns nil on success, or the name of the first failing test.
    static func firstFailure() -> String? {
        // SHA-256("abc") — FIPS 180-4
        if hex(PureCrypto.sha256(Array("abc".utf8))) != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" { return "SHA-256" }
        // HMAC-SHA256 — RFC 4231 case 1
        if hex(PureCrypto.hmacSHA256(key: [UInt8](repeating: 0x0b, count: 20), message: Array("Hi There".utf8))) != "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7" { return "HMAC-SHA256" }
        // PBKDF2-HMAC-SHA256 — RFC 7914 (P="passwd", S="salt", c=1, dkLen=64) first 32 bytes
        if hex(PureCrypto.pbkdf2SHA256(password: Array("passwd".utf8), salt: Array("salt".utf8), iterations: 1, keyLength: 32)) != "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc" { return "PBKDF2" }
        // AES-128 block — FIPS-197
        guard let a128 = AES(key: bytes("000102030405060708090a0b0c0d0e0f")) else { return "AES-128 init" }
        if hex(a128.encryptBlock(bytes("00112233445566778899aabbccddeeff"))) != "69c4e0d86a7b0430d8cdb78070b4c55a" { return "AES-128 encrypt" }
        if hex(a128.decryptBlock(bytes("69c4e0d86a7b0430d8cdb78070b4c55a"))) != "00112233445566778899aabbccddeeff" { return "AES-128 decrypt" }
        // AES-256 block — FIPS-197
        guard let a256 = AES(key: bytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")) else { return "AES-256 init" }
        if hex(a256.encryptBlock(bytes("00112233445566778899aabbccddeeff"))) != "8ea2b7ca516745bfeafc49904b496089" { return "AES-256 encrypt" }
        // AES-128-CTR — NIST SP 800-38A F.5.1
        guard let ctr = AESCTR(key: bytes("2b7e151628aed2a6abf7158809cf4f3c"), iv: bytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")) else { return "AES-CTR init" }
        var block = bytes("6bc1bee22e409f96e93d7e117393172a")
        ctr.process(&block, start: 0, length: 16)
        if hex(block) != "874d6191b620e3261bef6864990db6ce" { return "AES-CTR" }
        return nil
    }

    /// Prints the outcome and exits non-zero on failure (used by CI and startup).
    static func runOrExit() {
        if let failure = firstFailure() {
            FileHandle.standardError.write(Data("crypto self-test FAILED: \(failure)\n".utf8))
            exit(1)
        }
        print("crypto self-test: all vectors PASS")
    }
}

enum AESCBC {
    /// AES-128/256-CBC decrypt with PKCS7 padding removal — matches
    /// CommonCrypto's CCCrypt(kCCDecrypt, kCCOptionPKCS7Padding).
    static func decrypt(key: [UInt8], iv: [UInt8], ciphertext: [UInt8]) -> [UInt8]? {
        guard let aes = AES(key: key), ciphertext.count % 16 == 0, !ciphertext.isEmpty, iv.count == 16 else { return nil }
        var output = [UInt8]()
        output.reserveCapacity(ciphertext.count)
        var previous = iv
        var offset = 0
        while offset < ciphertext.count {
            let block = Array(ciphertext[offset ..< offset + 16])
            var plain = aes.decryptBlock(block)
            for i in 0..<16 { plain[i] ^= previous[i] }
            output.append(contentsOf: plain)
            previous = block
            offset += 16
        }
        // Strip PKCS7 padding.
        guard let pad = output.last, pad >= 1, pad <= 16, output.count >= Int(pad) else { return output }
        return Array(output.prefix(output.count - Int(pad)))
    }
}
