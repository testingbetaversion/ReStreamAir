import Foundation

/// Pure-Swift SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256, with no platform
/// crypto dependency.
///
/// Nothing in the app calls this any more — AuthStore hashes passwords with the
/// C core's rs_crypto, on every platform. It stays as the reference the port is
/// measured against: CoreParitySelfTest runs both over the same inputs on every
/// `restreamair selftest` and fails on any difference, which is what keeps a
/// password hashed by an older build (by this code on Linux, or by CommonCrypto
/// on macOS) verifying against the C implementation today.
enum PureCrypto {
    // MARK: SHA-256 (FIPS 180-4)

    private static let k: [UInt32] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ]

    static let blockSize = 64
    static let digestSize = 32

    private static func rotr(_ x: UInt32, _ n: UInt32) -> UInt32 { (x >> n) | (x << (32 - n)) }

    static func sha256(_ message: [UInt8]) -> [UInt8] {
        var h: [UInt32] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]

        // Padding: append 0x80, then zeros, then 64-bit big-endian bit length.
        var msg = message
        let bitLength = UInt64(message.count) * 8
        msg.append(0x80)
        while msg.count % 64 != 56 { msg.append(0) }
        for shift in stride(from: 56, through: 0, by: -8) { msg.append(UInt8((bitLength >> UInt64(shift)) & 0xff)) }

        var w = [UInt32](repeating: 0, count: 64)
        for chunkStart in stride(from: 0, to: msg.count, by: 64) {
            for i in 0..<16 {
                let j = chunkStart + i * 4
                w[i] = (UInt32(msg[j]) << 24) | (UInt32(msg[j + 1]) << 16) | (UInt32(msg[j + 2]) << 8) | UInt32(msg[j + 3])
            }
            for i in 16..<64 {
                let s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)
                let s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10)
                w[i] = w[i - 16] &+ s0 &+ w[i - 7] &+ s1
            }

            var a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7]
            for i in 0..<64 {
                let s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)
                let ch = (e & f) ^ (~e & g)
                let t1 = hh &+ s1 &+ ch &+ k[i] &+ w[i]
                let s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)
                let maj = (a & b) ^ (a & c) ^ (b & c)
                let t2 = s0 &+ maj
                hh = g; g = f; f = e; e = d &+ t1; d = c; c = b; b = a; a = t1 &+ t2
            }
            h[0] = h[0] &+ a; h[1] = h[1] &+ b; h[2] = h[2] &+ c; h[3] = h[3] &+ d
            h[4] = h[4] &+ e; h[5] = h[5] &+ f; h[6] = h[6] &+ g; h[7] = h[7] &+ hh
        }

        var out = [UInt8]()
        out.reserveCapacity(32)
        for value in h {
            out.append(UInt8((value >> 24) & 0xff)); out.append(UInt8((value >> 16) & 0xff))
            out.append(UInt8((value >> 8) & 0xff)); out.append(UInt8(value & 0xff))
        }
        return out
    }

    // MARK: HMAC-SHA256 (RFC 2104)

    static func hmacSHA256(key: [UInt8], message: [UInt8]) -> [UInt8] {
        var k = key
        if k.count > blockSize { k = sha256(k) }
        if k.count < blockSize { k += [UInt8](repeating: 0, count: blockSize - k.count) }
        let outerPad = k.map { $0 ^ 0x5c }
        let innerPad = k.map { $0 ^ 0x36 }
        return sha256(outerPad + sha256(innerPad + message))
    }

    // MARK: PBKDF2-HMAC-SHA256 (RFC 8018)

    /// Derives `keyLength` bytes. Matches CommonCrypto's
    /// CCKeyDerivationPBKDF(kCCPBKDF2, …, kCCPRFHmacAlgSHA256, …) for the same
    /// password/salt/iterations, so hashes are interchangeable between the
    /// macOS and Linux builds.
    static func pbkdf2SHA256(password: [UInt8], salt: [UInt8], iterations: Int, keyLength: Int) -> [UInt8] {
        var derived = [UInt8]()
        var blockIndex: UInt32 = 1
        while derived.count < keyLength {
            var block = salt
            block.append(UInt8((blockIndex >> 24) & 0xff)); block.append(UInt8((blockIndex >> 16) & 0xff))
            block.append(UInt8((blockIndex >> 8) & 0xff)); block.append(UInt8(blockIndex & 0xff))
            var u = hmacSHA256(key: password, message: block)
            var t = u
            for _ in 1..<iterations {
                u = hmacSHA256(key: password, message: u)
                for i in 0..<t.count { t[i] ^= u[i] }
            }
            derived += t
            blockIndex += 1
        }
        return Array(derived.prefix(keyLength))
    }
}
