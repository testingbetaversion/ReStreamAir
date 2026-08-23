cmake_minimum_required(VERSION 3.16)
project(restreamair LANGUAGES C CXX)

# This is the whole build: the C core, the self-test, and the panel executable,
# on macOS, Linux and Windows alike. There is no second build system and no
# other language in the tree. Sources are listed explicitly (never globbed), so
# adding a file is a visible change rather than something a wildcard picks up.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(RS_WARNINGS /W4)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(RS_WARNINGS -Wall -Wextra -Wconversion -Wno-sign-conversion)
endif()

# -DRS_SANITIZE=ON builds the server with AddressSanitizer + UndefinedBehavior-
# Sanitizer. It is the tool for a crash that only shows up after hours of real
# traffic: instead of dying somewhere unrelated later, the process aborts at the
# instruction that corrupted memory and prints the allocation and the stack.
# Roughly 2x slower and ~3x the RSS, so it is a diagnostic build, not a
# production one. Run it with:
#   ASAN_OPTIONS=abort_on_error=1:disable_coredump=0 ./restreamair-server ...
option(RS_SANITIZE "Build with AddressSanitizer + UBSan (diagnostic builds)" OFF)
if(RS_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "restreamair: sanitizers enabled — diagnostic build, do not ship")
endif()

# ---- restream_base: portable logic, no sockets and no third-party code -------
# Everything here is pure C11 and platform-independent, which is what lets the
# self-test binary link it without dragging in mongoose or Winsock.
add_library(restream_base STATIC
    core/src/common.c
    core/src/crypto.c
    core/src/aes.c
    core/src/url.c
    core/src/netmatch.c
    core/src/m3u8.c
    core/src/ffargs.c
    core/src/json.c
    core/src/random.c
    core/src/state.c
    core/src/auth.c
    core/src/panel.c
    core/src/sysstats.c
    core/src/cenc.c
    core/src/mpegts.c
    core/src/ttml.c
    core/src/hls_decrypt.c
    core/src/audio_delay.c
    core/src/live_window.c
    core/src/live_backoff.c
    core/src/script.c
    core/src/cdm.c
    core/src/metrics.c
    core/src/logo.c
    core/src/nm3u8dlre.c
    core/deps/cJSON.c
)
target_include_directories(restream_base PUBLIC core/include)
target_compile_options(restream_base PRIVATE ${RS_WARNINGS})
if(WIN32)
    # BCryptGenRandom, for rs_random_bytes.
    target_link_libraries(restream_base PUBLIC bcrypt)
endif()

# ---- restream_core: the HTTP server, built on mongoose ----------------------
add_library(restream_core STATIC
    core/src/server.c
    core/src/live.c
    core/deps/mongoose.c
)
target_include_directories(restream_core PUBLIC core/include PRIVATE core/deps)
target_link_libraries(restream_core PUBLIC restream_base)
# The live DASH engine runs a director plus one worker per representation, and
# fans segment downloads out across several more threads; glibc needs -pthread
# spelled out.
find_package(Threads REQUIRED)
target_link_libraries(restream_core PUBLIC Threads::Threads)
# Warnings apply to our source only; mongoose is vendored and compiles as-is.
set_source_files_properties(core/src/server.c core/src/live.c PROPERTIES COMPILE_OPTIONS "${RS_WARNINGS}")
if(WIN32)
    target_compile_definitions(restream_core PRIVATE _WIN32_WINNT=0x0601)  # Windows 7+
    target_link_libraries(restream_core PUBLIC ws2_32)
endif()

# ---- restream_selftest: known-answer vectors for the core modules ----------
add_executable(restream_selftest core/tests/selftest_main.c)
target_link_libraries(restream_selftest PRIVATE restream_base)
target_compile_options(restream_selftest PRIVATE ${RS_WARNINGS})

enable_testing()
add_test(NAME selftest COMMAND restream_selftest)

# ---- restreamair-server: the Linux/Windows panel executable ----------------
# Source auto-detect (/api/probe) needs outbound HTTPS (libcurl) and MPD parsing
# (libxml2). These are linked into the server executable ONLY — the core never
# sees them, since the probe handler is registered at runtime through a function
# pointer (see restream_server_set_probe_handler). That is what lets the core
# and the self-test build on a platform where those two dev packages are
# awkward to get.
#
# -DRS_BUILD_SERVER=OFF builds the libraries and the self-test without them, so
# a platform where those two dev packages are awkward to get (Windows, without
# vcpkg) can still compile and verify the core on every CI run.
option(RS_BUILD_SERVER "Build restreamair-server (needs libcurl + libxml2)" ON)
if(NOT RS_BUILD_SERVER)
    message(STATUS "restreamair: RS_BUILD_SERVER=OFF — libraries and self-test only")
    return()
endif()

find_package(CURL REQUIRED)
find_package(LibXml2 REQUIRED)
add_executable(restreamair-server
    apps/server/main.cpp
    apps/server/webroot.cpp
    apps/server/net.c
    apps/server/probe.c
    apps/server/rs_dash.c
    apps/server/ffrun.c
    apps/server/service.c
)
target_link_libraries(restreamair-server PRIVATE restream_core CURL::libcurl LibXml2::LibXml2)
target_include_directories(restreamair-server PRIVATE apps/server)
target_compile_options(restreamair-server PRIVATE ${RS_WARNINGS})
set_source_files_properties(apps/server/webroot.cpp apps/server/probe.c apps/server/net.c apps/server/rs_dash.c apps/server/ffrun.c apps/server/service.c PROPERTIES COMPILE_OPTIONS "${RS_WARNINGS}")
