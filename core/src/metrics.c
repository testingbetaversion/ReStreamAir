#include "rs_metrics.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rs_internal.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

static int64_t current_time_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

// How long a byte sample stays in a rate window. Serving is smooth — a player
// pulls a segment every few seconds and several players interleave — so ten
// seconds reads as a steady figure.
#define RS_METRICS_RATE_WINDOW_MS 10000
// Pulling is not smooth. The live engine fetches a batch of segments as fast as
// the origin will give them up, then sits idle until the manifest advertises
// more, so over ten seconds a stream alternates between its burst rate and a
// flat zero. Thirty seconds is long enough to span the idle gap and report what
// the stream actually costs upstream, which is the question the figure is there
// to answer.
#define RS_METRICS_INPUT_WINDOW_MS 30000

typedef struct rs_byte_sample rs_byte_sample;

typedef struct rs_conn_info {
    char *stream_id;
    char *identity;
    char *client_ip;
    char *user_agent;
    int64_t last_seen_ms;
    int64_t first_seen_ms;
    int64_t total_bytes;
    rs_byte_sample *recent_bytes;
    int error_count;
    struct rs_conn_info *next;
} rs_conn_info;

struct rs_byte_sample {
    int64_t ts_ms;
    int bytes;
    struct rs_byte_sample *next;
};

typedef struct rs_stream_stats {
    char *stream_id;
    int64_t total_bytes;
    rs_byte_sample *recent_bytes;
    // Bytes pulled from the origin, tracked separately from the bytes served:
    // the live engine keeps fetching with no client connected, and a proxied
    // segment is fetched encrypted and served decrypted, so the two counts
    // never did agree.
    int64_t input_total_bytes;
    rs_byte_sample *input_recent_bytes;
    struct rs_stream_stats *next;
} rs_stream_stats;

struct rs_metrics {
    rs_conn_info *conns;
    rs_stream_stats *streams;
};

rs_metrics* rs_metrics_create(void) {
    rs_metrics *m = calloc(1, sizeof(rs_metrics));
    return m;
}

static void free_samples(rs_byte_sample *s) {
    while (s) {
        rs_byte_sample *next = s->next;
        free(s);
        s = next;
    }
}

void rs_metrics_destroy(rs_metrics *m) {
    if (!m) return;
    rs_conn_info *c = m->conns;
    while (c) {
        rs_conn_info *next = c->next;
        rs_free(c->stream_id);
        rs_free(c->identity);
        rs_free(c->client_ip);
        rs_free(c->user_agent);
        free_samples(c->recent_bytes);
        free(c);
        c = next;
    }
    rs_stream_stats *s = m->streams;
    while (s) {
        rs_stream_stats *next = s->next;
        rs_free(s->stream_id);
        free_samples(s->recent_bytes);
        free_samples(s->input_recent_bytes);
        free(s);
        s = next;
    }
    free(m);
}

// Push one timestamped sample onto a rate window. Silently drops the sample if
// the allocation fails: a missing sample makes the reported rate slightly low
// for ten seconds, which is a better failure than losing the byte total too.
static void push_sample(rs_byte_sample **head, int64_t now_ms, int bytes) {
    rs_byte_sample *sample = calloc(1, sizeof(rs_byte_sample));
    if (!sample) return;
    sample->ts_ms = now_ms;
    sample->bytes = bytes;
    sample->next = *head;
    *head = sample;
}

// Everything in a rate window at or after `cutoff_ms`. Samples older than that
// are dropped by rs_metrics_prune, but a window is read between prunes too.
static int64_t sum_samples(const rs_byte_sample *s, int64_t cutoff_ms) {
    int64_t total = 0;
    for (; s; s = s->next)
        if (s->ts_ms >= cutoff_ms) total += s->bytes;
    return total;
}

// Drops everything older than `cutoff_ms` from one rate window.
static void prune_samples(rs_byte_sample **head, int64_t cutoff_ms) {
    while (*head) {
        rs_byte_sample *sample = *head;
        if (sample->ts_ms < cutoff_ms) { *head = sample->next; free(sample); }
        else head = &sample->next;
    }
}

static rs_stream_stats* get_or_create_stream(rs_metrics *m, const char *stream_id) {
    rs_stream_stats *s = m->streams;
    while (s) {
        if (strcmp(s->stream_id, stream_id) == 0) return s;
        s = s->next;
    }
    s = calloc(1, sizeof(rs_stream_stats));
    s->stream_id = rs_strdup(stream_id);
    s->next = m->streams;
    m->streams = s;
    return s;
}

// Keyed by (stream_id, identity, client IP), not identity alone: an identity defaults to
// the client IP when no playback key is configured, so the same viewer
// switching between two streams (or two tabs watching two streams at once)
// must not collapse into one connection record. The IP is also part of the key
// because one API key can legitimately be shared by several TVs/players.
// Collapsing those would hide connections and make per-client bandwidth wrong.
static rs_conn_info* get_or_create_conn(rs_metrics *m, const char *stream_id, const char *identity,
                                        const char *client_ip) {
    rs_conn_info *c = m->conns;
    while (c) {
        if (strcmp(c->stream_id, stream_id) == 0 && strcmp(c->identity, identity) == 0 &&
            strcmp(c->client_ip, client_ip ? client_ip : "") == 0) return c;
        c = c->next;
    }
    c = calloc(1, sizeof(rs_conn_info));
    c->stream_id = rs_strdup(stream_id);
    c->identity = rs_strdup(identity);
    c->client_ip = rs_strdup(client_ip ? client_ip : "");
    c->user_agent = rs_strdup("");
    c->first_seen_ms = current_time_ms();
    c->next = m->conns;
    m->conns = c;
    return c;
}

void rs_metrics_record(rs_metrics *m, const char *stream_id, const char *identity,
                       const char *client_ip, const char *user_agent, int bytes) {
    if (!m || !stream_id || !identity) return;

    int64_t now = current_time_ms();

    rs_stream_stats *s = get_or_create_stream(m, stream_id);
    s->total_bytes += bytes;
    push_sample(&s->recent_bytes, now, bytes);

    rs_conn_info *c = get_or_create_conn(m, stream_id, identity, client_ip);
    c->last_seen_ms = now;
    c->total_bytes += bytes;
    push_sample(&c->recent_bytes, now, bytes);
    if (client_ip && client_ip[0]) {
        rs_free(c->client_ip);
        c->client_ip = rs_strdup(client_ip);
    }
    if (user_agent && user_agent[0]) {
        rs_free(c->user_agent);
        c->user_agent = rs_strdup(user_agent);
    }
}

void rs_metrics_record_input(rs_metrics *m, const char *stream_id, long long bytes) {
    if (!m || !stream_id || !stream_id[0] || bytes <= 0) return;
    int64_t now = current_time_ms();
    rs_stream_stats *s = get_or_create_stream(m, stream_id);
    if (!s) return;
    s->input_total_bytes += bytes;
    // A single sample is one segment, so it never approaches INT_MAX; clamp
    // anyway rather than let a bad caller wrap the window negative.
    push_sample(&s->input_recent_bytes, now, bytes > 0x7fffffffLL ? 0x7fffffff : (int)bytes);
}

void rs_metrics_record_error(rs_metrics *m, const char *identity) {
    if (!m || !identity) return;
    int64_t now = current_time_ms();
    rs_conn_info *c = m->conns;
    while (c) {
        if (strcmp(c->identity, identity) == 0) {
            c->error_count++;
            c->last_seen_ms = now;
            break;
        }
        c = c->next;
    }
}

void rs_metrics_prune(rs_metrics *m) {
    if (!m) return;
    int64_t now = current_time_ms();
    int64_t activity_cutoff = now - 45000;
    int64_t rate_cutoff = now - RS_METRICS_RATE_WINDOW_MS;
    int64_t input_cutoff = now - RS_METRICS_INPUT_WINDOW_MS;

    rs_conn_info **cp = &m->conns;
    while (*cp) {
        rs_conn_info *c = *cp;
        if (c->last_seen_ms < activity_cutoff) {
            *cp = c->next;
            rs_free(c->stream_id);
            rs_free(c->identity);
            rs_free(c->client_ip);
            rs_free(c->user_agent);
            free_samples(c->recent_bytes);
            free(c);
        } else {
            prune_samples(&c->recent_bytes, rate_cutoff);
            cp = &c->next;
        }
    }

    for (rs_stream_stats *s = m->streams; s; s = s->next) {
        prune_samples(&s->recent_bytes, rate_cutoff);
        prune_samples(&s->input_recent_bytes, input_cutoff);
    }
}

int rs_metrics_active_clients(const rs_metrics *m, const char *stream_id) {
    if (!m || !stream_id) return 0;
    int64_t now = current_time_ms();
    int64_t activity_cutoff = now - 45000;
    int count = 0;
    rs_conn_info *c = m->conns;
    while (c) {
        if (strcmp(c->stream_id, stream_id) == 0 && c->last_seen_ms >= activity_cutoff) {
            count++;
        }
        c = c->next;
    }
    return count;
}

static const rs_stream_stats* find_stream(const rs_metrics *m, const char *stream_id) {
    if (!m || !stream_id) return NULL;
    for (const rs_stream_stats *s = m->streams; s; s = s->next)
        if (strcmp(s->stream_id, stream_id) == 0) return s;
    return NULL;
}

double rs_metrics_bytes_per_sec(const rs_metrics *m, const char *stream_id) {
    const rs_stream_stats *s = find_stream(m, stream_id);
    if (!s) return 0.0;
    int64_t cutoff = current_time_ms() - RS_METRICS_RATE_WINDOW_MS;
    return (double)sum_samples(s->recent_bytes, cutoff) / (RS_METRICS_RATE_WINDOW_MS / 1000.0);
}

int64_t rs_metrics_total_bytes(const rs_metrics *m, const char *stream_id) {
    const rs_stream_stats *s = find_stream(m, stream_id);
    return s ? s->total_bytes : 0;
}

double rs_metrics_input_bytes_per_sec(const rs_metrics *m, const char *stream_id) {
    const rs_stream_stats *s = find_stream(m, stream_id);
    if (!s) return 0.0;
    int64_t cutoff = current_time_ms() - RS_METRICS_INPUT_WINDOW_MS;
    return (double)sum_samples(s->input_recent_bytes, cutoff) / (RS_METRICS_INPUT_WINDOW_MS / 1000.0);
}

int64_t rs_metrics_input_total_bytes(const rs_metrics *m, const char *stream_id) {
    const rs_stream_stats *s = find_stream(m, stream_id);
    return s ? s->input_total_bytes : 0;
}

void rs_metrics_each_connection(const rs_metrics *m, rs_metrics_connection_fn fn, void *ctx) {
    if (!m || !fn) return;
    int64_t now = current_time_ms();
    int64_t activity_cutoff = now - 45000;
    int64_t rate_cutoff = now - RS_METRICS_RATE_WINDOW_MS;
    for (const rs_conn_info *c = m->conns; c; c = c->next) {
        if (c->last_seen_ms < activity_cutoff) continue;
        double recent = (double)sum_samples(c->recent_bytes, rate_cutoff) /
                        (RS_METRICS_RATE_WINDOW_MS / 1000.0);
        fn(ctx, c->stream_id, c->identity, c->client_ip, c->user_agent,
           (now - c->first_seen_ms) / 1000, c->error_count,
           recent, c->total_bytes);
    }
}
