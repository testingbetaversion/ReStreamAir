import Foundation

/// `env PATH` lookup alone misses ffmpeg/ffprobe on plenty of real setups —
/// GUI-launched processes (double-clicked, launchd, a login item) don't
/// inherit an interactive shell's PATH, so a Homebrew install under
/// /opt/homebrew/bin (Apple Silicon) or /usr/local/bin (Intel) can be
/// present and on the user's own PATH yet invisible to us. Fall back to the
/// well-known install locations before giving up.
enum FFmpegLocator {
    static func resolve(_ binary: String) -> String? {
        if let envPath = ProcessInfo.processInfo.environment["PATH"] {
            for dir in envPath.split(separator: ":") {
                let candidate = "\(dir)/\(binary)"
                if FileManager.default.isExecutableFile(atPath: candidate) { return candidate }
            }
        }
        let commonDirs = ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", "/snap/bin"]
        for dir in commonDirs {
            let candidate = "\(dir)/\(binary)"
            if FileManager.default.isExecutableFile(atPath: candidate) { return candidate }
        }
        return nil
    }
}

/// Resolves how to install ffmpeg on this OS — deliberately just the one
/// well-known package manager per platform (Homebrew on macOS, apt on
/// Linux) rather than trying to detect every possible one. Homebrew never
/// needs sudo, so it's run directly; apt normally does, and this
/// intentionally does *not* attempt sudo itself (no way to supply a
/// password non-interactively without a much bigger, riskier surface) — on
/// Linux it just runs the plain command and lets it fail with apt's own
/// clear permission-denied output if it isn't already running as root,
/// which tells the person exactly what to run themselves.
enum FFmpegInstaller {
    struct Plan {
        let executable: String
        let arguments: [String]
        let displayCommand: String
    }

    static func plan() -> Plan? {
        #if os(macOS)
        guard let brew = FFmpegLocator.resolve("brew") else { return nil }
        return Plan(executable: brew, arguments: ["install", "ffmpeg"], displayCommand: "brew install ffmpeg")
        #elseif os(Linux)
        guard let apt = FFmpegLocator.resolve("apt-get") ?? FFmpegLocator.resolve("apt") else { return nil }
        return Plan(executable: apt, arguments: ["install", "-y", "ffmpeg"], displayCommand: "apt-get install -y ffmpeg")
        #else
        return nil
        #endif
    }

    /// What to show even when `plan()` is nil (e.g. Homebrew itself isn't
    /// installed) so the UI can still tell someone what to run by hand.
    static var manualCommand: String {
        #if os(macOS)
        return "brew install ffmpeg"
        #elseif os(Linux)
        return "sudo apt-get install -y ffmpeg"
        #else
        return "Install ffmpeg using your system's package manager."
        #endif
    }
}

struct FFProbe {
    static func duration(for url: URL) -> Double? {
        guard let ffprobePath = FFmpegLocator.resolve("ffprobe") else { return nil }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: ffprobePath)
        process.arguments = [
            "-v", "error",
            "-show_entries", "format=duration",
            "-of", "default=noprint_wrappers=1:nokey=1",
            url.path
        ]

        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()

        do {
            try process.run()
            process.waitUntilExit()
            guard process.terminationStatus == 0 else { return nil }
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            let text = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines)
            return text.flatMap(Double.init)
        } catch {
            return nil
        }
    }
}
