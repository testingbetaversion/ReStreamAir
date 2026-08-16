#include "rs_mpegts.h"

#include <stdlib.h>
#include <string.h>

// --- little helpers ---------------------------------------------------------

static uint16_t rd_u16(const uint8_t *b) {
    return (uint16_t)(((uint32_t)b[0] << 8) | b[1]);
}
static uint32_t rd_u24(const uint8_t *b) {
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
}
static uint32_t rd_u32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static uint64_t rd_u64(const uint8_t *b) {
    return ((uint64_t)rd_u32(b) << 32) | rd_u32(b + 4);
}

// A growable byte buffer. Everything that assembles bytes here uses one.
typedef struct {
    uint8_t *p;
    size_t len, cap;
} bbuf;

static bool bb_reserve(bbuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return true;
    size_t want = b->cap ? b->cap : 4096;
    while (want < b->len + extra) want *= 2;
    uint8_t *np = (uint8_t *)realloc(b->p, want);
    if (!np) return false;
    b->p = np;
    b->cap = want;
    return true;
}

static void bb_add(bbuf *b, const uint8_t *data, size_t len) {
    if (!len || !bb_reserve(b, len)) return;
    memcpy(b->p + b->len, data, len);
    b->len += len;
}

static void bb_free(bbuf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

// --- MP4 box walking --------------------------------------------------------
//
// Deliberately not shared with cenc.c's parser: that one materialises the whole
// tree up front because it has to rewrite boxes in place, while everything here
// is a single downward search for one known path. A flat iterator over one
// level, called recursively, is both smaller and allocation-free.

typedef struct {
    const uint8_t *data;   // payload start
    size_t len;            // payload length
} mp4span;

// Finds the first child box of `type` directly inside [base, base+len).
// Returns false when absent or when a declared size runs past the end.
static bool box_find(const uint8_t *base, size_t len, const char *type, mp4span *out) {
    size_t off = 0;
    while (off + 8 <= len) {
        uint64_t size = rd_u32(base + off);
        size_t hdr = 8;
        if (size == 1) {
            if (off + 16 > len) return false;
            size = rd_u64(base + off + 8);
            hdr = 16;
        } else if (size == 0) {
            size = len - off;  // "to end of container"
        }
        if (size < hdr || off + size > len) return false;
        if (memcmp(base + off + 4, type, 4) == 0) {
            out->data = base + off + hdr;
            out->len = (size_t)size - hdr;
            return true;
        }
        off += (size_t)size;
    }
    return false;
}

static bool box_child(const mp4span *in, const char *type, mp4span *out) {
    return box_find(in->data, in->len, type, out);
}

// Iterates every box of `type` at one level. `*cursor` starts at 0 and is
// advanced past each hit, so repeated calls walk them all.
//
// Boxes that can legitimately repeat are the reason this exists rather than
// box_find alone: a CMAF low-latency segment is a *sequence* of moof+mdat
// chunks, not one of each, and a traf may carry several truns. Parsing only
// the first of each silently drops the rest of the segment — which presents as
// a periodic hole in the output, not as an error.
static bool box_next(const uint8_t *base, size_t len, const char *type,
                     size_t *cursor, mp4span *out, size_t *out_box_start) {
    size_t off = *cursor;
    while (off + 8 <= len) {
        uint64_t size = rd_u32(base + off);
        size_t hdr = 8;
        if (size == 1) {
            if (off + 16 > len) return false;
            size = rd_u64(base + off + 8);
            hdr = 16;
        } else if (size == 0) {
            size = len - off;
        }
        if (size < hdr || off + size > len) return false;
        size_t this_start = off;
        off += (size_t)size;
        if (memcmp(base + this_start + 4, type, 4) == 0) {
            out->data = base + this_start + hdr;
            out->len = (size_t)size - hdr;
            if (out_box_start) *out_box_start = this_start;
            *cursor = off;
            return true;
        }
    }
    *cursor = off;
    return false;
}

// --- codec configuration ----------------------------------------------------

enum { TS_CODEC_NONE = 0, TS_CODEC_H264, TS_CODEC_H265, TS_CODEC_AAC };

// Annex B start code, written before every NAL unit.
static const uint8_t k_start_code[4] = { 0x00, 0x00, 0x00, 0x01 };

typedef struct {
    uint8_t *payload;   // PES payload: an Annex B access unit, or ADTS frames
    size_t len;
    int64_t dts;        // 90 kHz, rebased onto the mux origin
    int64_t pts;
    bool key;
} ts_sample;

typedef struct {
    bool used;
    int codec;
    bool is_video;
    uint32_t track_id;
    uint32_t timescale;
    uint16_t pid;
    uint8_t stream_id;    // PES stream_id (0xE0 video, 0xC0 audio)
    uint8_t cc;           // continuity counter, 4 bits

    int nal_length_size;  // AVCC/HVCC length prefix width
    uint8_t *param_sets;  // SPS/PPS (+VPS) as Annex B, prepended to each keyframe
    size_t param_len;

    uint8_t aac_object_type, aac_freq_index, aac_channels;

    bool ended;
    bool have_high_water;
    int64_t high_water;   // largest DTS pushed so far
    bool have_last_dts;
    int64_t last_dts;     // last DTS actually emitted, for discontinuity detection

    ts_sample *queue;
    size_t nq, capq, head;
} ts_track;

struct rs_ts_mux {
    ts_track tracks[RS_TS_MAX_TRACKS];
    size_t ntracks;
    bbuf out;

    bool have_origin;
    int64_t origin;         // first DTS seen, in 90 kHz, before rebasing

    uint16_t pcr_pid;
    uint8_t pat_cc, pmt_cc;
    bool wrote_tables;
    int64_t last_table_dts;  // tables are re-sent every RS_TS_TABLE_PERIOD
    bool started;
    bool aligned;            // leading track-start skew has been trimmed

    // Offsets into the buffer being built where a viewer may join; reset at the
    // start of every take, so they always describe what that take returns.
    size_t *joins, njoins, capjoins;
};

// Where the rebased timeline starts. A live source's audio fragment can begin
// slightly before the video one; starting at ten seconds rather than zero keeps
// the earlier of the two positive, and leaves room for the PCR to lead the
// first frame it accompanies.
#define RS_TS_START_PTS   900000       // 10 s at 90 kHz
#define RS_TS_PCR_LEAD    18000        // 0.2 s at 90 kHz
#define RS_TS_TABLE_PERIOD 9000        // re-send PAT/PMT every 0.1 s of media
// A DTS step larger than this is a timeline break, not the next frame. Normal
// steps are a frame (40 ms) or an audio frame (21 ms), so a whole second is
// comfortably clear of anything a continuous stream produces.
#define RS_TS_DISC_THRESHOLD 90000     // 1 s at 90 kHz
#define RS_TS_PMT_PID     0x1000
#define RS_TS_BASE_PID    0x0100

// --- CRC -------------------------------------------------------------------

uint32_t rs_ts_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
    }
    return crc;
}

// --- init segment parsing ---------------------------------------------------

// Adds one Annex B NAL (start code + body) to the track's parameter-set blob.
static void params_add(bbuf *b, const uint8_t *nal, size_t len) {
    if (!len) return;
    bb_add(b, k_start_code, 4);
    bb_add(b, nal, len);
}

static bool parse_avcc(ts_track *t, const uint8_t *d, size_t len) {
    if (len < 7) return false;
    t->nal_length_size = (d[4] & 3) + 1;
    bbuf ps = {0};
    size_t off = 5;
    unsigned num_sps = d[off++] & 0x1Fu;
    for (unsigned i = 0; i < num_sps; i++) {
        if (off + 2 > len) { bb_free(&ps); return false; }
        size_t n = rd_u16(d + off);
        off += 2;
        if (off + n > len) { bb_free(&ps); return false; }
        params_add(&ps, d + off, n);
        off += n;
    }
    if (off >= len) { bb_free(&ps); return false; }
    unsigned num_pps = d[off++];
    for (unsigned i = 0; i < num_pps; i++) {
        if (off + 2 > len) { bb_free(&ps); return false; }
        size_t n = rd_u16(d + off);
        off += 2;
        if (off + n > len) { bb_free(&ps); return false; }
        params_add(&ps, d + off, n);
        off += n;
    }
    t->param_sets = ps.p;
    t->param_len = ps.len;
    t->codec = TS_CODEC_H264;
    return true;
}

static bool parse_hvcc(ts_track *t, const uint8_t *d, size_t len) {
    if (len < 23) return false;
    t->nal_length_size = (d[21] & 3) + 1;
    unsigned num_arrays = d[22];
    size_t off = 23;
    bbuf ps = {0};
    for (unsigned a = 0; a < num_arrays; a++) {
        if (off + 3 > len) { bb_free(&ps); return false; }
        off += 1;  // array_completeness + NAL_unit_type
        unsigned count = rd_u16(d + off);
        off += 2;
        for (unsigned i = 0; i < count; i++) {
            if (off + 2 > len) { bb_free(&ps); return false; }
            size_t n = rd_u16(d + off);
            off += 2;
            if (off + n > len) { bb_free(&ps); return false; }
            params_add(&ps, d + off, n);
            off += n;
        }
    }
    t->param_sets = ps.p;
    t->param_len = ps.len;
    t->codec = TS_CODEC_H265;
    return true;
}

// Pulls the AudioSpecificConfig out of an esds box. The descriptor lengths are
// 7-bit varints, so each one has to be walked rather than indexed past.
static size_t desc_len(const uint8_t *d, size_t len, size_t *off) {
    size_t v = 0;
    for (int i = 0; i < 4 && *off < len; i++) {
        uint8_t b = d[(*off)++];
        v = (v << 7) | (b & 0x7Fu);
        if (!(b & 0x80u)) break;
    }
    return v;
}

static bool parse_esds(ts_track *t, const uint8_t *d, size_t len) {
    if (len < 5) return false;
    size_t off = 4;  // version + flags
    if (off >= len || d[off++] != 0x03) return false;
    desc_len(d, len, &off);
    if (off + 3 > len) return false;
    off += 2;  // ES_ID
    uint8_t flags = d[off++];
    if (flags & 0x80u) off += 2;                       // streamDependenceFlag
    if (flags & 0x40u) {                               // URL_Flag
        if (off >= len) return false;
        off += 1 + d[off];
    }
    if (flags & 0x20u) off += 2;                       // OCRstreamFlag
    if (off >= len || d[off++] != 0x04) return false;  // DecoderConfigDescriptor
    desc_len(d, len, &off);
    if (off + 13 > len) return false;
    off += 13;                                          // objectType..avgBitrate
    if (off >= len || d[off++] != 0x05) return false;   // DecSpecificInfo
    size_t asc_len = desc_len(d, len, &off);
    if (asc_len < 2 || off + asc_len > len) return false;

    // AudioSpecificConfig: 5 bits object type, 4 bits sampling index, 4 channels.
    const uint8_t *asc = d + off;
    unsigned object_type = (unsigned)(asc[0] >> 3);
    unsigned freq_index, channels;
    if (object_type == 31) return false;  // escape form: not AAC-LC/HE, refuse
    freq_index = (unsigned)(((asc[0] & 0x07u) << 1) | (asc[1] >> 7));
    if (freq_index == 15) return false;   // explicit rate: no ADTS index for it
    channels = (unsigned)((asc[1] >> 3) & 0x0Fu);
    if (object_type == 0 || channels == 0 || channels > 7) return false;
    t->aac_object_type = (uint8_t)object_type;
    t->aac_freq_index = (uint8_t)freq_index;
    t->aac_channels = (uint8_t)channels;
    t->codec = TS_CODEC_AAC;
    return true;
}

// Locates the codec configuration box inside a sample entry, stepping over the
// fixed VisualSampleEntry / AudioSampleEntry preamble that precedes it.
static bool parse_sample_entry(ts_track *t, const uint8_t *entry, size_t len) {
    if (len < 16) return false;
    char fourcc[5] = {0};
    memcpy(fourcc, entry + 4, 4);

    bool video = memcmp(fourcc, "avc1", 4) == 0 || memcmp(fourcc, "avc3", 4) == 0 ||
                 memcmp(fourcc, "hvc1", 4) == 0 || memcmp(fourcc, "hev1", 4) == 0 ||
                 memcmp(fourcc, "encv", 4) == 0 || memcmp(fourcc, "dvh1", 4) == 0 ||
                 memcmp(fourcc, "dvhe", 4) == 0;
    bool audio = memcmp(fourcc, "mp4a", 4) == 0 || memcmp(fourcc, "enca", 4) == 0;
    if (!video && !audio) return false;

    size_t body;
    if (video) {
        body = 86;  // 16 sample-entry header + 70 VisualSampleEntry fields
    } else {
        if (len < 24) return false;
        unsigned version = rd_u16(entry + 16);  // AudioSampleEntry version
        body = 36;                              // 16 + 20 (version 0)
        if (version == 1) body += 16;
        else if (version == 2) body += 36;
    }
    if (body >= len) return false;

    const uint8_t *inner = entry + body;
    size_t inner_len = len - body;
    mp4span cfg;
    if (video) {
        if (box_find(inner, inner_len, "avcC", &cfg)) return parse_avcc(t, cfg.data, cfg.len);
        if (box_find(inner, inner_len, "hvcC", &cfg)) return parse_hvcc(t, cfg.data, cfg.len);
        // Still-encrypted entry: the configuration hides under sinf/frma-restored
        // siblings, which the caller was supposed to have patched already.
        return false;
    }
    if (box_find(inner, inner_len, "esds", &cfg)) return parse_esds(t, cfg.data, cfg.len);
    return false;
}

// Reads track_id, timescale and codec config out of an init segment.
static bool parse_init(ts_track *t, const uint8_t *init, size_t len) {
    mp4span moov, trak, tkhd, mdia, mdhd, minf, stbl, stsd;
    if (!box_find(init, len, "moov", &moov)) return false;
    if (!box_child(&moov, "trak", &trak)) return false;
    if (!box_child(&trak, "tkhd", &tkhd) || tkhd.len < 20) return false;

    uint8_t tkhd_version = tkhd.data[0];
    t->track_id = tkhd_version == 1 ? rd_u32(tkhd.data + 20) : rd_u32(tkhd.data + 12);

    if (!box_child(&trak, "mdia", &mdia)) return false;
    if (!box_child(&mdia, "mdhd", &mdhd) || mdhd.len < 20) return false;
    uint8_t mdhd_version = mdhd.data[0];
    t->timescale = mdhd_version == 1 ? rd_u32(mdhd.data + 20) : rd_u32(mdhd.data + 12);
    if (t->timescale == 0) return false;

    if (!box_child(&mdia, "minf", &minf)) return false;
    if (!box_child(&minf, "stbl", &stbl)) return false;
    if (!box_child(&stbl, "stsd", &stsd) || stsd.len < 8) return false;

    // stsd: version/flags(4) + entry_count(4), then the sample entries.
    uint32_t entries = rd_u32(stsd.data + 4);
    size_t off = 8;
    for (uint32_t i = 0; i < entries && off + 8 <= stsd.len; i++) {
        uint32_t size = rd_u32(stsd.data + off);
        if (size < 8 || off + size > stsd.len) break;
        if (parse_sample_entry(t, stsd.data + off, size)) return true;
        off += size;
    }
    return false;
}

// --- fragment parsing -------------------------------------------------------

typedef struct {
    uint32_t duration, size, flags;
    int32_t cts_offset;
} frag_sample;

// Converts one AVCC/HVCC sample into an Annex B access unit, prepending the
// access unit delimiter and — on a keyframe — the parameter sets, so a player
// that joined mid-stream can start decoding at the next keyframe without ever
// having seen an init segment.
static uint8_t *to_annexb(const ts_track *t, const uint8_t *sample, size_t len,
                          bool key, size_t *out_len) {
    bbuf o = {0};
    if (t->codec == TS_CODEC_H264) {
        static const uint8_t aud[2] = { 0x09, 0xF0 };
        bb_add(&o, k_start_code, 4);
        bb_add(&o, aud, sizeof aud);
    } else {
        static const uint8_t aud[3] = { 0x46, 0x01, 0x50 };  // HEVC AUD, nal type 35
        bb_add(&o, k_start_code, 4);
        bb_add(&o, aud, sizeof aud);
    }
    if (key && t->param_len) bb_add(&o, t->param_sets, t->param_len);

    size_t off = 0;
    int n = t->nal_length_size;
    while (off + (size_t)n <= len) {
        size_t nal_len = 0;
        for (int i = 0; i < n; i++) nal_len = (nal_len << 8) | sample[off + (size_t)i];
        off += (size_t)n;
        if (nal_len == 0 || off + nal_len > len) break;
        bb_add(&o, k_start_code, 4);
        bb_add(&o, sample + off, nal_len);
        off += nal_len;
    }
    *out_len = o.len;
    return o.p;
}

// Wraps a raw AAC access unit in an ADTS header. Samples that already carry one
// (some sources mux ADTS straight into mp4a) are passed through untouched.
static uint8_t *to_adts(const ts_track *t, const uint8_t *sample, size_t len, size_t *out_len) {
    if (len >= 2 && sample[0] == 0xFF && (sample[1] & 0xF0u) == 0xF0u) {
        uint8_t *copy = (uint8_t *)malloc(len);
        if (!copy) return NULL;
        memcpy(copy, sample, len);
        *out_len = len;
        return copy;
    }
    size_t total = len + 7;
    if (total > 0x1FFF) return NULL;  // ADTS frame_length is 13 bits
    uint8_t *o = (uint8_t *)malloc(total);
    if (!o) return NULL;
    uint8_t profile = (uint8_t)(t->aac_object_type - 1);  // ADTS profile is objectType-1
    o[0] = 0xFF;
    o[1] = 0xF1;  // MPEG-4, layer 00, no CRC
    o[2] = (uint8_t)((profile << 6) | (t->aac_freq_index << 2) | (t->aac_channels >> 2));
    o[3] = (uint8_t)(((t->aac_channels & 3u) << 6) | (uint8_t)((total >> 11) & 0x03u));
    o[4] = (uint8_t)((total >> 3) & 0xFFu);
    o[5] = (uint8_t)(((total & 0x07u) << 5) | 0x1Fu);
    o[6] = 0xFC;  // buffer fullness 0x7FF, 1 raw data block
    memcpy(o + 7, sample, len);
    *out_len = total;
    return o;
}

static bool queue_push(ts_track *t, ts_sample s) {
    if (t->nq == t->capq) {
        size_t want = t->capq ? t->capq * 2 : 64;
        ts_sample *nq = (ts_sample *)realloc(t->queue, want * sizeof(*nq));
        if (!nq) return false;
        t->queue = nq;
        t->capq = want;
    }
    t->queue[t->nq++] = s;
    return true;
}

// 90 kHz is the MPEG-TS clock; the media timeline is in the track's own
// timescale. Done in 64-bit so a live tfdt (which can be an epoch in timescale
// units) doesn't overflow before the rebase subtracts the origin.
static int64_t to_90k(uint64_t v, uint32_t timescale) {
    return (int64_t)((v * 90000ull) / timescale);
}

// Parses one traf: its tfhd defaults, its tfdt decode time, and every trun it
// carries. Returns the number of samples queued.
static int push_traf(rs_ts_mux *m, ts_track *t, const uint8_t *frag, size_t frag_len,
                     const mp4span *traf, size_t moof_start) {
    mp4span tfhd, trun;
    if (!box_child(traf, "tfhd", &tfhd) || tfhd.len < 8) return 0;

    uint32_t tf_flags = rd_u24(tfhd.data + 1);
    uint32_t track_id = rd_u32(tfhd.data + 4);
    if (t->track_id && track_id != t->track_id) return 0;  // not this rendition

    size_t off = 8;
    uint64_t base_offset = moof_start;
    uint32_t def_duration = 0, def_size = 0, def_flags = 0;
    if (tf_flags & 0x000001u) {  // base-data-offset-present
        if (off + 8 > tfhd.len) return 0;
        base_offset = rd_u64(tfhd.data + off);
        off += 8;
    }
    if (tf_flags & 0x000002u) off += 4;  // sample-description-index
    if (tf_flags & 0x000008u) {
        if (off + 4 > tfhd.len) return 0;
        def_duration = rd_u32(tfhd.data + off);
        off += 4;
    }
    if (tf_flags & 0x000010u) {
        if (off + 4 > tfhd.len) return 0;
        def_size = rd_u32(tfhd.data + off);
        off += 4;
    }
    if (tf_flags & 0x000020u) {
        if (off + 4 > tfhd.len) return 0;
        def_flags = rd_u32(tfhd.data + off);
    }

    uint64_t base_media_decode_time = 0;
    mp4span tfdt;
    if (box_child(traf, "tfdt", &tfdt) && tfdt.len >= 8) {
        base_media_decode_time = tfdt.data[0] == 1 ? rd_u64(tfdt.data + 4) : rd_u32(tfdt.data + 4);
    }

    uint64_t dts_ticks = base_media_decode_time;
    // Where the next trun's samples start when it declares no data-offset of its
    // own: immediately after the previous one's.
    uint64_t running_pos = base_offset;
    int queued = 0;
    size_t trun_cursor = 0;

    while (box_next(traf->data, traf->len, "trun", &trun_cursor, &trun, NULL)) {
    if (trun.len < 8) continue;
    uint32_t tr_flags = rd_u24(trun.data + 1);
    uint8_t trun_version = trun.data[0];
    uint32_t sample_count = rd_u32(trun.data + 4);
    size_t toff = 8;
    int32_t data_offset = 0;
    uint32_t first_flags = 0;
    bool have_first_flags = false;
    if (tr_flags & 0x000001u) {
        if (toff + 4 > trun.len) continue;
        data_offset = (int32_t)rd_u32(trun.data + toff);
        toff += 4;
    }
    if (tr_flags & 0x000004u) {
        if (toff + 4 > trun.len) continue;
        first_flags = rd_u32(trun.data + toff);
        have_first_flags = true;
        toff += 4;
    }

    uint64_t data_pos = (tr_flags & 0x000001u)
        ? base_offset + (uint64_t)(int64_t)data_offset
        : running_pos;

    for (uint32_t i = 0; i < sample_count; i++) {
        frag_sample s = { def_duration, def_size, def_flags, 0 };
        if (tr_flags & 0x000100u) {
            if (toff + 4 > trun.len) break;
            s.duration = rd_u32(trun.data + toff); toff += 4;
        }
        if (tr_flags & 0x000200u) {
            if (toff + 4 > trun.len) break;
            s.size = rd_u32(trun.data + toff); toff += 4;
        }
        if (tr_flags & 0x000400u) {
            if (toff + 4 > trun.len) break;
            s.flags = rd_u32(trun.data + toff); toff += 4;
        }
        if (tr_flags & 0x000800u) {
            if (toff + 4 > trun.len) break;
            uint32_t raw = rd_u32(trun.data + toff); toff += 4;
            // Version 0 declares the offset unsigned; version 1 signed, which is
            // what a stream with B-frames needs to express a negative shift.
            s.cts_offset = trun_version == 0 ? (int32_t)(raw & 0x7FFFFFFFu) : (int32_t)raw;
        }
        if (i == 0 && have_first_flags) s.flags = first_flags;

        if (data_pos + s.size > frag_len || s.size == 0) break;
        const uint8_t *bytes = frag + data_pos;

        // sample_is_non_sync_sample; audio has no non-sync samples to speak of.
        bool key = t->is_video ? (((s.flags >> 16) & 1u) == 0) : true;

        size_t payload_len = 0;
        uint8_t *payload = t->codec == TS_CODEC_AAC
            ? to_adts(t, bytes, s.size, &payload_len)
            : to_annexb(t, bytes, s.size, key, &payload_len);

        if (payload && payload_len) {
            int64_t raw_dts = to_90k(dts_ticks, t->timescale);
            // A negative composition offset (B-frames) larger than the decode
            // time itself would wrap the unsigned conversion; clamp instead.
            int64_t pts_ticks = (int64_t)dts_ticks + s.cts_offset;
            if (pts_ticks < 0) pts_ticks = 0;
            int64_t raw_pts = to_90k((uint64_t)pts_ticks, t->timescale);
            if (!m->have_origin) {
                m->origin = raw_dts;
                m->have_origin = true;
            }
            ts_sample out;
            out.payload = payload;
            out.len = payload_len;
            out.dts = raw_dts - m->origin + RS_TS_START_PTS;
            out.pts = raw_pts - m->origin + RS_TS_START_PTS;
            if (out.pts < out.dts) out.pts = out.dts;
            out.key = key;
            if (queue_push(t, out)) {
                t->high_water = out.dts;
                t->have_high_water = true;
                queued++;
            } else {
                free(payload);
            }
        } else {
            free(payload);
        }

        data_pos += s.size;
        dts_ticks += s.duration;
    }
    running_pos = data_pos;
    }
    return queued;
}

int rs_ts_mux_push(rs_ts_mux *m, int track, const uint8_t *frag, size_t frag_len) {
    if (!m || track < 0 || (size_t)track >= m->ntracks || !frag) return -1;
    ts_track *t = &m->tracks[track];
    if (!t->used) return -1;

    // A segment is a *sequence* of moof+mdat chunks under CMAF low-latency
    // packaging (claro's DASH publishes two per 2s segment), each with its own
    // traf and its own decode time. Taking only the first drops the rest of the
    // segment — silently, since what remains still parses and still plays.
    int queued = 0;
    bool saw_moof = false;
    size_t moof_cursor = 0, moof_start = 0;
    mp4span moof;
    while (box_next(frag, frag_len, "moof", &moof_cursor, &moof, &moof_start)) {
        saw_moof = true;
        // Sample data offsets default to being relative to this moof, which is
        // why each chunk has to carry its own base rather than the segment's.
        size_t traf_cursor = 0;
        mp4span traf;
        while (box_next(moof.data, moof.len, "traf", &traf_cursor, &traf, NULL))
            queued += push_traf(m, t, frag, frag_len, &traf, moof_start);
    }
    return saw_moof ? queued : -1;
}

// --- TS packet emission -----------------------------------------------------

// Writes one 188-byte packet: header, an adaptation field sized to whatever the
// payload leaves over, then the payload.
static void ts_packet(rs_ts_mux *m, uint16_t pid, uint8_t *cc, bool pusi,
                      bool want_pcr, int64_t pcr_90k, bool rai, bool discontinuity,
                      const uint8_t *payload, size_t payload_len) {
    uint8_t pkt[RS_TS_PACKET_SIZE];
    size_t af_total = 184 - payload_len;

    pkt[0] = 0x47;
    pkt[1] = (uint8_t)((pusi ? 0x40u : 0x00u) | ((uint32_t)pid >> 8));
    pkt[2] = (uint8_t)(pid & 0xFFu);
    uint8_t afc = (uint8_t)(af_total ? (payload_len ? 0x30u : 0x20u) : 0x10u);
    pkt[3] = (uint8_t)(afc | (*cc & 0x0Fu));
    *cc = (uint8_t)((*cc + 1) & 0x0Fu);

    size_t o = 4;
    if (af_total >= 1) {
        pkt[o++] = (uint8_t)(af_total - 1);
        if (af_total >= 2) {
            uint8_t flags = 0;
            if (discontinuity) flags |= 0x80u;
            if (rai) flags |= 0x40u;
            if (want_pcr) flags |= 0x10u;
            pkt[o++] = flags;
            if (want_pcr) {
                int64_t base = pcr_90k;
                if (base < 0) base = 0;
                uint64_t b = (uint64_t)base & 0x1FFFFFFFFull;
                pkt[o++] = (uint8_t)((b >> 25) & 0xFFu);
                pkt[o++] = (uint8_t)((b >> 17) & 0xFFu);
                pkt[o++] = (uint8_t)((b >> 9) & 0xFFu);
                pkt[o++] = (uint8_t)((b >> 1) & 0xFFu);
                pkt[o++] = (uint8_t)(((b & 1ull) << 7) | 0x7Eu);  // reserved bits set
                pkt[o++] = 0x00;                                   // extension 0
            }
            // af_total counts the length byte too, so what is left to stuff is
            // af_total minus everything written since the header.
            size_t stuffing = af_total - (o - 4);
            memset(pkt + o, 0xFF, stuffing);
            o += stuffing;
        }
    }
    if (payload_len) memcpy(pkt + o, payload, payload_len);
    bb_add(&m->out, pkt, RS_TS_PACKET_SIZE);
}

// A section (PAT or PMT) always fits one packet at these sizes: pointer field,
// section, CRC, then 0xFF to the end of the packet.
static void ts_section(rs_ts_mux *m, uint16_t pid, uint8_t *cc,
                       const uint8_t *section, size_t len) {
    uint8_t payload[184];
    memset(payload, 0xFF, sizeof payload);
    payload[0] = 0x00;  // pointer_field
    if (len > sizeof payload - 1) return;
    memcpy(payload + 1, section, len);
    ts_packet(m, pid, cc, true, false, 0, false, false, payload, sizeof payload);
}

static void write_pat(rs_ts_mux *m) {
    uint8_t s[16];
    size_t n = 0;
    s[n++] = 0x00;                      // table_id
    s[n++] = 0xB0;                      // syntax indicator + length high
    s[n++] = 0x0D;                      // section_length = 13
    s[n++] = 0x00; s[n++] = 0x01;       // transport_stream_id
    s[n++] = 0xC1;                      // version 0, current
    s[n++] = 0x00; s[n++] = 0x00;       // section_number / last_section_number
    s[n++] = 0x00; s[n++] = 0x01;       // program_number 1
    s[n++] = (uint8_t)(0xE0u | (RS_TS_PMT_PID >> 8));
    s[n++] = (uint8_t)(RS_TS_PMT_PID & 0xFFu);
    uint32_t crc = rs_ts_crc32(s, n);
    s[n++] = (uint8_t)(crc >> 24); s[n++] = (uint8_t)(crc >> 16);
    s[n++] = (uint8_t)(crc >> 8);  s[n++] = (uint8_t)crc;
    ts_section(m, 0x0000, &m->pat_cc, s, n);
}

static void write_pmt(rs_ts_mux *m) {
    uint8_t s[64];
    size_t n = 0;
    size_t es_count = 0;
    for (size_t i = 0; i < m->ntracks; i++) if (m->tracks[i].used) es_count++;
    size_t section_length = 9 + 5 * es_count + 4;

    s[n++] = 0x02;                                     // table_id
    s[n++] = (uint8_t)(0xB0u | ((section_length >> 8) & 0x0Fu));
    s[n++] = (uint8_t)(section_length & 0xFFu);
    s[n++] = 0x00; s[n++] = 0x01;                      // program_number
    s[n++] = 0xC1;                                     // version 0, current
    s[n++] = 0x00; s[n++] = 0x00;                      // section / last section
    s[n++] = (uint8_t)(0xE0u | (m->pcr_pid >> 8));
    s[n++] = (uint8_t)(m->pcr_pid & 0xFFu);
    s[n++] = 0xF0; s[n++] = 0x00;                      // program_info_length 0

    for (size_t i = 0; i < m->ntracks; i++) {
        const ts_track *t = &m->tracks[i];
        if (!t->used) continue;
        uint8_t stream_type = t->codec == TS_CODEC_H264 ? 0x1B
                            : t->codec == TS_CODEC_H265 ? 0x24
                            : 0x0F;
        s[n++] = stream_type;
        s[n++] = (uint8_t)(0xE0u | (t->pid >> 8));
        s[n++] = (uint8_t)(t->pid & 0xFFu);
        s[n++] = 0xF0; s[n++] = 0x00;                  // ES_info_length 0
    }
    uint32_t crc = rs_ts_crc32(s, n);
    s[n++] = (uint8_t)(crc >> 24); s[n++] = (uint8_t)(crc >> 16);
    s[n++] = (uint8_t)(crc >> 8);  s[n++] = (uint8_t)crc;
    ts_section(m, RS_TS_PMT_PID, &m->pmt_cc, s, n);
}

// PTS/DTS are 33 bits split across five bytes by four marker bits.
static void write_timestamp(uint8_t *o, uint8_t prefix, int64_t ts) {
    uint64_t v = (uint64_t)(ts < 0 ? 0 : ts) & 0x1FFFFFFFFull;
    o[0] = (uint8_t)((prefix << 4) | ((v >> 29) & 0x0Eu) | 0x01u);
    o[1] = (uint8_t)((v >> 22) & 0xFFu);
    o[2] = (uint8_t)(((v >> 14) & 0xFEu) | 0x01u);
    o[3] = (uint8_t)((v >> 7) & 0xFFu);
    o[4] = (uint8_t)(((v << 1) & 0xFEu) | 0x01u);
}

static void emit_sample(rs_ts_mux *m, ts_track *t, const ts_sample *s) {
    uint8_t header[19];
    size_t hn = 0;
    bool with_dts = s->pts != s->dts;

    // A live source that splices (an ad break) or that the engine fell behind
    // enough to skip past leaves a hole in the media timeline. MPEG-TS has a
    // flag for exactly that; without it a decoder reads the jump as a broken
    // clock and either stalls or spends the next few seconds resynchronising.
    bool discontinuity = t->have_last_dts &&
        (s->dts < t->last_dts || s->dts - t->last_dts > RS_TS_DISC_THRESHOLD);
    t->last_dts = s->dts;
    t->have_last_dts = true;

    header[hn++] = 0x00; header[hn++] = 0x00; header[hn++] = 0x01;
    header[hn++] = t->stream_id;
    // PES_packet_length counts everything after the length field itself: the
    // three flag/length bytes plus the timestamps plus the access unit.
    size_t pes_payload = s->len + (with_dts ? 13u : 8u);
    // Only a video stream may declare length 0 ("until the next start"), which
    // is what an access unit larger than 65535 bytes needs.
    if (pes_payload > 0xFFFF) {
        if (!t->is_video) return;
        header[hn++] = 0x00; header[hn++] = 0x00;
    } else {
        header[hn++] = (uint8_t)((pes_payload >> 8) & 0xFFu);
        header[hn++] = (uint8_t)(pes_payload & 0xFFu);
    }
    header[hn++] = 0x84;  // '10', no scrambling, data_alignment_indicator
    header[hn++] = with_dts ? 0xC0 : 0x80;
    header[hn++] = with_dts ? 10 : 5;
    write_timestamp(header + hn, with_dts ? 0x3u : 0x2u, s->pts);
    hn += 5;
    if (with_dts) {
        write_timestamp(header + hn, 0x1u, s->dts);
        hn += 5;
    }

    // The PES header and the access unit are one contiguous stream to packetise;
    // sending the header separately would waste a packet per frame.
    size_t total = hn + s->len;
    size_t sent = 0;
    bool first = true;
    while (sent < total) {
        bool want_pcr = first && t->pid == m->pcr_pid;
        bool rai = first && s->key;
        size_t reserve = want_pcr ? 8u : (rai ? 2u : 0u);
        size_t take = 184 - reserve;
        if (take > total - sent) take = total - sent;

        uint8_t chunk[184];
        for (size_t i = 0; i < take; i++) {
            size_t pos = sent + i;
            chunk[i] = pos < hn ? header[pos] : s->payload[pos - hn];
        }
        ts_packet(m, t->pid, &t->cc, first, want_pcr, s->dts - RS_TS_PCR_LEAD, rai,
                  first && discontinuity, chunk, take);
        sent += take;
        first = false;
    }
    m->started = true;
}

// --- interleaving -----------------------------------------------------------

// The track holding the earliest queued sample, or SIZE_MAX when every queue is
// empty.
static size_t earliest_track(const rs_ts_mux *m) {
    size_t best = (size_t)-1;
    int64_t best_dts = 0;
    for (size_t i = 0; i < m->ntracks; i++) {
        const ts_track *t = &m->tracks[i];
        if (!t->used || t->head >= t->nq) continue;
        int64_t dts = t->queue[t->head].dts;
        if (best == (size_t)-1 || dts < best_dts) {
            best = i;
            best_dts = dts;
        }
    }
    return best;
}

// A sample is safe to emit once every other track has either finished or already
// delivered something at least as late — at that point nothing earlier can still
// arrive, so the output stays in DTS order.
static bool safe_to_emit(const rs_ts_mux *m, size_t track, int64_t dts) {
    for (size_t i = 0; i < m->ntracks; i++) {
        const ts_track *t = &m->tracks[i];
        if (i == track || !t->used || t->ended) continue;
        if (!t->have_high_water) return false;
        if (t->high_water < dts) return false;
    }
    return true;
}

// Drops the samples already emitted from the front of a queue once they pile up,
// so a long-running session doesn't grow the array without bound.
static void queue_compact(ts_track *t) {
    if (t->head == 0 || t->head < 256) return;
    size_t remain = t->nq - t->head;
    memmove(t->queue, t->queue + t->head, remain * sizeof(*t->queue));
    t->nq = remain;
    t->head = 0;
}

static void join_record(rs_ts_mux *m, size_t offset) {
    if (m->njoins == m->capjoins) {
        size_t want = m->capjoins ? m->capjoins * 2 : 16;
        size_t *nj = (size_t *)realloc(m->joins, want * sizeof(*nj));
        if (!nj) return;
        m->joins = nj;
        m->capjoins = want;
    }
    m->joins[m->njoins++] = offset;
}

// Each rendition is tailed from its own queue, and those queues start at
// whatever segment boundary was nearest the live edge — which is not the same
// media instant for video and audio. Emitting from each track's own first
// sample would put a stretch of one-track-only output at the head of the
// stream, and a player that probes only the beginning concludes the other track
// has no parameters (ffmpeg: "could not find codec parameters ... unspecified
// sample rate"). Trimming every track forward to the latest first-sample time
// costs a fraction of a segment and makes the first byte a viewer sees carry
// both. Returns false while a track that could still deliver has nothing yet.
static bool align_tracks(rs_ts_mux *m, bool flush) {
    if (m->aligned) return true;
    int64_t floor_dts = 0;
    bool any = false;
    for (size_t i = 0; i < m->ntracks; i++) {
        ts_track *t = &m->tracks[i];
        if (!t->used || t->ended) continue;
        if (t->head >= t->nq) return flush;  // still waiting on this one
        int64_t first = t->queue[t->head].dts;
        if (!any || first > floor_dts) floor_dts = first;
        any = true;
    }
    if (!any) return flush;
    for (size_t i = 0; i < m->ntracks; i++) {
        ts_track *t = &m->tracks[i];
        if (!t->used) continue;
        while (t->head < t->nq && t->queue[t->head].dts < floor_dts) {
            free(t->queue[t->head].payload);
            t->queue[t->head].payload = NULL;
            t->head++;
        }
    }
    m->aligned = true;
    return true;
}

uint8_t *rs_ts_mux_take(rs_ts_mux *m, bool flush, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!m || !out_len) return NULL;
    m->njoins = 0;
    if (!align_tracks(m, flush)) return NULL;

    for (;;) {
        size_t idx = earliest_track(m);
        if (idx == (size_t)-1) break;
        ts_track *t = &m->tracks[idx];
        ts_sample *s = &t->queue[t->head];
        if (!flush && !safe_to_emit(m, idx, s->dts)) break;

        // A video keyframe is where a new viewer can start, so it always gets a
        // fresh PAT/PMT in front of it — otherwise the join point would carry
        // picture the viewer can decode but no table telling it which PID is
        // which. Otherwise tables just repeat on their period.
        bool random_access = t->is_video && s->key;
        if (!m->wrote_tables || random_access ||
            s->dts - m->last_table_dts >= RS_TS_TABLE_PERIOD) {
            if (random_access) join_record(m, m->out.len);
            write_pat(m);
            write_pmt(m);
            m->wrote_tables = true;
            m->last_table_dts = s->dts;
        }

        emit_sample(m, t, s);
        free(s->payload);
        s->payload = NULL;
        t->head++;
        queue_compact(t);
    }

    if (!m->out.len) return NULL;
    uint8_t *bytes = m->out.p;
    *out_len = m->out.len;
    m->out.p = NULL;
    m->out.len = m->out.cap = 0;
    return bytes;
}

// --- lifecycle --------------------------------------------------------------

rs_ts_mux *rs_ts_mux_create(void) {
    rs_ts_mux *m = (rs_ts_mux *)calloc(1, sizeof(*m));
    if (m) m->pcr_pid = RS_TS_BASE_PID;
    return m;
}

int rs_ts_mux_add_track(rs_ts_mux *m, const char *kind, const uint8_t *init, size_t init_len) {
    if (!m || !init || m->ntracks >= RS_TS_MAX_TRACKS) return -1;
    ts_track *t = &m->tracks[m->ntracks];
    memset(t, 0, sizeof(*t));
    t->is_video = !(kind && strcmp(kind, "audio") == 0);
    if (!parse_init(t, init, init_len)) {
        free(t->param_sets);
        memset(t, 0, sizeof(*t));
        return -1;
    }
    // The init segment is the authority on what this actually is; the caller's
    // label only decides the PID range when both agree.
    t->is_video = t->codec != TS_CODEC_AAC;
    t->pid = (uint16_t)(RS_TS_BASE_PID + m->ntracks);
    t->stream_id = t->is_video ? 0xE0 : 0xC0;
    t->used = true;
    int handle = (int)m->ntracks;
    m->ntracks++;
    // PCR rides the video PID when there is one — it is the track with the
    // steadier packet cadence, and the one a decoder clocks itself against.
    if (t->is_video) {
        bool only_video = true;
        for (size_t i = 0; i + 1 < m->ntracks; i++)
            if (m->tracks[i].used && m->tracks[i].is_video) only_video = false;
        if (only_video) m->pcr_pid = t->pid;
    } else if (m->ntracks == 1) {
        m->pcr_pid = t->pid;
    }
    return handle;
}

void rs_ts_mux_end_track(rs_ts_mux *m, int track) {
    if (!m || track < 0 || (size_t)track >= m->ntracks) return;
    m->tracks[track].ended = true;
}

size_t rs_ts_mux_join_points(const rs_ts_mux *m, const size_t **out_offsets) {
    if (!m) return 0;
    if (out_offsets) *out_offsets = m->joins;
    return m->njoins;
}

size_t rs_ts_mux_pending(const rs_ts_mux *m) {
    if (!m) return 0;
    size_t n = 0;
    for (size_t i = 0; i < m->ntracks; i++)
        if (m->tracks[i].used) n += m->tracks[i].nq - m->tracks[i].head;
    return n;
}

bool rs_ts_mux_started(const rs_ts_mux *m) {
    return m && m->started;
}

void rs_ts_mux_destroy(rs_ts_mux *m) {
    if (!m) return;
    for (size_t i = 0; i < m->ntracks; i++) {
        ts_track *t = &m->tracks[i];
        for (size_t j = t->head; j < t->nq; j++) free(t->queue[j].payload);
        free(t->queue);
        free(t->param_sets);
    }
    bb_free(&m->out);
    free(m->joins);
    free(m);
}
