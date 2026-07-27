#ifndef RS_AUDIO_DELAY_H
#define RS_AUDIO_DELAY_H

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t rs_audio_mdhd_timescale(const uint8_t *data, size_t len);
uint64_t rs_audio_base_decode_time(const uint8_t *data, size_t len);
uint8_t* rs_audio_shift_segment(const uint8_t *data, size_t len, int64_t delta_units, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif  // RS_AUDIO_DELAY_H
