import Foundation

/// Auto-assigns provider/stream logos in two hops: Clearbit's keyless
/// autocomplete endpoint (https://autocomplete.clearbit.com) resolves a name
/// to a company's domain, then Google's favicon service
/// (google.com/s2/favicons) serves an actual PNG for that domain. Simple
/// Icons (the previous source here) is a curated set of a few thousand
/// *software/brand* marks — it has near nothing for actual TV channels, so
/// imported channels/events were landing on nonsense matches (a "jest" or
/// "testcafe" icon for a streaming provider, for example).
///
/// Two hops, not one, because Clearbit shut down its own free logo-hosting
/// subdomain (`logo.clearbit.com` no longer resolves at all) — the
/// autocomplete/suggest endpoint is still alive and still returns a
/// `domain` per match, just a permanently-null `logo` field now. Google's
/// favicon endpoint is the replacement for turning that domain into an
/// actual image.
///
/// Best effort only: returns nil rather than forcing a low-confidence
/// match, and never throws (a failed lookup must never block saving a
/// provider/stream) — callers already treat a nil logo as "fall back to the
/// name's first letter", so a miss here just means that, not a broken save.
///
/// Sends the name being looked up to Clearbit's servers to search — that's
/// the only way a "search by name" logo lookup works. Fine for a provider
/// ("HBO Max") or channel ("ESPN") name; be aware of that if a name itself
/// is sensitive.
final class LogoLookup {
    let cacheFile: URL
    private let suggestBase = "https://autocomplete.clearbit.com/v1/companies/suggest?query="
    private let faviconBase = "https://www.google.com/s2/favicons?sz=128&domain="

    private let lock = NSLock()
    // Keyed by normalized name; a stored empty string means "looked up
    // before, no match" (not "not looked up yet") so a confirmed miss
    // doesn't get re-queried over the network every time.
    private var cache: [String: String] = [:]

    init(cacheFile: URL) {
        self.cacheFile = cacheFile
        loadFromDiskCache()
    }

    func bestMatch(for name: String) -> String? {
        let query = name.trimmingCharacters(in: .whitespaces)
        guard !query.isEmpty else { return nil }
        let key = normalize(query)

        lock.lock()
        if let cached = cache[key] {
            lock.unlock()
            return cached.isEmpty ? nil : cached
        }
        lock.unlock()

        let result = fetchLogo(for: query)
        lock.lock()
        cache[key] = result ?? ""
        lock.unlock()
        persist()
        return result
    }

    private func fetchLogo(for query: String) -> String? {
        guard let domain = fetchDomain(for: query) else { return nil }
        return faviconBase + domain
    }

    private func fetchDomain(for query: String) -> String? {
        guard let encoded = query.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed),
              let url = URL(string: suggestBase + encoded) else { return nil }
        guard let data = try? HTTPClient(options: HTTPRequestOptions(timeout: 6)).get(url) else { return nil }
        guard let results = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return nil }
        for result in results {
            if let domain = result["domain"] as? String, !domain.isEmpty { return domain }
        }
        return nil
    }

    private func normalize(_ text: String) -> String {
        text.lowercased().unicodeScalars.filter { CharacterSet.alphanumerics.contains($0) }.map(String.init).joined()
    }

    private func persist() {
        lock.lock()
        let snapshot = cache
        lock.unlock()
        guard let data = try? JSONSerialization.data(withJSONObject: snapshot) else { return }
        try? data.write(to: cacheFile, options: [.atomic])
    }

    private func loadFromDiskCache() {
        guard let data = try? Data(contentsOf: cacheFile),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: String] else { return }
        cache = object
    }
}
