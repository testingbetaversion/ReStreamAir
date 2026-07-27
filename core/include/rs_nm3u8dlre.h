#ifndef RS_NM3U8DLRE_H
#define RS_NM3U8DLRE_H

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Locate N_m3u8DL-RE on PATH (returns malloc'd path or NULL)
char* rs_nm3u8dlre_resolve(void);

// Generate installation plan (returns malloc'd shell command string or NULL)
char* rs_nm3u8dlre_install_plan(void);

#ifdef __cplusplus
}
#endif

#endif  // RS_NM3U8DLRE_H
