import Foundation

enum NM3U8DLRELocator {
    // Reuse the shared, cross-platform PATH lookup (handles ';' vs ':' and the
    // .exe suffix on Windows) rather than duplicating it.
    static func resolve() -> String? {
        FFmpegLocator.resolve("N_m3u8DL-RE")
    }
}

enum NM3U8DLREInstaller {
    struct Plan {
        let executable: String
        let arguments: [String]
        let displayCommand: String
    }

    static func plan() -> Plan? {
        #if os(macOS)
        #if arch(arm64)
        let asset = "N_m3u8DL-RE_Beta_macOS_arm64.tar.gz"
        #else
        let asset = "N_m3u8DL-RE_Beta_macOS_x64.tar.gz"
        #endif
        #elseif os(Linux)
        #if arch(arm64)
        let asset = "N_m3u8DL-RE_Beta_linux-arm64.tar.gz"
        #else
        let asset = "N_m3u8DL-RE_Beta_linux-x64.tar.gz"
        #endif
        #else
        return nil
        #endif

        // The auto-install uses sh/curl/tar, which only exist on macOS/Linux;
        // guard the whole body (which references `asset`, undefined elsewhere) so
        // Windows/other platforms compile — they just return nil above.
        #if os(macOS) || os(Linux)
        let script = """
        curl -sL https://api.github.com/repos/nilaoda/N_m3u8DL-RE/releases/latest | grep "browser_download_url.*\(asset)" | cut -d '"' -f 4 | xargs curl -sLO
        tar -xzf \(asset)
        find . -name "N_m3u8DL-RE" -type f -exec cp {} /usr/local/bin/ \\;
        rm -rf N_m3u8DL-RE_* \(asset)
        """

        let displayScript = "curl -sLO https://github.com/nilaoda/N_m3u8DL-RE/releases/latest/download/\(asset) && tar -xzf \(asset) && sudo cp */N_m3u8DL-RE /usr/local/bin/"

        guard let sh = FFmpegLocator.resolve("sh") else { return nil }
        return Plan(executable: sh, arguments: ["-c", script], displayCommand: displayScript)
        #endif
    }

    static var manualCommand: String {
        return "Download N_m3u8DL-RE from https://github.com/nilaoda/N_m3u8DL-RE/releases and place it in your PATH."
    }
}
