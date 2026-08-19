// Known-answer self-test for the C core. Two kinds of check live here:
//
//   * Published NIST/FIPS/RFC vectors for the crypto, so a build proves its
//     SHA-256, PBKDF2 and AES produce the right bytes on that platform rather
//     than merely compiling. These mirror CryptoSelfTest in AES.swift and add
//     the multi-block and streaming cases the Swift version does not cover.
//   * Goldens captured from the Swift implementations (goldens.h) for the URL
//     resolver, playlist rewriter and ffmpeg argument builder, so the C port is
//     measured against the behaviour the app ships today.
//
// No test framework: a failure prints what broke and exits non-zero, which is
// all ctest and CI need.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs_aes.h"
#include "rs_auth.h"
#include "rs_cenc.h"
#include "rs_crypto.h"
#include "rs_ffargs.h"
#include "rs_json.h"
#include "rs_live.h"
#include "rs_m3u8.h"
#include "rs_mpegts.h"
#include "rs_netmatch.h"
#include "rs_panel.h"
#include "rs_state.h"
#include "rs_ttml.h"
#include "rs_url.h"

#include "fixtures.h"
#include "goldens.h"

static int checks_run = 0;
static int failures = 0;

static void fail(const char *name, const char *detail) {
    failures++;
    fprintf(stderr, "FAIL %s\n", name);
    if (detail && detail[0]) fprintf(stderr, "     %s\n", detail);
}

static void check(const char *name, bool condition) {
    checks_run++;
    if (!condition) fail(name, NULL);
}

static void check_str(const char *name, const char *actual, const char *expected) {
    checks_run++;
    if (!actual || strcmp(actual, expected) != 0) {
        failures++;
        fprintf(stderr, "FAIL %s\n     expected: %s\n     actual:   %s\n",
                name, expected, actual ? actual : "(null)");
    }
}

// --- hex helpers ------------------------------------------------------------

static size_t from_hex(const char *hex, uint8_t *out, size_t out_cap) {
    size_t len = strlen(hex) / 2;
    if (len > out_cap) {
        fprintf(stderr, "internal error: hex literal too long\n");
        exit(2);
    }
    for (size_t i = 0; i < len; i++) {
        unsigned value = 0;
        sscanf(hex + i * 2, "%2x", &value);
        out[i] = (uint8_t)value;
    }
    return len;
}

static void to_hex(const uint8_t *bytes, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) sprintf(out + i * 2, "%02x", bytes[i]);
    out[len * 2] = '\0';
}

static void check_hex(const char *name, const uint8_t *bytes, size_t len, const char *expected) {
    char actual[256];
    if (len * 2 + 1 > sizeof(actual)) {
        fail(name, "digest too long for the comparison buffer");
        return;
    }
    to_hex(bytes, len, actual);
    check_str(name, actual, expected);
}

// Values captured from the Swift implementations. A missing key means the
// goldens and this file have drifted apart, which is a broken test rather than
// a failing one.
static const char *golden(const char *key) {
    for (size_t i = 0; i < sizeof(rs_goldens) / sizeof(rs_goldens[0]); i++) {
        if (strcmp(rs_goldens[i].key, key) == 0) return rs_goldens[i].value;
    }
    fprintf(stderr, "internal error: no golden named '%s' — rerun scripts/gen-goldens.py\n", key);
    exit(2);
}

// --- crypto -----------------------------------------------------------------

static void test_sha256(void) {
    uint8_t digest[RS_SHA256_DIGEST_LEN];

    rs_sha256((const uint8_t *)"abc", 3, digest);
    check_hex("sha256/abc", digest, sizeof(digest),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    rs_sha256((const uint8_t *)"", 0, digest);
    check_hex("sha256/empty", digest, sizeof(digest),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // 448 bits, so padding spills into a second block.
    const char *two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    rs_sha256((const uint8_t *)two_block, strlen(two_block), digest);
    check_hex("sha256/two-block", digest, sizeof(digest),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // The same input fed in awkward chunks must land on the same digest — the
    // streaming path has no counterpart in the Swift one-shot implementation.
    rs_sha256_ctx ctx;
    rs_sha256_init(&ctx);
    size_t offsets[] = {1, 13, 64, 65};
    size_t position = 0;
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]) && position < strlen(two_block); i++) {
        size_t take = offsets[i];
        if (position + take > strlen(two_block)) take = strlen(two_block) - position;
        rs_sha256_update(&ctx, (const uint8_t *)two_block + position, take);
        position += take;
    }
    if (position < strlen(two_block)) {
        rs_sha256_update(&ctx, (const uint8_t *)two_block + position, strlen(two_block) - position);
    }
    rs_sha256_final(&ctx, digest);
    check_hex("sha256/streaming", digest, sizeof(digest),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Exactly one block, and one byte past it: the boundary the padding gets
    // wrong most easily.
    uint8_t block[65];
    memset(block, 'a', sizeof(block));
    rs_sha256(block, 64, digest);
    check_hex("sha256/64-bytes", digest, sizeof(digest),
              "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    rs_sha256(block, 65, digest);
    check_hex("sha256/65-bytes", digest, sizeof(digest),
              "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
}

static void test_hmac(void) {
    uint8_t digest[RS_SHA256_DIGEST_LEN];
    uint8_t key[200];

    // RFC 4231 case 1
    memset(key, 0x0b, 20);
    rs_hmac_sha256(key, 20, (const uint8_t *)"Hi There", 8, digest);
    check_hex("hmac/rfc4231-1", digest, sizeof(digest),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // RFC 4231 case 2 — a key shorter than the digest.
    rs_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, digest);
    check_hex("hmac/rfc4231-2", digest, sizeof(digest),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // RFC 4231 case 6 — a 131-byte key, which must be hashed down first.
    memset(key, 0xaa, 131);
    rs_hmac_sha256(key, 131, (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54,
                   digest);
    check_hex("hmac/rfc4231-6", digest, sizeof(digest),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    // A key of exactly one block, where the "hash it first" branch must not
    // fire. Value from Python's hmac, an implementation unrelated to this one.
    memset(key, 0x0c, 64);
    rs_hmac_sha256(key, 64, (const uint8_t *)"block-sized key", 15, digest);
    check_hex("hmac/block-sized-key", digest, sizeof(digest),
              "b8f2ae281ed73e99880decadfae4c149bcd78c9d7360cd2f07e5ddf8f44e1e72");
}

static void test_pbkdf2(void) {
    uint8_t derived[64];

    // RFC 7914 section 11: P="passwd", S="salt", c=1, dkLen=64.
    check("pbkdf2/rfc7914-1",
          rs_pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1, derived, 64) == 0);
    check_hex("pbkdf2/rfc7914-1-first-32", derived, 32,
              "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc");

    // A derived length that is not a multiple of the digest exercises the
    // partial final block.
    check("pbkdf2/40-bytes",
          rs_pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 2, derived, 40) == 0);
    check_hex("pbkdf2/40-bytes-value", derived, 40,
              "2d412f896e76685e30df569f0a740634e31f031f749d607d9e44210b"
              "ffb91a6ab670f500c7886200");
    check("pbkdf2/rejects-zero-iterations",
          rs_pbkdf2_sha256((const uint8_t *)"p", 1, (const uint8_t *)"s", 1, 0, derived, 32) == -1);
    check("pbkdf2/rejects-zero-length",
          rs_pbkdf2_sha256((const uint8_t *)"p", 1, (const uint8_t *)"s", 1, 1, derived, 0) == -1);

    // RFC 6070 vector re-keyed to SHA-256 (the widely published value), which
    // also proves the many-iteration loop.
    check("pbkdf2/4096",
          rs_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, derived, 32) == 0);
    check_hex("pbkdf2/4096-value", derived, 32,
              "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
}

static void test_aes_blocks(void) {
    uint8_t key[32], in[16], out[16];
    rs_aes aes;

    // FIPS-197 appendix C
    from_hex("000102030405060708090a0b0c0d0e0f", key, sizeof(key));
    check("aes128/init", rs_aes_init(&aes, key, 16) == 0);
    from_hex("00112233445566778899aabbccddeeff", in, sizeof(in));
    rs_aes_encrypt_block(&aes, in, out);
    check_hex("aes128/encrypt", out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");
    rs_aes_decrypt_block(&aes, out, in);
    check_hex("aes128/decrypt", in, 16, "00112233445566778899aabbccddeeff");

    // AES-192 has no vector in AES.swift's own self-test, so the expected
    // values come from running that implementation (see goldens.h).
    from_hex("000102030405060708090a0b0c0d0e0f1011121314151617", key, sizeof(key));
    check("aes192/init", rs_aes_init(&aes, key, 24) == 0);
    from_hex("00112233445566778899aabbccddeeff", in, sizeof(in));
    rs_aes_encrypt_block(&aes, in, out);
    check_hex("aes192/encrypt", out, 16, golden("aes:192-encrypt"));
    rs_aes_decrypt_block(&aes, out, in);
    check_hex("aes192/decrypt", in, 16, golden("aes:192-decrypt"));

    from_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, sizeof(key));
    check("aes256/init", rs_aes_init(&aes, key, 32) == 0);
    from_hex("00112233445566778899aabbccddeeff", in, sizeof(in));
    rs_aes_encrypt_block(&aes, in, out);
    check_hex("aes256/encrypt", out, 16, "8ea2b7ca516745bfeafc49904b496089");
    rs_aes_decrypt_block(&aes, out, in);
    check_hex("aes256/decrypt", in, 16, "00112233445566778899aabbccddeeff");

    check("aes/rejects-bad-key-length", rs_aes_init(&aes, key, 17) == -1);
}

static void test_aes_ctr(void) {
    uint8_t key[16], iv[16], data[64];
    from_hex("2b7e151628aed2a6abf7158809cf4f3c", key, sizeof(key));
    from_hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv, sizeof(iv));

    // NIST SP 800-38A F.5.1 block 1 — the vector AES.swift's own self-test
    // carries, so it is checked here as a literal too.
    rs_aes_ctr ctr;
    uint8_t block[16];
    from_hex("6bc1bee22e409f96e93d7e117393172a", block, sizeof(block));
    check("ctr/init", rs_aes_ctr_init(&ctr, key, 16, iv, 16) == 0);
    rs_aes_ctr_process(&ctr, block, 16);
    check_hex("ctr/f5.1-block-1", block, 16, "874d6191b620e3261bef6864990db6ce");

    // Four blocks of keystream, from the Swift AESCTR.
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
    rs_aes_ctr_init(&ctr, key, 16, iv, 16);
    rs_aes_ctr_process(&ctr, data, 64);
    check_hex("ctr/64-bytes", data, 64, golden("aes:ctr-64"));

    // The same keystream produced across ragged calls: the continuity CENC's
    // per-subsample decryption depends on.
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
    rs_aes_ctr_init(&ctr, key, 16, iv, 16);
    size_t chunks[] = {1, 15, 33, 15};
    size_t offset = 0;
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        rs_aes_ctr_process(&ctr, data + offset, chunks[i]);
        offset += chunks[i];
    }
    check("ctr/split-covers-input", offset == 64);
    check_hex("ctr/64-bytes-split", data, 64, golden("aes:ctr-64"));

    // A short IV is zero-padded rather than rejected, as AESCTR does.
    uint8_t short_iv[4] = {0xf0, 0xf1, 0xf2, 0xf3};
    uint8_t short_data[32];
    for (size_t i = 0; i < sizeof(short_data); i++) short_data[i] = (uint8_t)i;
    check("ctr/short-iv", rs_aes_ctr_init(&ctr, key, 16, short_iv, 4) == 0);
    rs_aes_ctr_process(&ctr, short_data, sizeof(short_data));
    check_hex("ctr/short-iv-value", short_data, sizeof(short_data), golden("aes:ctr-short-iv"));
    check("ctr/rejects-bad-key", rs_aes_ctr_init(&ctr, key, 7, iv, 16) == -1);

    // Counter carry across the 128-bit boundary.
    uint8_t max_iv[16];
    from_hex("ffffffffffffffffffffffffffffffff", max_iv, sizeof(max_iv));
    rs_aes_ctr_init(&ctr, key, 16, max_iv, 16);
    uint8_t two_blocks[32];
    memset(two_blocks, 0, sizeof(two_blocks));
    rs_aes_ctr_process(&ctr, two_blocks, 32);
    uint8_t expected_first[16], expected_second[16];
    rs_aes wrapped;
    rs_aes_init(&wrapped, key, 16);
    rs_aes_encrypt_block(&wrapped, max_iv, expected_first);
    uint8_t zero_block[16];
    memset(zero_block, 0, sizeof(zero_block));
    rs_aes_encrypt_block(&wrapped, zero_block, expected_second);
    check("ctr/counter-wraps",
          memcmp(two_blocks, expected_first, 16) == 0 &&
              memcmp(two_blocks + 16, expected_second, 16) == 0);
}

static void test_aes_cbc(void) {
    uint8_t key[16], iv[16], ct[64], out[64];
    size_t out_len = 0;

    from_hex("2b7e151628aed2a6abf7158809cf4f3c", key, sizeof(key));
    from_hex("000102030405060708090a0b0c0d0e0f", iv, sizeof(iv));

    // 48 bytes of content plus a full block of PKCS7 padding, so a correct
    // decrypt hands back 48. Ciphertext and plaintext both from AESCBC.
    size_t ct_len = from_hex(golden("aes:cbc-padded-cipher"), ct, sizeof(ct));
    check("cbc/padded", rs_aes_cbc_decrypt(key, 16, iv, 16, ct, ct_len, out, &out_len) == 0);
    check("cbc/padded-strips-a-full-block", ct_len == 64 && out_len == 48);
    check_hex("cbc/padded-plaintext", out, out_len, golden("aes:cbc-padded-plain"));

    // A plaintext whose final byte is not a valid pad comes back whole rather
    // than as an error — deliberate, and load-bearing for existing streams.
    ct_len = from_hex(golden("aes:cbc-unpadded-cipher"), ct, sizeof(ct));
    check("cbc/invalid-pad", rs_aes_cbc_decrypt(key, 16, iv, 16, ct, ct_len, out, &out_len) == 0);
    check("cbc/invalid-pad-keeps-everything", out_len == 32);
    check_hex("cbc/invalid-pad-plaintext", out, out_len, golden("aes:cbc-unpadded-plain"));

    check("cbc/rejects-empty", rs_aes_cbc_decrypt(key, 16, iv, 16, ct, 0, out, &out_len) == -1);
    check("cbc/rejects-misaligned", rs_aes_cbc_decrypt(key, 16, iv, 16, ct, 31, out, &out_len) == -1);
    check("cbc/rejects-short-iv", rs_aes_cbc_decrypt(key, 16, iv, 12, ct, 32, out, &out_len) == -1);
    check("cbc/rejects-bad-key", rs_aes_cbc_decrypt(key, 13, iv, 16, ct, 32, out, &out_len) == -1);
}

// --- goldens ----------------------------------------------------------------

static const char UNIT_SEP[] = "\x1f";
static const char RECORD_SEP[] = "\x1e";
static const char GROUP_SEP[] = "\x1d";

static const rs_playlist_fixture *find_playlist(const char *name) {
    for (size_t i = 0; i < sizeof(rs_playlist_fixtures) / sizeof(rs_playlist_fixtures[0]); i++) {
        if (strcmp(rs_playlist_fixtures[i].name, name) == 0) return &rs_playlist_fixtures[i];
    }
    return NULL;
}

static const rs_ffargs_inputs *find_ffargs(const char *name) {
    for (size_t i = 0; i < sizeof(rs_ffargs_cases) / sizeof(rs_ffargs_cases[0]); i++) {
        if (strcmp(rs_ffargs_case_names[i], name) == 0) return &rs_ffargs_cases[i];
    }
    return NULL;
}

// The transform scripts/dump-goldens.swift uses, so the two sides can be
// compared line for line.
static char *media_transform(void *userdata, const char *uri, rs_m3u8_line_kind kind, int64_t seq) {
    (void)userdata;
    const char *kind_name = kind == RS_M3U8_LINE_KEY ? "key" : (kind == RS_M3U8_LINE_MAP ? "map" : "seg");
    size_t size = strlen(uri) + 64;
    char *out = (char *)malloc(size);
    if (!out) return NULL;
    if (seq >= 0) {
        snprintf(out, size, "/p/%s?u=%s&s=%lld", kind_name, uri, (long long)seq);
    } else {
        snprintf(out, size, "/p/%s?u=%s", kind_name, uri);
    }
    return out;
}

static char *master_transform(void *userdata, const char *uri) {
    (void)userdata;
    size_t size = strlen(uri) + 8;
    char *out = (char *)malloc(size);
    if (out) snprintf(out, size, "/m?u=%s", uri);
    return out;
}

static void append_field(char **buffer, size_t *cap, size_t *len, const char *text) {
    size_t needed = *len + strlen(text) + 1;
    if (needed > *cap) {
        while (*cap < needed) *cap = *cap ? *cap * 2 : 256;
        *buffer = (char *)realloc(*buffer, *cap);
        if (!*buffer) {
            fprintf(stderr, "out of memory building a golden comparison\n");
            exit(2);
        }
    }
    memcpy(*buffer + *len, text, strlen(text) + 1);
    *len += strlen(text);
}

static void append_int(char **buffer, size_t *cap, size_t *len, int64_t value) {
    char text[32];
    snprintf(text, sizeof(text), "%lld", (long long)value);
    append_field(buffer, cap, len, text);
}

// Serialises a probe result the way the Swift dumper does.
static char *serialize_probe(const rs_m3u8_probe *probe) {
    char *buffer = NULL;
    size_t cap = 0, len = 0;
    append_field(&buffer, &cap, &len, "");
    for (size_t i = 0; i < probe->variant_count; i++) {
        const rs_m3u8_variant *v = &probe->variants[i];
        if (i > 0) append_field(&buffer, &cap, &len, RECORD_SEP);
        append_field(&buffer, &cap, &len, v->uri);
        append_field(&buffer, &cap, &len, UNIT_SEP);
        if (v->bandwidth >= 0) append_int(&buffer, &cap, &len, v->bandwidth);
        else append_field(&buffer, &cap, &len, "<nil>");
        append_field(&buffer, &cap, &len, UNIT_SEP);
        if (v->width >= 0) append_int(&buffer, &cap, &len, v->width);
        else append_field(&buffer, &cap, &len, "<nil>");
        append_field(&buffer, &cap, &len, UNIT_SEP);
        if (v->height >= 0) append_int(&buffer, &cap, &len, v->height);
        else append_field(&buffer, &cap, &len, "<nil>");
        append_field(&buffer, &cap, &len, UNIT_SEP);
        append_field(&buffer, &cap, &len, v->codecs ? v->codecs : "<nil>");
    }
    append_field(&buffer, &cap, &len, GROUP_SEP);
    for (size_t i = 0; i < probe->audio_count; i++) {
        const rs_m3u8_audio_track *t = &probe->audio_tracks[i];
        if (i > 0) append_field(&buffer, &cap, &len, RECORD_SEP);
        append_field(&buffer, &cap, &len, t->group_id);
        append_field(&buffer, &cap, &len, UNIT_SEP);
        append_field(&buffer, &cap, &len, t->name);
        append_field(&buffer, &cap, &len, UNIT_SEP);
        append_field(&buffer, &cap, &len, t->language ? t->language : "<nil>");
        append_field(&buffer, &cap, &len, UNIT_SEP);
        append_field(&buffer, &cap, &len, t->uri ? t->uri : "<nil>");
    }
    return buffer;
}

static char *serialize_command(const rs_ffargs_command *command) {
    char *buffer = NULL;
    size_t cap = 0, len = 0;
    append_field(&buffer, &cap, &len, "");
    for (size_t i = 0; i < command->argc; i++) {
        if (i > 0) append_field(&buffer, &cap, &len, UNIT_SEP);
        append_field(&buffer, &cap, &len, command->argv[i]);
    }
    append_field(&buffer, &cap, &len, GROUP_SEP);
    // The Swift dumper sorts environment keys; http_proxy precedes https_proxy
    // in that order, which is the order rs_ffargs_build emits them in anyway.
    for (size_t i = 0; i < command->env_count; i++) {
        if (i > 0) append_field(&buffer, &cap, &len, UNIT_SEP);
        append_field(&buffer, &cap, &len, command->env_keys[i]);
        append_field(&buffer, &cap, &len, "=");
        append_field(&buffer, &cap, &len, command->env_values[i]);
    }
    return buffer;
}

static void run_golden(const rs_golden *entry) {
    const char *key = entry->key;

    // The AES goldens are consumed directly by test_aes_* above.
    if (strncmp(key, "aes:", 4) == 0) return;

    if (strncmp(key, "resolve:", 8) == 0) {
        // The key is "resolve:<base><unit separator><ref>".
        const char *base_start = key + 8;
        const char *separator = strchr(base_start, '\x1f');
        if (!separator) { fail(key, "malformed resolve golden"); return; }
        char base[512];
        size_t base_len = (size_t)(separator - base_start);
        if (base_len >= sizeof(base)) { fail(key, "base too long"); return; }
        memcpy(base, base_start, base_len);
        base[base_len] = '\0';

        char *resolved = rs_url_resolve(base, separator + 1);
        check_str(key, resolved ? resolved : "<nil>", entry->value);
        rs_free(resolved);
        return;
    }

    if (strncmp(key, "mediaseq:", 9) == 0) {
        const rs_playlist_fixture *fixture = find_playlist(key + 9);
        if (!fixture) { fail(key, "unknown playlist fixture"); return; }
        char actual[32];
        snprintf(actual, sizeof(actual), "%lld", (long long)rs_m3u8_media_sequence(fixture->text));
        check_str(key, actual, entry->value);
        return;
    }

    if (strncmp(key, "ismaster:", 9) == 0) {
        const rs_playlist_fixture *fixture = find_playlist(key + 9);
        if (!fixture) { fail(key, "unknown playlist fixture"); return; }
        check_str(key, rs_m3u8_is_master(fixture->text) ? "1" : "0", entry->value);
        return;
    }

    if (strncmp(key, "rewrite:", 8) == 0) {
        char name[64];
        const char *rest = key + 8;
        const char *colon = strrchr(rest, ':');
        if (!colon || (size_t)(colon - rest) >= sizeof(name)) { fail(key, "malformed rewrite golden"); return; }
        memcpy(name, rest, (size_t)(colon - rest));
        name[colon - rest] = '\0';
        const rs_playlist_fixture *fixture = find_playlist(name);
        if (!fixture) { fail(key, "unknown playlist fixture"); return; }

        char *actual = rs_m3u8_rewrite(fixture->text, fixture->base, colon[1] == '1',
                                       media_transform, NULL);
        check_str(key, actual, entry->value);
        rs_free(actual);
        return;
    }

    if (strncmp(key, "master:", 7) == 0) {
        const rs_playlist_fixture *fixture = find_playlist(key + 7);
        if (!fixture) { fail(key, "unknown playlist fixture"); return; }
        char *actual = rs_m3u8_rewrite_master(fixture->text, fixture->base, master_transform, NULL);
        check_str(key, actual, entry->value);
        rs_free(actual);
        return;
    }

    if (strncmp(key, "probe:", 6) == 0) {
        const rs_playlist_fixture *fixture = find_playlist(key + 6);
        if (!fixture) { fail(key, "unknown playlist fixture"); return; }
        rs_m3u8_probe probe;
        if (rs_m3u8_probe_master(fixture->text, fixture->base, &probe) != 0) {
            fail(key, "probe failed");
            return;
        }
        char *actual = serialize_probe(&probe);
        check_str(key, actual, entry->value);
        free(actual);
        rs_m3u8_probe_dispose(&probe);
        return;
    }

    if (strncmp(key, "ffargs:", 7) == 0) {
        const rs_ffargs_inputs *inputs = find_ffargs(key + 7);
        if (!inputs) { fail(key, "unknown ffmpeg fixture"); return; }
        rs_ffargs_command command;
        if (rs_ffargs_build(inputs, &command) != 0) {
            fail(key, "build failed");
            return;
        }
        char *actual = serialize_command(&command);
        check_str(key, actual, entry->value);
        free(actual);
        rs_ffargs_command_dispose(&command);
        return;
    }

    fail(key, "no handler for this golden key");
}

// --- helpers that have no Swift counterpart to compare against --------------

static void test_ffargs_helpers(void) {
    char *key = rs_ffargs_first_clear_key("\n  \nkid1:abc123\nkid2:def\n");
    check_str("ffargs/first-clear-key", key ? key : "(null)", "abc123");
    rs_free(key);

    key = rs_ffargs_first_clear_key("bareKeyOnly");
    check_str("ffargs/bare-key", key ? key : "(null)", "bareKeyOnly");
    rs_free(key);

    key = rs_ffargs_first_clear_key("");
    check("ffargs/no-key", key == NULL);
    rs_free(key);

    char *block = rs_ffargs_header_block(" A: 1 \n\n B: 2 ");
    check_str("ffargs/header-block", block ? block : "(null)", "A: 1\r\nB: 2\r\n");
    rs_free(block);

    block = rs_ffargs_header_block("   \n  ");
    check("ffargs/no-headers", block == NULL);
    rs_free(block);

    char **tokens = NULL;
    size_t count = 0;
    check("ffargs/tokenize", rs_ffargs_tokenize("-f flv 'a b' \"c d\" e", &tokens, &count) == 0);
    check("ffargs/tokenize-count", count == 5);
    if (count == 5) {
        check_str("ffargs/tokenize-0", tokens[0], "-f");
        check_str("ffargs/tokenize-2", tokens[2], "a b");
        check_str("ffargs/tokenize-3", tokens[3], "c d");
        check_str("ffargs/tokenize-4", tokens[4], "e");
    }
    rs_free_strv(tokens, count);
}

// Subtitle mapping. This is not a golden: the Swift original never mapped
// subtitles at all, so there is nothing to be equal to — the rule being checked
// is that the map appears exactly where a subtitle track can survive the remux
// and nowhere else, because a bad `-map` fails ffmpeg at startup rather than
// dropping the track.
static void test_ffargs_subtitles(void) {
    struct {
        const char *name;
        const char *source;
        const char *output_mode;
        const char *target;
        bool expect;
    } cases[] = {
        // MPEG-TS in, MPEG-TS out: DVB subtitles and teletext copy straight
        // across, which is the whole case this exists for.
        {"srt-source-srt-out", "srt://feed.example.com:9000", "srtServer", "9100", true},
        {"udp-source-udp-out", "udp://239.0.0.1:1234", "udpSrt", "udp://239.0.0.2:1234", true},
        {"ts-over-http", "https://origin.example.com/live/stream.ts", "udpSrt", "udp://239.0.0.2:1234", true},
        // A query string naming another format must not decide the question.
        {"ts-with-query", "https://origin.example.com/s.ts?fallback=x.m3u8", "udpSrt", "udp://h:1", true},
        // HLS and DASH sources carry WebVTT or TTML, neither of which has an
        // MPEG-TS mapping — mapping them would abort the process.
        {"hls-source-srt-out", "https://origin.example.com/live/stream.m3u8", "srtServer", "9100", false},
        {"dash-source-udp-out", "https://origin.example.com/live/stream.mpd", "udpSrt", "udp://h:1", false},
        // HLS output has nowhere to put a bitmap subtitle regardless of source.
        {"ts-source-hls-out", "srt://feed.example.com:9000", "hls", "", false},
        // A custom output supplies its own maps; adding one behind the user's
        // back would duplicate or contradict them.
        {"ts-source-custom-out", "srt://feed.example.com:9000", "custom", "-f flv rtmp://x/y", false},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rs_ffargs_inputs in;
        memset(&in, 0, sizeof(in));
        in.source_url = cases[i].source;
        in.kind = "m3u8";
        in.input_mode = "ffmpegResident";
        in.output_mode = cases[i].output_mode;
        in.output_target = cases[i].target;
        in.temp_dir = "/tmp/restreamair-fixture";
        in.output_playlist = "/tmp/restreamair-fixture/live.m3u8";
        in.playlist_segments = 6;
        in.segment_seconds = 4;

        rs_ffargs_command cmd;
        char name[96];
        snprintf(name, sizeof(name), "ffargs/subtitles-%s", cases[i].name);
        if (rs_ffargs_build(&in, &cmd) != 0) { fail(name, "build failed"); continue; }
        bool mapped = false;
        for (size_t j = 0; j + 1 < cmd.argc; j++)
            if (strcmp(cmd.argv[j], "-map") == 0 && strcmp(cmd.argv[j + 1], "0:s?") == 0) mapped = true;
        check(name, mapped == cases[i].expect);
        rs_ffargs_command_dispose(&cmd);
    }
}

// --- JSON DOM ---------------------------------------------------------------

static void test_json(void) {
    // Round-trip every type, compact.
    const char *sample =
        "{\"a\":1,\"b\":-2.5,\"c\":\"hi\",\"d\":true,\"e\":null,"
        "\"f\":[1,2,3],\"g\":{\"nested\":\"x\"},\"h\":\"\\u00e9\\n\\t\\\"\"}";
    rs_json *v = rs_json_parse(sample, strlen(sample));
    check("json/parse", v != NULL);
    if (v) {
        check("json/type", rs_json_type_of(v) == RS_JSON_OBJ);
        check("json/num-int", rs_json_as_num(rs_json_obj_get(v, "a"), 0) == 1.0);
        check("json/num-real", rs_json_as_num(rs_json_obj_get(v, "b"), 0) == -2.5);
        check_str("json/str", rs_json_as_str(rs_json_obj_get(v, "c"), ""), "hi");
        check("json/bool", rs_json_as_bool(rs_json_obj_get(v, "d"), false) == true);
        check("json/null", rs_json_type_of(rs_json_obj_get(v, "e")) == RS_JSON_NULL);
        check("json/arr-len", rs_json_arr_len(rs_json_obj_get(v, "f")) == 3);
        // The escaped string decodes to é (UTF-8), a newline, a tab and a quote.
        check_str("json/unescape", rs_json_as_str(rs_json_obj_get(v, "h"), ""), "\xc3\xa9\n\t\"");

        // Integers stay integers; a re-serialize+parse is stable.
        char *out = rs_json_serialize(v, false);
        rs_json *again = out ? rs_json_parse(out, strlen(out)) : NULL;
        check("json/reparse", again != NULL);
        char *out2 = again ? rs_json_serialize(again, false) : NULL;
        check_str("json/round-trip-stable", out2 ? out2 : "", out ? out : "");
        rs_free(out);
        rs_free(out2);
        rs_json_free(again);
    }
    rs_json_free(v);

    // The load-bearing property: mutating one key must not disturb fields the
    // code never touched — this is what keeps state.json intact.
    const char *stored =
        "{\"providers\":[{\"id\":\"p1\",\"secretField\":42,\"nested\":{\"deep\":[1,2]}}],"
        "\"adminUsers\":[],\"unknownTopLevel\":\"keep me\"}";
    rs_json *state = rs_json_parse(stored, strlen(stored));
    check("json/state-parse", state != NULL);
    if (state) {
        rs_json *users = rs_json_new_arr();
        rs_json *admin = rs_json_new_obj();
        rs_json_obj_set(admin, "username", rs_json_new_str("me"));
        rs_json_arr_push(users, admin);
        rs_json_obj_set(state, "adminUsers", users);  // replace only adminUsers

        char *out = rs_json_serialize(state, false);
        check("json/preserve-unknown-toplevel", out && strstr(out, "\"unknownTopLevel\":\"keep me\"") != NULL);
        check("json/preserve-nested", out && strstr(out, "\"secretField\":42") != NULL);
        check("json/preserve-deep", out && strstr(out, "\"deep\":[1,2]") != NULL);
        check("json/mutation-applied", out && strstr(out, "\"username\":\"me\"") != NULL);
        rs_free(out);
    }
    rs_json_free(state);

    // Malformed input is rejected, not half-parsed.
    check("json/reject-trailing", rs_json_parse("{}x", 3) == NULL);
    check("json/reject-unclosed", rs_json_parse("{\"a\":", 5) == NULL);
    check("json/reject-bare", rs_json_parse("nope", 4) == NULL);

    // clone is independent of its source.
    rs_json *orig = rs_json_parse("{\"x\":[1,{\"y\":2}]}", 17);
    rs_json *copy = rs_json_clone(orig);
    rs_json_free(orig);
    char *copied = copy ? rs_json_serialize(copy, false) : NULL;
    check_str("json/clone", copied ? copied : "", "{\"x\":[1,{\"y\":2}]}");
    rs_free(copied);
    rs_json_free(copy);
}

// --- auth -------------------------------------------------------------------

static void test_auth(void) {
    // A hash verifies with the right password and rejects the wrong one, and a
    // fresh salt each time means two hashes of the same password differ.
    char *hash1 = NULL, *salt1 = NULL, *hash2 = NULL, *salt2 = NULL;
    check("auth/hash", rs_auth_hash_password("correct horse", &hash1, &salt1) == 0);
    check("auth/hash-again", rs_auth_hash_password("correct horse", &hash2, &salt2) == 0);
    check("auth/salted", hash1 && hash2 && strcmp(hash1, hash2) != 0);
    check("auth/verify-ok", rs_auth_verify_password("correct horse", hash1, salt1));
    check("auth/verify-wrong-password", !rs_auth_verify_password("wrong", hash1, salt1));
    check("auth/verify-wrong-salt", !rs_auth_verify_password("correct horse", hash1, salt2));

    // Interop with the AuthStore encoding: verifying against a hash+salt pair
    // computed independently (Python's hashlib.pbkdf2_hmac) proves the base64
    // framing and PBKDF2 parameters match what Swift persists. password
    // "hunter2", salt = base64("0123456789abcdef").
    check("auth/known-vector",
          rs_auth_verify_password("hunter2",
                                  "+wwwi9gM/eAJl7udbNFuJKYu39MDjh1mH7qtv6nNiFw=",
                                  "MDEyMzQ1Njc4OWFiY2RlZg=="));
    rs_free(hash1); rs_free(salt1); rs_free(hash2); rs_free(salt2);

    // Sessions: a created token resolves to its user, logout invalidates it, and
    // an unknown token resolves to nothing.
    rs_auth *auth = rs_auth_create();
    char *token = rs_auth_create_session(auth, "alice", false);
    check("auth/token", token != NULL);
    char *who = rs_auth_username_for_token(auth, token);
    check_str("auth/session-user", who ? who : "", "alice");
    rs_free(who);
    check("auth/unknown-token", rs_auth_username_for_token(auth, "nope") == NULL);
    rs_auth_end_session(auth, token);
    check("auth/logout", rs_auth_username_for_token(auth, token) == NULL);
    rs_free(token);
    rs_auth_destroy(auth);

    // Cookie parsing pulls the session out of a realistic Cookie header.
    char *t = rs_auth_cookie_token("theme=dark; restreamair_session=abc123; other=1");
    check_str("auth/cookie-parse", t ? t : "", "abc123");
    rs_free(t);
    check("auth/cookie-absent", rs_auth_cookie_token("theme=dark; other=1") == NULL);

    // Set-Cookie matches AuthStore.setCookieHeader byte for byte.
    char *set = rs_auth_set_cookie("TOK", false, false);
    check_str("auth/set-cookie", set ? set : "",
              "restreamair_session=TOK; HttpOnly; Path=/; SameSite=Lax");
    rs_free(set);
    char *set_remember = rs_auth_set_cookie("TOK", true, false);
    check_str("auth/set-cookie-remember", set_remember ? set_remember : "",
              "restreamair_session=TOK; HttpOnly; Path=/; SameSite=Lax; Max-Age=2592000");
    rs_free(set_remember);
    // Secure is appended last, and only when the request reached us over TLS.
    char *set_secure = rs_auth_set_cookie("TOK", true, true);
    check_str("auth/set-cookie-secure", set_secure ? set_secure : "",
              "restreamair_session=TOK; HttpOnly; Path=/; SameSite=Lax; Max-Age=2592000; Secure");
    rs_free(set_secure);
    char *clear_secure = rs_auth_clear_cookie(true);
    check_str("auth/clear-cookie-secure", clear_secure ? clear_secure : "",
              "restreamair_session=; HttpOnly; Path=/; Max-Age=0; SameSite=Lax; Secure");
    rs_free(clear_secure);

    // Basic auth decode.
    char *u = NULL, *p = NULL;
    check("auth/basic", rs_auth_parse_basic("Basic YWRtaW46c2VjcmV0", &u, &p) == 0);
    check_str("auth/basic-user", u ? u : "", "admin");
    check_str("auth/basic-pass", p ? p : "", "secret");
    rs_free(u); rs_free(p);
}

// Sessions survive a save/restore cycle, and what gets written is the hash of
// the token rather than the token — the property that makes it safe to keep
// them in state.json at all.
static void test_auth_sessions(void) {
    rs_auth *auth = rs_auth_create();
    char *token = rs_auth_create_session(auth, "alice", true);
    char *other = rs_auth_create_session(auth, "bob", false);
    rs_json *exported = rs_auth_export_sessions(auth);
    check("auth/export-count", rs_json_arr_len(exported) == 2);

    char *rendered = rs_json_serialize(exported, false);
    check("auth/export-hides-token", rendered && token && strstr(rendered, token) == NULL);
    check("auth/export-has-hash", rendered && strstr(rendered, "\"tokenHash\":\"") != NULL);
    // Timestamps use the 2001 reference epoch, matching Swift's JSONEncoder, so
    // a session written by either server is understood by the other. A Unix
    // timestamp would be a ten-digit number starting with 17…; this is nine.
    check("auth/export-uses-apple-epoch",
          rendered && strstr(rendered, "\"expiresAt\":8") != NULL);
    rs_free(rendered);

    // A fresh store loaded from the export accepts the original cookie.
    rs_auth *restored = rs_auth_create();
    rs_auth_import_sessions(restored, exported);
    char *who = rs_auth_username_for_token(restored, token);
    check_str("auth/session-restored", who ? who : "", "alice");
    rs_free(who);
    check("auth/session-restored-unknown", rs_auth_username_for_token(restored, "nope") == NULL);

    // Ending every session for one user leaves the others alone.
    check("auth/end-user-sessions", rs_auth_end_sessions_for_user(restored, "alice") == 1);
    check("auth/end-user-gone", rs_auth_username_for_token(restored, token) == NULL);
    char *still = rs_auth_username_for_token(restored, other);
    check_str("auth/end-user-kept-other", still ? still : "", "bob");
    rs_free(still);

    // An expired entry is dropped on import rather than resurrected.
    rs_json *stale = rs_json_new_arr();
    rs_json *entry = rs_json_new_obj();
    rs_json_obj_set_str(entry, "tokenHash", "deadbeef");
    rs_json_obj_set_str(entry, "username", "carol");
    rs_json_obj_set_int(entry, "expiresAt", 1);  // 2001, i.e. long expired
    rs_json_arr_push(stale, entry);
    rs_auth *expired_store = rs_auth_create();
    rs_auth_import_sessions(expired_store, stale);
    rs_json *reexported = rs_auth_export_sessions(expired_store);
    check("auth/import-drops-expired", rs_json_arr_len(reexported) == 0);
    rs_json_free(reexported);
    rs_json_free(stale);
    rs_auth_destroy(expired_store);

    rs_json_free(exported);
    rs_free(token);
    rs_free(other);
    rs_auth_destroy(restored);
    rs_auth_destroy(auth);
}

// The sign-in throttle: free attempts first, then a growing delay, cleared by a
// success.
static void test_auth_throttle(void) {
    rs_auth *auth = rs_auth_create();
    const char *who = "alice|203.0.113.9";

    check("throttle/starts-open", rs_auth_throttle_delay(auth, who) == 0);
    for (int i = 0; i < 5; i++) {
        check("throttle/free-attempt", rs_auth_throttle_record_failure(auth, who) == 0);
    }
    check("throttle/still-open-at-limit", rs_auth_throttle_delay(auth, who) == 0);

    // The sixth failure starts the backoff, and it doubles from there.
    check("throttle/first-delay", rs_auth_throttle_record_failure(auth, who) == 2);
    check("throttle/delay-reported", rs_auth_throttle_delay(auth, who) > 0);
    check("throttle/second-delay", rs_auth_throttle_record_failure(auth, who) == 4);
    check("throttle/third-delay", rs_auth_throttle_record_failure(auth, who) == 8);

    // It is capped, not unbounded.
    for (int i = 0; i < 40; i++) rs_auth_throttle_record_failure(auth, who);
    check("throttle/capped", rs_auth_throttle_record_failure(auth, who) == 15 * 60);

    // Another identity is unaffected — one attacker cannot lock out everyone.
    check("throttle/per-identity", rs_auth_throttle_delay(auth, "alice|198.51.100.4") == 0);

    rs_auth_throttle_reset(auth, who);
    check("throttle/reset", rs_auth_throttle_delay(auth, who) == 0);
    rs_auth_destroy(auth);
}

// --- trusted proxies --------------------------------------------------------

static void test_netmatch(void) {
    uint8_t addr[16];
    check("ip/parse-v4", rs_ip_parse("192.168.1.7", addr));
    check("ip/parse-v6", rs_ip_parse("2001:db8::1", addr));
    check("ip/parse-v6-full", rs_ip_parse("2001:0db8:0000:0000:0000:0000:0000:0001", addr));
    check("ip/parse-v6-loopback", rs_ip_parse("::1", addr));
    check("ip/parse-v6-any", rs_ip_parse("::", addr));
    check("ip/parse-v4-mapped", rs_ip_parse("::ffff:192.0.2.128", addr));
    check("ip/parse-bracketed", rs_ip_parse("[2001:db8::1]", addr));
    check("ip/parse-zone", rs_ip_parse("fe80::1%eth0", addr));
    check("ip/reject-empty", !rs_ip_parse("", addr));
    check("ip/reject-text", !rs_ip_parse("example.com", addr));
    check("ip/reject-octet-range", !rs_ip_parse("192.168.1.256", addr));
    check("ip/reject-leading-zero", !rs_ip_parse("192.168.01.1", addr));
    check("ip/reject-short-v4", !rs_ip_parse("192.168.1", addr));
    check("ip/reject-double-compressor", !rs_ip_parse("1::2::3", addr));
    check("ip/reject-too-many-groups", !rs_ip_parse("1:2:3:4:5:6:7:8:9", addr));

    // CIDR containment, both families.
    check("ip/cidr-v4-in", rs_ip_matches("10.4.5.6", "10.0.0.0/8"));
    check("ip/cidr-v4-out", !rs_ip_matches("11.4.5.6", "10.0.0.0/8"));
    check("ip/cidr-v4-boundary", rs_ip_matches("172.31.255.255", "172.16.0.0/12"));
    check("ip/cidr-v4-just-outside", !rs_ip_matches("172.32.0.0", "172.16.0.0/12"));
    check("ip/cidr-v6-in", rs_ip_matches("2001:db8::dead", "2001:db8::/32"));
    check("ip/cidr-v6-out", !rs_ip_matches("2001:db9::dead", "2001:db8::/32"));
    check("ip/host-exact", rs_ip_matches("192.168.1.7", "192.168.1.7"));
    check("ip/host-mismatch", !rs_ip_matches("192.168.1.8", "192.168.1.7"));

    // A v4 rule never silently swallows v6 and vice versa.
    check("ip/family-isolation", !rs_ip_matches("2001:db8::1", "0.0.0.0/0"));
    check("ip/family-isolation-v4", !rs_ip_matches("10.0.0.1", "::/0"));

    // Named groups.
    check("ip/loopback-v4", rs_ip_matches("127.0.0.1", "loopback"));
    check("ip/loopback-v6", rs_ip_matches("::1", "loopback"));
    check("ip/loopback-rejects-lan", !rs_ip_matches("192.168.1.7", "loopback"));
    check("ip/private-lan", rs_ip_matches("192.168.1.7", "private"));
    check("ip/private-rejects-public", !rs_ip_matches("8.8.8.8", "private"));
    check("ip/any", rs_ip_matches("8.8.8.8", "any"));

    // Lists, with the separators an operator is likely to type.
    check("ip/list", rs_ip_in_list("10.1.2.3", "loopback, 10.0.0.0/8"));
    check("ip/list-spaces", rs_ip_in_list("10.1.2.3", "loopback 10.0.0.0/8"));
    check("ip/list-miss", !rs_ip_in_list("8.8.8.8", "loopback, 10.0.0.0/8"));
    check("ip/list-empty-trusts-nothing", !rs_ip_in_list("127.0.0.1", ""));

    // X-Forwarded-For resolution. An untrusted peer's header is ignored
    // outright — this is the check that stops a client forging its own address
    // past the login throttle.
    char *ip = rs_client_ip("203.0.113.5", "1.2.3.4", "loopback");
    check_str("xff/untrusted-peer-ignored", ip ? ip : "", "203.0.113.5");
    rs_free(ip);

    ip = rs_client_ip("127.0.0.1", "198.51.100.7", "loopback");
    check_str("xff/trusted-peer-honoured", ip ? ip : "", "198.51.100.7");
    rs_free(ip);

    // Rightmost untrusted hop wins: the client may have prepended anything.
    ip = rs_client_ip("127.0.0.1", "9.9.9.9, 198.51.100.7", "loopback");
    check_str("xff/rightmost-untrusted", ip ? ip : "", "198.51.100.7");
    rs_free(ip);

    // A chain of our own proxies is walked through to the real client.
    ip = rs_client_ip("127.0.0.1", "198.51.100.7, 10.0.0.2, 10.0.0.3", "loopback, 10.0.0.0/8");
    check_str("xff/skips-trusted-chain", ip ? ip : "", "198.51.100.7");
    rs_free(ip);

    ip = rs_client_ip("127.0.0.1", NULL, "loopback");
    check_str("xff/absent-header", ip ? ip : "", "127.0.0.1");
    rs_free(ip);

    ip = rs_client_ip("127.0.0.1", "not-an-address", "loopback");
    check_str("xff/garbage-header", ip ? ip : "", "127.0.0.1");
    rs_free(ip);

    // X-Forwarded-Proto, under the same trust rule.
    check("proto/direct-tls", rs_request_is_secure("203.0.113.5", NULL, "", true));
    check("proto/trusted-https", rs_request_is_secure("127.0.0.1", "https", "loopback", false));
    check("proto/trusted-http", !rs_request_is_secure("127.0.0.1", "http", "loopback", false));
    check("proto/untrusted-claim", !rs_request_is_secure("203.0.113.5", "https", "loopback", false));
    check("proto/multi-value", rs_request_is_secure("127.0.0.1", "https, http", "loopback", false));
    check("proto/absent", !rs_request_is_secure("127.0.0.1", NULL, "loopback", false));
}

// --- panel control plane ----------------------------------------------------

// Parses a JSON literal by its own length, so a miscounted constant can't
// silently break a test.
static rs_json *parse_json(const char *text) { return rs_json_parse(text, strlen(text)); }

static void test_panel(void) {
    // Slug: alphanumeric runs joined by single dashes, lowercased.
    char *slug = rs_panel_slugify("My Cool Stream! (HD)");
    check_str("panel/slugify", slug ? slug : "", "my-cool-stream-hd");
    free(slug);
    slug = rs_panel_slugify("   ");
    check_str("panel/slugify-empty", slug ? slug : "x", "");
    free(slug);

    // A create/update/delete round-trip over an in-memory state, asserting the
    // DOM ends where it should and unknown fields are never disturbed.
    rs_state st;
    st.root = parse_json("{\"providers\":[],\"keepThis\":\"untouched\"}");
    st.path = NULL;
    check("panel/state-seed", st.root != NULL);

    const char *err = NULL;
    rs_json *pbody = parse_json("{\"name\":\"P1\"}");
    check("panel/create-provider", rs_panel_create_provider(&st, pbody, &err) == 0);
    rs_json_free(pbody);
    const rs_json *providers = rs_json_obj_get(st.root, "providers");
    check("panel/provider-count", rs_json_arr_len(providers) == 1);
    const char *pid = rs_json_obj_str(rs_json_arr_at(providers, 0), "id", "");

    // A stream with a sub-floor playlistSegments must clamp to 3.
    rs_json *sbody = parse_json(
        "{\"name\":\"S1\",\"kind\":\"mpd\",\"url\":\"https://e.com/a.mpd\",\"playlistSegments\":1}");
    check("panel/create-stream", rs_panel_create_stream(&st, pid, sbody, &err) == 0);
    rs_json_free(sbody);
    const rs_json *streams = rs_json_obj_get(rs_json_arr_at(providers, 0), "streams");
    check("panel/stream-count", rs_json_arr_len(streams) == 1);
    check("panel/clamp", rs_json_obj_int(rs_json_arr_at(streams, 0), "playlistSegments", 0) == 3);

    // A bad URL is rejected with a 400.
    rs_json *bad = parse_json("{\"name\":\"S2\",\"url\":\"ftp://nope\"}");
    check("panel/reject-bad-url", rs_panel_create_stream(&st, pid, bad, &err) == -400);
    rs_json_free(bad);

    // The view exposes computed fields and a slug-based play URL.
    rs_json *view = rs_panel_view(&st, "host:1");
    char *rendered = rs_json_serialize(view, false);
    check("panel/view-playurl", rendered && strstr(rendered, "http://host:1/play/s1/index.m3u8") != NULL);
    check("panel/view-running", rendered && strstr(rendered, "\"running\":false") != NULL);
    rs_free(rendered);
    rs_json_free(view);

    // Delete leaves an empty provider list but keeps the unknown top-level key.
    check("panel/delete-provider", rs_panel_delete_provider(&st, pid, &err) == 0);
    check("panel/empty-after-delete", rs_json_arr_len(rs_json_obj_get(st.root, "providers")) == 0);
    char *final = rs_json_serialize(st.root, false);
    check("panel/preserve-unknown", final && strstr(final, "\"keepThis\":\"untouched\"") != NULL);
    rs_free(final);
    rs_json_free(st.root);
}

// A DASH segment file is not always a single fragment: CMAF sources routinely
// ship several moof/mdat pairs per segment. The decryptor used to stop after
// the first moof, so every later fragment reached the player as ciphertext —
// visible as h264 failing at "MB 0 0" with the whole slice unread and AAC
// decoding to noise, for roughly half of every segment.
//
// This builds a two-fragment segment by hand — fragment 0 subsample-encrypted
// (how video is protected), fragment 1 full-sample (how audio is) — encrypts
// it with a known key, and requires that BOTH fragments come back as the
// original plaintext with their senc/saiz/saio retyped to 'free'. Against the
// pre-fix decryptor, fragment 1 fails both halves of that.
#define PUT32(p, v) do { uint8_t *_p = (p); uint32_t _v = (v); \
    _p[0] = (uint8_t)(_v >> 24); _p[1] = (uint8_t)(_v >> 16); \
    _p[2] = (uint8_t)(_v >> 8);  _p[3] = (uint8_t)_v; } while (0)

// Appends a box header and returns the offset of its size field, so the caller
// can patch the final length once the payload is written.
static size_t open_box(uint8_t *b, size_t *n, const char *type) {
    size_t at = *n;
    PUT32(b + at, 0);
    memcpy(b + at + 4, type, 4);
    *n += 8;
    return at;
}
static void close_box(uint8_t *b, size_t n, size_t at) { PUT32(b + at, (uint32_t)(n - at)); }

static void test_cenc_multifragment(void) {
    enum { NFRAG = 2, NSAMP = 3, SAMPLE_LEN = 96, CLEAR_PREFIX = 16, IV_SIZE = 8 };
    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;

    uint8_t seg[4096], plain[NFRAG][NSAMP][SAMPLE_LEN];
    size_t n = 0, mdat_payload[NFRAG];

    for (int f = 0; f < NFRAG; f++) {
        bool subsampled = (f == 0);
        uint8_t ivs[NSAMP][IV_SIZE];
        for (int s = 0; s < NSAMP; s++)
            for (int i = 0; i < IV_SIZE; i++) ivs[s][i] = (uint8_t)(f * 16 + s);

        size_t moof = open_box(seg, &n, "moof");
        size_t mfhd = open_box(seg, &n, "mfhd");
        PUT32(seg + n, 0); PUT32(seg + n + 4, (uint32_t)f + 1); n += 8;
        close_box(seg, n, mfhd);

        size_t traf = open_box(seg, &n, "traf");
        size_t tfhd = open_box(seg, &n, "tfhd");
        PUT32(seg + n, 0x020000);  // default-base-is-moof, no optional fields
        PUT32(seg + n + 4, 1); n += 8;
        close_box(seg, n, tfhd);

        // trun: data-offset-present | sample-size-present
        size_t trun = open_box(seg, &n, "trun");
        size_t trun_data_offset_at = n + 8;
        PUT32(seg + n, 0x000201); PUT32(seg + n + 4, NSAMP); PUT32(seg + n + 8, 0); n += 12;
        for (int s = 0; s < NSAMP; s++) { PUT32(seg + n, SAMPLE_LEN); n += 4; }
        close_box(seg, n, trun);

        size_t saiz = open_box(seg, &n, "saiz");
        PUT32(seg + n, 0); n += 4;
        seg[n++] = (uint8_t)(IV_SIZE + (subsampled ? 2 + 6 : 0));  // default_sample_info_size
        PUT32(seg + n, NSAMP); n += 4;
        close_box(seg, n, saiz);

        size_t saio = open_box(seg, &n, "saio");
        PUT32(seg + n, 0); PUT32(seg + n + 4, 1); PUT32(seg + n + 8, 0); n += 12;
        close_box(seg, n, saio);

        size_t senc = open_box(seg, &n, "senc");
        PUT32(seg + n, subsampled ? 0x000002 : 0); PUT32(seg + n + 4, NSAMP); n += 8;
        for (int s = 0; s < NSAMP; s++) {
            memcpy(seg + n, ivs[s], IV_SIZE); n += IV_SIZE;
            if (subsampled) {
                seg[n] = 0; seg[n + 1] = 1; n += 2;                       // subsample_count
                seg[n] = 0; seg[n + 1] = CLEAR_PREFIX; n += 2;            // bytes of clear data
                PUT32(seg + n, SAMPLE_LEN - CLEAR_PREFIX); n += 4;        // bytes of protected data
            }
        }
        close_box(seg, n, senc);
        close_box(seg, n, traf);
        close_box(seg, n, moof);

        size_t mdat = open_box(seg, &n, "mdat");
        PUT32(seg + trun_data_offset_at, (uint32_t)(n - moof));  // trun data_offset is moof-relative
        mdat_payload[f] = n;
        for (int s = 0; s < NSAMP; s++) {
            for (int i = 0; i < SAMPLE_LEN; i++) plain[f][s][i] = (uint8_t)(f * 40 + s * 7 + i);
            memcpy(seg + n, plain[f][s], SAMPLE_LEN);
            // Encrypt exactly the protected span, the way a packager would.
            rs_aes_ctr ctx;
            rs_aes_ctr_init(&ctx, key, sizeof key, ivs[s], IV_SIZE);
            size_t clear = subsampled ? CLEAR_PREFIX : 0;
            rs_aes_ctr_process(&ctx, seg + n + clear, SAMPLE_LEN - clear);
            n += SAMPLE_LEN;
        }
        close_box(seg, n, mdat);
    }

    size_t out_len = 0;
    uint8_t *out = rs_cenc_decrypt_segment(seg, n, &out_len, key, IV_SIZE);
    check("cenc/multifrag-length", out != NULL && out_len == n);
    if (!out) return;

    for (int f = 0; f < NFRAG; f++) {
        char name[64];
        bool samples_clear = true;
        for (int s = 0; s < NSAMP; s++)
            if (memcmp(out + mdat_payload[f] + (size_t)s * SAMPLE_LEN, plain[f][s], SAMPLE_LEN) != 0)
                samples_clear = false;
        snprintf(name, sizeof name, "cenc/multifrag-samples-clear-frag%d", f);
        check(name, samples_clear);

        // Signalling that survives decryption makes the fragment self-
        // contradictory, and Chrome rejects it outright.
        size_t tc = 0;
        bool signalling_gone = true;
        for (size_t i = 0; i + 8 <= out_len; ) {
            uint32_t sz = ((uint32_t)out[i] << 24) | ((uint32_t)out[i + 1] << 16) |
                          ((uint32_t)out[i + 2] << 8) | out[i + 3];
            if (sz < 8) break;
            if (memcmp(out + i + 4, "moof", 4) == 0) {
                if (tc++ == (size_t)f) {
                    for (size_t j = i; j + 8 <= i + sz; j++)
                        if (memcmp(out + j + 4, "senc", 4) == 0 || memcmp(out + j + 4, "saiz", 4) == 0 ||
                            memcmp(out + j + 4, "saio", 4) == 0)
                            signalling_gone = false;
                }
            }
            i += sz;
        }
        snprintf(name, sizeof name, "cenc/multifrag-signalling-cleared-frag%d", f);
        check(name, signalling_gone);
    }
    rs_free(out);
}

// The DASH describe call is anchored at the live edge: it returns the newest N
// segments. A fixed N only covers N * segment_duration seconds of source
// output, so any poll cycle slower than that permanently loses everything
// older than the window — the engine then publishes less media than real time,
// its advertised window shrinks until players stall, and every hole shows up as
// an EXT-X-DISCONTINUITY indistinguishable from ordinary ad-break splicing.
//
// The invariant that has to hold: the window always covers the elapsed time.
// --- MPEG-TS muxer ----------------------------------------------------------
//
// Builds a minimal but structurally real fMP4 (an init with an avcC carrying
// one SPS and one PPS, then fragments of length-prefixed NALs) and checks the
// bytes that come out are a stream a decoder can actually follow: the packet
// grid, the tables, the PES timestamps, and the two indicators that make a live
// stream joinable and splice-tolerant.

// Reads the PID out of a TS packet header.
static uint16_t ts_pid(const uint8_t *pkt) {
    return (uint16_t)(((pkt[1] & 0x1Fu) << 8) | pkt[2]);
}

// The adaptation field's flags byte, or 0 when the packet has no adaptation
// field (or an empty one).
static uint8_t ts_af_flags(const uint8_t *pkt) {
    if (!((pkt[3] >> 4) & 0x2u)) return 0;
    return pkt[4] > 0 ? pkt[5] : 0u;
}

// Writes an init segment for one track. `video` picks an avc1/avcC sample entry
// with parameter sets, otherwise an mp4a/esds with an AudioSpecificConfig.
static size_t build_init(uint8_t *b, bool video, uint32_t track_id, uint32_t timescale) {
    size_t n = 0;
    size_t ftyp = open_box(b, &n, "ftyp");
    memcpy(b + n, "isom", 4); n += 4;
    PUT32(b + n, 0); n += 4;
    close_box(b, n, ftyp);

    size_t moov = open_box(b, &n, "moov");
    size_t trak = open_box(b, &n, "trak");

    size_t tkhd = open_box(b, &n, "tkhd");
    PUT32(b + n, 0); n += 4;                      // version 0 + flags
    PUT32(b + n, 0); PUT32(b + n + 4, 0); n += 8; // creation / modification
    PUT32(b + n, track_id); n += 4;
    PUT32(b + n, 0); PUT32(b + n + 4, 0); n += 8; // reserved + duration
    close_box(b, n, tkhd);

    size_t mdia = open_box(b, &n, "mdia");
    size_t mdhd = open_box(b, &n, "mdhd");
    PUT32(b + n, 0); n += 4;
    PUT32(b + n, 0); PUT32(b + n + 4, 0); n += 8;
    PUT32(b + n, timescale); n += 4;
    PUT32(b + n, 0); n += 4;                      // duration
    PUT32(b + n, 0); n += 4;                      // language + pre_defined
    close_box(b, n, mdhd);

    size_t minf = open_box(b, &n, "minf");
    size_t stbl = open_box(b, &n, "stbl");
    size_t stsd = open_box(b, &n, "stsd");
    PUT32(b + n, 0); n += 4;                      // version + flags
    PUT32(b + n, 1); n += 4;                      // entry_count

    if (video) {
        size_t avc1 = open_box(b, &n, "avc1");
        memset(b + n, 0, 6); n += 6;              // reserved
        b[n] = 0; b[n + 1] = 1; n += 2;           // data_reference_index
        memset(b + n, 0, 70); n += 70;            // VisualSampleEntry body
        size_t avcC = open_box(b, &n, "avcC");
        b[n++] = 1;                               // configurationVersion
        b[n++] = 0x64; b[n++] = 0x00; b[n++] = 0x1E;
        b[n++] = 0xFF;                            // lengthSizeMinusOne = 3 -> 4 bytes
        b[n++] = 0xE1;                            // numOfSPS = 1
        b[n++] = 0; b[n++] = 4;                   // SPS length
        b[n++] = 0x67; b[n++] = 0x64; b[n++] = 0x00; b[n++] = 0x1E;
        b[n++] = 1;                               // numOfPPS
        b[n++] = 0; b[n++] = 3;                   // PPS length
        b[n++] = 0x68; b[n++] = 0xEE; b[n++] = 0x3C;
        close_box(b, n, avcC);
        close_box(b, n, avc1);
    } else {
        size_t mp4a = open_box(b, &n, "mp4a");
        memset(b + n, 0, 6); n += 6;
        b[n] = 0; b[n + 1] = 1; n += 2;           // data_reference_index
        memset(b + n, 0, 20); n += 20;            // AudioSampleEntry body (version 0)
        size_t esds = open_box(b, &n, "esds");
        PUT32(b + n, 0); n += 4;                  // version + flags
        b[n++] = 0x03; b[n++] = 0x19;             // ES_Descriptor
        b[n++] = 0; b[n++] = 1;                   // ES_ID
        b[n++] = 0x00;                            // flags: no dependency/URL/OCR
        b[n++] = 0x04; b[n++] = 0x11;             // DecoderConfigDescriptor
        b[n++] = 0x40; b[n++] = 0x15;             // AAC, audio stream
        memset(b + n, 0, 11); n += 11;            // bufferSize + bitrates
        b[n++] = 0x05; b[n++] = 0x02;             // DecSpecificInfo, 2 bytes
        // AudioSpecificConfig: AAC-LC (2), 48 kHz (index 3), stereo (2).
        b[n++] = 0x11; b[n++] = 0x90;
        close_box(b, n, esds);
        close_box(b, n, mp4a);
    }
    close_box(b, n, stsd);
    close_box(b, n, stbl);
    close_box(b, n, minf);
    close_box(b, n, mdia);
    close_box(b, n, trak);
    close_box(b, n, moov);
    return n;
}

// One fragment of `nsamp` samples, each a single length-prefixed NAL of
// `sample_len` bytes of payload, starting at decode time `base_time`.
static size_t build_frag(uint8_t *b, uint32_t track_id, uint64_t base_time,
                         int nsamp, uint32_t duration, size_t payload_len, bool key) {
    size_t n = 0;
    const size_t sample_len = payload_len + 4;  // 4-byte AVCC length prefix

    size_t moof = open_box(b, &n, "moof");
    size_t mfhd = open_box(b, &n, "mfhd");
    PUT32(b + n, 0); PUT32(b + n + 4, 1); n += 8;
    close_box(b, n, mfhd);

    size_t traf = open_box(b, &n, "traf");
    size_t tfhd = open_box(b, &n, "tfhd");
    PUT32(b + n, 0x020000);                       // default-base-is-moof
    PUT32(b + n + 4, track_id); n += 8;
    close_box(b, n, tfhd);

    size_t tfdt = open_box(b, &n, "tfdt");
    b[n] = 1; b[n + 1] = 0; b[n + 2] = 0; b[n + 3] = 0; n += 4;  // version 1
    PUT32(b + n, (uint32_t)(base_time >> 32));
    PUT32(b + n + 4, (uint32_t)base_time); n += 8;
    close_box(b, n, tfdt);

    // trun: data-offset + first-sample-flags + duration + size + flags
    size_t trun = open_box(b, &n, "trun");
    PUT32(b + n, 0x000705); n += 4;
    PUT32(b + n, (uint32_t)nsamp); n += 4;
    size_t data_offset_at = n;
    PUT32(b + n, 0); n += 4;                      // patched once mdat is placed
    // first_sample_flags: sample_is_non_sync_sample clear on a keyframe.
    PUT32(b + n, key ? 0x02000000u : 0x01010000u); n += 4;
    for (int s = 0; s < nsamp; s++) {
        PUT32(b + n, duration); n += 4;
        PUT32(b + n, (uint32_t)sample_len); n += 4;
        PUT32(b + n, (s == 0 && key) ? 0x02000000u : 0x01010000u); n += 4;
    }
    close_box(b, n, trun);
    close_box(b, n, traf);
    close_box(b, n, moof);

    size_t mdat = open_box(b, &n, "mdat");
    PUT32(b + data_offset_at, (uint32_t)(n - moof));  // relative to the moof
    for (int s = 0; s < nsamp; s++) {
        PUT32(b + n, (uint32_t)payload_len); n += 4;
        b[n] = key && s == 0 ? 0x65 : 0x41;           // IDR / non-IDR NAL header
        for (size_t i = 1; i < payload_len; i++) b[n + i] = (uint8_t)(s + i);
        n += payload_len;
    }
    close_box(b, n, mdat);
    return n;
}

static void test_mpegts(void) {
    // The MPEG-2 section CRC over a known PAT body. Same polynomial ffmpeg and
    // every broadcast muxer stamp their sections with; a wrong CRC makes every
    // player discard the table and see an empty program.
    static const uint8_t pat[] = {
        0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xF0, 0x00
    };
    check("mpegts/crc32", rs_ts_crc32(pat, sizeof pat) == 0x2AB104B2u);

    static uint8_t vinit[1024], ainit[1024], frag[8192];
    size_t vlen = build_init(vinit, true, 1, 90000);
    size_t alen = build_init(ainit, false, 1, 48000);

    rs_ts_mux *m = rs_ts_mux_create();
    int vt = rs_ts_mux_add_track(m, "video", vinit, vlen);
    int at = rs_ts_mux_add_track(m, "audio", ainit, alen);
    check("mpegts/add video track", vt == 0);
    check("mpegts/add audio track", at == 1);
    check("mpegts/reject garbage init", rs_ts_mux_add_track(m, "video", vinit, 8) < 0);

    // Two seconds of each track, video first sample a keyframe.
    size_t n = build_frag(frag, 1, 0, 4, 22500, 64, true);
    check("mpegts/push video", rs_ts_mux_push(m, vt, frag, n) == 4);
    n = build_frag(frag, 1, 0, 4, 12000, 32, true);
    check("mpegts/push audio", rs_ts_mux_push(m, at, frag, n) == 4);

    size_t out_len = 0;
    uint8_t *out = rs_ts_mux_take(m, true, &out_len);
    check("mpegts/produced output", out != NULL && out_len > 0);
    if (!out) { rs_ts_mux_destroy(m); return; }

    check("mpegts/188-byte grid", out_len % RS_TS_PACKET_SIZE == 0);
    bool sync_ok = true, cc_ok = true, saw_pat = false, saw_pmt = false;
    bool saw_video_pes = false, saw_audio_pes = false, saw_pcr = false, saw_rai = false;
    uint8_t expect_cc[0x2000];
    bool seen_pid[0x2000];
    memset(expect_cc, 0, sizeof expect_cc);
    memset(seen_pid, 0, sizeof seen_pid);
    uint8_t stream_type_video = 0, stream_type_audio = 0;

    for (size_t off = 0; off + RS_TS_PACKET_SIZE <= out_len; off += RS_TS_PACKET_SIZE) {
        const uint8_t *pkt = out + off;
        if (pkt[0] != 0x47) { sync_ok = false; continue; }
        uint16_t pid = ts_pid(pkt);
        uint8_t cc = pkt[3] & 0x0Fu;
        if (seen_pid[pid] && cc != expect_cc[pid]) cc_ok = false;
        seen_pid[pid] = true;
        expect_cc[pid] = (uint8_t)((cc + 1) & 0x0Fu);

        uint8_t af = ts_af_flags(pkt);
        if (af & 0x10u) saw_pcr = true;
        if (af & 0x40u) saw_rai = true;

        bool pusi = (pkt[1] & 0x40u) != 0;
        size_t payload = 4;
        if ((pkt[3] >> 4) & 0x2u) payload += (size_t)pkt[4] + 1;
        if (pid == 0x0000 && pusi) {
            saw_pat = true;
            // pointer_field, then the PAT: program 1 must point at the PMT PID.
            const uint8_t *sec = pkt + payload + 1;
            check("mpegts/pat table id", sec[0] == 0x00);
            uint16_t pmt_pid = (uint16_t)(((sec[10] & 0x1Fu) << 8) | sec[11]);
            check("mpegts/pat points at pmt", pmt_pid == 0x1000);
        }
        if (pid == 0x1000 && pusi) {
            saw_pmt = true;
            const uint8_t *sec = pkt + payload + 1;
            check("mpegts/pmt table id", sec[0] == 0x02);
            stream_type_video = sec[12];
            stream_type_audio = sec[17];
        }
        if (pusi && (pid == 0x0100 || pid == 0x0101)) {
            const uint8_t *pes = pkt + payload;
            check("mpegts/pes start code",
                  pes[0] == 0x00 && pes[1] == 0x00 && pes[2] == 0x01);
            if (pid == 0x0100) {
                saw_video_pes = true;
                check("mpegts/video stream id", pes[3] == 0xE0);
            } else {
                saw_audio_pes = true;
                check("mpegts/audio stream id", pes[3] == 0xC0);
                // ADTS sync word right after the PES header.
                size_t hdr = 9 + (size_t)pes[8];
                check("mpegts/adts syncword",
                      pes[hdr] == 0xFF && (pes[hdr + 1] & 0xF0u) == 0xF0u);
                check("mpegts/adts sampling index", ((pes[hdr + 2] >> 2) & 0x0Fu) == 3);
            }
        }
    }
    check("mpegts/sync bytes", sync_ok);
    check("mpegts/continuity counters", cc_ok);
    check("mpegts/pat present", saw_pat);
    check("mpegts/pmt present", saw_pmt);
    check("mpegts/h264 stream type", stream_type_video == 0x1B);
    check("mpegts/aac stream type", stream_type_audio == 0x0F);
    check("mpegts/video pes", saw_video_pes);
    check("mpegts/audio pes", saw_audio_pes);
    check("mpegts/pcr emitted", saw_pcr);
    check("mpegts/random access flagged", saw_rai);

    // A keyframe is a join point, and the parameter sets from the avcC must be
    // in the elementary stream there — a viewer that starts at one has never
    // seen the init segment.
    const size_t *joins = NULL;
    check("mpegts/join point recorded", rs_ts_mux_join_points(m, &joins) > 0);
    bool found_sps = false;
    for (size_t i = 0; i + 8 < out_len; i++)
        if (out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 0 && out[i + 3] == 1 &&
            out[i + 4] == 0x67) found_sps = true;
    check("mpegts/sps in stream", found_sps);
    rs_free(out);

    // CMAF low-latency packaging puts several moof+mdat chunks in one segment
    // (claro's DASH ships two per 2s segment). Parsing only the first drops
    // half the media without erroring — it presents as a periodic hole in the
    // output, so nothing catches it except a check that every sample arrives.
    {
        static uint8_t multi[8192];
        size_t a = build_frag(multi, 1, 90000ull * 100, 4, 22500, 64, true);
        size_t b = build_frag(multi + a, 1, 90000ull * 100 + 90000, 4, 22500, 64, false);
        rs_ts_mux *cm = rs_ts_mux_create();
        static uint8_t vi2[1024];
        size_t vl2 = build_init(vi2, true, 1, 90000);
        int ct = rs_ts_mux_add_track(cm, "video", vi2, vl2);
        check("mpegts/chunked: both chunks parsed",
              rs_ts_mux_push(cm, ct, multi, a + b) == 8);
        rs_ts_mux_destroy(cm);
    }

    // A timeline jump (a splice, or segments the engine skipped) has to be
    // flagged, or a decoder reads it as a broken clock.
    n = build_frag(frag, 1, 90000ull * 30, 4, 22500, 64, true);
    rs_ts_mux_push(m, vt, frag, n);
    n = build_frag(frag, 1, 48000ull * 30, 4, 12000, 32, true);
    rs_ts_mux_push(m, at, frag, n);
    out = rs_ts_mux_take(m, true, &out_len);
    bool saw_disc = false;
    if (out) {
        for (size_t off = 0; off + RS_TS_PACKET_SIZE <= out_len; off += RS_TS_PACKET_SIZE)
            if (ts_af_flags(out + off) & 0x80u) saw_disc = true;
        rs_free(out);
    }
    check("mpegts/discontinuity flagged", saw_disc);

    rs_ts_mux_destroy(m);
}

static void test_live_window(void) {
    // First poll of a representation: no backlog, so stay at the live edge
    // rather than dragging in the whole DVR buffer.
    check("live-window/first-poll", rs_live_window_size(8, 0, 2.0) == 8);
    check("live-window/negative-elapsed", rs_live_window_size(8, -5, 2.0) == 8);

    // Keeping up: elapsed time needs fewer segments than configured, so the
    // configured ask wins and behaviour is unchanged from before the fix.
    check("live-window/keeping-up", rs_live_window_size(8, 2.0, 2.0) == 8);

    // Falling behind: this is the case the old fixed count got wrong. A 30s
    // cycle over 2s segments means 15 segments were published; the ask has to
    // cover them, not stay at 8.
    check("live-window/slow-cycle", rs_live_window_size(8, 30.0, 2.0) == 15 + 1 + RS_LIVE_WINDOW_MARGIN);

    // The whole point: whatever the numbers, the window covers the elapsed
    // time. Sweep well past the point where the old constant broke down.
    bool covers = true;
    for (double elapsed = 0.5; elapsed <= 100.0; elapsed += 0.5) {
        for (double dur = 1.0; dur <= 6.0; dur += 0.5) {
            int n = rs_live_window_size(8, elapsed, dur);
            if (n >= RS_LIVE_MAX_WINDOW) continue;  // clamped, checked separately
            if ((double)n * dur < elapsed) covers = false;
        }
    }
    check("live-window/always-covers-elapsed", covers);

    // A long stall must not turn into an unbounded ask.
    check("live-window/clamped", rs_live_window_size(8, 100000.0, 2.0) == RS_LIVE_MAX_WINDOW);

    // Degenerate inputs must not divide by zero or return something unusable.
    check("live-window/zero-duration", rs_live_window_size(8, 30.0, 0) > 8);
    check("live-window/zero-ahead", rs_live_window_size(0, 0, 2.0) >= 1);
}

// The catch-up policy (live_window.c).
//
// rs_live_window_size deliberately asks for a WIDER window the later a poll is,
// so nothing is missed while the engine is merely a little behind. That is only
// safe because this function refuses to let the backlog grow without bound: it
// is what turns "we are behind" into "publish the live edge" instead of "try to
// fetch two minutes of history at 1x".
static void test_live_catch_up(void) {
    // Keeping up: everything new fits, nothing is thrown away.
    check("live-catchup/keeping-up", rs_live_catch_up_drop(8, 0, 4) == 0);
    check("live-catchup/exactly-full", rs_live_catch_up_drop(8, 0, 8) == 0);
    check("live-catchup/partly-occupied", rs_live_catch_up_drop(8, 5, 3) == 0);
    check("live-catchup/nothing-new", rs_live_catch_up_drop(8, 8, 0) == 0);

    // Behind: only the newest `depth` are kept.
    check("live-catchup/overflow", rs_live_catch_up_drop(8, 0, 20) == 12);
    check("live-catchup/queue-partly-full", rs_live_catch_up_drop(8, 6, 10) == 8);
    // Queue already full — this poll's entire find is discarded, which is
    // exactly the "snap to the live edge" case.
    check("live-catchup/queue-full", rs_live_catch_up_drop(8, 8, 5) == 5);
    check("live-catchup/queue-overfull", rs_live_catch_up_drop(8, 99, 5) == 5);

    // The invariant that matters: after applying the drop, what is actually
    // taken never pushes the queue past `depth`, for any combination. This is
    // the property the spiral violated — there was no bound at all.
    bool bounded = true, keeps_newest = true;
    for (int depth = 1; depth <= 40; depth++) {
        for (int queued = 0; queued <= depth + 5; queued++) {
            for (int fresh = 0; fresh <= 80; fresh++) {
                int drop = rs_live_catch_up_drop(depth, queued, fresh);
                if (drop < 0 || drop > fresh) { bounded = false; continue; }
                int taken = fresh - drop;
                if (queued + taken > depth && taken > 0) bounded = false;
                // Never discard anything while there is room for it: dropping
                // is a last resort, not a throttle.
                if (queued + fresh <= depth && drop != 0) keeps_newest = false;
            }
        }
    }
    check("live-catchup/never-exceeds-depth", bounded);
    check("live-catchup/only-drops-when-full", keeps_newest);

    // Degenerate inputs.
    check("live-catchup/zero-depth", rs_live_catch_up_drop(0, 0, 3) <= 3);
    check("live-catchup/negative-fresh", rs_live_catch_up_drop(8, 0, -1) == 0);
}

// Backoff after a failed manifest poll (live_backoff.c).
//
// The invariant that has to hold: a run of failures must make the next poll
// strictly later than the normal cadence would, and a 429 must never be retried
// on anything like the poll period — those two together are what kept a
// rate-limited origin refusing a real stream for 364 seconds.
static void test_live_backoff(void) {
    // A poll that succeeded is not backed off at all: the caller keeps its
    // normal period. This is the overwhelmingly common case and it must be
    // exactly the old behaviour.
    check("live-backoff/success", rs_live_backoff_delay(0, 0, 2.0) == 0);
    check("live-backoff/success-throttle-flag-irrelevant", rs_live_backoff_delay(0, 1, 2.0) == 0);
    check("live-backoff/negative", rs_live_backoff_delay(-3, 0, 2.0) == 0);

    // Plain failures double from the poll period.
    check("live-backoff/first", rs_live_backoff_delay(1, 0, 2.0) == 2.0);
    check("live-backoff/second", rs_live_backoff_delay(2, 0, 2.0) == 4.0);
    check("live-backoff/third", rs_live_backoff_delay(3, 0, 2.0) == 8.0);

    // An explicit rate refusal starts at the floor instead — retrying a 429
    // two seconds later is the behaviour that caused the problem.
    check("live-backoff/throttled-first",
          rs_live_backoff_delay(1, 1, 2.0) == RS_LIVE_THROTTLE_BACKOFF_MIN);
    check("live-backoff/throttled-beats-period",
          rs_live_backoff_delay(1, 1, 2.0) > rs_live_backoff_delay(1, 0, 2.0));

    // The floor is a floor, not an override: once doubling exceeds it, the
    // larger value wins.
    check("live-backoff/throttled-grows", rs_live_backoff_delay(6, 1, 2.0) > RS_LIVE_THROTTLE_BACKOFF_MIN);

    // Never unbounded, however long it has been failing — the stream still has
    // to notice the origin coming back.
    check("live-backoff/capped", rs_live_backoff_delay(500, 0, 2.0) == RS_LIVE_BACKOFF_MAX);
    check("live-backoff/capped-throttled", rs_live_backoff_delay(500, 1, 10.0) == RS_LIVE_BACKOFF_MAX);
    check("live-backoff/no-overflow", rs_live_backoff_delay(1000000, 0, 2.0) == RS_LIVE_BACKOFF_MAX);

    // Monotonic and always positive across a long run of failures: a later
    // failure may never be retried sooner than an earlier one.
    bool monotonic = true, positive = true, bounded = true;
    double prev = 0;
    for (int n = 1; n <= 200; n++) {
        double d = rs_live_backoff_delay(n, 0, 2.0);
        if (d < prev) monotonic = false;
        if (d <= 0) positive = false;
        if (d > RS_LIVE_BACKOFF_MAX) bounded = false;
        prev = d;
    }
    check("live-backoff/monotonic", monotonic);
    check("live-backoff/positive", positive);
    check("live-backoff/bounded", bounded);

    // A zero/absent period must not collapse the backoff to nothing.
    check("live-backoff/zero-period", rs_live_backoff_delay(1, 0, 0) > 0);

    // Which statuses count as "you are asking too often".
    check("live-backoff/429-throttles", rs_live_status_is_throttle(429));
    check("live-backoff/503-throttles", rs_live_status_is_throttle(503));
    check("live-backoff/403-throttles", rs_live_status_is_throttle(403));
    check("live-backoff/404-does-not", !rs_live_status_is_throttle(404));
    check("live-backoff/500-does-not", !rs_live_status_is_throttle(500));
    check("live-backoff/200-does-not", !rs_live_status_is_throttle(200));
    check("live-backoff/unknown-does-not", !rs_live_status_is_throttle(0));
}

// TTML -> WebVTT. Unlike the modules above, this one has no Swift original to
// capture goldens from, so the expectations are read off the TTML1 timing rules
// (section 10.3.1) and the WebVTT syntax directly.
static void test_ttml(void) {
    // Namespace-prefixed elements, clock times, a hard break, entities, and the
    // one styling case WebVTT can express. Every one of these is something a
    // real broadcast packager emits.
    const char *doc =
        "<?xml version=\"1.0\"?>"
        "<tt:tt xmlns:tt=\"http://www.w3.org/ns/ttml\" xmlns:tts=\"http://www.w3.org/ns/ttml#styling\">"
        "<tt:head><tt:styling/></tt:head><tt:body><tt:div>"
        "<tt:p begin=\"00:00:10.500\" end=\"00:00:13\">Hello   &amp; goodbye<tt:br/>second line</tt:p>"
        "<tt:p begin=\"00:00:14\" end=\"00:00:16\">a <tt:span tts:fontStyle=\"italic\">whisper</tt:span> here</tt:p>"
        "<tt:p begin=\"00:00:17\" dur=\"2s\">5 &lt; 6</tt:p>"
        "</tt:div></tt:body></tt:tt>";
    char *vtt = rs_ttml_to_webvtt((const uint8_t *)doc, strlen(doc), -1, 0);
    check("ttml/parsed", vtt != NULL);
    if (vtt) {
        check("ttml/header", strncmp(vtt, "WEBVTT\n", 7) == 0);
        check("ttml/no-timestamp-map-without-start", strstr(vtt, "X-TIMESTAMP-MAP") == NULL);
        check("ttml/clock-time", strstr(vtt, "00:00:10.500 --> 00:00:13.000") != NULL);
        // Whitespace collapses (xml:space="default"), <br/> is a hard newline,
        // and a decoded '&' is re-escaped because WebVTT reads it as markup.
        check("ttml/collapse-and-break", strstr(vtt, "Hello &amp; goodbye\nsecond line") != NULL);
        // The space before a styled span belongs outside the tag.
        check("ttml/italic-span", strstr(vtt, "a <i>whisper</i> here") != NULL);
        check("ttml/dur", strstr(vtt, "00:00:17.000 --> 00:00:19.000") != NULL);
        check("ttml/lt-escaped", strstr(vtt, "5 &lt; 6") != NULL);
        rs_free(vtt);
    }

    // Offset times against ttp:tickRate, plus a <div begin> that shifts its
    // children (TTML's default time container is `par`).
    const char *ticks =
        "<tt xmlns=\"http://www.w3.org/ns/ttml\" ttp:tickRate=\"10000000\"><body><div begin=\"5s\">"
        "<p begin=\"10000000t\" end=\"30000000t\">ticks</p></div></body></tt>";
    vtt = rs_ttml_to_webvtt((const uint8_t *)ticks, strlen(ticks), -1, 0);
    check("ttml/tick-rate-and-container-offset",
          vtt && strstr(vtt, "00:00:06.000 --> 00:00:08.000") != NULL);
    rs_free(vtt);

    // "hh:mm:ss:ff" counts frames in the fourth field, and
    // frameRateMultiplier turns 30 into 29.97 — worth a second of drift over
    // half an hour if it is ignored.
    const char *frames =
        "<tt ttp:frameRate=\"30\" ttp:frameRateMultiplier=\"1000 1001\"><body><div>"
        "<p begin=\"00:00:01:15\" end=\"00:00:02:00\">frames</p></div></body></tt>";
    vtt = rs_ttml_to_webvtt((const uint8_t *)frames, strlen(frames), -1, 0);
    check("ttml/frame-clock-time", vtt && strstr(vtt, "00:00:01.501 --> 00:00:02.000") != NULL);
    rs_free(vtt);

    // stpp carriage: the document arrives inside an fMP4 fragment's mdat, and
    // the segment's own start becomes the X-TIMESTAMP-MAP anchor.
    const char *inner =
        "<tt xmlns=\"http://www.w3.org/ns/ttml\"><body><div>"
        "<p begin=\"00:01:40.000\" end=\"00:01:42.000\">wrapped</p></div></body></tt>";
    size_t inner_len = strlen(inner);
    uint8_t frag[512];
    size_t o = 0;
    memcpy(frag + o, "\0\0\0\10styp", 8); o += 8;
    memcpy(frag + o, "\0\0\0\10moof", 8); o += 8;
    uint32_t mdat_size = (uint32_t)(8 + inner_len);
    frag[o++] = (uint8_t)(mdat_size >> 24); frag[o++] = (uint8_t)(mdat_size >> 16);
    frag[o++] = (uint8_t)(mdat_size >> 8);  frag[o++] = (uint8_t)mdat_size;
    memcpy(frag + o, "mdat", 4); o += 4;
    memcpy(frag + o, inner, inner_len); o += inner_len;
    vtt = rs_ttml_to_webvtt(frag, o, 100.0, 2.0);
    check("ttml/stpp-mdat", vtt && strstr(vtt, "00:01:40.000 --> 00:01:42.000") != NULL);
    // 100s at 90 kHz, and the same instant as a local clock time.
    check("ttml/timestamp-map",
          vtt && strstr(vtt, "X-TIMESTAMP-MAP=MPEGTS:9000000,LOCAL:00:01:40.000") != NULL);
    rs_free(vtt);

    // The same fragment authored from zero instead of on the presentation
    // timeline — cues bounded by one segment while the segment starts much
    // later can only be fragment-relative, so they are lifted onto it.
    const char *rel = "<tt><body><div><p begin=\"0.5s\" end=\"1.5s\">rel</p></div></body></tt>";
    vtt = rs_ttml_to_webvtt((const uint8_t *)rel, strlen(rel), 100.0, 2.0);
    check("ttml/fragment-relative-lift",
          vtt && strstr(vtt, "00:01:40.500 --> 00:01:41.500") != NULL);
    rs_free(vtt);

    // A text/vtt rendition passes through this same call untouched.
    const char *already = "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nhi\n";
    vtt = rs_ttml_to_webvtt((const uint8_t *)already, strlen(already), 5.0, 2.0);
    check_str("ttml/webvtt-passthrough", vtt, already);
    rs_free(vtt);

    // An empty subtitle fragment is normal and must still render a valid
    // document; input that is not timed text at all must be refused, so the
    // caller can fall back to serving the original bytes.
    const char *empty = "<tt><body><div/></body></tt>";
    vtt = rs_ttml_to_webvtt((const uint8_t *)empty, strlen(empty), -1, 0);
    check("ttml/empty-fragment", vtt && strncmp(vtt, "WEBVTT", 6) == 0);
    rs_free(vtt);
    vtt = rs_ttml_to_webvtt((const uint8_t *)"not xml", 7, -1, 0);
    check("ttml/rejects-non-ttml", vtt == NULL);
    rs_free(vtt);

    // A comment ends at "-->", not at the next '>', or parsing resumes inside
    // it — and "-->" is also the WebVTT cue arrow, so getting this wrong
    // corrupts output rather than merely dropping it.
    const char *cm = "<tt><body><div><!-- a > b --><p begin=\"1s\" end=\"2s\">ok</p></div></body></tt>";
    vtt = rs_ttml_to_webvtt((const uint8_t *)cm, strlen(cm), -1, 0);
    check("ttml/comment-with-gt",
          vtt && strstr(vtt, "00:00:01.000 --> 00:00:02.000\nok\n") != NULL);
    rs_free(vtt);

    // Cues with no begin are skipped rather than guessed at.
    const char *nobegin = "<tt><body><div><p>floating</p><p begin=\"1s\" end=\"2s\">kept</p></div></body></tt>";
    vtt = rs_ttml_to_webvtt((const uint8_t *)nobegin, strlen(nobegin), -1, 0);
    check("ttml/skips-untimed-cue", vtt && strstr(vtt, "floating") == NULL);
    check("ttml/keeps-timed-cue", vtt && strstr(vtt, "kept") != NULL);
    rs_free(vtt);
}

int main(void) {
    test_sha256();
    test_hmac();
    test_pbkdf2();
    test_aes_blocks();
    test_aes_ctr();
    test_aes_cbc();
    test_ffargs_helpers();
    test_ffargs_subtitles();
    test_json();
    test_auth();
    test_auth_sessions();
    test_auth_throttle();
    test_netmatch();
    test_panel();
    test_cenc_multifragment();
    test_mpegts();
    test_live_window();
    test_live_catch_up();
    test_live_backoff();
    test_ttml();

    for (size_t i = 0; i < sizeof(rs_goldens) / sizeof(rs_goldens[0]); i++) {
        run_golden(&rs_goldens[i]);
    }

    if (failures > 0) {
        fprintf(stderr, "\nself-test FAILED: %d of %d checks\n", failures, checks_run);
        return 1;
    }
    printf("self-test: all %d checks PASS\n", checks_run);
    return 0;
}
