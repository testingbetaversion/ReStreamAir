#ifndef RS_CDM_H
#define RS_CDM_H

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char **kids;           // hex KID strings
    size_t kids_count;
    char *pssh_widevine;   // base64
    char *pssh_playready;  // base64
    char *pssh_fairplay;   // base64
    char **pssh_all;       // all base64 PSSH boxes
    size_t pssh_all_count;
    char **key_uris;       // HLS EXT-X-KEY URIs
    size_t key_uris_count;
} rs_drm_challenge;

rs_drm_challenge rs_cdm_challenge_from_mpd(const char *xml);
rs_drm_challenge rs_cdm_challenge_from_hls(const char *playlist);

// Whether `b64` decodes to a well-formed `pssh` box. Used to check what a
// script's `pssh` hook hands back before licensing against it.
bool rs_cdm_is_pssh_box(const char *b64);

// Whether the manifest scrape found nothing at all worth sending to a script.
bool rs_drm_challenge_is_empty(const rs_drm_challenge *ch);

// Adds one hex KID (dashes allowed). Ignores duplicates and anything that isn't
// 32 hex digits.
void rs_cdm_challenge_add_kid(rs_drm_challenge *ch, const char *hex);

// Adds one already-extracted base64 PSSH box to the challenge, classifying it
// by system id and harvesting any KIDs a v1 box lists. Ignores duplicates and
// anything that isn't a well-formed `pssh` box.
void rs_cdm_challenge_add_pssh(rs_drm_challenge *ch, const char *b64);

// Adds every unique KID, PSSH and HLS key URI from `source` to `destination`.
// This is used when DRM is split between an HLS master and its media playlist.
void rs_cdm_challenge_merge(rs_drm_challenge *destination,
                            const rs_drm_challenge *source);

// Scans an ISO-BMFF init segment for the DRM the manifest didn't carry: every
// `pssh` box, and the `default_KID` of every `tenc`. Some sources advertise a
// bare `default_KID` (or nothing) in the manifest and only ship the real boxes
// in the initialization segment, which is the third place PSSH can live.
void rs_cdm_challenge_add_from_init(rs_drm_challenge *ch, const uint8_t *data, size_t len);

// Builds a Widevine PSSH box (base64) carrying `count` hex KIDs, for sources
// that advertise KIDs but no `cenc:pssh` — the C equivalent of pywidevine's
// PSSH.new(key_ids=...). Returns NULL when no KID is usable.
char *rs_cdm_pssh_from_kids(char *const *kids, size_t count);

// Fills in what the source left out: when the challenge has KIDs but no
// Widevine box, synthesize one from them. Returns true if it added one.
bool rs_cdm_challenge_synthesize_pssh(rs_drm_challenge *ch);
// Turns a `cdm` action's stdout into newline-separated "kid:key" lines: the
// documented JSON shape when it is one, every bare <32 hex>:<32 hex> pair
// otherwise. Returns a fresh string (possibly empty), never NULL.
char *rs_cdm_parse_key_output(const char *text);

char* rs_cdm_resolve_keys(const char *script_path, const char *cdm_type, const rs_drm_challenge *ch, const char **extra_args, int extra_argc, double timeout);
void rs_drm_challenge_free(rs_drm_challenge *ch);

#ifdef __cplusplus
}
#endif

#endif // RS_CDM_H
