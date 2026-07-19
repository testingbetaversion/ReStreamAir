// swift-tools-version:6.0
//
// This Package.swift exists ONLY to drive the fully-static Linux build via the
// musl Static Linux SDK (`swift build --swift-sdk x86_64-swift-linux-musl`),
// which produces a single dependency-free binary. Everyday macOS/dev builds
// still use the plain `swiftc` one-liner in the README — this file doesn't
// change that. Sources are auto-discovered (every .swift in the root except the
// excluded ones) so a newly added file can't silently be left out of the musl
// build — which is exactly what happened when CDMKeyResolver.swift was added.
import PackageDescription

let package = Package(
    name: "restreamair",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "restreamair",
            path: ".",
            // Everything that isn't a compilable source of THIS target: the
            // macOS-only menu-bar companion, and non-source files/dirs in the
            // repo root. Package.swift itself is excluded by SwiftPM automatically.
            exclude: [
                "MenuBarApp.swift", "public", "scripts", "runtime", "logs",
                "README.md", "SCRIPTING.md", "state.json", "logo-cache.json",
            ],
            // The plain swiftc build compiles in Swift 5 mode; match it so
            // Swift 6 strict-concurrency checks don't reject the existing code.
            swiftSettings: [.swiftLanguageMode(.v5)]
        )
    ]
)
