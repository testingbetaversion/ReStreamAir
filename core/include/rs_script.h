#ifndef RS_SCRIPT_H
#define RS_SCRIPT_H

#include "rs_common.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declare rs_strv from rs_internal.h, or since we cannot easily forward declare it,
// we will just define it if we include rs_internal.h in the source.
// But we need the type here.
typedef struct rs_strv_s rs_strv_s;
// Let's just include the definition from rs_internal.h instead.
#include "../src/rs_internal.h"

char* rs_script_encode_value(const char *value, bool force);
char* rs_script_decode_value(const char *value);
rs_strv rs_script_split_params(const char *text);
char* rs_script_arg(const char *key, const char *value, bool force);

// Run script synchronously using posix_spawn (or CreateProcess on Windows).
// For .py files use python3, for .sh use /bin/sh.
// Return exit code (0 on success, non-zero on failure). Fill stdout/stderr strings (caller frees with rs_free).
int rs_script_run_sync(const char *script_path, const char **args, int argc, double timeout, char **out_stdout, char **out_stderr);

#ifdef __cplusplus
}
#endif

#endif // RS_SCRIPT_H
