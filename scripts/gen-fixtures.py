#!/usr/bin/env python3
"""Emits core/tests/fixtures.h from the tables below.

The self-test's inputs live here rather than inline in the C, so that a case can
be added or an edge widened in one readable place instead of hand-escaped into a
header. The header is generated and committed; rerun this after editing:

    python3 scripts/gen-fixtures.py
"""

import os

# --- playlists --------------------------------------------------------------
# Each entry is (name, base URL, text). Between them these cover every branch of
# M3U8Rewriter: key/map URI attributes, relative and rooted and absolute segment
# URIs, dot segments, a URI Foundation refuses to parse (the space), comment and
# blank lines, a playlist with no media sequence, and a master whose last line is
# a STREAM-INF with no URI after it.

PLAYLISTS = [
    (
        "media",
        "https://origin.example.com/live/hls/media.m3u8",
        "#EXTM3U\n"
        "#EXT-X-VERSION:6\n"
        "#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:42\n"
        '#EXT-X-KEY:METHOD=AES-128,URI="key.bin",IV=0x0123456789abcdef0123456789abcdef\n'
        '#EXT-X-MAP:URI="init.mp4"\n'
        "\n"
        "#EXTINF:4.000,\n"
        "seg1.m4s\n"
        "#EXTINF:4.000,\n"
        "./sub/seg2.m4s\n"
        "#EXTINF:4.000,\n"
        "../up/seg3.m4s\n"
        "#EXTINF:4.000,\n"
        "/rooted/seg4.m4s\n"
        "#EXTINF:4.000,\n"
        "https://cdn.example.com/abs/seg5.m4s\n"
        "#EXTINF:4.000,\n"
        "  spaced/seg6.m4s  \n"
        "#EXTINF:4.000,\n"
        "seg 7.m4s\n"
        "#EXT-X-ENDLIST\n",
    ),
    (
        "media_plain",
        "https://origin.example.com/a/b/c.m3u8",
        "#EXTM3U\n"
        "#EXTINF:2.000,\n"
        "s0.ts\n"
        '#EXT-X-KEY:METHOD=AES-128,URI="https://keys.example.com/k?id=1"\n'
        "#EXTINF:2.000,\n"
        "s1.ts\n",
    ),
    (
        "master",
        "https://origin.example.com/live/master.m3u8",
        "#EXTM3U\n"
        '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="aac",NAME="English",LANGUAGE="en",DEFAULT=YES,URI="audio/en.m3u8"\n'
        '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="aac",NAME="Commentary, extended",URI="audio/cm.m3u8"\n'
        '#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="subs",NAME="English",URI="subs/en.m3u8"\n'
        '#EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=1280x720,CODECS="avc1.4d401f,mp4a.40.2",AUDIO="aac"\n'
        "720p/index.m3u8\n"
        '#EXT-X-STREAM-INF:BANDWIDTH=2560000,RESOLUTION=1920x1080,CODECS="avc1.640028,mp4a.40.2",AUDIO="aac"\n'
        "https://cdn.example.com/1080p/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
        "audio-only.m3u8\n",
    ),
    (
        "master_tail",
        "https://origin.example.com/live/master.m3u8",
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100",
    ),
]

# --- ffmpeg command lines ---------------------------------------------------
# One case per input/output mode combination, plus the header/key/proxy/params
# variations that change the input options.

FFARGS = [
    {
        "name": "ts-hls",
        "source_url": "https://origin.example.com/live/stream.m3u8",
        "kind": "m3u8",
        "input_mode": "ffmpegTsHls",
        "output_mode": "hls",
        "output_target": "",
        "decryption_keys": "",
        "headers": "",
        "proxy": "",
        "segment_url_params": "",
        "representations": [],
    },
    {
        "name": "fmp4-hls-with-headers-and-key",
        "source_url": "https://origin.example.com/live/stream.mpd",
        "kind": "mpd",
        "input_mode": "ffmpegFmp4Hls",
        "output_mode": "hls",
        "output_target": "",
        "decryption_keys": "  \nabcdef0123456789abcdef0123456789:00112233445566778899aabbccddeeff\n",
        "headers": "User-Agent: ReStreamAir/1.0\n\n  Referer: https://example.com  \n",
        "proxy": "http://127.0.0.1:8888",
        "segment_url_params": " ?token=abc&region=eu ",
        "representations": [],
    },
    {
        "name": "multi-ts-hls",
        "source_url": "https://origin.example.com/live/stream.mpd",
        "kind": "mpd",
        "input_mode": "ffmpegMultiTsHls",
        "output_mode": "hls",
        "output_target": "",
        "decryption_keys": "00112233445566778899aabbccddeeff",
        "headers": "",
        "proxy": "",
        "segment_url_params": "",
        "representations": [("v1", "video"), ("v2", "video"), ("a1", "audio")],
    },
    {
        "name": "multi-ts-hls-single-video",
        "source_url": "https://origin.example.com/live/stream.mpd",
        "kind": "mpd",
        "input_mode": "ffmpegMultiTsHls",
        "output_mode": "hls",
        "output_target": "",
        "decryption_keys": "",
        "headers": "",
        "proxy": "",
        "segment_url_params": "",
        "representations": [("v1", "video"), ("a1", "audio")],
    },
    {
        "name": "resident-srt-server-bare-port",
        "source_url": "https://origin.example.com/live/stream.mpd",
        "kind": "mpd",
        "input_mode": "ffmpegResident",
        "output_mode": "srtServer",
        "output_target": "9100",
        "decryption_keys": "",
        "headers": "",
        "proxy": "",
        "segment_url_params": "",
        "representations": [],
    },
    {
        "name": "resident-srt-server-empty-port",
        "source_url": "https://origin.example.com/live/stream.mpd",
        "kind": "mpd",
        "input_mode": "ffmpegResident",
        "output_mode": "srtServer",
        "output_target": "",
        "decryption_keys": "",
        "headers": "",
        "proxy": "",
        "segment_url_params": "",
        "representations": [],
    },
    {
        "name": "resident-udp-push",
        "source_url": "rtmp://origin.example.com/live/stream",
        "kind": "mpd",
        "input_mode": "ffmpegResident",
        "output_mode": "udpSrt",
        "output_target": "udp://239.0.0.1:1234?pkt_size=1316",
        "decryption_keys": "",
        "headers": "",
        "proxy": "",
        "segment_url_params": "ignored=1",
        "representations": [],
    },
    {
        "name": "resident-custom-quoted",
        "source_url": "https://origin.example.com/live/stream.m3u8?a=1",
        "kind": "m3u8",
        "input_mode": "ffmpegResident",
        "output_mode": "custom",
        "output_target": "-f flv 'rtmp://a.example.com/app/key with spaces' -metadata title=\"My Stream\"",
        "decryption_keys": ":deadbeefdeadbeefdeadbeefdeadbeef",
        "headers": "",
        "proxy": "",
        "segment_url_params": "&more=2",
        "representations": [],
    },
]

TEMP_DIR = "/tmp/restreamair-fixture"
OUTPUT_PLAYLIST = "/tmp/restreamair-fixture/live.m3u8"
PLAYLIST_SEGMENTS = 6
SEGMENT_SECONDS = 4
FFMPEG_PATH = "/usr/local/bin/ffmpeg"

BANNER = (
    "// Generated by scripts/gen-fixtures.py — do not edit by hand.\n"
    "// Input for the C self-test, kept in one table so a case can be added without\n"
    "// hand-escaping it into a header.\n"
)


def c_string(s):
    # Iterates UTF-8 bytes, not code points: a C string literal is a byte
    # sequence, and emitting "\u00e9" as one \351 byte would hand the C code
    # Latin-1 where the fixture text is UTF-8.
    out = ['"']
    for byte in s.encode("utf-8"):
        ch = chr(byte)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append('\\n"\n    "')
        elif ch == "\t":
            out.append("\\t")
        elif byte < 0x20 or byte > 0x7E:
            # Octal, not hex: a hex escape has no length limit, so "\\x1f"
            # followed by a digit would be read as one enormous character.
            out.append("\\%03o" % byte)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)



def write_c(path):
    lines = [BANNER, "", "#ifndef RS_FIXTURES_H", "#define RS_FIXTURES_H", "",
             '#include "rs_ffargs.h"', ""]

    lines.append("typedef struct {")
    lines.append("    const char *name;")
    lines.append("    const char *base;")
    lines.append("    const char *text;")
    lines.append("} rs_playlist_fixture;")
    lines.append("")
    lines.append("static const rs_playlist_fixture rs_playlist_fixtures[] = {")
    for name, base, text in PLAYLISTS:
        lines.append("    {%s, %s," % (c_string(name), c_string(base)))
        lines.append("     " + c_string(text) + "},")
    lines.append("};")
    lines.append("")

    for index, case in enumerate(FFARGS):
        ids = [rep[0] for rep in case["representations"]]
        types = [rep[1] for rep in case["representations"]]
        if ids:
            lines.append("static const char *const rs_ff%d_ids[] = {%s};"
                         % (index, ", ".join(c_string(i) for i in ids)))
            lines.append("static const char *const rs_ff%d_types[] = {%s};"
                         % (index, ", ".join(c_string(t) for t in types)))
    lines.append("")

    lines.append("static const char *const rs_ffargs_case_names[] = {")
    for case in FFARGS:
        lines.append("    %s," % c_string(case["name"]))
    lines.append("};")
    lines.append("")

    lines.append("static const rs_ffargs_inputs rs_ffargs_cases[] = {")
    for index, case in enumerate(FFARGS):
        ids = "rs_ff%d_ids" % index if case["representations"] else "NULL"
        types_ids = "rs_ff%d_ids" % index if case["representations"] else "NULL"
        types = "rs_ff%d_types" % index if case["representations"] else "NULL"
        count = len(case["representations"])
        lines.append("    {")
        lines.append("        %s, %s, %s," % (c_string(case["source_url"]), c_string(case["kind"]),
                                              c_string(case["input_mode"])))
        lines.append("        %s, %s," % (c_string(case["output_mode"]), c_string(case["output_target"])))
        lines.append("        %s," % c_string(case["decryption_keys"]))
        lines.append("        %s," % c_string(case["headers"]))
        lines.append("        %s, %s," % (c_string(case["proxy"]), c_string(case["segment_url_params"])))
        lines.append("        %s, %d, %s, %s, %d," % (ids, count, types_ids, types, count))
        lines.append("        %s, %s, %d, %d," % (c_string(TEMP_DIR), c_string(OUTPUT_PLAYLIST),
                                                  PLAYLIST_SEGMENTS, SEGMENT_SECONDS))
        lines.append("    },")
    lines.append("};")
    lines.append("")
    lines.append("#endif  // RS_FIXTURES_H")
    lines.append("")

    with open(path, "w") as handle:
        handle.write("\n".join(lines))


root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
write_c(os.path.join(root, "core", "tests", "fixtures.h"))
print("wrote core/tests/fixtures.h")
