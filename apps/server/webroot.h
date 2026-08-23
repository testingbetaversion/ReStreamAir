#ifndef RS_WEBROOT_H
#define RS_WEBROOT_H

#include <string>

// Where the panel's front-end comes from.
//
// The front-end is the handful of files in public/ — index.html, app.js,
// styles.css, the service worker, hls.js and the icons. A checkout or a release
// tarball has that directory sitting next to the binary and nothing here does
// any work. A bare executable — one copied to /usr/local/bin, an image built
// from the binary alone — fetches public/ from the public GitHub repository on
// first run and keeps it in a cache directory (~/.cache, or /tmp when home is
// not writable), so the front-end never has to be shipped alongside the
// executable or written into the install directory.

struct rs_webroot_options {
    std::string explicit_root;  // --root: used as given, never downloaded over
    std::string ref = "main";   // --web-ref: branch, tag or commit to fetch
    bool refresh = false;       // --refresh-web: re-download over a cached copy
    bool no_download = false;   // --no-download: local copies only, never fetch
    bool verbose = false;       // print each file as it arrives
};

// Returns an absolute path to a directory holding the panel's static files, or
// an empty string with the reason in *err when there is none and none could be
// fetched. `*downloaded` (optional) says whether this call went to the network.
std::string rs_webroot_resolve(const rs_webroot_options &options, std::string *err,
                               bool *downloaded);

// The repository the front-end is fetched from, for startup messages.
const char *rs_webroot_repo(void);

#endif  // RS_WEBROOT_H
