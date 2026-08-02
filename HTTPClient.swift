import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

enum HTTPClientError: Error, CustomStringConvertible {
    case invalidProxy(String)
    case noResponse(URL)
    case timeout(URL, TimeInterval)
    case httpStatus(URL, Int)
    case writeFailed(URL, URL, String)

    var description: String {
        switch self {
        case .invalidProxy(let proxy):
            return "Invalid proxy URL '\(proxy)'. Use http://host:port or https://host:port."
        case .noResponse(let url):
            return "No response from \(url.absoluteString)."
        case .timeout(let url, let seconds):
            return "Request timed out after \(seconds)s for \(url.absoluteString)."
        case .httpStatus(let url, let status):
            return "HTTP \(status) for \(url.absoluteString)."
        case .writeFailed(let source, let destination, let message):
            return "Could not write \(source.absoluteString) to \(destination.path): \(message)"
        }
    }
}

struct HTTPRequestOptions {
    var timeout: TimeInterval = 30
    var headers: [(String, String)] = []
    var proxy: String?
}

final class HTTPResultBox {
    var result: Result<(Data, URLResponse), Error>?
}

/// A `URLSession` per request (the original implementation) is expensive to
/// spin up and tear down — each carries its own delegate queue and
/// connection machinery, and `invalidateAndCancel()` doesn't guarantee that
/// teardown finishes before the next request creates another one. A live
/// restream polls its manifest and downloads segments every couple of
/// seconds for as long as the stream runs, so that overhead compounds
/// without bound: an hour-long stream means thousands of sessions
/// created/discarded, which is exactly what shows up as runaway CPU% and an
/// ever-growing thread count.
///
/// Sessions are pooled by configuration (timeout + proxy — the only two
/// things baked into a URLSessionConfiguration here; request headers are
/// applied per-call in `get`) and reused for the process's lifetime. This is
/// what actually delivers the win on the m3u8 proxy path, where every
/// segment/key/manifest request builds a fresh HTTPClient: constructing one
/// is now cheap because it just looks up an already-warm shared session
/// instead of standing up a new one. A URLSession is thread-safe and designed
/// to serve many concurrent requests, so sharing one across callers with the
/// same proxy config is exactly its intended use.
final class HTTPClient {
    let options: HTTPRequestOptions
    private let session: URLSession

    private static let sessionCacheLock = NSLock()
    private static var sessionCache: [String: URLSession] = [:]

    init(options: HTTPRequestOptions = HTTPRequestOptions()) {
        self.options = options
        self.session = HTTPClient.sharedSession(timeout: options.timeout, proxy: options.proxy)
    }

    private static func sharedSession(timeout: TimeInterval, proxy: String?) -> URLSession {
        let key = "\(timeout)|\(proxy ?? "")"
        sessionCacheLock.lock(); defer { sessionCacheLock.unlock() }
        if let existing = sessionCache[key] { return existing }
        let configuration = URLSessionConfiguration.default
        configuration.timeoutIntervalForRequest = timeout
        configuration.timeoutIntervalForResource = timeout
        // A Basic-auth proxy (`http://user:pass@host:port`) answers CONNECT
        // with 407 before anything else happens — `connectionProxyDictionary`
        // only carries host/port, so without a delegate to answer that
        // challenge every request through such a proxy just fails with no
        // credentials ever offered.
        var delegate: ProxyAuthDelegate?
        if let proxy {
            do {
                let resolved = try HTTPClient.proxyDictionary(proxy)
                configuration.connectionProxyDictionary = resolved.dictionary
                if let user = resolved.username, let password = resolved.password {
                    delegate = ProxyAuthDelegate(username: user, password: password)
                }
            } catch {
                // A live poll loop would otherwise repeat this same failure
                // every request forever; fail open (no proxy) with one
                // visible warning instead (once per distinct proxy config now,
                // since the session it's attached to is cached).
                FileHandle.standardError.write(Data("warning: \(error) — continuing without a proxy.\n".utf8))
            }
        }
        let session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
        sessionCache[key] = session
        return session
    }

    func get(_ url: URL) throws -> Data {
        try fetch(url).data
    }

    /// Like `get`, but also reports the URL the response actually came from.
    /// `URLSession` follows redirects transparently, so a signed-CDN manifest
    /// that 302s to a token-bearing host (query params only present on the
    /// *redirected* URL, not the one we requested) would otherwise leave no
    /// trace of that token anywhere the caller could see — needed by
    /// "inherit URL params" to pick up a token minted at redirect time rather
    /// than one baked into the configured source URL.
    func fetch(_ url: URL) throws -> (data: Data, finalURL: URL) {
        var request = URLRequest(url: url)
        request.timeoutInterval = options.timeout
        for (name, value) in options.headers {
            request.setValue(value, forHTTPHeaderField: name)
        }

        let semaphore = DispatchSemaphore(value: 0)
        let box = HTTPResultBox()

        session.dataTask(with: request) { data, response, error in
            defer { semaphore.signal() }
            if let error {
                box.result = .failure(error)
                return
            }
            guard let data, let response else {
                box.result = .failure(HTTPClientError.noResponse(url))
                return
            }
            box.result = .success((data, response))
        }.resume()

        let waitResult = semaphore.wait(timeout: .now() + options.timeout + 1)

        if waitResult == .timedOut {
            throw HTTPClientError.timeout(url, options.timeout)
        }
        guard let result = box.result else {
            throw HTTPClientError.noResponse(url)
        }

        let (data, response) = try result.get()
        if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            throw HTTPClientError.httpStatus(url, http.statusCode)
        }
        return (data, response.url ?? url)
    }

    @discardableResult
    func download(_ url: URL, to destination: URL) throws -> Int {
        let data = try get(url)
        do {
            try FileManager.default.createDirectory(
                at: destination.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            try data.write(to: destination, options: [.atomic])
            return data.count
        } catch {
            throw HTTPClientError.writeFailed(url, destination, "\(error)")
        }
    }

    /// `proxy` may embed Basic-auth credentials (`http://user:pass@host:port`,
    /// as e.g. paid rotating-IP proxies commonly require) — `connectionProxyDictionary`
    /// only carries host/port, so those are pulled out separately for
    /// `ProxyAuthDelegate` to answer the 407 challenge with.
    private static func proxyDictionary(_ proxy: String) throws -> (dictionary: [AnyHashable: Any], username: String?, password: String?) {
        guard let url = URL(string: proxy), let host = url.host else {
            throw HTTPClientError.invalidProxy(proxy)
        }
        let port = url.port ?? (url.scheme == "https" ? 443 : 80)
        let dictionary: [AnyHashable: Any]
        if url.scheme == "https" {
            dictionary = [
                "HTTPSEnable": true,
                "HTTPSProxy": host,
                "HTTPSPort": port
            ]
        } else {
            dictionary = [
                "HTTPEnable": true,
                "HTTPProxy": host,
                "HTTPPort": port
            ]
        }
        return (dictionary, url.user, url.password)
    }
}

/// Answers a proxy's Basic-auth challenge with credentials parsed out of the
/// proxy URL. Without this, `URLSession` has nothing to offer a 407 and every
/// request through a credentialed proxy just fails — there is no interactive
/// user here to enter one.
private final class ProxyAuthDelegate: NSObject, URLSessionTaskDelegate {
    let username: String
    let password: String

    init(username: String, password: String) {
        self.username = username
        self.password = password
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        guard challenge.protectionSpace.isProxy(), challenge.previousFailureCount == 0 else {
            completionHandler(.performDefaultHandling, nil)
            return
        }
        completionHandler(.useCredential, URLCredential(user: username, password: password, persistence: .forSession))
    }
}

/// Appends a raw query-string fragment (e.g. `token=abc&region=us`, with or
/// without a leading `?`/`&`) to a URL's existing query. Shared by the DASH
/// segment downloader and the HLS proxy path so "append params to segments"
/// is one implementation, not duplicated per pipeline.
func appendingQueryParams(_ url: URL, _ paramString: String) -> URL {
    var trimmed = paramString.trimmingCharacters(in: CharacterSet(charactersIn: " ?&"))
    guard !trimmed.isEmpty, var components = URLComponents(url: url, resolvingAgainstBaseURL: true) else { return url }
    // The `percentEncodedQuery` setter requires an already-valid encoded
    // query and raises/traps on anything else. This string is free text from
    // the stream editor, so a lone space, "#", or bare "%" typed there must
    // be encoded here — not allowed to crash every segment request.
    let allowed = CharacterSet.urlQueryAllowed
    if trimmed.removingPercentEncoding == nil || !trimmed.unicodeScalars.allSatisfy({ allowed.contains($0) }) {
        var literal = allowed
        literal.remove(charactersIn: "%")
        guard let encoded = trimmed.addingPercentEncoding(withAllowedCharacters: literal) else { return url }
        trimmed = encoded
    }
    let existing = components.percentEncodedQuery
    components.percentEncodedQuery = [existing, trimmed].compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: "&")
    return components.url ?? url
}
