import Foundation

/// Executes external "provider scripts" — arbitrary Python/sh/etc. programs
/// that own login/session/DRM logic ReStreamAir doesn't implement itself.
/// Every invocation gets a flat argv of "key=value" tokens (the same
/// convention PanelServer.start() already uses for its own worker process —
/// see the `args` build-up there), so scripts written against that shared
/// protocol work against ReStreamAir unmodified.
enum ScriptRunner {
    enum ScriptRunnerError: Error, CustomStringConvertible {
        case scriptNotFound(String)
        case timedOut
        // stderr is almost always where the actual reason lives (a Python
        // traceback, a missing-import error, ...) — without it, every
        // failure just says "exited with code 1" and there's no way to
        // tell why short of running the script by hand outside the panel.
        case nonZeroExit(code: Int32, stderr: String)

        var description: String {
            switch self {
            case .scriptNotFound(let path): return "Script not found or not readable at \(path)."
            case .timedOut: return "Script timed out."
            case .nonZeroExit(let code, let stderr):
                let trimmed = stderr.trimmingCharacters(in: .whitespacesAndNewlines)
                return trimmed.isEmpty ? "Script exited with code \(code)." : "Script exited with code \(code): \(trimmed)"
            }
        }
    }

    private struct Invocation {
        var executable: URL
        var arguments: [String]
    }

    /// Process spawns the executable directly — there's no shell in between
    /// to interpret backslash escapes — but a path pasted from a terminal
    /// (e.g. after tab-completion, or copied from `pwd`) commonly arrives
    /// shell-escaped (`Mobile\ Documents`, `com\~apple\~CloudDocs`). Treated
    /// literally, that backslash is just an extra character the real
    /// filesystem path doesn't have, so the file is never found. Un-escape
    /// before treating the string as a path so both forms work.
    static func normalizePath(_ path: String) -> String {
        var result = ""
        result.reserveCapacity(path.count)
        var iterator = path.makeIterator()
        while let char = iterator.next() {
            if char == "\\", let next = iterator.next() {
                result.append(next)
            } else {
                result.append(char)
            }
        }
        return result
    }

    /// .py -> python3 <path>, .sh/.bash -> /bin/sh <path>, anything else is
    /// executed directly (must be chmod +x with its own shebang).
    private static func invocation(for scriptPath: String) -> Invocation {
        switch (scriptPath as NSString).pathExtension.lowercased() {
        case "py":
            return Invocation(executable: URL(fileURLWithPath: "/usr/bin/env"), arguments: ["python3", scriptPath])
        case "sh", "bash":
            return Invocation(executable: URL(fileURLWithPath: "/bin/sh"), arguments: [scriptPath])
        default:
            return Invocation(executable: URL(fileURLWithPath: scriptPath), arguments: [])
        }
    }

    /// Splits a free-text "key=value key2=value2" string (a ScriptAccount's
    /// `params`, or any other extra-params field) into individual argv
    /// tokens. Whitespace is the token separator — matches how `sys.argv`
    /// tokens arrive on the script side, so values can't contain spaces, but
    /// can contain their own '=' (o11.parse_params splits only on the first
    /// '=').
    static func splitParams(_ text: String) -> [String] {
        text.split(separator: " ").map(String.init).filter { !$0.isEmpty }
    }

    /// The "common to all scripts" argument prefix per the shared protocol,
    /// followed by the active account's user=/password= (omitted if blank —
    /// pairing is typically credential-less) and its extra params. Login
    /// authenticates with exactly user=/password= and nothing else an
    /// account might carry — extra params (region, device id, ...) are for
    /// actions that look up content, not for the credential exchange
    /// itself, so they're left out there.
    static func commonArgs(action: String, bind: String, proxy: String, doh: String, worker: String, account: ScriptAccount) -> [String] {
        var args = ["action=\(action)", "bind=\(bind)", "proxy=\(proxy)", "doh=\(doh)", "worker=\(worker)"]
        // Falls back to the account's Name when Username is left blank —
        // accounts are so often named after the login itself (e.g. an
        // account named "user" meant to log in as "user") that requiring
        // the same value typed twice just produced silently-empty user=
        // for people who only filled in Name.
        let user = account.username.isEmpty ? account.name : account.username
        if !user.isEmpty { args.append("user=\(user)") }
        if !account.password.isEmpty { args.append("password=\(account.password)") }
        if action == "login" { return args }
        return args + splitParams(account.params)
    }

    static func scriptExists(_ scriptPath: String) -> Bool {
        FileManager.default.isReadableFile(atPath: scriptPath)
    }

    /// Runs a bounded, one-shot script action and waits for it to exit,
    /// capturing all stdout/stderr. For actions that complete on their own
    /// without waiting on a human (manifest/cdm/channels/events/heartbeat —
    /// none of Phase 1's UI calls this yet, it's here for the next phase).
    /// Process has no built-in timeout, hence the poll loop.
    static func runSync(scriptPath rawScriptPath: String, args: [String], timeout: Double = 30) throws -> (stdout: String, stderr: String) {
        let scriptPath = normalizePath(rawScriptPath)
        guard scriptExists(scriptPath) else { throw ScriptRunnerError.scriptNotFound(scriptPath) }
        let spawn = invocation(for: scriptPath)
        let process = Process()
        process.executableURL = spawn.executable
        process.arguments = spawn.arguments + args
        let stdoutPipe = Pipe()
        let stderrPipe = Pipe()
        process.standardOutput = stdoutPipe
        process.standardError = stderrPipe
        try process.run()

        let deadline = Date().addingTimeInterval(timeout)
        while process.isRunning && Date() < deadline {
            Thread.sleep(forTimeInterval: 0.1)
        }
        if process.isRunning {
            process.terminate()
            throw ScriptRunnerError.timedOut
        }
        let stdout = String(data: stdoutPipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        let stderr = String(data: stderrPipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        guard process.terminationStatus == 0 else { throw ScriptRunnerError.nonZeroExit(code: process.terminationStatus, stderr: stderr) }
        return (stdout, stderr)
    }

    /// Runs a script action in the background, feeding stdout/stderr lines
    /// to `onLine` as they arrive rather than batching until exit — needed
    /// for login/pair, where a script may print a pairing code and then
    /// block for a while waiting on the user to act on it elsewhere (see
    /// disneyplus.py's commented-out pair() for exactly that shape). `onExit`
    /// fires once the process terminates (nil error on a clean exit).
    @discardableResult
    static func runStreaming(scriptPath rawScriptPath: String, args: [String], onLine: @escaping (String) -> Void, onExit: @escaping (Error?) -> Void) -> Process? {
        let scriptPath = normalizePath(rawScriptPath)
        guard scriptExists(scriptPath) else {
            onExit(ScriptRunnerError.scriptNotFound(scriptPath))
            return nil
        }
        let spawn = invocation(for: scriptPath)
        let process = Process()
        process.executableURL = spawn.executable
        process.arguments = spawn.arguments + args
        let stdoutPipe = Pipe()
        let stderrPipe = Pipe()
        process.standardOutput = stdoutPipe
        process.standardError = stderrPipe

        // Same leak PanelServer.start() already hit once with RestreamJob: a
        // closed pipe reads as perpetually "readable", so the handler MUST
        // be cleared on exit or its fd-monitoring thread spins forever.
        let forward: (FileHandle) -> Void = { handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
                onLine(String(line))
            }
        }
        stdoutPipe.fileHandleForReading.readabilityHandler = forward
        stderrPipe.fileHandleForReading.readabilityHandler = forward
        process.terminationHandler = { proc in
            stdoutPipe.fileHandleForReading.readabilityHandler = nil
            stderrPipe.fileHandleForReading.readabilityHandler = nil
            try? stdoutPipe.fileHandleForReading.close()
            try? stderrPipe.fileHandleForReading.close()
            // stderr here was already streamed line-by-line to onLine as it
            // happened, so there's nothing further to attach — unlike
            // runSync, which only reports once at the end and needs it.
            onExit(proc.terminationStatus == 0 ? nil : ScriptRunnerError.nonZeroExit(code: proc.terminationStatus, stderr: ""))
        }
        do {
            try process.run()
        } catch {
            stdoutPipe.fileHandleForReading.readabilityHandler = nil
            stderrPipe.fileHandleForReading.readabilityHandler = nil
            onExit(error)
            return nil
        }
        return process
    }
}
