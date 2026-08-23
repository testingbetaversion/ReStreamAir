#include "../include/rs_audio_delay.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    size_t payload_start;
    size_t end;
} rs_box_info;

static inline uint32_t read_u32(const uint8_t *d) {
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
}

static inline uint64_t read_u64(const uint8_t *d) {
    return ((uint64_t)read_u32(d) << 32) | read_u32(d + 4);
}

static inline void write_u32(uint8_t *d, uint32_t val) {
    // The casts are the point of the masks made explicit: GCC's -Wconversion
    // cannot see that the & 0xFF already made each byte fit.
    d[0] = (uint8_t)((val >> 24) & 0xFF);
    d[1] = (uint8_t)((val >> 16) & 0xFF);
    d[2] = (uint8_t)((val >> 8) & 0xFF);
    d[3] = (uint8_t)(val & 0xFF);
}

static inline void write_u64(uint8_t *d, uint64_t val) {
    write_u32(d, (uint32_t)(val >> 32));
    write_u32(d + 4, (uint32_t)(val & 0xFFFFFFFF));
}

static bool find_box(const uint8_t *data, size_t start, size_t end, const char *type, rs_box_info *out) {
    size_t pos = start;
    while (pos + 8 <= end) {
        uint32_t size = read_u32(data + pos);
        if (size < 8 && size != 1) return false;
        
        size_t box_end;
        size_t payload_start;
        
        if (size == 1) {
            if (pos + 16 > end) return false;
            uint64_t extsize = read_u64(data + pos + 8);
            if (extsize < 16) return false;
            box_end = pos + extsize;
            payload_start = pos + 16;
        } else {
            box_end = pos + size;
            payload_start = pos + 8;
        }
        
        if (box_end > end || box_end < pos) return false;
        
        if (memcmp(data + pos + 4, type, 4) == 0) {
            out->payload_start = payload_start;
            out->end = box_end;
            return true;
        }
        
        pos = box_end;
    }
    return false;
}

uint32_t rs_audio_mdhd_timescale(const uint8_t *data, size_t len) {
    rs_box_info moov, trak, mdia, mdhd;
    if (!find_box(data, 0, len, "moov", &moov)) return 0;
    if (!find_box(data, moov.payload_start, moov.end, "trak", &trak)) return 0;
    if (!find_box(data, trak.payload_start, trak.end, "mdia", &mdia)) return 0;
    if (!find_box(data, mdia.payload_start, mdia.end, "mdhd", &mdhd)) return 0;
    
    size_t ps = mdhd.payload_start;
    if (ps >= len) return 0;
    uint8_t version = data[ps];
    size_t offset = ps + (version == 1 ? 1 + 3 + 8 + 8 : 1 + 3 + 4 + 4);
    if (offset + 4 <= len) {
        return read_u32(data + offset);
    }
    return 0;
}

uint64_t rs_audio_base_decode_time(const uint8_t *data, size_t len) {
    rs_box_info moof, traf, tfdt;
    if (!find_box(data, 0, len, "moof", &moof)) return 0;
    if (!find_box(data, moof.payload_start, moof.end, "traf", &traf)) return 0;
    if (!find_box(data, traf.payload_start, traf.end, "tfdt", &tfdt)) return 0;
    
    size_t ps = tfdt.payload_start;
    if (ps >= len) return 0;
    uint8_t version = data[ps];
    size_t offset = ps + 4;
    
    if (version == 1) {
        if (offset + 8 <= len) return read_u64(data + offset);
    } else {
        if (offset + 4 <= len) return (uint64_t)read_u32(data + offset);
    }
    return 0;
}

static uint64_t clamped_add(uint64_t base, int64_t delta) {
    if (delta >= 0) return base + (uint64_t)delta;
    uint64_t magnitude = (uint64_t)(-delta);
    return base > magnitude ? base - magnitude : 0;
}

uint8_t* rs_audio_shift_segment(const uint8_t *data, size_t len, int64_t delta_units, size_t *out_len) {
    if (!data || !out_len) return NULL;
    
    uint8_t *out = malloc(len);
    if (!out) return NULL;
    memcpy(out, data, len);
    *out_len = len;
    
    if (delta_units == 0) return out;
    
    rs_box_info moof, traf, tfdt;
    if (!find_box(out, 0, len, "moof", &moof)) return out;
    if (!find_box(out, moof.payload_start, moof.end, "traf", &traf)) return out;
    if (!find_box(out, traf.payload_start, traf.end, "tfdt", &tfdt)) return out;
    
    size_t ps = tfdt.payload_start;
    if (ps >= len) return out;
    uint8_t version = out[ps];
    size_t offset = ps + 4;
    
    if (version == 1) {
        if (offset + 8 <= len) {
            uint64_t current = read_u64(out + offset);
            uint64_t shifted = clamped_add(current, delta_units);
            write_u64(out + offset, shifted);
        }
    } else {
        if (offset + 4 <= len) {
            uint64_t current = (uint64_t)read_u32(out + offset);
            uint64_t shifted = clamped_add(current, delta_units);
            write_u32(out + offset, (uint32_t)shifted);
        }
    }
    
    return out;
}
