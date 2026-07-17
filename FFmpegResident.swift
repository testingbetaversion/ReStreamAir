import Foundation

/// Builds the command line for a *resident* ffmpeg process — a single
/// long-running `ffmpeg` that reads the source directly (DASH/HLS, decrypting
/// CENC clearkey itself) and remuxes with `-c copy` to one of several delivery
/// targets: a local HLS playlist the panel already serves, or an SRT/UDP/TCP
/// push. This is the alternative to the internal Swift downloader/mixer
/// (`inputMode == "internal"`), selected per stream via `inputMode` +
/// `outputMode`.
///
/// The shape follows the hand-written ffmpeg commands this is meant to
/// replace: `-headers` for auth/UA, `-cenc_decryption_key` (DASH) or
/// `-decryption_key` (HLS) for clearkey, `-c copy` (no re-encode), and an HLS
/// muxer configured for a rolling live window with `delete_segments`.
enum FFmpegResident {
    struct Inputs {
        var ffmpegPath: String
        var sourceURL: String
        var kind: String                 // "mpd" | "m3u8"
        var inputMode: String            // ffmpegResident | ffmpegTsHls | ffmpegMultiTsHls | ffmpegFmp4Hls
        var outputMode: String           // hls | srtServer | udpSrt | custom
        var outputTarget: String
        /// "KID:KEY" (or bare "KEY") lines, newline-joined — the same field the
        /// internal decryptor consumes.
        var decryptionKeys: String
        /// HTTP headers as "Name: value" lines, newline-joined.
        var headers: String
        var proxy: String
        /// Raw query fragment appended to the source URL (token=…&region=…).
        var segmentUrlParams: String
        /// Selected representation ids (for the multi-variant mode) and their
        /// audio/video typing.
        var representationIds: [String]
        var representationTypes: [String: String]   // id -> "video"|"audio"|...
        var tempDir: URL
        var outputPlaylist: URL          // <tempDir>/live.m3u8
        var playlistSegments: Int
        var segmentSeconds: Int
    }

    struct Command {
        var arguments: [String]
        /// Extra environment (proxy) layered onto the process's own.
        var environment: [String: String]
    }

    /// The first clearkey value ffmpeg needs — it wants the 16-byte content key
    /// as hex, without the KID. Accepts "KID:KEY" or a bare "KEY" line.
    static func firstClearKey(_ keys: String) -> String? {
        for rawLine in keys.split(whereSeparator: { $0 == "\n" || $0 == "\r" || $0 == "|" }) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard !line.isEmpty else { continue }
            let parts = line.split(separator: ":", maxSplits: 1)
            let key = (parts.count == 2 ? parts[1] : parts[0]).trimmingCharacters(in: .whitespaces)
            if !key.isEmpty { return key }
        }
        return nil
    }

    /// ffmpeg's `-headers` takes a single CRLF-separated, CRLF-terminated
    /// string, not one flag per header.
    static func headerBlock(_ headers: String) -> String? {
        let lines = headers
            .split(whereSeparator: { $0 == "\n" || $0 == "\r" })
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        guard !lines.isEmpty else { return nil }
        return lines.joined(separator: "\r\n") + "\r\n"
    }

    private static func withParams(_ url: String, _ params: String) -> String {
        let trimmed = params.trimmingCharacters(in: CharacterSet(charactersIn: " ?&"))
        guard !trimmed.isEmpty else { return url }
        return url.contains("?") ? "\(url)&\(trimmed)" : "\(url)?\(trimmed)"
    }

    static func command(_ inputs: Inputs) -> Command {
        var args: [String] = [
            "-hide_banner",
            "-loglevel", "warning",
            "-nostdin",
            "-y",
        ]

        // ---- Input options (must precede -i) ----
        // Survive routine live-edge network blips instead of exiting.
        if inputs.sourceURL.hasPrefix("http") {
            args += ["-reconnect", "1", "-reconnect_streamed", "1",
                     "-reconnect_on_network_error", "1", "-reconnect_delay_max", "2"]
            // microseconds; guards a wedged socket at the live edge.
            args += ["-rw_timeout", "15000000"]
        }
        if let block = headerBlock(inputs.headers) {
            args += ["-headers", block]
        }
        // Clearkey: ffmpeg decrypts CENC itself given the content key. DASH uses
        // -cenc_decryption_key; HLS SAMPLE-AES/clearkey uses -decryption_key.
        if let key = firstClearKey(inputs.decryptionKeys) {
            args += [inputs.kind == "m3u8" ? "-decryption_key" : "-cenc_decryption_key", key]
        }
        args += ["-i", withParams(inputs.sourceURL, inputs.segmentUrlParams)]

        // ---- Mapping + codec ----
        // Copy only (container remux, no transcode — the whole point of a
        // passthrough restream).
        let multi = inputs.inputMode == "ffmpegMultiTsHls"
        let videoIds = inputs.representationIds.filter { inputs.representationTypes[$0] != "audio" }

        if multi && videoIds.count > 1 {
            // One HLS variant per selected video, each mapped to a distinct
            // source video stream, all sharing the first audio. The trailing
            // "?" keeps a missing stream non-fatal. The var_stream_map built in
            // hlsOutputArguments lines up with this ordering (N videos, 1 audio).
            for index in 0..<videoIds.count { args += ["-map", "0:v:\(index)?"] }
            args += ["-map", "0:a:0?"]
        } else {
            args += ["-map", "0:v:0?", "-map", "0:a:0?"]
        }
        args += ["-c", "copy"]

        // ---- Output / delivery ----
        switch inputs.outputMode {
        case "srtServer":
            // Listen for pulls. outputTarget is a bare port (or full srt:// URL).
            let target = inputs.outputTarget.contains("://")
                ? inputs.outputTarget
                : "srt://0.0.0.0:\(inputs.outputTarget.isEmpty ? "9000" : inputs.outputTarget)?mode=listener"
            args += ["-f", "mpegts", target]
        case "udpSrt":
            // Push out. outputTarget is the full udp:// / srt:// / tcp:// URL.
            args += ["-f", "mpegts", inputs.outputTarget]
        case "custom":
            // Raw pass-through: the user supplies -map/-f/target themselves.
            args += tokenize(inputs.outputTarget)
        default: // "hls"
            args += hlsOutputArguments(inputs, multi: multi, variants: max(1, videoIds.count))
        }

        var env: [String: String] = [:]
        if !inputs.proxy.isEmpty {
            env["http_proxy"] = inputs.proxy
            env["https_proxy"] = inputs.proxy
        }
        return Command(arguments: args, environment: env)
    }

    private static func hlsOutputArguments(_ inputs: Inputs, multi: Bool, variants: Int) -> [String] {
        // ffmpegTsHls / ffmpegMultiTsHls → MPEG-TS segments; the fmp4 modes (and
        // the generic resident default) → CMAF/fMP4 segments.
        let useTS = inputs.inputMode == "ffmpegTsHls" || inputs.inputMode == "ffmpegMultiTsHls"
        let ext = useTS ? "ts" : "m4s"
        let dir = inputs.tempDir.path

        var out: [String] = [
            "-f", "hls",
            "-hls_time", String(inputs.segmentSeconds),
            "-hls_list_size", String(inputs.playlistSegments),
            "-hls_delete_threshold", "1",
            "-hls_flags", "delete_segments+independent_segments+program_date_time+temp_file",
        ]
        out += ["-hls_segment_type", useTS ? "mpegts" : "fmp4"]
        if !useTS {
            out += ["-hls_fmp4_init_filename", "init.mp4"]
            // MPEG-TS/HLS sources carry AAC as ADTS; muxing that into fMP4 with
            // -c copy fails ("Malformed AAC bitstream … use aac_adtstoasc")
            // unless the ADTS headers are stripped to ASC. Harmless no-op when
            // the source audio is already fMP4/CMAF (e.g. a DASH origin).
            out += ["-bsf:a", "aac_adtstoasc"]
        }

        if multi && variants > 1 {
            // Multi-variant master + one media playlist per variant under %v/.
            out += ["-hls_segment_filename", "\(dir)/%v/seg_%05d.\(ext)"]
            out += ["-master_pl_name", "master.m3u8"]
            let map = (0..<variants).map { "v:\($0),a:0" }.joined(separator: " ")
            out += ["-var_stream_map", map]
            out += ["\(dir)/%v/live.m3u8"]
        } else {
            out += ["-hls_segment_filename", "\(dir)/seg_%05d.\(ext)"]
            out += [inputs.outputPlaylist.path]
        }
        return out
    }

    /// Whitespace tokenizer that honours single/double quotes so a custom
    /// output string can contain quoted arguments.
    static func tokenize(_ text: String) -> [String] {
        var tokens: [String] = []
        var current = ""
        var quote: Character? = nil
        for ch in text {
            if let q = quote {
                if ch == q { quote = nil } else { current.append(ch) }
            } else if ch == "\"" || ch == "'" {
                quote = ch
            } else if ch == " " || ch == "\t" || ch == "\n" {
                if !current.isEmpty { tokens.append(current); current = "" }
            } else {
                current.append(ch)
            }
        }
        if !current.isEmpty { tokens.append(current) }
        return tokens
    }
}
