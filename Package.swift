// swift-tools-version:6.0
//
// The macOS build. It compiles the C core (core/src, core/deps) and the Swift
// app together, and also drives the fully-static Linux build via the musl
// Static Linux SDK. Linux and Windows otherwise build the core through
// CMakeLists.txt instead, which lists the same C sources — keep the two in step.
//
// Swift sources are auto-discovered (every .swift in the root except the
// excluded ones) so a newly added file cannot silently be left out of the musl
// build, which is exactly what happened when CDMKeyResolver.swift was added.
import PackageDescription

let package = Package(
    name: "restreamair",
    platforms: [.macOS(.v13)],
    targets: [
        // The C core. `sources` is an allowlist, so core/tests and core/include
        // are not compiled here — and nothing under src/ or deps/ may ever
        // define main(), or the executable below stops linking.
        .target(
            name: "restream_core",
            path: "core",
            sources: ["src", "deps"],
            publicHeadersPath: "include"
        ),
        .executableTarget(
            name: "restreamair",
            dependencies: ["restream_core"],
            path: ".",
            // Everything that isn't a compilable Swift source of THIS target:
            // the macOS-only menu-bar companion, the C/C++ trees (SwiftPM
            // refuses a target with mixed languages), and non-source files.
            // Package.swift itself is excluded by SwiftPM automatically.
            exclude: [
                "MenuBarApp.swift", "public", "scripts", "runtime", "logs",
                "README.md", "SCRIPTING.md", "state.json", "logo-cache.json",
                "core", "apps", "build", "CMakeLists.txt",
            ],
            // The plain swiftc build compiles in Swift 5 mode; match it so
            // Swift 6 strict-concurrency checks don't reject the existing code.
            swiftSettings: [.swiftLanguageMode(.v5)]
        )
    ]
)
