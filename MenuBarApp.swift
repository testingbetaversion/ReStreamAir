#if os(macOS)
import AppKit
import Foundation

final class MenuBarController: NSObject, NSApplicationDelegate {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let statusLabel = NSMenuItem(title: "Stopped", action: nil, keyEquivalent: "")
    private let toggleItem = NSMenuItem(title: "Start", action: #selector(toggleServer), keyEquivalent: "")
    private let summaryItem = NSMenuItem(title: "", action: nil, keyEquivalent: "")

    private var process: Process?
    private var pollTimer: Timer?
    private let port: UInt16
    private let serverURL: URL

    override init() {
        port = UInt16(ProcessInfo.processInfo.environment["PORT"] ?? "8787") ?? 8787
        serverURL = MenuBarController.resolveServerPath()
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        if let button = statusItem.button {
            let symbol = NSImage(systemSymbolName: "antenna.radiowaves.left.and.right", accessibilityDescription: "ReStreamAir")
            symbol?.isTemplate = true
            button.image = symbol
            button.imagePosition = .imageOnly
        }

        let menu = NSMenu()
        statusLabel.isEnabled = false
        menu.addItem(statusLabel)
        summaryItem.isEnabled = false
        menu.addItem(summaryItem)
        menu.addItem(.separator())
        toggleItem.target = self
        menu.addItem(toggleItem)
        menu.addItem(.separator())
        let openItem = NSMenuItem(title: "Open Panel", action: #selector(openPanel), keyEquivalent: "o")
        openItem.target = self
        menu.addItem(openItem)
        menu.addItem(.separator())
        let quitItem = NSMenuItem(title: "Quit", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)
        statusItem.menu = menu

        refreshStatus()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
            self?.refreshStatus()
        }
    }

    private static func resolveServerPath() -> URL {
        if let override = ProcessInfo.processInfo.environment["RESTREAMAIR_PATH"] {
            return URL(fileURLWithPath: override)
        }
        let selfURL = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
        return selfURL.deletingLastPathComponent().appendingPathComponent("restreamair")
    }

    @objc private func toggleServer() {
        if process != nil {
            stopServer()
        } else {
            startServer()
        }
    }

    private func startServer() {
        guard process == nil else { return }
        guard FileManager.default.isExecutableFile(atPath: serverURL.path) else {
            statusLabel.title = "Missing restreamair binary"
            return
        }
        let task = Process()
        task.executableURL = serverURL
        task.arguments = ["serve", "--port", "\(port)"]
        task.currentDirectoryURL = serverURL.deletingLastPathComponent()
        var environment = ProcessInfo.processInfo.environment
        environment["PORT"] = "\(port)"
        task.environment = environment
        task.terminationHandler = { [weak self] _ in
            DispatchQueue.main.async {
                self?.process = nil
                self?.refreshStatus()
            }
        }
        do {
            try task.run()
            process = task
        } catch {
            statusLabel.title = "Failed to start: \(error.localizedDescription)"
        }
        refreshStatus()
    }

    private func stopServer() {
        process?.terminate()
        process = nil
        refreshStatus()
    }

    @objc private func openPanel() {
        NSWorkspace.shared.open(URL(string: "http://127.0.0.1:\(port)")!)
    }

    @objc private func quit() {
        process?.terminate()
        NSApp.terminate(nil)
    }

    private func refreshStatus() {
        let running = process != nil
        statusLabel.title = running ? "Running on port \(port)" : "Stopped"
        toggleItem.title = running ? "Stop" : "Start"
        guard running else {
            summaryItem.title = ""
            return
        }
        pollState()
    }

    private func pollState() {
        let url = URL(string: "http://127.0.0.1:\(port)/api/state")!
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let data = try? HTTPClient(options: HTTPRequestOptions(timeout: 3)).get(url) else { return }
            guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let providers = object["providers"] as? [[String: Any]] else { return }
            let streams = providers.flatMap { ($0["streams"] as? [[String: Any]]) ?? [] }
            let runningCount = streams.filter { ($0["running"] as? Bool) == true }.count
            DispatchQueue.main.async {
                self?.summaryItem.title = "\(runningCount) of \(streams.count) stream(s) running"
            }
        }
    }
}

@main
struct MenuBarMain {
    static func main() {
        let app = NSApplication.shared
        let controller = MenuBarController()
        app.delegate = controller
        app.run()
    }
}
#else
@main
struct MenuBarMain {
    static func main() {
        fputs("restreamair-menubar is only supported on macOS.\n", stderr)
        exit(1)
    }
}
#endif
