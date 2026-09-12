#ifndef RS_EPG_H
#define RS_EPG_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// Re-express XMLTV programme start/stop instants in a fixed UTC offset.
char *rs_epg_timezone(const char *xml, size_t len, int offset_minutes, char *err, size_t errlen);
#ifdef __cplusplus
}
#endif
#endif
