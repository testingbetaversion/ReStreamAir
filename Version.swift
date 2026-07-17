import Foundation

/// Build identity for this binary. The defaults below are what a local
/// `./run` / `swiftc` build reports ("dev"); the GitHub Actions release
/// workflow rewrites this file from the pushed git tag before compiling, so a
/// released binary reports its real version (e.g. "1.2.0") and commit. The
/// panel compares `version` against the latest GitHub release to tell the user
/// when their binary is out of date.
enum AppVersion {
    /// Semantic version without the leading "v" (e.g. "1.2.0"), or "dev" for a
    /// local build.
    static let version = "dev"
    /// Short commit SHA, or "unknown".
    static let commit = "unknown"

    static var isReleaseBuild: Bool { version != "dev" }

    /// GitHub repo the update check queries.
    static let repoOwner = "testingbetaversion"
    static let repoName = "ReStreamAir"
}
