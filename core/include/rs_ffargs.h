#ifndef RS_FFARGS_H
#define RS_FFARGS_H

// Builds the command line for a resident ffmpeg process. Pure string assembly:
// nothing here spawns anything.
//
// The ffmpeg executable is deliberately NOT part of the argument list; argv
// starts at "-hide_banner" and the caller prepends the binary path.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *source_url;
    const char *kind;           // "mpd" | "m3u8"
    const char *input_mode;     // ffmpegResident | ffmpegTsHls | ffmpegMultiTsHls | ffmpegFmp4Hls
    const char *output_mode;    // hls | srtServer | udpSrt | custom
    const char *output_target;

    // "KID:KEY" or bare "KEY" lines, separated by newlines or "|" — the same
    // field the internal decryptor consumes.
    const char *decryption_keys;
    // HTTP headers as "Name: value" lines, newline-separated.
    const char *headers;
    const char *proxy;
    // Raw query fragment appended to the source URL (token=…&region=…).
    const char *segment_url_params;

    // Selected representation ids and their audio/video typing, as parallel
    // arrays: rep_type_ids[i] is typed by rep_type_values[i] ("video"/"audio").
    const char *const *representation_ids;
    size_t representation_id_count;
    const char *const *rep_type_ids;
    const char *const *rep_type_values;
    size_t rep_type_count;

    const char *temp_dir;         // filesystem path
    const char *output_playlist;  // filesystem path, <temp_dir>/live.m3u8
    int playlist_segments;
    int segment_seconds;
} rs_ffargs_inputs;

typedef struct {
    char **argv;  // argv[argc] is NULL, so the array can be handed to exec directly
    size_t argc;
    // Extra environment (proxy) to layer onto the process's own.
    char **env_keys;
    char **env_values;
    size_t env_count;
} rs_ffargs_command;

// Returns 0 on success. The result is released with rs_ffargs_command_dispose.
int rs_ffargs_build(const rs_ffargs_inputs *inputs, rs_ffargs_command *out);
void rs_ffargs_command_dispose(rs_ffargs_command *cmd);

// The first clearkey value ffmpeg needs: the content key as hex, without the
// KID. Accepts "KID:KEY" or a bare "KEY" line. NULL if there is none.
char *rs_ffargs_first_clear_key(const char *keys);

// ffmpeg's -headers takes one CRLF-separated, CRLF-terminated string rather
// than a flag per header. NULL if there are no headers.
char *rs_ffargs_header_block(const char *headers);

// Whitespace tokenizer that honours single and double quotes, so a custom
// output string can contain quoted arguments. Returns 0 on success; release
// with rs_free_strv(*tokens, *count).
int rs_ffargs_tokenize(const char *text, char ***tokens, size_t *count);

// Locate ffmpeg on PATH and standard locations (returns malloc'd path or NULL).
char *rs_ffmpeg_resolve(void);

// What a located ffmpeg can actually do, as opposed to whether it exists.
//
// "ffmpeg is installed" is not the question that matters. A DASH source needs
// the `dash` demuxer, which libavformat only builds when configured with
// --enable-libxml2 — and plenty of distribution and Homebrew builds omit it. On
// such a build `-i whatever.mpd` fails outright, which looks like a broken
// stream rather than a missing feature, so the panel checks for the capability
// and says so. Package names are deliberately not consulted: "ffmpeg-full"
// means something only to Homebrew, while the demuxer list is the truth on every
// platform.
typedef struct {
    bool has_dash;      // the dash demuxer (requires libxml2)
    bool has_hls;       // the hls demuxer
    bool has_libxml2;   // --enable-libxml2 in the build configuration
    bool has_srt;       // srt:// protocol, for the SRT output modes
    bool has_https;     // https:// protocol
    char *version;      // first line of `ffmpeg -version`, or NULL
} rs_ffmpeg_caps;

// Probes `path` (or a resolved ffmpeg when NULL) by running it. Costs two short
// subprocess invocations, so callers should cache the result. Release with
// rs_ffmpeg_caps_dispose.
int rs_ffmpeg_probe_caps(const char *path, rs_ffmpeg_caps *out);
void rs_ffmpeg_caps_dispose(rs_ffmpeg_caps *caps);
// Generate installation plan for ffmpeg (returns malloc'd shell command string or NULL).
char *rs_ffmpeg_install_plan(void);

#ifdef __cplusplus
}
#endif

#endif  // RS_FFARGS_H
