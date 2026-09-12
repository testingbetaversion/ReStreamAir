#ifndef RS_PROVIDER_OPTIONS_H
#define RS_PROVIDER_OPTIONS_H

#include "rs_json.h"
#include "rs_source_policy.h"

bool rs_provider_timezone_offset(const char *value, int *minutes);

// Shared API metadata, validation and defaults for provider.options.
rs_json *rs_provider_options_schema(void);
bool rs_provider_options_patch_valid(const rs_json *saved, const rs_json *patch, const char **err);
bool rs_provider_options_valid(const rs_json *options, const char **err);
rs_json *rs_provider_options_merge(const rs_json *saved, const rs_json *patch);
long long rs_provider_option_int(const rs_json *provider, const char *name);
bool rs_provider_option_bool(const rs_json *provider, const char *name);
const char *rs_provider_option_str(const rs_json *provider, const char *name);
void rs_provider_source_policy(const rs_json *provider, rs_source_policy *out);
// Includes the dedicated User-Agent/X-Forwarded-For fields unless the generic
// headers already contain that header. Caller frees.
char *rs_provider_headers(const rs_json *provider);
// Provider output overrides; zero leaves the stream's existing value in use.
int rs_provider_segment_seconds(const rs_json *provider, const rs_json *stream);
int rs_provider_playlist_segments(const rs_json *provider, const rs_json *stream);

#endif
