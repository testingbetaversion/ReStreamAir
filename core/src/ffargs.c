#include "rs_ffargs.h"

#include <stdlib.h>
#include <string.h>

#include "rs_internal.h"

static bool str_equal(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static const char *or_empty(const char *s) {
    return s ? s : "";
}

static bool is_key_separator(char c) {
    return c == '\n' || c == '\r' || c == '|';
}

char *rs_ffargs_first_clear_key(const char *keys) {
    if (!keys) return NULL;
    size_t len = strlen(keys);
    size_t i = 0;
    while (i < len) {
        while (i < len && is_key_separator(keys[i])) i++;  // Swift's split omits empties
        size_t start = i;
        while (i < len && !is_key_separator(keys[i])) i++;

        const char *line = NULL;
        size_t line_len = rs_trim(keys + start, i - start, false, &line);
        if (line_len == 0) continue;

        // "KID:KEY" splits into two pieces and the key is the second; a bare
        // "KEY" (or a line whose other half is empty) leaves just one piece,
        // which is then the key itself.
        const char *colon = (const char *)memchr(line, ':', line_len);
        const char *candidate = line;
        size_t candidate_len = line_len;
        if (colon) {
            size_t left_len = (size_t)(colon - line);
            size_t right_len = line_len - left_len - 1;
            if (left_len > 0 && right_len > 0) {
                candidate = colon + 1;
                candidate_len = right_len;
            } else if (left_len > 0) {
                candidate_len = left_len;
            } else if (right_len > 0) {
                candidate = colon + 1;
                candidate_len = right_len;
            } else {
                continue;
            }
        }
        char *key = rs_trim_dup(candidate, candidate_len, false);
        if (key && key[0] != '\0') return key;
        free(key);
    }
    return NULL;
}

char *rs_ffargs_header_block(const char *headers) {
    if (!headers) return NULL;
    size_t len = strlen(headers);
    rs_buf out = RS_BUF_INIT;
    bool any = false;
    size_t i = 0;
    while (i < len) {
        while (i < len && (headers[i] == '\n' || headers[i] == '\r')) i++;
        size_t start = i;
        while (i < len && headers[i] != '\n' && headers[i] != '\r') i++;

        const char *line = NULL;
        size_t line_len = rs_trim(headers + start, i - start, false, &line);
        if (line_len == 0) continue;
        if (any) rs_buf_append_str(&out, "\r\n");
        rs_buf_append(&out, line, line_len);
        any = true;
    }
    if (!any) {
        rs_buf_dispose(&out);
        return NULL;
    }
    rs_buf_append_str(&out, "\r\n");
    return rs_buf_take(&out);
}

int rs_ffargs_tokenize(const char *text, char ***tokens, size_t *count) {
    if (!tokens || !count) return -1;
    *tokens = NULL;
    *count = 0;
    if (!text) return 0;

    rs_strv out = RS_STRV_INIT;
    rs_buf current = RS_BUF_INIT;
    size_t current_len = 0;
    char quote = '\0';
    for (const char *p = text; *p; p++) {
        char c = *p;
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                rs_buf_append_char(&current, c);
                current_len++;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (current_len > 0) {
                rs_strv_push_owned(&out, rs_buf_take(&current));
                current_len = 0;
            }
        } else {
            rs_buf_append_char(&current, c);
            current_len++;
        }
    }
    if (current_len > 0) {
        rs_strv_push_owned(&out, rs_buf_take(&current));
    } else {
        rs_buf_dispose(&current);
    }

    if (out.err) {
        rs_strv_dispose(&out);
        return -1;
    }
    *tokens = out.items;
    *count = out.len;
    return 0;
}

// Appends the query fragment to the source URL, trimming any leading or
// trailing " ?&" the caller left on it.
static char *with_params(const char *url, const char *params) {
    const char *trimmed = NULL;
    size_t len = params ? strlen(params) : 0;
    size_t start = 0;
    while (start < len && (params[start] == ' ' || params[start] == '?' || params[start] == '&')) start++;
    while (len > start && (params[len - 1] == ' ' || params[len - 1] == '?' || params[len - 1] == '&')) len--;
    trimmed = params + start;
    size_t trimmed_len = len - start;

    if (trimmed_len == 0) return rs_strdup(or_empty(url));

    rs_buf buf = RS_BUF_INIT;
    rs_buf_append_str(&buf, or_empty(url));
    rs_buf_append_char(&buf, strchr(or_empty(url), '?') ? '&' : '?');
    rs_buf_append(&buf, trimmed, trimmed_len);
    return rs_buf_take(&buf);
}

static const char *representation_type(const rs_ffargs_inputs *in, const char *id) {
    for (size_t i = 0; i < in->rep_type_count; i++) {
        if (str_equal(in->rep_type_ids[i], id)) return in->rep_type_values[i];
    }
    return NULL;
}

static void append_hls_output(rs_strv *args, const rs_ffargs_inputs *in, bool multi, size_t variants) {
    // The TS modes emit MPEG-TS segments; the fmp4 mode and the generic
    // resident default emit CMAF/fMP4.
    bool use_ts = str_equal(in->input_mode, "ffmpegTsHls") || str_equal(in->input_mode, "ffmpegMultiTsHls");
    const char *ext = use_ts ? "ts" : "m4s";
    const char *dir = or_empty(in->temp_dir);

    rs_strv_push(args, "-f");
    rs_strv_push(args, "hls");
    rs_strv_push(args, "-hls_time");
    rs_strv_pushf(args, "%d", in->segment_seconds);
    rs_strv_push(args, "-hls_list_size");
    rs_strv_pushf(args, "%d", in->playlist_segments);
    rs_strv_push(args, "-hls_delete_threshold");
    rs_strv_push(args, "1");
    rs_strv_push(args, "-hls_flags");
    rs_strv_push(args, "delete_segments+independent_segments+program_date_time+temp_file");
    rs_strv_push(args, "-hls_segment_type");
    rs_strv_push(args, use_ts ? "mpegts" : "fmp4");
    if (!use_ts) {
        rs_strv_push(args, "-hls_fmp4_init_filename");
        rs_strv_push(args, "init.mp4");
        // MPEG-TS sources carry AAC as ADTS; muxing that into fMP4 with -c copy
        // fails unless the headers are stripped to ASC. A no-op when the source
        // audio is already fMP4/CMAF.
        rs_strv_push(args, "-bsf:a");
        rs_strv_push(args, "aac_adtstoasc");
    }

    if (multi && variants > 1) {
        rs_strv_push(args, "-hls_segment_filename");
        rs_strv_pushf(args, "%s/%%v/seg_%%05d.%s", dir, ext);
        rs_strv_push(args, "-master_pl_name");
        rs_strv_push(args, "master.m3u8");

        rs_buf map = RS_BUF_INIT;
        for (size_t i = 0; i < variants; i++) {
            if (i > 0) rs_buf_append_char(&map, ' ');
            rs_buf_appendf(&map, "v:%zu,a:0", i);
        }
        rs_strv_push(args, "-var_stream_map");
        rs_strv_push_owned(args, rs_buf_take(&map));
        rs_strv_pushf(args, "%s/%%v/live.m3u8", dir);
    } else {
        rs_strv_push(args, "-hls_segment_filename");
        rs_strv_pushf(args, "%s/seg_%%05d.%s", dir, ext);
        rs_strv_push(args, or_empty(in->output_playlist));
    }
}

int rs_ffargs_build(const rs_ffargs_inputs *in, rs_ffargs_command *out) {
    if (!in || !out) return -1;
    memset(out, 0, sizeof(*out));

    rs_strv args = RS_STRV_INIT;
    rs_strv_push(&args, "-hide_banner");
    rs_strv_push(&args, "-loglevel");
    rs_strv_push(&args, "warning");
    rs_strv_push(&args, "-nostdin");
    rs_strv_push(&args, "-y");

    // Input options, which must precede -i.
    const char *source = or_empty(in->source_url);
    if (strncmp(source, "http", 4) == 0) {
        // Survive routine live-edge network blips instead of exiting.
        rs_strv_push(&args, "-reconnect");
        rs_strv_push(&args, "1");
        rs_strv_push(&args, "-reconnect_streamed");
        rs_strv_push(&args, "1");
        rs_strv_push(&args, "-reconnect_on_network_error");
        rs_strv_push(&args, "1");
        rs_strv_push(&args, "-reconnect_delay_max");
        rs_strv_push(&args, "2");
        // Microseconds; guards a wedged socket at the live edge.
        rs_strv_push(&args, "-rw_timeout");
        rs_strv_push(&args, "15000000");
    }
    char *block = rs_ffargs_header_block(in->headers);
    if (block) {
        rs_strv_push(&args, "-headers");
        rs_strv_push_owned(&args, block);
    }
    // Clearkey: ffmpeg decrypts CENC itself given the content key. DASH uses
    // -cenc_decryption_key; HLS SAMPLE-AES/clearkey uses -decryption_key.
    char *key = rs_ffargs_first_clear_key(in->decryption_keys);
    if (key) {
        rs_strv_push(&args, str_equal(in->kind, "m3u8") ? "-decryption_key" : "-cenc_decryption_key");
        rs_strv_push_owned(&args, key);
    }
    rs_strv_push(&args, "-i");
    rs_strv_push_owned(&args, with_params(source, in->segment_url_params));

    // Mapping and codec: copy only, no transcode.
    bool multi = str_equal(in->input_mode, "ffmpegMultiTsHls");
    size_t video_count = 0;
    for (size_t i = 0; i < in->representation_id_count; i++) {
        const char *type = representation_type(in, in->representation_ids[i]);
        if (!str_equal(type, "audio")) video_count++;
    }

    if (multi && video_count > 1) {
        // One HLS variant per selected video, each mapped to a distinct source
        // video stream, all sharing the first audio. The trailing "?" keeps a
        // missing stream non-fatal, and the var_stream_map built in
        // append_hls_output lines up with this ordering.
        for (size_t i = 0; i < video_count; i++) {
            rs_strv_push(&args, "-map");
            rs_strv_pushf(&args, "0:v:%zu?", i);
        }
        rs_strv_push(&args, "-map");
        rs_strv_push(&args, "0:a:0?");
    } else {
        rs_strv_push(&args, "-map");
        rs_strv_push(&args, "0:v:0?");
        rs_strv_push(&args, "-map");
        rs_strv_push(&args, "0:a:0?");
    }
    rs_strv_push(&args, "-c");
    rs_strv_push(&args, "copy");

    // Output and delivery.
    const char *target = or_empty(in->output_target);
    if (str_equal(in->output_mode, "srtServer")) {
        // Listen for pulls. output_target is a bare port, or a full srt:// URL.
        rs_strv_push(&args, "-f");
        rs_strv_push(&args, "mpegts");
        if (strstr(target, "://")) {
            rs_strv_push(&args, target);
        } else {
            rs_strv_pushf(&args, "srt://0.0.0.0:%s?mode=listener", target[0] ? target : "9000");
        }
    } else if (str_equal(in->output_mode, "udpSrt")) {
        // Push out. output_target is the full udp:// / srt:// / tcp:// URL.
        rs_strv_push(&args, "-f");
        rs_strv_push(&args, "mpegts");
        rs_strv_push(&args, target);
    } else if (str_equal(in->output_mode, "custom")) {
        // Raw pass-through: the user supplies -map/-f/target themselves.
        char **tokens = NULL;
        size_t token_count = 0;
        if (rs_ffargs_tokenize(target, &tokens, &token_count) != 0) {
            args.err = true;
        } else {
            for (size_t i = 0; i < token_count; i++) rs_strv_push(&args, tokens[i]);
            rs_free_strv(tokens, token_count);
        }
    } else {
        append_hls_output(&args, in, multi, video_count > 1 ? video_count : 1);
    }

    rs_strv env_keys = RS_STRV_INIT;
    rs_strv env_values = RS_STRV_INIT;
    if (in->proxy && in->proxy[0] != '\0') {
        rs_strv_push(&env_keys, "http_proxy");
        rs_strv_push(&env_values, in->proxy);
        rs_strv_push(&env_keys, "https_proxy");
        rs_strv_push(&env_values, in->proxy);
    }

    if (args.err || !args.items || env_keys.err || env_values.err) {
        rs_strv_dispose(&args);
        rs_strv_dispose(&env_keys);
        rs_strv_dispose(&env_values);
        return -1;
    }

    args.items[args.len] = NULL;  // rs_strv always keeps room for the terminator
    out->argv = args.items;
    out->argc = args.len;
    out->env_keys = env_keys.items;
    out->env_values = env_values.items;
    out->env_count = env_keys.len;
    return 0;
}

void rs_ffargs_command_dispose(rs_ffargs_command *cmd) {
    if (!cmd) return;
    rs_free_strv(cmd->argv, cmd->argc);
    rs_free_strv(cmd->env_keys, cmd->env_count);
    rs_free_strv(cmd->env_values, cmd->env_count);
    memset(cmd, 0, sizeof(*cmd));
}
