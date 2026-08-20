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
        free(s);
        s = next;
    }
    free(m);
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
    rs_byte_sample *sample = calloc(1, sizeof(rs_byte_sample));
    sample->ts_ms = now;
    sample->bytes = bytes;
    sample->next = s->recent_bytes;
    s->recent_bytes = sample;

    rs_conn_info *c = get_or_create_conn(m, stream_id, identity, client_ip);
    c->last_seen_ms = now;
    c->total_bytes += bytes;
    rs_byte_sample *client_sample = calloc(1, sizeof(rs_byte_sample));
    if (client_sample) {
        client_sample->ts_ms = now;
        client_sample->bytes = bytes;
        client_sample->next = c->recent_bytes;
        c->recent_bytes = client_sample;
    }
    if (client_ip && client_ip[0]) {
        rs_free(c->client_ip);
        c->client_ip = rs_strdup(client_ip);
    }
    if (user_agent && user_agent[0]) {
        rs_free(c->user_agent);
        c->user_agent = rs_strdup(user_agent);
    }
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
    int64_t rate_cutoff = now - 10000;

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
            rs_byte_sample **sp = &c->recent_bytes;
            while (*sp) {
                rs_byte_sample *sample = *sp;
                if (sample->ts_ms < rate_cutoff) { *sp = sample->next; free(sample); }
                else sp = &sample->next;
            }
            cp = &c->next;
        }
    }

    rs_stream_stats *s = m->streams;
    while (s) {
        rs_byte_sample **sp = &s->recent_bytes;
        while (*sp) {
            rs_byte_sample *sample = *sp;
            if (sample->ts_ms < rate_cutoff) {
                *sp = sample->next;
                free(sample);
            } else {
                sp = &sample->next;
            }
        }
        s = s->next;
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

double rs_metrics_bytes_per_sec(const rs_metrics *m, const char *stream_id) {
    if (!m || !stream_id) return 0.0;
    int64_t now = current_time_ms();
    int64_t rate_cutoff = now - 10000;
    rs_stream_stats *s = m->streams;
    while (s) {
        if (strcmp(s->stream_id, stream_id) == 0) {
            int recent_bytes = 0;
            rs_byte_sample *sample = s->recent_bytes;
            while (sample) {
                if (sample->ts_ms >= rate_cutoff) {
                    recent_bytes += sample->bytes;
                }
                sample = sample->next;
            }
            return (double)recent_bytes / 10.0;
        }
        s = s->next;
    }
    return 0.0;
}

int64_t rs_metrics_total_bytes(const rs_metrics *m, const char *stream_id) {
    if (!m || !stream_id) return 0;
    rs_stream_stats *s = m->streams;
    while (s) {
        if (strcmp(s->stream_id, stream_id) == 0) {
            return s->total_bytes;
        }
        s = s->next;
    }
    return 0;
}

void rs_metrics_each_connection(const rs_metrics *m, rs_metrics_connection_fn fn, void *ctx) {
    if (!m || !fn) return;
    int64_t now = current_time_ms();
    int64_t activity_cutoff = now - 45000;
    int64_t rate_cutoff = now - 10000;
    for (const rs_conn_info *c = m->conns; c; c = c->next) {
        if (c->last_seen_ms < activity_cutoff) continue;
        long long recent = 0;
        for (const rs_byte_sample *sample = c->recent_bytes; sample; sample = sample->next)
            if (sample->ts_ms >= rate_cutoff) recent += sample->bytes;
        fn(ctx, c->stream_id, c->identity, c->client_ip, c->user_agent,
           (now - c->first_seen_ms) / 1000, c->error_count,
           (double)recent / 10.0, c->total_bytes);
    }
}
