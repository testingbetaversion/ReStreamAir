// Finding — and, on a first run, fetching — the panel's static files.
//
// See webroot.h for the shape of the problem. The order is: an explicit --root,
// then public/ next to the working directory, then a freshly downloaded copy
// with the previous cache as an offline fallback. Nothing is ever written next
// to the binary: the cache lives in the user's cache directory, falling back
// to a per-uid directory under /tmp when that is not writable (a systemd unit
// with ProtectHome=true, a container running as a user with no home).

#include "webroot.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define rs_mkdir_one(p) _mkdir(p)
#define rs_realpath(rel, abs) _fullpath((abs), (rel), _MAX_PATH)
#define RS_PATH_MAX _MAX_PATH
#else
#include <limits.h>
#include <unistd.h>
#define rs_mkdir_one(p) mkdir((p), 0700)
#define rs_realpath(rel, abs) realpath((rel), (abs))
#define RS_PATH_MAX PATH_MAX
#endif

#include "net.h"
#include "rs_json.h"

namespace {

const char *const kRepo = "testingbetaversion/ReStreamAir";
// Written last, once every file is on disk: its presence is what makes a cache
// directory count as complete, so an interrupted download is retried rather
// than served half-empty.
const char *const kStamp = ".restreamair-webroot.json";

constexpr size_t kMaxFileBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxTotalBytes = 96u * 1024u * 1024u;
constexpr size_t kMaxFiles = 256;
constexpr long kTimeoutMs = 30000;

// The tree as `git ls-files public` has it. Only used when the listing API
// cannot be reached — it is rate-limited per IP and lives on a different host
// than the files themselves, so a first run should not fail on it alone.
const char *const kFallbackFiles[] = {
    "index.html", "app.js", "styles.css", "sw.js", "manifest.webmanifest",
    "hls.min.js", "icons/icon-192.png", "icons/icon-512.png",
    "icons/icon-maskable-512.png",
};

bool is_dir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

bool is_file(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && !(st.st_mode & S_IFDIR);
}

std::string join(const std::string &dir, const std::string &leaf) {
    if (dir.empty()) return leaf;
    char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') return dir + leaf;
    return dir + "/" + leaf;
}

std::string parent_of(const std::string &path) {
    size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

// mongoose's mg_http_serve_dir rejects any root containing "..", so a relative
// path like "../public" has to be made absolute before it's handed over.
std::string absolute_path(const std::string &path) {
    char buffer[RS_PATH_MAX];
    if (rs_realpath(path.c_str(), buffer)) return buffer;
    return path;  // best effort — let the server report the failure
}

// mkdir -p. Intermediate failures are ignored (a Windows drive prefix, a
// directory that already exists); what matters is the directory at the end.
bool mkdir_p(const std::string &path) {
    if (path.empty()) return true;
    for (size_t i = 1; i <= path.size(); i++) {
        if (i != path.size() && path[i] != '/' && path[i] != '\\') continue;
        std::string prefix = path.substr(0, i);
        (void)rs_mkdir_one(prefix.c_str());
    }
    return is_dir(path);
}

// A predictable path under a shared /tmp is somebody else's to create first,
// and whatever lands in it is served to browsers as the panel's own
// JavaScript. A directory we did not create, or one anyone else can write to,
// is not used.
bool dir_is_ours(const std::string &path) {
#ifdef _WIN32
    (void)path;
    return true;  // per-user LOCALAPPDATA; there is no shared-tmp candidate
#else
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return true;  // not there yet — we make it
    if (!S_ISDIR(st.st_mode)) return false;          // a file, or a symlink to one
    if (st.st_uid != geteuid()) return false;
    if (st.st_mode & (S_IWGRP | S_IWOTH)) return false;
    return true;
#endif
}

// public/, ../public/, ./public/ — the layout of a checkout and of the release
// tarballs, where the binary runs from build/ with public/ a sibling.
std::string autodetect_local() {
    for (const char *candidate : {"public", "../public", "./public"}) {
        if (is_dir(candidate) && is_file(join(candidate, "index.html"))) return candidate;
    }
    return "";
}

// Cache locations, best first. RESTREAMAIR_WEB_CACHE overrides the lot.
std::vector<std::string> cache_candidates() {
    std::vector<std::string> out;
    const char *forced = std::getenv("RESTREAMAIR_WEB_CACHE");
    if (forced && forced[0]) out.push_back(forced);
#ifdef _WIN32
    const char *appdata = std::getenv("LOCALAPPDATA");
    if (appdata && appdata[0]) out.push_back(std::string(appdata) + "\\ReStreamAir\\public");
#else
    const char *home = std::getenv("HOME");
#ifdef __APPLE__
    if (home && home[0]) out.push_back(std::string(home) + "/Library/Caches/ReStreamAir/public");
#else
    const char *xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0]) out.push_back(std::string(xdg) + "/restreamair/public");
    else if (home && home[0]) out.push_back(std::string(home) + "/.cache/restreamair/public");
#endif
    const char *tmp = std::getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    std::string tmpdir = tmp;
    while (tmpdir.size() > 1 && tmpdir[tmpdir.size() - 1] == '/') tmpdir.erase(tmpdir.size() - 1);
    char uid[32];
    std::snprintf(uid, sizeof(uid), "%lu", (unsigned long)geteuid());
    out.push_back(tmpdir + "/restreamair-public-" + uid);
#endif
    return out;
}

bool cache_is_complete(const std::string &dir) {
    return is_file(join(dir, "index.html")) && is_file(join(dir, kStamp));
}

// A path out of the repository listing is remote input: it decides what gets
// written where. Only plain, relative, single-level-at-a-time names pass.
bool safe_relative(const std::string &rel) {
    if (rel.empty() || rel.size() > 200) return false;
    if (rel[0] == '/' || rel[0] == '.') return false;
    if (rel.find('\\') != std::string::npos) return false;
    if (rel.find("//") != std::string::npos) return false;
    if (rel[rel.size() - 1] == '/') return false;
    size_t start = 0;
    while (start <= rel.size()) {
        size_t end = rel.find('/', start);
        std::string part = rel.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part[0] == '.') return false;  // "", ".", "..", dotfiles
        for (size_t i = 0; i < part.size(); i++) {
            unsigned char c = (unsigned char)part[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '-' || c == '_';
            if (!ok) return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

// A git ref goes straight into a URL; keep it to what a ref may actually be.
bool safe_ref(const std::string &ref) {
    if (ref.empty() || ref.size() > 120) return false;
    if (ref.find("..") != std::string::npos) return false;
    for (size_t i = 0; i < ref.size(); i++) {
        unsigned char c = (unsigned char)ref[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_' || c == '/';
        if (!ok) return false;
    }
    return ref[0] != '/' && ref[ref.size() - 1] != '/';
}

bool fetch(const std::string &url, const char *accept, size_t limit,
           std::string &body, std::string &err) {
    char *out = nullptr;
    size_t len = 0;
    long status = 0;
    char errbuf[512] = {0};
    std::string headers = accept ? std::string("Accept: ") + accept : std::string();
    int rc = rs_fetch_url(url.c_str(), nullptr, headers.empty() ? nullptr : headers.c_str(),
                          nullptr, nullptr, nullptr, 0, 0, &out, &len, &status, nullptr, nullptr,
                          nullptr, errbuf, sizeof(errbuf), kTimeoutMs, nullptr, nullptr);
    if (rc != 0) {
        err = errbuf[0] ? errbuf : "request failed";
        std::free(out);
        return false;
    }
    if (len > limit) {
        err = "response is larger than the " + std::to_string(limit / (1024 * 1024)) + " MB limit";
        std::free(out);
        return false;
    }
    body.assign(out, len);
    std::free(out);
    return true;
}

// Asks GitHub what is in public/ at `ref`. One request; the files themselves
// come from raw.githubusercontent.com, which is not API-rate-limited.
bool list_remote_files(const std::string &ref, std::vector<std::string> &files,
                       std::string &commit, std::string &err) {
    std::string url = std::string("https://api.github.com/repos/") + kRepo +
                      "/git/trees/" + ref + "?recursive=1";
    std::string body;
    if (!fetch(url, "application/vnd.github+json", kMaxFileBytes, body, err)) return false;

    rs_json *root = rs_json_parse(body.data(), body.size());
    if (!root) {
        err = "the repository listing was not readable JSON";
        return false;
    }
    commit = rs_json_as_str(rs_json_obj_get(root, "sha"), "");
    bool truncated = rs_json_as_bool(rs_json_obj_get(root, "truncated"), false);
    const rs_json *tree = rs_json_obj_get(root, "tree");
    size_t count = rs_json_arr_len(tree);
    for (size_t i = 0; i < count && files.size() < kMaxFiles; i++) {
        const rs_json *entry = rs_json_arr_at(tree, i);
        if (std::strcmp(rs_json_as_str(rs_json_obj_get(entry, "type"), ""), "blob") != 0) continue;
        const char *path = rs_json_as_str(rs_json_obj_get(entry, "path"), "");
        if (std::strncmp(path, "public/", 7) != 0) continue;
        files.push_back(path + 7);
    }
    rs_json_free(root);

    if (truncated) {
        err = "the repository listing came back truncated";
        files.clear();
        return false;
    }
    if (files.empty()) {
        err = "the repository listing has no public/ files";
        return false;
    }
    return true;
}

bool write_file(const std::string &path, const std::string &data, std::string &err) {
    // Write beside the target and rename, so a reader never sees a half file
    // and a crash mid-download leaves the previous copy intact.
    std::string temp = path + ".part";
    std::remove(temp.c_str());
    FILE *f = std::fopen(temp.c_str(), "wb");
    if (!f) {
        err = std::string("cannot write ") + temp + ": " + std::strerror(errno);
        return false;
    }
    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        err = std::string("cannot write ") + temp + ": " + std::strerror(errno);
        std::remove(temp.c_str());
        return false;
    }
    std::remove(path.c_str());  // rename() won't replace on Windows
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        err = std::string("cannot place ") + path + ": " + std::strerror(errno);
        std::remove(temp.c_str());
        return false;
    }
    return true;
}

// Undoes the mkdir -p of a failed attempt, so a download that never completed
// doesn't leave empty directories behind in a cache root. rmdir only removes
// empty directories, which is exactly the guard wanted here.
void remove_if_empty(const std::string &dir, int levels) {
    std::string path = dir;
    for (int i = 0; i < levels && !path.empty(); i++) {
#ifdef _WIN32
        if (_rmdir(path.c_str()) != 0) return;
#else
        if (rmdir(path.c_str()) != 0) return;
#endif
        path = parent_of(path);
    }
}

bool download_into(const std::string &dir, const rs_webroot_options &options, std::string &err) {
    std::vector<std::string> files;
    std::string commit, listing_err;
    bool listed = list_remote_files(options.ref, files, commit, listing_err);
    if (!listed) {
        files.assign(std::begin(kFallbackFiles), std::end(kFallbackFiles));
        if (options.verbose) {
            std::printf("  listing unavailable (%s) — using the built-in file list\n",
                        listing_err.c_str());
        }
    }
    if (!mkdir_p(dir)) {
        err = "cannot create " + dir + ": " + std::strerror(errno);
        return false;
    }
    // Drop the stamp first: until every file is back, this cache is incomplete.
    std::remove(join(dir, kStamp).c_str());

    size_t total = 0;
    // Pin all raw-file reads to the SHA returned with the listing. Otherwise a
    // push to a moving branch midway through the loop could create a mixture of
    // files from two commits.
    const std::string source_ref = listed && !commit.empty() ? commit : options.ref;
    for (size_t i = 0; i < files.size(); i++) {
        const std::string &rel = files[i];
        if (!safe_relative(rel)) {
            err = "refusing an unexpected path from the repository listing: " + rel;
            return false;
        }
        std::string url = std::string("https://raw.githubusercontent.com/") + kRepo + "/" +
                          source_ref + "/public/" + rel;
        std::string body;
        if (!fetch(url, nullptr, kMaxFileBytes, body, err)) {
            err = "public/" + rel + ": " + err;
            return false;
        }
        total += body.size();
        if (total > kMaxTotalBytes) {
            err = "the front-end download went past the size limit";
            return false;
        }
        std::string dest = join(dir, rel);
        if (!mkdir_p(parent_of(dest))) {
            err = "cannot create " + parent_of(dest) + ": " + std::strerror(errno);
            return false;
        }
        if (!write_file(dest, body, err)) return false;
        if (options.verbose) {
            std::printf("  public/%s (%zu bytes)\n", rel.c_str(), body.size());
        }
    }
    if (!is_file(join(dir, "index.html"))) {
        err = "the downloaded copy has no index.html";
        return false;
    }

    char stamp[512];
    std::snprintf(stamp, sizeof(stamp),
                  "{\"repo\":\"%s\",\"ref\":\"%s\",\"commit\":\"%s\",\"files\":%zu,"
                  "\"bytes\":%zu,\"fetched\":%lld,\"listed\":%s}\n",
                  kRepo, options.ref.c_str(), commit.c_str(), files.size(), total,
                  (long long)std::time(nullptr), listed ? "true" : "false");
    if (!write_file(join(dir, kStamp), stamp, err)) return false;
    return true;
}

// Download into a sibling directory, then swap it into place. Updating the
// live cache file-by-file could leave old HTML loading new JavaScript (or the
// reverse) when the network drops halfway through a refresh. The old cache is
// kept until the complete replacement, including its stamp, is ready.
bool refresh_cache(const std::string &dir, const rs_webroot_options &options, std::string &err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string stage = dir + ".refresh";
    std::string backup = dir + ".previous";

    if (!dir_is_ours(stage) || !dir_is_ours(backup)) {
        err = "a refresh directory exists but is not ours to use";
        return false;
    }
    fs::remove_all(stage, ec);
    if (!ec && fs::exists(stage, ec)) {
        err = "cannot clear the refresh directory";
        return false;
    }
    if (ec) {
        err = "cannot clear the refresh directory";
        return false;
    }
    if (!download_into(stage, options, err)) {
        fs::remove_all(stage, ec);
        return false;
    }

    fs::remove_all(backup, ec);
    if (!ec && fs::exists(backup, ec)) {
        err = "cannot clear the previous-cache directory";
        fs::remove_all(stage, ec);
        return false;
    }
    if (ec) {
        err = "cannot clear the previous-cache directory";
        fs::remove_all(stage, ec);
        return false;
    }
    bool had_old = is_dir(dir);
    if (had_old) {
        fs::rename(dir, backup, ec);
        if (ec) {
            err = "cannot preserve the previous cache: " + ec.message();
            fs::remove_all(stage, ec);
            return false;
        }
    }

    fs::rename(stage, dir, ec);
    if (ec) {
        std::string message = ec.message();
        if (had_old) {
            std::error_code restore_error;
            fs::rename(backup, dir, restore_error);
        }
        fs::remove_all(stage, ec);
        err = "cannot activate the refreshed cache: " + message;
        return false;
    }
    fs::remove_all(backup, ec);
    return true;
}

}  // namespace

const char *rs_webroot_repo(void) { return kRepo; }

std::string rs_webroot_resolve(const rs_webroot_options &options, std::string *err,
                               bool *downloaded) {
    std::string sink;
    if (!err) err = &sink;
    if (downloaded) *downloaded = false;

    if (!options.explicit_root.empty()) {
        if (!is_dir(options.explicit_root)) {
            *err = "--root " + options.explicit_root + " is not a directory";
            return "";
        }
        return absolute_path(options.explicit_root);
    }

    // --refresh-web means "fetch a fresh copy into the cache". It deliberately
    // skips the local tree instead of writing over a checkout's public/.
    if (!options.refresh) {
        std::string local = autodetect_local();
        if (!local.empty()) return absolute_path(local);
    }

    std::vector<std::string> candidates = cache_candidates();

    // With downloads disabled, a complete cache is still usable. Normally a
    // missing local public/ means checking Git on every launch, so a new binary
    // does not silently keep serving front-end files fetched by an older one.
    if (options.no_download) {
        for (size_t i = 0; i < candidates.size(); i++) {
            if (dir_is_ours(candidates[i]) && cache_is_complete(candidates[i])) {
                return absolute_path(candidates[i]);
            }
        }
        *err = "no public/ beside the binary and no cached copy (--no-download)";
        return "";
    }
    if (!safe_ref(options.ref)) {
        *err = "--web-ref '" + options.ref + "' is not a usable branch, tag or commit";
        return "";
    }

    std::string last = "no cache directory was usable";
    for (size_t i = 0; i < candidates.size(); i++) {
        if (!dir_is_ours(candidates[i])) {
            last = candidates[i] + " exists but is not ours to use";
            continue;
        }
        std::printf("fetching the panel's front-end from github.com/%s (%s) into %s\n",
                    kRepo, options.ref.c_str(), candidates[i].c_str());
        std::fflush(stdout);
        bool had_cache = cache_is_complete(candidates[i]);
        std::string attempt;
        if (refresh_cache(candidates[i], options, attempt)) {
            if (downloaded) *downloaded = true;
            return absolute_path(candidates[i]);
        }
        std::printf("  %s\n", attempt.c_str());
        std::fflush(stdout);
        if (had_cache) {
            std::printf("  using the previous cached front-end\n");
            std::fflush(stdout);
            return absolute_path(candidates[i]);
        }
        if (!is_dir(candidates[i])) remove_if_empty(candidates[i], 3);
        last = attempt;
    }
    *err = "could not fetch the panel's front-end: " + last;
    return "";
}
