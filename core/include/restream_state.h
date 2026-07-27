#ifndef RESTREAM_STATE_H
#define RESTREAM_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
typedef struct cJSON cJSON;

// ---------------------------------------------------------------------------
// RepresentationMetaEntry
// ---------------------------------------------------------------------------
typedef struct {
    char *type;        // "video" | "audio"
    int   bandwidth;   // -1 = absent
    int   width;       // -1 = absent
    int   height;      // -1 = absent
    char *codecs;      // NULL = absent
    char *language;    // NULL = absent
} rs_rep_meta_t;

// ---------------------------------------------------------------------------
// RepresentationMeta map  (key -> rs_rep_meta_t)
// ---------------------------------------------------------------------------
typedef struct {
    char          **keys;
    rs_rep_meta_t  *values;
    size_t          count;
    size_t          capacity;
} rs_rep_meta_map_t;

// ---------------------------------------------------------------------------
// ScriptAccount
// ---------------------------------------------------------------------------
typedef struct {
    char *id;
    char *name;
    char *username;    // default ""
    char *password;    // default ""
    char *params;      // default ""
    bool  enabled;     // default true
} rs_script_account_t;

// ---------------------------------------------------------------------------
// StreamConfig
// ---------------------------------------------------------------------------
typedef struct {
    // Core
    char   *id;
    char   *name;
    char   *kind;              // "m3u8" | "mpd"
    char   *url;
    char   *representation;
    char   *period;
    char   *proxy;
    int     playlist_segments; // default 6
    int     keep_segments;     // default 10
    int     download_ahead;    // default 8
    double  poll_interval;     // default 2.0
    bool    force_offline;     // default false
    char   *status;            // "stopped" | "running" | "error"
    char   *last_error;        // NULL = absent
    char   *logo;              // default ""

    // Decryption
    char   *decryption_keys;   // default ""
    char   *hls_key;           // default ""
    char   *hls_iv;            // default ""

    // Representations
    char  **representations;
    size_t  representations_count;
    rs_rep_meta_map_t representation_meta;

    // HTTP headers
    char   *manifest_headers;  // default ""
    char   *media_headers;     // default ""
    char   *hls_key_headers;   // default ""

    // Audio
    int     audio_delay_ms;    // default 0

    // Scripting
    char   *source_type;       // default ""
    char   *mode;              // default "live"
    bool    session_manifest;  // default false
    char   *script_params;     // default ""
    char   *cdm_type;          // default ""
    bool    use_cdm;           // default false
    char   *script_override;   // default ""
    // scriptActionsOverride: stored as JSON array string or NULL
    char   *script_actions_override; // NULL = absent
    char   *cdm_mode;          // default "external"
    bool    proxy_script;      // default true
    bool    proxy_manifest;    // default true
    bool    proxy_media;       // default true
    bool    heartbeat_enabled; // default true
    int     heartbeat_seconds; // default 0
    char   *script_video_selector; // default ""
    char   *script_audio_selector; // default ""
    bool    on_demand;         // default false
    bool    speed_up;          // default false
    bool    autostart;         // default false
    int64_t script_start;      // -1 = absent (Unix timestamp)
    int64_t script_end;        // -1 = absent
    bool    record_event;      // default false

    // Pipeline
    char   *input_mode;        // default "internal"
    char   *output_mode;       // default "hls"
    char   *output_target;     // default ""

    // CDN
    char  **cdn_urls;
    size_t  cdn_urls_count;
    char   *nm3u8dlre_params;  // default ""
    bool    direct_source;     // default false
} rs_stream_config_t;

// ---------------------------------------------------------------------------
// Provider
// ---------------------------------------------------------------------------
typedef struct {
    char   *id;
    char   *name;
    char   *logo;
    char   *proxy;                // default ""
    char   *headers;              // default ""
    char   *segment_url_params;   // default ""
    bool    inherit_url_params;   // default false
    char   *script_path;          // default ""
    char   *script_bind;          // default ""
    char   *script_doh;           // default ""
    char   *script_worker;        // default ""

    rs_script_account_t *script_accounts;
    size_t               script_accounts_count;

    char   *active_script_account_id; // default ""
    char   *account_selection_mode;   // default "fixed"
    char   *last_rotated_account_id;  // default ""

    char  **script_actions;       // default ["login","pair","channels","events"]
    size_t  script_actions_count;

    rs_stream_config_t *streams;
    size_t              streams_count;
} rs_provider_t;

// ---------------------------------------------------------------------------
// APIKey
// ---------------------------------------------------------------------------
typedef struct {
    char   *id;
    char   *key;
    char   *label;
    double  created_at;   // Swift Date (seconds since 2001-01-01)
    int64_t requests;     // default 0
    int64_t bytes;        // default 0
    double  last_seen_at; // -1 = absent
} rs_api_key_t;

// ---------------------------------------------------------------------------
// BandwidthTotals
// ---------------------------------------------------------------------------
typedef struct {
    char   **keys;
    int64_t *values;
    size_t   count;
    size_t   capacity;
} rs_int64_map_t;

typedef struct {
    int64_t        all_time_bytes;              // default 0
    rs_int64_map_t per_stream_all_time_bytes;
    int64_t        input_all_time_bytes;         // default 0
    rs_int64_map_t per_stream_input_all_time_bytes;
} rs_bandwidth_totals_t;

// ---------------------------------------------------------------------------
// SystemPeaks
// ---------------------------------------------------------------------------
typedef struct {
    double   max_cpu_percent;   // default 0
    uint64_t max_memory_bytes;  // default 0
} rs_system_peaks_t;

// ---------------------------------------------------------------------------
// AdminUser
// ---------------------------------------------------------------------------
typedef struct {
    char  *id;
    char  *username;
    char  *password_hash;  // Base64 PBKDF2 digest
    char  *salt;           // Base64 16-byte salt
    double created_at;     // Swift Date
} rs_admin_user_t;

// ---------------------------------------------------------------------------
// PanelSettings
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t port;          // default 8787
    char    *bind_address;  // default ""
} rs_panel_settings_t;

// ---------------------------------------------------------------------------
// PanelState — top-level persisted state
// ---------------------------------------------------------------------------
typedef struct {
    rs_provider_t        *providers;
    size_t                providers_count;

    rs_api_key_t         *api_keys;
    size_t                api_keys_count;

    rs_bandwidth_totals_t bandwidth_totals;
    rs_system_peaks_t     system_peaks;
    rs_panel_settings_t   settings;

    rs_admin_user_t      *admin_users;
    size_t                admin_users_count;

    // We keep the raw cJSON tree so we can round-trip unknown keys
    cJSON                *_raw;
} rs_panel_state_t;

// ---------------------------------------------------------------------------
// LogEntry
// ---------------------------------------------------------------------------
typedef struct {
    double  timestamp;   // Unix timestamp
    char   *stream_id;   // NULL = absent
    char   *level;       // "info" | "error"
    char   *event;
    char   *url;         // NULL = absent
    char   *message;     // NULL = absent
    char   *raw;         // NULL = absent
} rs_log_entry_t;

// ---------------------------------------------------------------------------
// API: State load/save
// ---------------------------------------------------------------------------

// Load state from a JSON file. Returns a heap-allocated state.
// Caller must free with rs_state_free().
rs_panel_state_t* rs_state_load(const char *path);

// Save state to a JSON file (pretty-printed, sorted keys).
bool rs_state_save(const rs_panel_state_t *state, const char *path);

// Create a default empty state.
rs_panel_state_t* rs_state_default(void);

// Free all memory owned by a state object.
void rs_state_free(rs_panel_state_t *state);

// ---------------------------------------------------------------------------
// Utility: safe string duplication
// ---------------------------------------------------------------------------
char* rs_strdup(const char *s);
char* rs_strdup_or_empty(const char *s);

#ifdef __cplusplus
}
#endif

#endif // RESTREAM_STATE_H
