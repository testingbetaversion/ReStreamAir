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
char* rs_cdm_resolve_keys(const char *script_path, const char *cdm_type, const rs_drm_challenge *ch, const char **extra_args, int extra_argc, double timeout);
void rs_drm_challenge_free(rs_drm_challenge *ch);

#ifdef __cplusplus
}
#endif

#endif // RS_CDM_H
