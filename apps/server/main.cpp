// The Linux/Windows panel executable. It is deliberately thin: everything it
// does lives in the C core, and this file only parses argv, runs the poll loop
// and shuts down cleanly. The panel API isn't implemented in C yet (the core
// answers /ping and serves public/ statically); the Swift binary is still the
// one that runs the real panel.

#include <sys/stat.h>

#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <direct.h>
#define rs_realpath(rel, abs) _fullpath((abs), (rel), _MAX_PATH)
#define RS_PATH_MAX _MAX_PATH
#else
#include <limits.h>
#define rs_realpath(rel, abs) realpath((rel), (abs))
#define RS_PATH_MAX PATH_MAX
#endif

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
                 "  serve             optional; accepted so the argv matches the Swift binary\n"
                 "  -p, --port N      port to listen on (default 8080)\n"
                 "  -b, --bind ADDR   address to bind (default 0.0.0.0)\n"
                 "  --root DIR        serve the panel's static files from DIR (auto-detects public/)\n"
                 "  --verbose         full mongoose trace logging (default: errors only)\n"
                 "  -h, --help        this message\n",
                 program);
}

bool is_dir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

// Looks for the panel's public/ directory next to the current directory or one
// level up (the binary usually runs from build/, with public/ a sibling of it).
std::string autodetect_web_root() {
    for (const char *candidate : {"public", "../public", "./public"}) {
        if (is_dir(candidate)) return candidate;
    }
    return "";
}

// mongoose's mg_http_serve_dir rejects any root containing "..", so a relative
// path like "../public" has to be made absolute before it's handed over.
std::string absolute_path(const std::string &path) {
    char buffer[RS_PATH_MAX];
    if (rs_realpath(path.c_str(), buffer)) return buffer;
    return path;  // best effort — let the server report the failure
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

int main(int argc, char **argv) {
    unsigned short port = 8080;
    std::string bind_address = "0.0.0.0";
    std::string web_root;
    bool web_root_set = false;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        // Accept a leading `serve` so muscle memory from the Swift binary
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
        } else if (std::strcmp(arg, "-b") == 0 || std::strcmp(arg, "--bind") == 0) {
            if (!needs_value(arg, i, argc, arg)) return 2;
            bind_address = argv[++i];
        } else if (std::strcmp(arg, "--root") == 0) {
            if (!needs_value(arg, i, argc, arg)) return 2;
            web_root = argv[++i];
            web_root_set = true;
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

    if (!web_root_set) web_root = autodetect_web_root();
    if (!web_root.empty()) web_root = absolute_path(web_root);

    restream_server_set_verbose(verbose);

    restream_server_t *server = restream_server_create();
    if (!server) {
        std::fprintf(stderr, "restreamair-server: out of memory\n");
        return 1;
    }
    if (!web_root.empty()) {
        restream_server_set_web_root(server, web_root.c_str());
    }
    if (!restream_server_start(server, port, bind_address.c_str())) {
        std::fprintf(stderr, "restreamair-server: cannot listen on %s:%u\n", bind_address.c_str(), port);
        restream_server_destroy(server);
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::printf("restreamair-server listening on http://%s:%u\n", bind_address.c_str(), port);
    if (web_root.empty()) {
        std::printf("  no web root — serving /ping only. Pass --root public/ to serve the panel's files.\n");
    } else {
        std::printf("  serving %s + the panel API: auth, provider/stream/user/key management,\n"
                    "  live monitoring, and direct-source playback. Proxied/DASH streaming isn't in C yet.\n",
                    web_root.c_str());
    }
    std::fflush(stdout);

    while (g_stop == 0) {
        restream_server_poll(server, 200);
    }

    std::printf("\nrestreamair-server stopping\n");
    restream_server_stop(server);
    restream_server_destroy(server);
    return 0;
}
