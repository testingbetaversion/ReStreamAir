cmake_minimum_required(VERSION 3.16)
project(restreamair LANGUAGES C CXX)

# This builds the C core and the Linux/Windows C++ server. The macOS Swift app
# compiles the same C sources through Package.swift instead, so the two source
# lists below and the ones in Package.swift describe the same files — a source
# added to only one of them will fail to link somewhere. The sources are listed
# explicitly (never globbed) so that stays visible.

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

# ---- restream_base: portable logic, no sockets and no third-party code -------
# Everything here is pure C11 and platform-independent, which is what lets the
# self-test binary link it without dragging in mongoose or Winsock.
add_library(restream_base STATIC
    core/src/common.c
    core/src/crypto.c
    core/src/aes.c
    core/src/url.c
    core/src/m3u8.c
    core/src/ffargs.c
    core/src/json.c
    core/src/random.c
    core/src/state.c
    core/src/auth.c
    core/src/panel.c
    core/src/sysstats.c
    core/src/cenc.c
    core/src/hls_decrypt.c
    core/src/audio_delay.c
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
    core/deps/mongoose.c
)
target_include_directories(restream_core PUBLIC core/include PRIVATE core/deps)
target_link_libraries(restream_core PUBLIC restream_base)
# Warnings apply to our source only; mongoose is vendored and compiles as-is.
set_source_files_properties(core/src/server.c PROPERTIES COMPILE_OPTIONS "${RS_WARNINGS}")
if(WIN32)
    target_compile_definitions(restream_core PRIVATE _WIN32_WINNT=0x0601)  # Windows 7+
    target_link_libraries(restream_core PUBLIC ws2_32)
endif()

# ---- restream_selftest: known-answer vectors for the ported modules ---------
add_executable(restream_selftest core/tests/selftest_main.c)
target_link_libraries(restream_selftest PRIVATE restream_base)
target_compile_options(restream_selftest PRIVATE ${RS_WARNINGS})

enable_testing()
add_test(NAME selftest COMMAND restream_selftest)

# ---- restreamair-server: the Linux/Windows panel executable ----------------
# Source auto-detect (/api/probe) needs outbound HTTPS (libcurl) and MPD parsing
# (libxml2). These are linked into the server executable ONLY — the core and the
# Swift build never see them, since the probe handler is registered at runtime
# through a function pointer (see restream_server_set_probe_handler).
find_package(CURL REQUIRED)
find_package(LibXml2 REQUIRED)
add_executable(restreamair-server
    apps/server/main.cpp
    apps/server/net.c
    apps/server/probe.c
)
target_link_libraries(restreamair-server PRIVATE restream_core CURL::libcurl LibXml2::LibXml2)
target_include_directories(restreamair-server PRIVATE apps/server)
target_compile_options(restreamair-server PRIVATE ${RS_WARNINGS})
set_source_files_properties(apps/server/probe.c apps/server/net.c PROPERTIES COMPILE_OPTIONS "${RS_WARNINGS}")
