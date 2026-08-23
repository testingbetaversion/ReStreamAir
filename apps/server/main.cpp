// The panel executable. It is deliberately thin: everything it does lives in
// the core, and this file only parses argv, wires up the handlers that need
// libcurl and libxml2, runs the poll loop and shuts down cleanly.

#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <curl/curl.h>

#include "net.h"
#include "probe.h"
#include "rs_dash.h"
#include "ffrun.h"
#include "service.h"
#include "webroot.h"
#include "restream.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

extern "C" void handle_signal(int) {
    g_stop = 1;
}

void print_usage(const char *program) {
    std::fprintf(stderr,
                 "usage: %s [serve] [-p|--port N] [-b|--bind ADDRESS] [--root DIR] [--verbose]\n"
                 "\n"
                 "  serve             optional; the default action, accepted for symmetry\n"
                 "  -p, --port N      port to listen on (default 8787, or the panel's stored value)\n"
                 "  -b, --bind ADDR   address to bind (default 0.0.0.0)\n"
                 "  --root DIR        serve the panel's static files from DIR (auto-detects public/,\n"
                 "                    and downloads it from GitHub into a cache if there is none)\n"
                 "  --refresh-web     re-download the front-end over the cached copy\n"
                 "  --web-ref REF     branch, tag or commit to fetch the front-end from (default main)\n"
                 "  --no-download     never fetch; use public/ or an existing cache only\n"
                 "  --verbose         full mongoose trace logging (default: errors only)\n"
                 "  -h, --help        this message\n",
                 program);
}

bool needs_value(const char *arg, int index, int argc, const char *flag) {
    if (index + 1 >= argc) {
        std::fprintf(stderr, "restreamair-server: %s needs a value\n", flag);
        return false;
    }
    (void)arg;
    return true;
}

}  // namespace

static rs_ffrun *g_ffrun = nullptr;

static void pipeline_log(void *ctx, const char *stream_id, const char *level,
                         const char *event, const char *message) {
    restream_server_log_external(static_cast<restream_server_t *>(ctx), stream_id,
                                 level, event, message);
}

static int pipeline_start(const char *stream_id, const char *const *argv,
                          const char *const *producer_argv,
                          const char *const *env_keys, const char *const *env_values,
                          size_t env_count) {
    return g_ffrun ? rs_ffrun_start(g_ffrun, stream_id, argv, producer_argv,
                                    env_keys, env_values, env_count) : -1;
}

static void pipeline_stop(const char *stream_id) {
    if (g_ffrun) rs_ffrun_stop(g_ffrun, stream_id);
}

static bool pipeline_running(const char *stream_id) {
    return g_ffrun && rs_ffrun_is_running(g_ffrun, stream_id);
}

static void pipeline_poll(void) {
    if (g_ffrun) rs_ffrun_poll(g_ffrun);
}

// --- service management bridge ----------------------------------------------
//
// core/ knows nothing about systemd; these two adapt rs_service_* to the
// function-pointer hook, the same way probe/fetch/dash are wired.

static void json_escape_into(std::string &out, const char *s) {
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)*p < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", *p); out += b; }
                else out += *p;
        }
    }
}

static char *service_status_json(void) {
    rs_service_info info;
    rs_service_probe(nullptr, &info);
    std::string j = "{";
    auto boolean = [&](const char *k, bool v) { j += "\""; j += k; j += "\":"; j += v ? "true" : "false"; j += ","; };
    auto text = [&](const char *k, const char *v) {
        j += "\""; j += k; j += "\":";
        if (!v) { j += "null,"; return; }
        j += "\""; json_escape_into(j, v); j += "\",";
    };
    boolean("systemdAvailable", info.systemd_available);
    boolean("unitInstalled", info.unit_installed);
    boolean("active", info.active);
    boolean("enabled", info.enabled);
    boolean("canManage", info.can_manage);
    text("unitName", info.unit_name);
    text("unitPath", info.unit_path);
    text("execStart", info.exec_start);
    text("selfPath", info.self_path);
    text("workingDir", info.working_dir);
    if (j.size() > 1 && j.back() == ',') j.pop_back();
    j += "}";
    rs_service_info_dispose(&info);
    return rs_strdup(j.c_str());
}

static int service_action(const char *action, unsigned port, const char *bind,
                         char *errbuf, size_t errbuf_len) {
    if (!action) return -1;
    // "restart-check" validates without acting, so the route can fail the request
    // before it promises the operator a restart it cannot perform.
    if (std::strcmp(action, "restart-check") == 0) {
        rs_service_info info;
        rs_service_probe(nullptr, &info);
        int rc = 0;
        if (!info.systemd_available) {
            std::snprintf(errbuf, errbuf_len, "systemd is not available on this host");
            rc = -1;
        } else if (!info.unit_installed) {
            std::snprintf(errbuf, errbuf_len, "no systemd unit is installed yet — install it first");
            rc = -1;
        } else if (!info.can_manage) {
            std::snprintf(errbuf, errbuf_len, "restarting the service needs root");
            rc = -1;
        }
        rs_service_info_dispose(&info);
        return rc;
    }
    if (std::strcmp(action, "restart") == 0) return rs_service_restart(nullptr, errbuf, errbuf_len);
    if (std::strcmp(action, "install") == 0) return rs_service_install(nullptr, port, bind, errbuf, errbuf_len);
    std::snprintf(errbuf, errbuf_len, "unknown service action");
    return -1;
}

int main(int argc, char **argv) {
    unsigned short port = 8787;
    std::string bind_address = "0.0.0.0";
    rs_webroot_options web;
    bool verbose = false;
    // Whether the port/bind came from the command line. The panel's Settings
    // page stores both and tells the operator they "take effect after restart" —
    // which was untrue, because startup only ever looked at argv. An explicit
    // flag still wins (that is what a flag is for); otherwise the stored
    // setting is honoured, so the promise the UI makes is now kept.
    bool port_from_argv = false;
    bool bind_from_argv = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        // Accept a leading `serve` so the documented invocation
        // (`restreamair serve --port …`) works here too — it's a no-op, since
        // serving is all this binary does.
        if (i == 1 && std::strcmp(arg, "serve") == 0) {
            continue;
        }
        if ((std::strcmp(arg, "-p") == 0 || std::strcmp(arg, "--port") == 0)) {
            if (!needs_value(arg, i, argc, arg)) return 2;
            long value = std::strtol(argv[++i], nullptr, 10);
            if (value <= 0 || value > 65535) {
                std::fprintf(stderr, "restreamair-server: invalid port '%s'\n", argv[i]);
                return 2;
            }
            port = static_cast<unsigned short>(value);
            port_from_argv = true;
        } else if (std::strcmp(arg, "-b") == 0 || std::strcmp(arg, "--bind") == 0) {
            if (!needs_value(arg, i, argc, arg)) return 2;
            bind_address = argv[++i];
            bind_from_argv = true;
        } else if (std::strcmp(arg, "--root") == 0) {
            if (!needs_value(arg, i, argc, arg)) return 2;
            web.explicit_root = argv[++i];
        } else if (std::strcmp(arg, "--refresh-web") == 0) {
            web.refresh = true;
        } else if (std::strcmp(arg, "--web-ref") == 0) {
            if (!needs_value(arg, i, argc, arg)) return 2;
            web.ref = argv[++i];
        } else if (std::strcmp(arg, "--no-download") == 0) {
            web.no_download = true;
        } else if (std::strcmp(arg, "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "restreamair-server: unrecognised argument '%s'\n", arg);
            print_usage(argv[0]);
            return 2;
        }
    }

    restream_server_set_verbose(verbose);

    // Enable source auto-detect (/api/probe): libcurl for the fetch, libxml2 for
    // MPD parsing. Both are initialised once here; the handler lives in probe.c.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // The front-end: public/ if it is next to us, otherwise a cached copy, and
    // failing that a download from the (public) repository — the binary alone
    // is enough to run the panel, and nothing is written into the install
    // directory. Needs libcurl up, hence its place here.
    web.verbose = verbose;
    std::string web_root_error;
    std::string web_root = rs_webroot_resolve(web, &web_root_error, nullptr);

    restream_server_set_probe_handler(rs_probe_source);
    restream_server_set_fetch_handler(rs_fetch_url);
    restream_server_set_dash_handler(rs_dash_describe);

    restream_server_set_service_handler(service_status_json, service_action);

    restream_server_t *server = restream_server_create();
    if (!server) {
        std::fprintf(stderr, "restreamair-server: out of memory\n");
        return 1;
    }
    g_ffrun = rs_ffrun_create(pipeline_log, server);
    if (!g_ffrun) {
        std::fprintf(stderr, "restreamair-server: could not create FFmpeg supervisor\n");
        restream_server_destroy(server);
        curl_global_cleanup();
        return 1;
    }
    restream_server_set_pipeline_handler(pipeline_start, pipeline_stop,
                                         pipeline_running, pipeline_poll);
    if (!web_root.empty()) {
        restream_server_set_web_root(server, web_root.c_str());
    }
    // Honour the panel's stored settings unless the command line overrode them.
    if (!port_from_argv) {
        if (uint16_t stored = restream_server_stored_port(server)) port = stored;
    }
    if (!bind_from_argv) {
        if (const char *stored = restream_server_stored_bind(server)) bind_address = stored;
    }

    if (!restream_server_start(server, port, bind_address.c_str())) {
        std::fprintf(stderr, "restreamair-server: cannot listen on %s:%u\n", bind_address.c_str(), port);
        rs_ffrun_destroy(g_ffrun);
        g_ffrun = nullptr;
        restream_server_destroy(server);
        curl_global_cleanup();
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::printf("restreamair-server listening on http://%s:%u\n", bind_address.c_str(), port);
    if (web_root.empty()) {
        std::printf("  no web root — serving /ping only: %s.\n", web_root_error.c_str());
        std::printf("  pass --root DIR, or allow the front-end to be fetched from github.com/%s.\n",
                    rs_webroot_repo());
    } else {
        std::printf("  serving %s + the panel API: auth, provider/stream/user/key management,\n"
                    "  live monitoring, source auto-detect, script providers, and direct /\n"
                    "  HLS-proxy / DASH playback (DASH ClearKey CENC decrypted in-server).\n"
                    "  FFmpeg URL and program-pipe inputs are supervised; N_m3u8DL-RE is not implemented.\n",
                    web_root.c_str());
    }
    std::fflush(stdout);

    while (g_stop == 0) {
        restream_server_poll(server, 200);
    }

    std::printf("\nrestreamair-server stopping\n");
    restream_server_stop(server);
    rs_ffrun_destroy(g_ffrun);
    g_ffrun = nullptr;
    restream_server_set_pipeline_handler(nullptr, nullptr, nullptr, nullptr);
    restream_server_destroy(server);
    curl_global_cleanup();
    return 0;
}
