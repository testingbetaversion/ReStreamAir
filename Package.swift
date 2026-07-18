// swift-tools-version:6.0
//
// This Package.swift exists ONLY to drive the fully-static Linux build via the
// musl Static Linux SDK (`swift build --swift-sdk x86_64-swift-linux-musl`),
// which produces a single dependency-free binary. Everyday macOS/dev builds
// still use the plain `swiftc` one-liner in the README — this file doesn't
// change that. The source list mirrors that one-liner (every .swift except the
// macOS-only menu-bar companion).
import PackageDescription

let package = Package(
    name: "restreamair",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "restreamair",
            path: ".",
            // Non-source files/dirs in the repo root, so SwiftPM doesn't flag them.
            exclude: [
                "MenuBarApp.swift", "public", "scripts", "runtime", "logs",
                "README.md", "SCRIPTING.md", "state.json", "logo-cache.json",
            ],
            sources: [
                "HTTPClient.swift",
                "Version.swift",
                "Crypto.swift",
                "AES.swift",
                "Net.swift",
                "DashSegments.swift",
                "M3U8Rewriter.swift",
                "FFmpegLocator.swift",
                "FFmpegResident.swift",
                "LiveMPDToM3U8.swift",
                "PanelServer.swift",
                "CENCDecryptor.swift",
                "HLSDecryptor.swift",
                "MetricsStore.swift",
                "SystemStats.swift",
                "LogoLookup.swift",
                "AuthStore.swift",
                "LogStore.swift",
                "AudioDelay.swift",
                "ScriptRunner.swift",
                "PublicAssets.swift",
            ],
            // The plain swiftc build compiles in Swift 5 mode; match it so
            // Swift 6 strict-concurrency checks don't reject the existing code.
            swiftSettings: [.swiftLanguageMode(.v5)]
        )
    ]
)
