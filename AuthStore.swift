import Foundation
import restream_core

/// What an account may do. `admin` is full control; `viewer` may read the panel
/// — state, logs, monitoring — and nothing else.
enum AdminRole: String, Codable {
    case admin
    case viewer
}

struct AdminUser: Codable {
    var id: String
    var username: String
    var passwordHash: String
    var salt: String
    var createdAt: Date
    /// Absent in state.json written before roles existed. Those accounts are
    /// admins, which is what they have always been — silently demoting the only
    /// account on upgrade would lock the operator out of their own panel.
    var role: AdminRole = .admin

    init(id: String, username: String, passwordHash: String, salt: String,
         createdAt: Date, role: AdminRole = .admin) {
        self.id = id
        self.username = username
        self.passwordHash = passwordHash
        self.salt = salt
        self.createdAt = createdAt
        self.role = role
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(String.self, forKey: .id)
        username = try container.decode(String.self, forKey: .username)
        passwordHash = try container.decode(String.self, forKey: .passwordHash)
        salt = try container.decode(String.self, forKey: .salt)
        createdAt = try container.decode(Date.self, forKey: .createdAt)
        role = try container.decodeIfPresent(AdminRole.self, forKey: .role) ?? .admin
    }
}

/// One signed-in session as it is persisted. What is stored is the SHA-256 of
/// the token, never the token: state.json is a file operators copy around and
/// back up, and a live bearer token sitting in it would be a spare key to the
/// panel.
struct StoredSession: Codable {
    var tokenHash: String
    var username: String
    var expiresAt: Date
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

/// Sessions are keyed by the SHA-256 of their token and persisted through
/// `exportSessions`/`importSessions` into state.json, so a restart no longer
/// signs everyone out and "remember me for 30 days" means what it says. Password
/// hashing uses PBKDF2-HMAC-SHA256 from the C core, which works identically on
/// every platform and produces digests interchangeable with the CommonCrypto
/// and pure-Swift implementations earlier builds used.
///
/// Failed sign-ins are throttled per username+address. PBKDF2 at 100k iterations
/// makes each guess expensive, but nothing made the *number* of guesses
/// expensive until now. That counter is deliberately not persisted: writing it
/// down would let anyone who can reach the login form lock an account out across
/// restarts.
final class AuthStore {
    private let lock = NSLock()
    /// Keyed by token hash, never by the token itself.
    private var sessions: [String: (username: String, expiresAt: Date)] = [:]
    private var throttles: [String: (failures: Int, lastFailure: Date, retryAfter: Date)] = [:]
    private let defaultSessionLifetime: TimeInterval = 24 * 60 * 60
    private let rememberedSessionLifetime: TimeInterval = 30 * 24 * 60 * 60
    private let cookieName = "restreamair_session"

    /// The first `throttleFreeAttempts` failures cost nothing, so a typo is not
    /// punished; after that each failure doubles the wait up to
    /// `throttleMaxDelay`, and a quiet `throttleWindow` forgets the record so an
    /// operator is never locked out permanently. Mirrors the C core's numbers.
    private let throttleFreeAttempts = 5
    private let throttleBaseDelay: TimeInterval = 2
    private let throttleMaxDelay: TimeInterval = 15 * 60
    private let throttleWindow: TimeInterval = 60 * 60

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

    /// SHA-256 of a token as lowercase hex — the session store's key, and what
    /// gets persisted. Uses the same C core digest as everything else, so a
    /// session created by the C server resolves here and vice versa.
    static func tokenHash(_ token: String) -> String {
        var digest = [UInt8](repeating: 0, count: Int(RS_SHA256_DIGEST_LEN))
        let bytes = Array(token.utf8)
        bytes.withUnsafeBufferPointer { rs_sha256($0.baseAddress, bytes.count, &digest) }
        return digest.map { String(format: "%02x", $0) }.joined()
    }

    func createSession(username: String, remember: Bool) -> String {
        let token = UUID().uuidString + UUID().uuidString
        let lifetime = remember ? rememberedSessionLifetime : defaultSessionLifetime
        lock.lock()
        sessions[AuthStore.tokenHash(token)] = (username, Date().addingTimeInterval(lifetime))
        lock.unlock()
        return token
    }

    func endSession(token: String) {
        lock.lock(); sessions.removeValue(forKey: AuthStore.tokenHash(token)); lock.unlock()
    }

    /// Ends every session belonging to `username` — what deleting an account or
    /// changing its password must do, so an old cookie cannot outlive the
    /// credential it was issued against.
    func endSessions(forUser username: String) {
        lock.lock()
        sessions = sessions.filter { $0.value.username != username }
        lock.unlock()
    }

    func username(forSessionCookie header: String?) -> String? {
        guard let token = cookieValue(header, name: cookieName) else { return nil }
        lock.lock(); defer { lock.unlock() }
        guard let session = sessions[AuthStore.tokenHash(token)], session.expiresAt > Date() else { return nil }
        return session.username
    }

    // MARK: - Persistence

    /// The live, unexpired sessions, for writing into state.json.
    func exportSessions() -> [StoredSession] {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        return sessions.compactMap { hash, session in
            session.expiresAt > now
                ? StoredSession(tokenHash: hash, username: session.username, expiresAt: session.expiresAt)
                : nil
        }
    }

    /// Restores persisted sessions, skipping any that have expired.
    func importSessions(_ stored: [StoredSession]) {
        let now = Date()
        lock.lock()
        for entry in stored where entry.expiresAt > now {
            sessions[entry.tokenHash] = (entry.username, entry.expiresAt)
        }
        lock.unlock()
    }

    // MARK: - Login throttling

    /// Seconds the caller must wait before another attempt for `identity` will
    /// be considered, or 0 if one is allowed right now.
    func throttleDelay(for identity: String) -> Int {
        lock.lock(); defer { lock.unlock() }
        pruneThrottlesLocked()
        guard let record = throttles[identity] else { return 0 }
        let remaining = record.retryAfter.timeIntervalSinceNow
        return remaining > 0 ? Int(remaining.rounded(.up)) : 0
    }

    /// Records a failed attempt and returns the delay now imposed, 0 while still
    /// inside the free allowance.
    @discardableResult
    func recordFailedLogin(for identity: String) -> Int {
        lock.lock(); defer { lock.unlock() }
        pruneThrottlesLocked()
        let now = Date()
        let failures = (throttles[identity]?.failures ?? 0) + 1
        guard failures > throttleFreeAttempts else {
            throttles[identity] = (failures, now, now)
            return 0
        }
        // 2s, 4s, 8s … capped. The exponent is clamped long before it could
        // overflow the multiplication.
        let steps = min(failures - throttleFreeAttempts - 1, 20)
        let delay = min(throttleBaseDelay * pow(2, Double(steps)), throttleMaxDelay)
        throttles[identity] = (failures, now, now.addingTimeInterval(delay))
        return Int(delay)
    }

    func resetThrottle(for identity: String) {
        lock.lock(); throttles.removeValue(forKey: identity); lock.unlock()
    }

    /// Drops records nothing has touched for a full window, so a long-running
    /// server doesn't accumulate one entry per guessed username forever.
    private func pruneThrottlesLocked() {
        let cutoff = Date().addingTimeInterval(-throttleWindow)
        throttles = throttles.filter { $0.value.lastFailure > cutoff }
    }

    /// "Remember me" gets a persistent cookie (survives browser restarts, 30
    /// days). Without it, the cookie has no Max-Age so the browser drops it
    /// as soon as it closes — the server-side session is still good for a
    /// day as a safety net, but the browser won't hand the cookie back.
    ///
    /// `secure` must be set whenever the user reached us over HTTPS, and must
    /// not be over plain HTTP: a Secure cookie on an http:// origin is dropped
    /// by the browser, which presents as "signing in silently does nothing".
    func setCookieHeader(token: String, remember: Bool, secure: Bool = false) -> String {
        var header = "\(cookieName)=\(token); HttpOnly; Path=/; SameSite=Lax"
        if remember { header += "; Max-Age=\(Int(rememberedSessionLifetime))" }
        if secure { header += "; Secure" }
        return header
    }

    func clearCookieHeader(secure: Bool = false) -> String {
        "\(cookieName)=; HttpOnly; Path=/; Max-Age=0; SameSite=Lax" + (secure ? "; Secure" : "")
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
