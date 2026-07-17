import Foundation
#if canImport(CommonCrypto)
import CommonCrypto
#endif

enum HLSDecryptError: Error, CustomStringConvertible {
    case invalidKey(String)
    case cryptoFailed(String)
    case unsupportedPlatform(String)

    var description: String {
        switch self {
        case .invalidKey(let m), .cryptoFailed(let m), .unsupportedPlatform(let m): return m
        }
    }
}

/// Decrypts standard HLS `#EXT-X-KEY:METHOD=AES-128` segments (AES-128-CBC,
/// PKCS7 padding) server-side, so the panel can serve already-clear segments
/// to players that don't handle encrypted HLS themselves.
enum HLSDecryptor {
    static func decryptSegment(_ data: Data, keyHex: String, ivHex: String, sequence: Int) throws -> Data {
        #if canImport(CommonCrypto)
        guard let key = CENCDecryptor.dataFromHex(keyHex), key.count == 16 else {
            throw HLSDecryptError.invalidKey("HLS key must be 32 hex characters (16 bytes).")
        }
        let iv: Data
        if ivHex.trimmingCharacters(in: .whitespaces).isEmpty {
            iv = sequenceIV(sequence)
        } else if let parsed = CENCDecryptor.dataFromHex(ivHex), parsed.count == 16 {
            iv = parsed
        } else {
            throw HLSDecryptError.invalidKey("HLS IV must be 32 hex characters (16 bytes) when provided.")
        }

        var outBuffer = [UInt8](repeating: 0, count: data.count + kCCBlockSizeAES128)
        var outMoved = 0
        let status = data.withUnsafeBytes { dataPtr -> CCCryptorStatus in
            key.withUnsafeBytes { keyPtr in
                iv.withUnsafeBytes { ivPtr in
                    CCCrypt(
                        CCOperation(kCCDecrypt), CCAlgorithm(kCCAlgorithmAES), CCOptions(kCCOptionPKCS7Padding),
                        keyPtr.baseAddress, keyPtr.count, ivPtr.baseAddress,
                        dataPtr.baseAddress, dataPtr.count,
                        &outBuffer, outBuffer.count, &outMoved
                    )
                }
            }
        }
        guard status == kCCSuccess else {
            throw HLSDecryptError.cryptoFailed("AES-128-CBC decrypt failed (status \(status)).")
        }
        return Data(outBuffer.prefix(outMoved))
        #else
        throw HLSDecryptError.unsupportedPlatform("HLS AES-128 decryption requires CommonCrypto and is only supported on Apple platforms.")
        #endif
    }

    /// Per RFC 8216 §5.2: when no IV attribute is present, the IV is the
    /// big-endian 16-byte representation of the segment's media sequence number.
    private static func sequenceIV(_ sequence: Int) -> Data {
        var bytes = [UInt8](repeating: 0, count: 16)
        var value = UInt64(max(0, sequence))
        for i in stride(from: 15, through: 8, by: -1) {
            bytes[i] = UInt8(value & 0xFF)
            value >>= 8
        }
        return Data(bytes)
    }
}
