import Foundation

/// One playback client currently (or recently) pulling a stream — tracked
/// separately from the coarser activeClients/bandwidth counters so the
/// monitoring view can show a real connections table (client IP, user
/// agent, per-connection throughput, errors) rather than just a headcount.
private struct ConnectionInfo {
    var streamId: String
    var identity: String
    var clientIP: String
    var userAgent: String
    var firstSeen: Date
    var lastSeen: Date
    var totalBytes: Int64 = 0
    var recentBytes: [(Date, Int)] = []
    var errorCount: Int = 0
}

final class MetricsStore {
    private let lock = NSLock()
    private var activityByStream: [String: [String: [Date]]] = [:]

    // Output (served-to-client) bandwidth — unchanged from before.
    private var perStreamAllTime: [String: Int64] = [:]
    private var globalAllTime: Int64 = 0
    private var perStreamRecent: [String: [(Date, Int)]] = [:]
    private var globalRecent: [(Date, Int)] = []

    // Input (downloaded-from-source) bandwidth.
    private var perStreamInputAllTime: [String: Int64] = [:]
    private var globalInputAllTime: Int64 = 0
    private var perStreamInputRecent: [String: [(Date, Int)]] = [:]
    private var globalInputRecent: [(Date, Int)] = []

    private var keyStats: [String: (requests: Int, bytes: Int64, lastSeenAt: Date?)] = [:]
    private var connections: [String: ConnectionInfo] = [:]

    private let activityWindow: TimeInterval = 45
    private let rateWindow: TimeInterval = 10

    func seed(bandwidth: BandwidthTotals) {
        lock.lock(); defer { lock.unlock() }
        globalAllTime = bandwidth.allTimeBytes
        perStreamAllTime = bandwidth.perStreamAllTimeBytes
        globalInputAllTime = bandwidth.inputAllTimeBytes
        perStreamInputAllTime = bandwidth.perStreamInputAllTimeBytes
    }

    func recordRequest(streamId: String?, identity: String, bytes: Int, clientIP: String = "", userAgent: String = "") {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        if let streamId {
            var identities = activityByStream[streamId] ?? [:]
            identities[identity, default: []].append(now)
            activityByStream[streamId] = identities
            perStreamAllTime[streamId, default: 0] += Int64(bytes)
            perStreamRecent[streamId, default: []].append((now, bytes))

            let key = "\(streamId)|\(clientIP)|\(identity)"
            var connection = connections[key] ?? ConnectionInfo(streamId: streamId, identity: identity, clientIP: clientIP, userAgent: userAgent, firstSeen: now, lastSeen: now)
            connection.lastSeen = now
            connection.totalBytes += Int64(bytes)
            connection.recentBytes.append((now, bytes))
            if !userAgent.isEmpty { connection.userAgent = userAgent }
            connections[key] = connection
        }
        globalAllTime += Int64(bytes)
        globalRecent.append((now, bytes))
        if identity.hasPrefix("key:") {
            let id = String(identity.dropFirst(4))
            var stat = keyStats[id] ?? (requests: 0, bytes: 0, lastSeenAt: nil)
            stat.requests += 1
            stat.bytes += Int64(bytes)
            stat.lastSeenAt = now
            keyStats[id] = stat
        }
    }

    func recordConnectionError(streamId: String, identity: String, clientIP: String) {
        lock.lock(); defer { lock.unlock() }
        let key = "\(streamId)|\(clientIP)|\(identity)"
        guard connections[key] != nil else { return }
        connections[key]?.errorCount += 1
        connections[key]?.lastSeen = Date()
    }

    /// Bytes downloaded from the source (live worker segment fetches, or the
    /// m3u8 proxy's upstream fetches) — the "Input Bandwidth" half of the
    /// monitoring view, as opposed to bytes served out to viewers.
    func recordInput(streamId: String, bytes: Int) {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        perStreamInputAllTime[streamId, default: 0] += Int64(bytes)
        perStreamInputRecent[streamId, default: []].append((now, bytes))
        globalInputAllTime += Int64(bytes)
        globalInputRecent.append((now, bytes))
    }

    func activeClients(streamId: String) -> Int {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        return connections.values.filter { $0.streamId == streamId && now.timeIntervalSince($0.lastSeen) <= activityWindow }.count
    }

    func bandwidthView(streamId: String) -> [String: Any] {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        let activeConns = connections.values.filter { $0.streamId == streamId && now.timeIntervalSince($0.lastSeen) <= activityWindow }
        let connRate = activeConns.reduce(0.0) { sum, conn in
            let recent = conn.recentBytes.filter { now.timeIntervalSince($0.0) <= rateWindow }
            let recentBytes = recent.reduce(0) { $0 + $1.1 }
            return sum + (Double(recentBytes) / rateWindow)
        }
        let streamRecent = (perStreamRecent[streamId] ?? []).filter { now.timeIntervalSince($0.0) <= rateWindow }
        let streamRate = Double(streamRecent.reduce(0) { $0 + $1.1 }) / rateWindow
        return [
            "bytesPerSecond": max(connRate, streamRate),
            "allTimeBytes": perStreamAllTime[streamId] ?? 0
        ]
    }

    func inputBandwidthView(streamId: String) -> [String: Any] {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        let recent = (perStreamInputRecent[streamId] ?? []).filter { now.timeIntervalSince($0.0) <= rateWindow }
        let recentBytes = recent.reduce(0) { $0 + $1.1 }
        return [
            "bytesPerSecond": Double(recentBytes) / rateWindow,
            "allTimeBytes": perStreamInputAllTime[streamId] ?? 0
        ]
    }

    func globalBandwidthView() -> [String: Any] {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        let recent = globalRecent.filter { now.timeIntervalSince($0.0) <= rateWindow }
        let recentBytes = recent.reduce(0) { $0 + $1.1 }
        return [
            "bytesPerSecond": Double(recentBytes) / rateWindow,
            "allTimeBytes": globalAllTime
        ]
    }

    func globalInputBandwidthView() -> [String: Any] {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        let recent = globalInputRecent.filter { now.timeIntervalSince($0.0) <= rateWindow }
        let recentBytes = recent.reduce(0) { $0 + $1.1 }
        return [
            "bytesPerSecond": Double(recentBytes) / rateWindow,
            "allTimeBytes": globalInputAllTime
        ]
    }

    func keyUsage(id: String) -> (requests: Int, bytes: Int64, lastSeenAt: Date?) {
        lock.lock(); defer { lock.unlock() }
        return keyStats[id] ?? (requests: 0, bytes: 0, lastSeenAt: nil)
    }

    func snapshotBandwidthTotals() -> BandwidthTotals {
        lock.lock(); defer { lock.unlock() }
        return BandwidthTotals(
            allTimeBytes: globalAllTime, perStreamAllTimeBytes: perStreamAllTime,
            inputAllTimeBytes: globalInputAllTime, perStreamInputAllTimeBytes: perStreamInputAllTime
        )
    }

    /// Active playback connections (last seen within the activity window),
    /// most recently active first. Caller enriches each with provider/stream
    /// display info — this store only knows raw identities, not names.
    func connectionsSnapshot() -> [[String: Any]] {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        let active = connections.values.filter { now.timeIntervalSince($0.lastSeen) <= activityWindow }
        return active.sorted { $0.lastSeen > $1.lastSeen }.map { connection in
            let recent = connection.recentBytes.filter { now.timeIntervalSince($0.0) <= rateWindow }
            let recentBytes = recent.reduce(0) { $0 + $1.1 }
            let key = "\(connection.streamId)|\(connection.clientIP)|\(connection.identity)"
            return [
                "streamId": connection.streamId,
                "identity": connection.identity,
                "clientIP": connection.clientIP.isEmpty ? "unknown" : connection.clientIP,
                "userAgent": connection.userAgent.isEmpty ? "unknown" : connection.userAgent,
                "uid": String(UInt32(bitPattern: Int32(truncatingIfNeeded: key.hashValue))),
                "uptimeSeconds": now.timeIntervalSince(connection.firstSeen),
                "errors": connection.errorCount,
                "bytesPerSecond": Double(recentBytes) / rateWindow,
                "allTimeBytes": connection.totalBytes
            ]
        }
    }

    /// Live activity+bandwidth snapshot for the given streams, used to feed the SSE metrics push.
    func snapshot(streamIds: [String]) -> [String: Any] {
        var perStream: [String: Any] = [:]
        for id in streamIds {
            perStream[id] = [
                "activeClients": activeClients(streamId: id),
                "bandwidth": bandwidthView(streamId: id),
                "inputBandwidth": inputBandwidthView(streamId: id)
            ]
        }
        return ["streams": perStream, "global": globalBandwidthView(), "globalInput": globalInputBandwidthView(), "connections": connectionsSnapshot()]
    }

    /// Drops timestamps/byte samples/connections that have aged out of their
    /// windows. Previously this ran inline on every recordRequest — an O(total
    /// tracked events) sweep on the request hot path — and, because only
    /// recordRequest called it, the input-bandwidth sample arrays for a running
    /// stream with no active viewers grew without bound. It's now driven once a
    /// second off the server's metrics timer instead, which both takes the
    /// sweep off the per-request path and guarantees pruning happens regardless
    /// of whether any output requests are arriving.
    func pruneExpired() {
        lock.lock(); defer { lock.unlock() }
        prune(now: Date())
    }

    private func prune(now: Date) {
        for (streamId, identities) in activityByStream {
            var updated: [String: [Date]] = [:]
            for (identity, timestamps) in identities {
                let kept = timestamps.filter { now.timeIntervalSince($0) <= activityWindow }
                if !kept.isEmpty { updated[identity] = kept }
            }
            activityByStream[streamId] = updated
        }
        for (streamId, recent) in perStreamRecent {
            perStreamRecent[streamId] = recent.filter { now.timeIntervalSince($0.0) <= rateWindow }
        }
        globalRecent = globalRecent.filter { now.timeIntervalSince($0.0) <= rateWindow }
        for (streamId, recent) in perStreamInputRecent {
            perStreamInputRecent[streamId] = recent.filter { now.timeIntervalSince($0.0) <= rateWindow }
        }
        globalInputRecent = globalInputRecent.filter { now.timeIntervalSince($0.0) <= rateWindow }
        connections = connections.filter { now.timeIntervalSince($0.value.lastSeen) <= activityWindow }
    }
}
