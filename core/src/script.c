#include "rs_script.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "rs_proc.h"

static bool needs_encoding(const char *value) {
    for (const char *p = value; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c > 0x7e) return true;
        if (c == '"' || c == '\'' || c == '\\') return true;
    }
    if (strncmp(value, "b64:", 4) == 0) return true;
    return false;
}

char* rs_script_encode_value(const char *value, bool force) {
    if (!value || value[0] == '\0') return rs_strdup(value ? value : "");
    if (!force && !needs_encoding(value)) return rs_strdup(value);
    
    size_t len = strlen(value);
    char *b64 = rs_base64_encode((const uint8_t*)value, len);
    if (!b64) return NULL;
    
    char *res = malloc(4 + strlen(b64) + 1);
    if (res) {
        strcpy(res, "b64:");
        strcat(res, b64);
    }
    rs_free(b64);
    return res;
}

char* rs_script_decode_value(const char *value) {
    if (!value) return NULL;
    if (strncmp(value, "b64:", 4) == 0) {
        const char *encoded = value + 4;
        size_t cap = strlen(encoded);
        uint8_t *out = malloc(cap + 1);
        if (!out) return rs_strdup(value);
        size_t out_len = 0;
        if (rs_base64_decode(encoded, out, cap, &out_len) == 0) {
            char *text = malloc(out_len + 1);
            if (text) {
                memcpy(text, out, out_len);
                text[out_len] = '\0';
                free(out);
                return text;
            }
        }
        free(out);
        return rs_strdup(value);
    }
    
    size_t len = strlen(value);
    if (len >= 4 && len % 4 == 0) {
        bool all_valid = true;
        for (size_t i = 0; i < len; i++) {
            char c = value[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
                all_valid = false;
                break;
            }
        }
        if (all_valid) {
            size_t cap = len;
            uint8_t *out = malloc(cap);
            if (out) {
                size_t out_len = 0;
                if (rs_base64_decode(value, out, cap, &out_len) == 0) {
                    char *text = malloc(out_len + 1);
                    if (text) {
                        memcpy(text, out, out_len);
                        text[out_len] = '\0';
                        
                        bool printable = out_len > 0;
                        for (size_t i = 0; i < out_len; i++) {
                            unsigned char c = text[i];
                            if (c < 0x20 && c != '\n' && c != '\t') {
                                printable = false;
                                break;
                            }
                        }
                        
                        char *reenc = rs_base64_encode(out, out_len);
                        if (reenc && strcmp(reenc, value) == 0 && printable) {
                            free(out);
                            rs_free(reenc);
                            return text;
                        }
                        rs_free(reenc);
                        free(text);
                    }
                }
                free(out);
            }
        }
    }
    
    return rs_strdup(value);
}

rs_strv rs_script_split_params(const char *text) {
    rs_strv out = RS_STRV_INIT;
    if (!text) return out;
    
    size_t len = strlen(text);
    size_t i = 0;
    while (i < len) {
        while (i < len && text[i] == ' ') i++;
        if (i == len) break;
        size_t start = i;
        while (i < len && text[i] != ' ') i++;
        
        size_t tlen = i - start;
        char *token = malloc(tlen + 1);
        if (token) {
            memcpy(token, text + start, tlen);
            token[tlen] = '\0';
            
            char *eq = strchr(token, '=');
            if (eq) {
                *eq = '\0';
                const char *k = token;
                const char *v = eq + 1;
                char *ev = rs_script_encode_value(v, false);
                rs_strv_pushf(&out, "%s=%s", k, ev);
                rs_free(ev);
            } else {
                rs_strv_push(&out, token);
            }
            free(token);
        }
    }
    return out;
}

char* rs_script_arg(const char *key, const char *value, bool force) {
    char *ev = rs_script_encode_value(value, force);
    if (!ev) return NULL;
    char *res = malloc(strlen(key) + 1 + strlen(ev) + 1);
    if (res) {
        strcpy(res, key);
        strcat(res, "=");
        strcat(res, ev);
    }
    rs_free(ev);
    return res;
}

int rs_script_run_stream(const char *script_path, const char **args, int argc, double timeout,
                         char **out_stdout, char **out_stderr,
                         rs_proc_output_fn output_fn, void *output_ctx) {
    if (out_stdout) *out_stdout = rs_strdup("");
    if (out_stderr) *out_stderr = rs_strdup("");
    if (!script_path || !script_path[0]) return -1;

    // The interpreter the extension implies, if any: python for .py, a shell for
    // .sh, cmd.exe or PowerShell for the Windows script kinds, and nothing at
    // all for a file that is directly executable. argv[0] is searched on PATH by
    // the spawn itself, so no /usr/bin/env indirection is needed to find it.
    const char *prefix[8];
    size_t nprefix = rs_proc_interpreter_for(script_path, prefix, 8);

    rs_strv spawn_args = RS_STRV_INIT;
    for (size_t i = 0; i < nprefix; i++) rs_strv_push(&spawn_args, prefix[i]);
    rs_strv_push(&spawn_args, script_path);
    for (int i = 0; i < argc; i++) rs_strv_push(&spawn_args, args[i]);
    if (spawn_args.err) { rs_strv_dispose(&spawn_args); return -1; }
    // rs_strv reserves the slot but does not write it, and an argv that is not
    // NULL-terminated is read past its end by the spawn.
    spawn_args.items[spawn_args.len] = NULL;

    // Both streams are captured, and rs_proc_run drains them while the script
    // runs — a script that writes more than a pipe buffer to stderr while the
    // caller reads stdout would otherwise wedge until the timeout.
    rs_run_result res;
    char run_error[256] = {0};
    int rc = rs_proc_run_stream((const char *const *)spawn_args.items, NULL, timeout,
                                true, true, NULL, &res, run_error, sizeof(run_error),
                                output_fn, output_ctx);
    rs_strv_dispose(&spawn_args);

    // Whatever it managed to say is worth returning even when it was killed: a
    // script that times out has usually already printed the reason.
    if (out_stdout && res.out) { rs_free(*out_stdout); *out_stdout = res.out; res.out = NULL; }
    if (out_stderr && res.err) { rs_free(*out_stderr); *out_stderr = res.err; res.err = NULL; }
    if (out_stderr && (!*out_stderr || !(*out_stderr)[0])) {
        char reason[320] = {0};
        if (run_error[0]) snprintf(reason, sizeof(reason), "%s", run_error);
        else if (res.timed_out) snprintf(reason, sizeof(reason), "script timed out after %.0f seconds", timeout);
        else if (res.term_signal) snprintf(reason, sizeof(reason), "script was terminated by signal %d", res.term_signal);
        if (reason[0]) {
            rs_free(*out_stderr);
            *out_stderr = rs_strdup(reason);
            if (output_fn) output_fn(output_ctx, true, reason, strlen(reason));
        }
    }
    rs_run_result_dispose(&res);

    if (rc != 0) return -1;                            // could not be started
    if (res.timed_out || res.term_signal != 0) return -1;   // killed, not finished
    return res.exit_code;
}

int rs_script_run_sync(const char *script_path, const char **args, int argc, double timeout,
                       char **out_stdout, char **out_stderr) {
    return rs_script_run_stream(script_path, args, argc, timeout, out_stdout, out_stderr,
                                NULL, NULL);
}
