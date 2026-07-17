import Foundation

struct LogEntry {
    let timestamp: Date
    let streamId: String?
    let level: String
    let event: String
    let url: String?
    let message: String?
    /// The original, unabridged JSON line emitted by the worker (when this
    /// entry came from one) — verbose mode shows this instead of the
    /// condensed one-line `message` summary.
    let raw: String?

    var json: [String: Any] {
        var object: [String: Any] = [
            "timestamp": ISO8601DateFormatter.sharedTimestamp.string(from: timestamp),
            "streamId": streamId as Any,
            "level": level,
            "event": event,
            "url": url as Any,
            "message": message as Any
        ]
        if let raw { object["raw"] = raw }
        return object
    }
}

/// Bounded ring buffer of operational events (manifest fetches, segment
/// downloads, proxy fetches, errors) so the panel can show what the restream
/// engine is actually doing without writing unbounded log files. The `live`
/// worker subprocess prints one JSON line per event to stdout; PanelServer's
/// existing per-job stdout capture forwards each line here via
/// `recordWorkerLine`. Every entry is also appended to a per-day file under
/// `logsDir` so "previous runs" survive a server restart — the in-memory
/// ring buffer alone only covers the current process's lifetime.
final class LogStore {
    private let lock = NSLock()
    private var entries: [LogEntry] = []
    private let capacity = 500
    let logsDir: URL
    private let fileLock = NSLock()
    private var openHandles: [String: FileHandle] = [:]
    private var fileSizes: [String: UInt64] = [:]
    /// Keep each day's active log file bounded — a run left going for
    /// weeks shouldn't be able to fill the disk. Once a file hits this
    /// size it's rotated (renamed with a timestamp suffix) and a fresh one
    /// starts; old rotations beyond maxRotationsPerDay are deleted.
    private let maxFileBytes: UInt64 = 8 * 1024 * 1024
    private let maxRotationsPerDay = 5
    /// Reused across every persisted line rather than reallocated per call —
    /// persist() runs once per log event (potentially every segment on a busy
    /// stream), and building a DateFormatter each time is needless churn.
    /// `.string(from:)` on an otherwise-immutable formatter is thread-safe.
    private let dayFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd"
        formatter.timeZone = TimeZone.current
        return formatter
    }()

    init(logsDir: URL) {
        self.logsDir = logsDir
        try? FileManager.default.createDirectory(at: logsDir, withIntermediateDirectories: true)
    }

    func record(streamId: String?, level: String, event: String, url: String?, message: String?, raw: String? = nil) {
        let entry = LogEntry(timestamp: Date(), streamId: streamId, level: level, event: event, url: url, message: message, raw: raw)
        lock.lock()
        entries.append(entry)
        if entries.count > capacity { entries.removeFirst(entries.count - capacity) }
        lock.unlock()
        persist(entry)
    }

    /// Parses one JSON log line emitted by the `live` worker (e.g.
    /// `{"event":"downloadSegment","url":"...","status":"success","bytes":1234}`)
    /// and records it tagged with the owning stream id.
    func recordWorkerLine(_ line: String, streamId: String) {
        guard let data = line.data(using: .utf8),
              let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let event = object["event"] as? String else { return }
        let level = (object["status"] as? String) == "error" ? "error" : "info"
        let url = object["url"] as? String
        var messageParts: [String] = []
        if let proxy = object["proxy"] as? String, !proxy.isEmpty { messageParts.append("via proxy \(proxy)") }
        if let bytes = object["bytes"] as? Int { messageParts.append("\(bytes) bytes") }
        if let status = object["status"] as? String { messageParts.append(status) }
        if let detail = object["message"] as? String { messageParts.append(detail) }
        record(streamId: streamId, level: level, event: event, url: url, message: messageParts.isEmpty ? nil : messageParts.joined(separator: " · "), raw: line)
    }

    /// Drops a stream's entries from the in-memory ring buffer — called on
    /// stream start so the Logs tab shows this run fresh instead of mixing
    /// in errors/noise from whatever happened the last time it ran. Leaves
    /// the per-day persisted files alone: those back the separate "previous
    /// runs" browser (availableRunDates/historicalEntries), which every
    /// other stream sharing that day's file still relies on, and is meant
    /// to be a durable record rather than something a restart erases.
    func clearStream(_ streamId: String) {
        lock.lock()
        entries.removeAll { $0.streamId == streamId }
        lock.unlock()
    }

    func recent(streamId: String?, limit: Int) -> [[String: Any]] {
        lock.lock(); defer { lock.unlock() }
        let filtered = streamId == nil ? entries : entries.filter { $0.streamId == streamId }
        return filtered.suffix(limit).reversed().map(\.json)
    }

    /// Log file dates available on disk (newest first), e.g. "2026-07-13" —
    /// lets the panel offer a "previous runs" picker beyond the in-memory
    /// window.
    func availableRunDates(limit: Int = 60) -> [String] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: logsDir.path)) ?? []
        // Only the active per-day file (yyyy-MM-dd.jsonl), not rotated
        // chunks (yyyy-MM-dd.<epoch>.jsonl) — those are bounded-size cold
        // storage, not meant to be individually browsable.
        return names
            .filter { $0.count == 16 && $0.hasSuffix(".jsonl") }
            .map { String($0.dropLast(6)) }
            .filter { $0.allSatisfy { c in c.isNumber || c == "-" } }
            .sorted(by: >)
            .prefix(limit)
            .map { $0 }
    }

    /// Reads a specific day's persisted log file back off disk (most recent
    /// first), optionally filtered to one stream.
    func historicalEntries(date: String, streamId: String?, limit: Int) -> [[String: Any]] {
        guard date.allSatisfy({ $0.isNumber || $0 == "-" }) else { return [] }
        let url = logsDir.appendingPathComponent("\(date).jsonl")
        guard let data = try? Data(contentsOf: url), let text = String(data: data, encoding: .utf8) else { return [] }
        var results: [[String: Any]] = []
        for line in text.split(separator: "\n").reversed() {
            guard let lineData = line.data(using: .utf8),
                  let object = (try? JSONSerialization.jsonObject(with: lineData)) as? [String: Any] else { continue }
            if let streamId, (object["streamId"] as? String) != streamId { continue }
            results.append(object)
            if results.count >= limit { break }
        }
        return results
    }

    private func dayFileName(for date: Date) -> String {
        "\(dayFormatter.string(from: date)).jsonl"
    }

    /// Best-effort append-only persistence — a failure here (disk full, odd
    /// permissions) should never take playback/the admin API down with it.
    private func persist(_ entry: LogEntry) {
        guard let json = try? JSONSerialization.data(withJSONObject: entry.json) else { return }
        let lineBytes = UInt64(json.count + 1)
        let fileName = dayFileName(for: entry.timestamp)
        fileLock.lock()
        defer { fileLock.unlock() }
        if openHandles[fileName] == nil {
            openFile(fileName)
        }
        if let currentSize = fileSizes[fileName], currentSize + lineBytes > maxFileBytes {
            rotate(fileName)
            openFile(fileName)
        }
        guard let handle = openHandles[fileName] else { return }
        handle.write(json)
        handle.write("\n".data(using: .utf8)!)
        fileSizes[fileName, default: 0] += lineBytes
    }

    /// Caller must hold fileLock.
    private func openFile(_ fileName: String) {
        let url = logsDir.appendingPathComponent(fileName)
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: nil)
        }
        let handle = try? FileHandle(forWritingTo: url)
        handle?.seekToEndOfFile()
        openHandles[fileName] = handle
        let attrs = try? FileManager.default.attributesOfItem(atPath: url.path)
        fileSizes[fileName] = (attrs?[.size] as? NSNumber)?.uint64Value ?? 0
    }

    /// Caller must hold fileLock. Renames the current file out of the way
    /// with an epoch-seconds suffix and prunes old rotations for that day
    /// beyond maxRotationsPerDay, so total disk usage per day stays bounded
    /// (~maxFileBytes * (maxRotationsPerDay + 1)) instead of growing forever.
    private func rotate(_ fileName: String) {
        openHandles[fileName]?.closeFile()
        openHandles[fileName] = nil
        fileSizes[fileName] = nil
        let url = logsDir.appendingPathComponent(fileName)
        let stamp = Int(Date().timeIntervalSince1970)
        let datePart = String(fileName.dropLast(6))
        let rotatedURL = logsDir.appendingPathComponent("\(datePart).\(stamp).jsonl")
        try? FileManager.default.moveItem(at: url, to: rotatedURL)

        let names = (try? FileManager.default.contentsOfDirectory(atPath: logsDir.path)) ?? []
        let rotations = names.filter { $0.hasPrefix(datePart + ".") && $0.hasSuffix(".jsonl") && $0 != fileName }.sorted()
        if rotations.count > maxRotationsPerDay {
            for name in rotations.prefix(rotations.count - maxRotationsPerDay) {
                try? FileManager.default.removeItem(at: logsDir.appendingPathComponent(name))
            }
        }
    }
}
