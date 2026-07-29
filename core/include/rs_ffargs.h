#ifndef RS_FFARGS_H
#define RS_FFARGS_H

// Builds the command line for a resident ffmpeg process — the C port of
// FFmpegResident.swift. Pure string assembly: nothing here spawns anything.
//
// As in the Swift version the ffmpeg executable is NOT part of the argument
// list; argv starts at "-hide_banner" and the caller prepends the binary path.

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
// Generate installation plan for ffmpeg (returns malloc'd shell command string or NULL).
char *rs_ffmpeg_install_plan(void);

#ifdef __cplusplus
}
#endif

#endif  // RS_FFARGS_H
