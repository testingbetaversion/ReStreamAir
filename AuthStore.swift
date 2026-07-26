import Foundation
import restream_core

struct AdminUser: Codable {
    var id: String
    var username: String
    var passwordHash: String
    var salt: String
    var createdAt: Date
}

enum AuthError: Error, CustomStringConvertible {
    case unsupportedPlatform(String)
    case badRequest(String)

    var description: String {
        switch self {
        case .unsupportedPlatform(let m), .badRequest(let m): return m
        }
    }
}

/// Session tokens live in memory only (cleared on restart, which simply
/// requires signing in again — acceptable for a LAN admin tool). Password
/// hashing uses PBKDF2-HMAC-SHA256 from the C core, which works identically on
/// every platform and produces digests interchangeable with the CommonCrypto
/// and pure-Swift implementations earlier builds used.
final class AuthStore {
    private let lock = NSLock()
    private var sessions: [String: (username: String, expiresAt: Date)] = [:]
    private let defaultSessionLifetime: TimeInterval = 24 * 60 * 60
    private let rememberedSessionLifetime: TimeInterval = 30 * 24 * 60 * 60
    private let cookieName = "restreamair_session"

    static func hashPassword(_ password: String) throws -> (hash: String, salt: String) {
        var saltBytes = [UInt8](repeating: 0, count: 16)
        fillRandomBytes(&saltBytes)
        let salt = Data(saltBytes)
        let hash = try pbkdf2(password: password, salt: salt)
        return (hash.base64EncodedString(), salt.base64EncodedString())
    }

    static func verifyPassword(_ password: String, hash: String, salt: String) -> Bool {
        guard let saltData = Data(base64Encoded: salt), let computed = try? pbkdf2(password: password, salt: saltData) else { return false }
        return computed.base64EncodedString() == hash
    }

    /// PBKDF2-HMAC-SHA256, 100k iterations, 32-byte key — one implementation on
    /// every platform now, from the C core. It produces the same digest the two
    /// implementations it replaces did (CommonCrypto on macOS, pure Swift on
    /// Linux), so an account created by any earlier build still logs in.
    /// `restreamair selftest` re-checks that equality at exactly these
    /// parameters on every run; see CoreParityTest.swift.
    private static func pbkdf2(password: String, salt: Data) throws -> Data {
        var derived = [UInt8](repeating: 0, count: 32)
        let passwordBytes = Array(password.utf8)
        let status = passwordBytes.withUnsafeBufferPointer { passwordPtr in
            salt.withUnsafeBytes { saltPtr in
                rs_pbkdf2_sha256(passwordPtr.baseAddress, passwordBytes.count,
                                 saltPtr.bindMemory(to: UInt8.self).baseAddress, saltPtr.count,
                                 100_000, &derived, derived.count)
            }
        }
        guard status == 0 else { throw AuthError.unsupportedPlatform("PBKDF2 derivation failed.") }
        return Data(derived)
    }

    func createSession(username: String, remember: Bool) -> String {
        let token = UUID().uuidString + UUID().uuidString
        let lifetime = remember ? rememberedSessionLifetime : defaultSessionLifetime
        lock.lock()
        sessions[token] = (username, Date().addingTimeInterval(lifetime))
        lock.unlock()
        return token
    }

    func endSession(token: String) {
        lock.lock(); sessions.removeValue(forKey: token); lock.unlock()
    }

    func username(forSessionCookie header: String?) -> String? {
        guard let token = cookieValue(header, name: cookieName) else { return nil }
        lock.lock(); defer { lock.unlock() }
        guard let session = sessions[token], session.expiresAt > Date() else { return nil }
        return session.username
    }

    /// "Remember me" gets a persistent cookie (survives browser restarts, 30
    /// days). Without it, the cookie has no Max-Age so the browser drops it
    /// as soon as it closes — the server-side session is still good for a
    /// day as a safety net, but the browser won't hand the cookie back.
    func setCookieHeader(token: String, remember: Bool) -> String {
        var header = "\(cookieName)=\(token); HttpOnly; Path=/; SameSite=Lax"
        if remember { header += "; Max-Age=\(Int(rememberedSessionLifetime))" }
        return header
    }

    func clearCookieHeader() -> String {
        "\(cookieName)=; HttpOnly; Path=/; Max-Age=0; SameSite=Lax"
    }

    private func cookieValue(_ header: String?, name: String) -> String? {
        guard let header else { return nil }
        for part in header.split(separator: ";") {
            let pair = part.trimmingCharacters(in: .whitespaces).split(separator: "=", maxSplits: 1)
            if pair.count == 2, pair[0] == name { return String(pair[1]) }
        }
        return nil
    }

    func basicCredentials(_ header: String?) -> (username: String, password: String)? {
        guard let header, header.hasPrefix("Basic ") else { return nil }
        let encoded = String(header.dropFirst(6))
        guard let data = Data(base64Encoded: encoded), let decoded = String(data: data, encoding: .utf8) else { return nil }
        guard let separator = decoded.firstIndex(of: ":") else { return nil }
        return (String(decoded[..<separator]), String(decoded[decoded.index(after: separator)...]))
    }
}

/// Swift's default SystemRandomNumberGenerator is CSPRNG-backed on every
/// supported platform, so this is fine for salt generation without pulling
/// in the Security framework just for one call.
private func fillRandomBytes(_ buffer: inout [UInt8]) {
    for i in buffer.indices { buffer[i] = UInt8.random(in: 0...255) }
}
