/* SPDX-License-Identifier: Apache-2.0
 * t5_pkg: inflate, zip reader, TOML subset, manifest, installer, grants.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "unity.h"
#include "t5_core/crc.h"
#include "t5_pkg/inflate.h"
#include "t5_pkg/manifest.h"
#include "t5_pkg/pkg.h"
#include "t5_pkg/toml.h"
#include "t5_pkg/zip.h"
#include "pkg_test.h"
#include "fixtures/pkg_fixtures.h"

static char s_dir[128];
static char s_sd[192], s_data[192], s_appdata[192];

static void sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sh(const char *fmt, ...)
{
    char cmd[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    TEST_ASSERT_EQUAL_MESSAGE(0, system(cmd), cmd);
}

void setUp(void)
{
    snprintf(s_dir, sizeof s_dir, "/tmp/t5_pkg_test_%d", (int)getpid());
    sh("rm -rf '%s' && mkdir -p '%s/sd' '%s/data'", s_dir, s_dir, s_dir);
    snprintf(s_sd, sizeof s_sd, "%s/sd/apps", s_dir);
    snprintf(s_data, sizeof s_data, "%s/data/apps", s_dir);
    snprintf(s_appdata, sizeof s_appdata, "%s/data/appdata", s_dir);
    t5_pkg_set_roots(s_sd, s_data, s_appdata);
}

void tearDown(void)
{
    sh("rm -rf '%s'", s_dir);
}

static const char *fixture(const char *name, const unsigned char *data, unsigned len)
{
    static char path[8][256];
    static int slot;
    char *p = path[slot++ % 8];
    snprintf(p, 256, "%s/%s", s_dir, name);
    FILE *f = fopen(p, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(len, fwrite(data, 1, len, f));
    fclose(f);
    return p;
}
#define FIXTURE(n) fixture(#n, n, n##_len)

static bool exists(const char *fmt, const char *a, const char *b)
{
    char p[512];
    snprintf(p, sizeof p, fmt, a, b);
    return access(p, F_OK) == 0;
}

static bool file_crc(const char *path, uint32_t *crc, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[4096];
    size_t n;
    *crc = 0;
    *len = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        *crc = t5_crc32(*crc, buf, n);
        *len += n;
    }
    fclose(f);
    return true;
}

/* ---- inflate ------------------------------------------------------------- */

static void check_stream(const unsigned char *src, unsigned len, uint32_t size, uint32_t crc)
{
    uint8_t *out = malloc(size + 16);
    size_t n = 12345;
    TEST_ASSERT_EQUAL(T5_OK, t5_inflate_mem(src, len, out, size + 16, &n));
    TEST_ASSERT_EQUAL(size, n);
    TEST_ASSERT_EQUAL_HEX32(crc, t5_crc32(0, out, n));
    free(out);
}

static void test_inflate_streams(void)
{
    check_stream(fx_def_hello, fx_def_hello_len, FX_DEF_HELLO_SIZE, FX_DEF_HELLO_CRC);
    check_stream(fx_def_fixed, fx_def_fixed_len, FX_DEF_FIXED_SIZE, FX_DEF_FIXED_CRC);
    check_stream(fx_def_stored, fx_def_stored_len, FX_DEF_STORED_SIZE, FX_DEF_STORED_CRC);
    check_stream(fx_def_medium, fx_def_medium_len, FX_DEF_MEDIUM_SIZE, FX_DEF_MEDIUM_CRC);
    check_stream(fx_def_big, fx_def_big_len, FX_DEF_BIG_SIZE, FX_DEF_BIG_CRC);
    check_stream(fx_def_empty, fx_def_empty_len, 0, 0);

    char text[128] = {0};
    size_t n;
    TEST_ASSERT_EQUAL(T5_OK, t5_inflate_mem(fx_def_hello, fx_def_hello_len, text, sizeof text - 1, &n));
    TEST_ASSERT_EQUAL_STRING(FX_DEF_HELLO_TEXT, text);
}

typedef struct {
    const uint8_t *p;
    size_t left;
    size_t chunk;
} drip_t;

static int drip_read(void *ctx, uint8_t *buf, size_t cap)
{
    drip_t *d = ctx;
    size_t n = d->left < d->chunk ? d->left : d->chunk;
    if (n > cap) n = cap;
    memcpy(buf, d->p, n);
    d->p += n;
    d->left -= n;
    return (int)n;
}

typedef struct {
    uint32_t crc;
    size_t len;
    int calls;
} crc_sink_t;

static t5_err_t crc_write(void *ctx, const uint8_t *data, size_t len)
{
    crc_sink_t *s = ctx;
    s->crc = t5_crc32(s->crc, data, len);
    s->len += len;
    s->calls++;
    return T5_OK;
}

static t5_err_t fail_write(void *ctx, const uint8_t *data, size_t len)
{
    return T5_ERR_IO;
}

static void test_inflate_streaming_big(void)
{
    /* One input byte at a time, output through the 32 KB window: >32 KB with
     * long back-references exercises the ring wrap. */
    size_t chunks[] = {1, 3, 1000, 1u << 20};
    for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        drip_t d = {fx_def_big, fx_def_big_len, chunks[i]};
        crc_sink_t s = {0};
        size_t n = 0;
        TEST_ASSERT_EQUAL(T5_OK, t5_inflate_stream(drip_read, &d, crc_write, &s, &n));
        TEST_ASSERT_EQUAL(FX_DEF_BIG_SIZE, n);
        TEST_ASSERT_EQUAL(FX_DEF_BIG_SIZE, s.len);
        TEST_ASSERT_EQUAL_HEX32(FX_DEF_BIG_CRC, s.crc);
        TEST_ASSERT_TRUE(s.calls >= 3);
    }
    drip_t d = {fx_def_big, fx_def_big_len, 4096};
    TEST_ASSERT_EQUAL(T5_ERR_IO, t5_inflate_stream(drip_read, &d, fail_write, NULL, NULL));
}

static void test_inflate_errors(void)
{
    uint8_t out[256];
    size_t n;
    /* Output too small. */
    TEST_ASSERT_EQUAL(T5_ERR_NO_MEM, t5_inflate_mem(fx_def_hello, fx_def_hello_len, out, 10, &n));
    /* Block type 3. */
    const uint8_t bad_type[] = {0x07, 0, 0, 0};
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_inflate_mem(bad_type, sizeof bad_type, out, sizeof out, &n));
    /* Stored block whose LEN/NLEN don't match. */
    const uint8_t bad_stored[] = {0x01, 0x05, 0x00, 0x00, 0x00, 'a', 'b', 'c', 'd', 'e'};
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_inflate_mem(bad_stored, sizeof bad_stored, out, sizeof out, &n));
    /* Empty input. */
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_inflate_mem(NULL, 0, out, sizeof out, &n));
    /* Every truncation of a valid stream fails cleanly. */
    for (unsigned cut = 0; cut < fx_def_hello_len; cut++) {
        TEST_ASSERT_NOT_EQUAL(T5_OK, t5_inflate_mem(fx_def_hello, cut, out, sizeof out, &n));
    }
    uint8_t *big = malloc(FX_DEF_BIG_SIZE);
    for (unsigned cut = 0; cut < fx_def_big_len; cut += 997) {
        TEST_ASSERT_NOT_EQUAL(T5_OK, t5_inflate_mem(fx_def_big, cut, big, FX_DEF_BIG_SIZE, &n));
    }
    free(big);
}

static void test_inflate_fuzz(void)
{
    srand(1234);
    size_t cap = FX_DEF_BIG_SIZE * 2;
    uint8_t *out = malloc(cap);
    uint8_t *buf = malloc(fx_def_big_len);
    const struct { const unsigned char *p; unsigned len; } src[] = {
        {fx_def_medium, fx_def_medium_len}, {fx_def_big, fx_def_big_len},
        {fx_def_fixed, fx_def_fixed_len}, {fx_def_stored, fx_def_stored_len},
    };
    int bad = 0;
    for (int it = 0; it < 1500; it++) {
        const unsigned char *p = src[it % 4].p;
        unsigned len = src[it % 4].len;
        memcpy(buf, p, len);
        int flips = 1 + rand() % 4;
        for (int f = 0; f < flips; f++) {
            buf[rand() % len] ^= (uint8_t)(1u << (rand() % 8));
        }
        if (rand() % 4 == 0) len = (unsigned)(rand() % len);
        size_t n;
        t5_err_t e = t5_inflate_mem(buf, len, out, cap, &n);
        TEST_ASSERT_TRUE(e == T5_OK || e == T5_ERR_INVALID_ARG || e == T5_ERR_NO_MEM);
        TEST_ASSERT_TRUE(n <= cap);
        bad += e != T5_OK;
    }
    for (int it = 0; it < 500; it++) {        /* pure noise */
        unsigned len = 1 + (unsigned)(rand() % 2000);
        for (unsigned i = 0; i < len; i++) buf[i] = (uint8_t)rand();
        size_t n;
        t5_err_t e = t5_inflate_mem(buf, len, out, cap, &n);
        TEST_ASSERT_TRUE(e == T5_OK || e == T5_ERR_INVALID_ARG || e == T5_ERR_NO_MEM);
    }
    TEST_ASSERT_TRUE(bad > 0);
    free(out);
    free(buf);
}

/* ---- zip ---------------------------------------------------------------- */

static void test_zip_list_and_extract_stored(void)
{
    t5_zip_t *z;
    TEST_ASSERT_EQUAL(T5_OK, t5_zip_open(FIXTURE(fx_zip_stored), &z));
    TEST_ASSERT_EQUAL(3, t5_zip_count(z));
    TEST_ASSERT_EQUAL_STRING("a.txt", t5_zip_entry(z, 0)->name);
    TEST_ASSERT_TRUE(t5_zip_entry(z, 1)->is_dir);
    TEST_ASSERT_EQUAL_STRING("dir/b.txt", t5_zip_entry(z, 2)->name);
    TEST_ASSERT_NULL(t5_zip_entry(z, 3));
    TEST_ASSERT_NULL(t5_zip_find(z, "nope"));
    char buf[32] = {0};
    size_t n;
    const t5_zip_entry_t *e = t5_zip_find(z, "a.txt");
    TEST_ASSERT_EQUAL(0, e->method);
    TEST_ASSERT_EQUAL(T5_OK, t5_zip_extract_mem(z, e, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL(6, n);
    TEST_ASSERT_EQUAL_STRING("alpha\n", buf);
    TEST_ASSERT_EQUAL(T5_ERR_NO_MEM, t5_zip_extract_mem(z, e, buf, 3, &n));
    char *text;
    TEST_ASSERT_EQUAL(T5_OK, t5_zip_extract_alloc(z, t5_zip_find(z, "dir/b.txt"), 1 << 20, &text, &n));
    TEST_ASSERT_EQUAL(FX_README_SIZE, n);
    TEST_ASSERT_EQUAL(n, strlen(text));
    free(text);
    t5_zip_close(z);
}

static void test_zip_deflate_to_file(void)
{
    t5_zip_t *z;
    TEST_ASSERT_EQUAL(T5_OK, t5_zip_open(FIXTURE(fx_zip_deflate), &z));
    TEST_ASSERT_EQUAL(4, t5_zip_count(z));
    const t5_zip_entry_t *e = t5_zip_find(z, "big.bin");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(8, e->method);
    TEST_ASSERT_EQUAL(FX_BIG_ELF_SIZE, e->size);
    char out[256];
    snprintf(out, sizeof out, "%s/big.out", s_dir);
    TEST_ASSERT_EQUAL(T5_OK, t5_zip_extract_file(z, e, out));
    uint32_t crc;
    size_t len;
    TEST_ASSERT_TRUE(file_crc(out, &crc, &len));
    TEST_ASSERT_EQUAL(FX_BIG_ELF_SIZE, len);
    TEST_ASSERT_EQUAL_HEX32(FX_BIG_ELF_CRC, crc);
    char *text;
    size_t n;
    TEST_ASSERT_EQUAL(T5_OK, t5_zip_extract_alloc(z, t5_zip_find(z, "a.txt"), 100, &text, &n));
    TEST_ASSERT_EQUAL_STRING("alpha\n", text);
    free(text);
    TEST_ASSERT_EQUAL(T5_ERR_NO_MEM, t5_zip_extract_alloc(z, e, 100, &text, &n));
    t5_zip_close(z);

    /* Also over an already open FILE*. */
    FILE *f = fopen(FIXTURE(fx_zip_deflate), "rb");
    TEST_ASSERT_EQUAL(T5_OK, t5_zip_open_file(f, false, &z));
    TEST_ASSERT_EQUAL(4, t5_zip_count(z));
    t5_zip_close(z);
    fclose(f);
}

static void test_zip_crc_and_corruption(void)
{
    t5_zip_t *z;
    char out[256];
    snprintf(out, sizeof out, "%s/x.out", s_dir);

    TEST_ASSERT_EQUAL(T5_OK, t5_zip_open(FIXTURE(fx_zip_badcrc), &z));
    char buf[64];
    size_t n;
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_zip_extract_mem(z, t5_zip_entry(z, 0), buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_zip_extract_file(z, t5_zip_entry(z, 0), out));
    TEST_ASSERT_EQUAL(-1, access(out, F_OK));          /* no half-written file left */
    t5_zip_close(z);

    TEST_ASSERT_EQUAL(T5_OK, t5_zip_open(FIXTURE(fx_zip_baddeflate), &z));
    TEST_ASSERT_NOT_EQUAL(T5_OK, t5_zip_extract_file(z, t5_zip_entry(z, 0), out));
    TEST_ASSERT_EQUAL(-1, access(out, F_OK));
    t5_zip_close(z);

    /* Not a zip, truncated zip, empty file. */
    const unsigned char junk[] = "this is not a zip file at all, just some text that is long enough";
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_zip_open(fixture("junk", junk, sizeof junk), &z));
    TEST_ASSERT_NULL(z);
    TEST_ASSERT_NOT_EQUAL(T5_OK, t5_zip_open(fixture("trunc", fx_zip_deflate, fx_zip_deflate_len / 2), &z));
    TEST_ASSERT_NOT_EQUAL(T5_OK, t5_zip_open(fixture("empty", junk, 0), &z));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_zip_open("/nonexistent/x.zip", &z));
}

static void test_zip_rejects_unsafe(void)
{
    t5_zip_t *z;
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_zip_open(FIXTURE(fx_zip_traversal), &z));
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_zip_open(FIXTURE(fx_zip_absolute), &z));
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_zip_open(FIXTURE(fx_zip_backslash), &z));
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_zip_open(FIXTURE(fx_zip_longname), &z));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_zip_open(FIXTURE(fx_zip_encrypted), &z));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_zip_open(FIXTURE(fx_zip_zip64), &z));

    const char *good[] = {"a", "a/b.txt", "dir/", ".hidden", "a/..b", "x...y"};
    const char *bad[] = {"", "/a", "../a", "a/../b", "a/..", "./a", "a//b", "a\\b", "c:x", "a/./b", "/", "a\nb"};
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(t5_zip_name_ok(good[i], strlen(good[i])), good[i]);
    }
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(t5_zip_name_ok(bad[i], strlen(bad[i])), bad[i]);
    }
    char longname[200];
    memset(longname, 'a', sizeof longname);
    TEST_ASSERT_FALSE(t5_zip_name_ok(longname, T5_ZIP_NAME_MAX));
    TEST_ASSERT_TRUE(t5_zip_name_ok(longname, T5_ZIP_NAME_MAX - 1));
}

static t5_err_t count_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)data;
    *(size_t *)ctx += len;
    return T5_OK;
}

static void test_zip_fuzz(void)
{
    srand(99);
    uint8_t *buf = malloc(fx_zip_deflate_len);
    char path[256];
    snprintf(path, sizeof path, "%s/fuzz.zip", s_dir);
    for (int it = 0; it < 300; it++) {
        memcpy(buf, fx_zip_deflate, fx_zip_deflate_len);
        unsigned len = fx_zip_deflate_len;
        int flips = 1 + rand() % 6;
        for (int f = 0; f < flips; f++) {
            /* bias towards the headers at the end, where the structure is */
            unsigned pos = rand() % 2 ? (unsigned)rand() % len : len - 1 - (unsigned)rand() % 400;
            buf[pos] = (uint8_t)rand();
        }
        FILE *f = fopen(path, "wb");
        fwrite(buf, 1, len, f);
        fclose(f);
        t5_zip_t *z;
        if (t5_zip_open(path, &z) != T5_OK) continue;
        for (int i = 0; i < t5_zip_count(z); i++) {
            /* An archive that opens names only safe paths, and whatever
             * extracts has the size and CRC its directory promised. */
            const t5_zip_entry_t *e = t5_zip_entry(z, i);
            TEST_ASSERT_TRUE(t5_zip_name_ok(e->name, strlen(e->name)));
            size_t got = 0;
            if (t5_zip_extract_cb(z, e, count_write, &got) == T5_OK) TEST_ASSERT_EQUAL(e->size, got);
        }
        t5_zip_close(z);
    }
    free(buf);
}

/* ---- toml --------------------------------------------------------------- */

static void test_toml_parse(void)
{
    const char *text =
        "# leading comment\r\n"
        "title = \"Tab \\\"5\\\"\\t\\\\ caf\\u00e9 \\U0001F600\"  # trailing\n"
        "path = 'C:\\raw\\no escapes'\n"
        "big = 1_000_000\n"
        "neg = -42\n"
        "zero = 0\n"
        "yes = true\n"
        "no = false\n"
        "list = [ \"a\", 'b',\n"
        "  # comment inside\n"
        "  \"c\", ]\n"
        "nums = [1, 2, 3]\n"
        "empty = []\n"
        "a.b.c = \"dotted\"\n"
        "\n"
        "[server]\n"
        "port = 8080\n"
        "[server.tls]\n"
        "on = true\n";
    t5_toml_doc_t d;
    char err[128];
    TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_toml_parse(text, strlen(text), &d, err, sizeof err), err);
    TEST_ASSERT_EQUAL_STRING("Tab \"5\"\t\\ caf\xc3\xa9 \xf0\x9f\x98\x80", t5_toml_get(&d, "title")->s);
    TEST_ASSERT_EQUAL_STRING("C:\\raw\\no escapes", t5_toml_get(&d, "path")->s);
    TEST_ASSERT_EQUAL_INT64(1000000, t5_toml_get(&d, "big")->i);
    TEST_ASSERT_EQUAL_INT64(-42, t5_toml_get(&d, "neg")->i);
    TEST_ASSERT_EQUAL_INT64(0, t5_toml_get(&d, "zero")->i);
    TEST_ASSERT_TRUE(t5_toml_get(&d, "yes")->b);
    TEST_ASSERT_EQUAL(T5_TOML_BOOL, t5_toml_get(&d, "no")->type);
    TEST_ASSERT_FALSE(t5_toml_get(&d, "no")->b);
    const t5_toml_value_t *l = t5_toml_get(&d, "list");
    TEST_ASSERT_EQUAL(T5_TOML_ARRAY, l->type);
    TEST_ASSERT_EQUAL(3, l->nitems);
    TEST_ASSERT_EQUAL_STRING("c", l->items[2].s);
    TEST_ASSERT_EQUAL_INT64(3, t5_toml_get(&d, "nums")->items[2].i);
    TEST_ASSERT_EQUAL(0, t5_toml_get(&d, "empty")->nitems);
    TEST_ASSERT_EQUAL_STRING("dotted", t5_toml_get(&d, "a.b.c")->s);
    TEST_ASSERT_EQUAL_INT64(8080, t5_toml_get(&d, "server.port")->i);
    TEST_ASSERT_TRUE(t5_toml_get(&d, "server.tls.on")->b);
    TEST_ASSERT_TRUE(t5_toml_has_table(&d, "server"));
    TEST_ASSERT_TRUE(t5_toml_has_table(&d, "a.b"));
    TEST_ASSERT_FALSE(t5_toml_has_table(&d, "serv"));
    TEST_ASSERT_NULL(t5_toml_get(&d, "port"));
    t5_toml_free(&d);
}

static void expect_toml_error(const char *text, const char *needle)
{
    t5_toml_doc_t d;
    char err[128];
    TEST_ASSERT_EQUAL_MESSAGE(T5_ERR_INVALID_ARG, t5_toml_parse(text, strlen(text), &d, err, sizeof err), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, needle), err);
    TEST_ASSERT_EQUAL(0, d.n);
    t5_toml_free(&d);
}

static void test_toml_errors(void)
{
    expect_toml_error("a = 1\nb = \"open\nc = 2\n", "line 2: unterminated string");
    expect_toml_error("a = 1\na = 2\n", "line 2: duplicate key 'a'");
    expect_toml_error("x = 1.5\n", "line 1: floats are not supported");
    expect_toml_error("x = \"\\q\"\n", "unknown escape");
    expect_toml_error("x = \"\\u12\"\n", "bad \\u escape");
    expect_toml_error("x = \"\\uD800\"\n", "invalid unicode escape");
    expect_toml_error("\n\nx =\n", "line 3: missing value");
    expect_toml_error("x = [1, \"a\"]\n", "mixed types");
    expect_toml_error("[t]\n[t]\n", "line 2: table [t] defined twice");
    expect_toml_error("a = 1 2\n", "expected end of line");
    expect_toml_error("a = 1__0\n", "between digits");
    expect_toml_error("a = 012\n", "leading zeros");
    expect_toml_error("a = 99999999999999999999\n", "out of range");
    expect_toml_error("a = [1, 2\n", "unterminated array");
    expect_toml_error("a = [[1]]\n", "nested arrays");
    expect_toml_error("a = { b = 1 }\n", "inline tables");
    expect_toml_error("\"q\" = 1\n", "quoted keys");
    expect_toml_error("[[t]]\n", "arrays of tables");
    expect_toml_error("a = \"\"\"x\"\"\"\n", "multi-line");
    expect_toml_error("a 1\n", "expected '='");
    expect_toml_error("a = 1\n[a]\n", "conflicts");
    expect_toml_error("a.b = 1\na = 2\n", "conflicts");
    expect_toml_error("a = tru\n", "invalid value");
    expect_toml_error("a = 1979-05-27\n", "dates");
}

/* ---- manifest ------------------------------------------------------------- */

static const char *DESIGN_EXAMPLE =
    "id = \"com.kanishka.airlog\"\n"
    "name = \"Air Log\"\n"
    "version = \"1.2.0\"\n"
    "api = 1\n"
    "runtime = \"native\"          # or \"micropython\"\n"
    "entry = \"app.elf\"\n"
    "min_heap_mb = 2\n"
    "\n"
    "[permissions]\n"
    "ports = [\"grove\"]\n"
    "net = true\n"
    "background_wake = true\n"
    "\n"
    "[background]\n"
    "entry = \"on_alarm\"\n"
    "max_seconds = 30\n";

static void test_manifest_design_example(void)
{
    t5_manifest_t m;
    char err[160];
    TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_manifest_parse(DESIGN_EXAMPLE, strlen(DESIGN_EXAMPLE), &m, err, sizeof err), err);
    TEST_ASSERT_EQUAL_STRING("com.kanishka.airlog", m.id);
    TEST_ASSERT_EQUAL_STRING("Air Log", m.name);
    TEST_ASSERT_EQUAL_STRING("1.2.0", m.version);
    TEST_ASSERT_EQUAL(1, m.api);
    TEST_ASSERT_EQUAL(T5_RUNTIME_NATIVE, m.runtime);
    TEST_ASSERT_EQUAL_STRING("app.elf", m.entry);
    TEST_ASSERT_EQUAL_STRING("", m.icon);
    TEST_ASSERT_EQUAL(2048, m.min_heap_kb);
    TEST_ASSERT_EQUAL_HEX32(T5_PERM_PORTS | T5_PERM_NET | T5_PERM_BACKGROUND_WAKE, m.perms);
    TEST_ASSERT_EQUAL(1, m.nports);
    TEST_ASSERT_EQUAL_STRING("grove", m.ports[0]);
    TEST_ASSERT_EQUAL(T5_ORIENT_LANDSCAPE, m.orientation);
    TEST_ASSERT_TRUE(m.background);
    TEST_ASSERT_EQUAL_STRING("on_alarm", m.bg_entry);
    TEST_ASSERT_EQUAL(30, m.bg_max_seconds);
}

static void test_manifest_other_fields(void)
{
    const char *t =
        "id = \"org.example.py_demo\"\nname = \"Py\"\nversion = \"0.1.0-beta.2\"\napi = 1\n"
        "runtime = \"micropython\"\nentry = \"src/main.py\"\nicon = \"icon.png\"\nmin_heap_kb = 300\n"
        "orientation = \"portrait\"\ndescription = \"ignored\"\n"
        "[permissions]\ncamera = true\nmic = false\nports = []\nsystem = true\n";
    t5_manifest_t m;
    char err[160];
    TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_manifest_parse(t, strlen(t), &m, err, sizeof err), err);
    TEST_ASSERT_EQUAL(T5_RUNTIME_MICROPYTHON, m.runtime);
    TEST_ASSERT_EQUAL_STRING("src/main.py", m.entry);
    TEST_ASSERT_EQUAL_STRING("icon.png", m.icon);
    TEST_ASSERT_EQUAL(300, m.min_heap_kb);
    TEST_ASSERT_EQUAL(T5_ORIENT_PORTRAIT, m.orientation);
    TEST_ASSERT_EQUAL_HEX32(T5_PERM_CAMERA | T5_PERM_SYSTEM, m.perms);   /* empty ports: no PORTS bit */
    TEST_ASSERT_EQUAL(0, m.nports);
    TEST_ASSERT_FALSE(m.background);
    TEST_ASSERT_EQUAL(30, m.bg_max_seconds);
}

static void manifest_edit(char *text, size_t n, const char *from, const char *to);

/* [shell]: full screen at launch. */
static void test_manifest_shell(void)
{
    t5_manifest_t m;
    char err[160], text[1024];
    TEST_ASSERT_EQUAL(T5_OK, t5_manifest_parse(DESIGN_EXAMPLE, strlen(DESIGN_EXAMPLE), &m, err, sizeof err));
    TEST_ASSERT_FALSE(m.fullscreen);           /* off unless asked */
    manifest_edit(text, sizeof text, NULL, "[shell]\nfullscreen = true\n");
    TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_manifest_parse(text, strlen(text), &m, err, sizeof err), err);
    TEST_ASSERT_TRUE(m.fullscreen);
}

static void test_manifest_mbus_ports(void)
{
    const char *t = "id = \"org.example.bus\"\nname = \"Bus\"\nversion = \"1.0.0\"\napi = 1\n"
                    "runtime = \"native\"\nentry = \"app.elf\"\n"
                    "[permissions]\nports = [\"mbus.i2c\", \"mbus.g2\"]\n";
    t5_manifest_t m;
    char err[160];
    TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_manifest_parse(t, strlen(t), &m, err, sizeof err), err);
    TEST_ASSERT_EQUAL(2, m.nports);
    TEST_ASSERT_EQUAL_STRING("mbus.i2c", m.ports[0]);
}

/* The design example with one fragment replaced (from NULL: to appended). */
static void manifest_edit(char *text, size_t n, const char *from, const char *to)
{
    const char *at = from ? strstr(DESIGN_EXAMPLE, from) : NULL;
    if (from) TEST_ASSERT_NOT_NULL_MESSAGE(at, from);
    if (at) {
        snprintf(text, n, "%.*s%s%s", (int)(at - DESIGN_EXAMPLE), DESIGN_EXAMPLE, to, at + strlen(from));
    } else {
        snprintf(text, n, "%s%s", DESIGN_EXAMPLE, to);
    }
}

/* Manifests the validator refuses: an edit of the design example and what
 * the error says. */
static const struct {
    const char *from, *to, *needle;
} s_bad_manifests[] = {
    { NULL, "[shell]\nfullscreen = \"yes\"\n", "shell.fullscreen: must be true or false" },
    { NULL, "[shell]\ncolour = 1\n", "unknown key 'shell.colour'" },
    { "id = \"com.kanishka.airlog\"\n", "", "id: missing" },
    { "com.kanishka.airlog", "airlog", "reverse-DNS" },
    { "com.kanishka.airlog", "Com.Kanishka.Airlog", "reverse-DNS" },
    { "com.kanishka.airlog", "com..airlog", "reverse-DNS" },
    { "com.kanishka.airlog", "com.kanishka-airlog.x", "reverse-DNS" },
    { "com.kanishka.airlog", "com.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "longer than" },
    { "api = 1", "api = 2", "API level 2" },
    { "api = 1", "api = 0", "at least 1" },
    { "api = 1", "api = \"1\"", "integer" },
    { "\"native\"", "\"wasm\"", "runtime: unknown 'wasm'" },
    { "\"1.2.0\"", "\"1.2\"", "version" },
    { "\"app.elf\"", "\"../app.elf\"", "entry" },
    { "name = \"Air Log\"\n", "", "name: missing" },
    { "[\"grove\"]", "[\"grove\", \"grove\"]", "listed twice" },
    { "[\"grove\"]", "[\"Grove!\"]", "bad port name" },
    { "[\"grove\"]", "\"grove\"", "array of port names" },
    { "[\"grove\"]", "[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]", "at most 8" },
    { "[\"grove\"]", "[1]", "array of port names" },
    { "background_wake = true\n", "", "needs background_wake" },
    { "net = true", "net = 1", "true or false" },
    { "net = true", "nett = true", "unknown key 'permissions.nett'" },
    { "max_seconds = 30", "max_seconds = 301", "out of range" },
    { "\"on_alarm\"", "\"on-alarm\"", "function name" },
    { "min_heap_mb = 2", "min_heap_mb = 2\nmin_heap_kb = 5", "exclusive" },
    { "min_heap_mb = 2", "min_heap_mb = 2.5", "" },
    { "min_heap_mb = 2\n", "min_heap_mb = 2\norientation = \"sideways\"\n", "orientation: unknown 'sideways'" },
    { "name = \"Air Log\"", "name = \"\"\"Air Log\"\"\"", "" },
    { "name = \"Air Log\"", "\"name\" = \"Air Log\"", "" },
    { NULL, "[extra]\n", "unknown table [extra]" },
    { NULL, "colour = \"red\"\n", "unknown key 'background.colour'" },
    { "runtime", "runtime = \"native\"\nruntime", "duplicate key" },
};

static void test_manifest_errors(void)
{
    for (size_t i = 0; i < sizeof s_bad_manifests / sizeof s_bad_manifests[0]; i++) {
        char text[1024], err[160];
        manifest_edit(text, sizeof text, s_bad_manifests[i].from, s_bad_manifests[i].to);
        t5_manifest_t m;
        TEST_ASSERT_EQUAL_MESSAGE(T5_ERR_INVALID_ARG, t5_manifest_parse(text, strlen(text), &m, err, sizeof err), text);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, s_bad_manifests[i].needle), err);
        TEST_ASSERT_EQUAL_STRING("", m.id);   /* cleared on failure */
    }
}

/* tools/t5/t5pack.py's verdict on a manifest: true if it would pack it. */
static bool t5pack_accepts(const char *text)
{
    char path[256], cmd[768];
    snprintf(path, sizeof path, "%s/manifest.toml", s_dir);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
    snprintf(cmd, sizeof cmd,
             "python3 -c \"import sys; sys.path.insert(0, sys.argv[1]); import t5pack; "
             "t5pack.validate(open(sys.argv[2], encoding='utf-8').read())\" '%s/tools/t5' '%s' > /dev/null 2>&1",
             T5_REPO_ROOT, path);
    return system(cmd) == 0;
}

/* The app SDK packs exactly what the device installs. */
static void test_manifest_rules_match_t5pack(void)
{
    char text[1024], err[160];
    t5_manifest_t m;
    const char *good[] = {
        DESIGN_EXAMPLE,
        "id = \"org.example.py_demo\"\nname = \"Py\"\nversion = \"0.1.0-beta.2\"\napi = 1\n"
        "runtime = \"micropython\"\nentry = \"src/main.py\"\nicon = \"icon.png\"\nmin_heap_kb = 300\n"
        "orientation = \"any\"\ndescription = \"d\"\nauthor = \"a\"\nhomepage = \"h\"\nlicense = \"MIT\"\n"
        "[permissions]\ncamera = true\nmic = false\nports = []\nsystem = true\nshared_read = true\n"
        "shared_write = true\nall_files = true\nusb = true\nble = true\nstorage = true\n",
        "id = \"org.example.bus\"\nname = \"Bus\"\nversion = \"1.0.0\"\napi = 1\n"
        "runtime = \"native\"\nentry = \"app.elf\"\norientation = \"portrait\"\n"
        "[permissions]\nports = [\"mbus.i2c\", \"mbus.g2\"]\n",
        "id = \"org.example.bar\"\nname = \"Bar\"\nversion = \"1.0.0\"\napi = 1\n"
        "runtime = \"native\"\nentry = \"app.elf\"\n[shell]\nfullscreen = false\n",
    };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_manifest_parse(good[i], strlen(good[i]), &m, err, sizeof err), err);
        TEST_ASSERT_TRUE_MESSAGE(t5pack_accepts(good[i]), good[i]);
    }
    for (size_t i = 0; i < sizeof s_bad_manifests / sizeof s_bad_manifests[0]; i++) {
        manifest_edit(text, sizeof text, s_bad_manifests[i].from, s_bad_manifests[i].to);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(T5_OK, t5_manifest_parse(text, strlen(text), &m, err, sizeof err), text);
        TEST_ASSERT_FALSE_MESSAGE(t5pack_accepts(text), text);
    }
    /* The retired app bar keys: packages built with them still install (and
     * get no bar), but t5pack won't build a new one. */
    manifest_edit(text, sizeof text, NULL, "[shell]\napp_bar = true\ntitle = \"CO2\"\n");
    TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_manifest_parse(text, strlen(text), &m, err, sizeof err), err);
    TEST_ASSERT_FALSE(t5pack_accepts(text));
}

/* Bytes in the regular files under dir. */
static uint64_t tree_bytes(const char *dir, int depth)
{
    uint64_t n = 0;
    DIR *d = opendir(dir);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_TRUE(depth < 8);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        TEST_ASSERT_EQUAL(0, stat(p, &st));
        n += S_ISDIR(st.st_mode) ? tree_bytes(p, depth + 1) : (uint64_t)st.st_size;
    }
    closedir(d);
    return n;
}

static void test_perm_names_and_versions(void)
{
    for (uint32_t bit = 1; bit & T5_PERM_ALL; bit <<= 1) {
        const char *n = t5_perm_name(bit);
        TEST_ASSERT_NOT_NULL(n);
        TEST_ASSERT_EQUAL_HEX32(bit, t5_perm_from_name(n));
    }
    TEST_ASSERT_EQUAL_STRING("background_wake", t5_perm_name(T5_PERM_BACKGROUND_WAKE));
    TEST_ASSERT_NULL(t5_perm_name(T5_PERM_NET | T5_PERM_MIC));
    TEST_ASSERT_EQUAL(0, t5_perm_from_name("gps"));
    TEST_ASSERT_EQUAL(0, t5_perm_from_name(NULL));

    TEST_ASSERT_TRUE(t5_version_cmp("1.0.0", "1.0.1") < 0);
    TEST_ASSERT_TRUE(t5_version_cmp("1.10.0", "1.9.9") > 0);
    TEST_ASSERT_TRUE(t5_version_cmp("2.0.0", "10.0.0") < 0);
    TEST_ASSERT_EQUAL(0, t5_version_cmp("1.2.3", "1.2.3"));
    TEST_ASSERT_EQUAL(0, t5_version_cmp("1.2.3+build5", "1.2.3"));
    TEST_ASSERT_TRUE(t5_version_cmp("1.0.0-rc.1", "1.0.0") < 0);
    TEST_ASSERT_TRUE(t5_version_cmp("1.0.0-alpha", "1.0.0-alpha.1") < 0);
    TEST_ASSERT_TRUE(t5_version_cmp("1.0.0-alpha.1", "1.0.0-alpha.b") < 0);
    TEST_ASSERT_TRUE(t5_version_cmp("1.0.0-beta.2", "1.0.0-beta.11") < 0);
    TEST_ASSERT_TRUE(t5_version_cmp("1.0.0-rc.1", "1.0.0-beta.11") > 0);
    TEST_ASSERT_TRUE(t5_version_cmp("junk", "0.0.1") < 0);
    TEST_ASSERT_TRUE(t5_version_ok("0.1.0-beta.2"));
    TEST_ASSERT_FALSE(t5_version_ok("01.0.0"));
    TEST_ASSERT_FALSE(t5_version_ok("1.0"));
    TEST_ASSERT_FALSE(t5_version_ok("1.0.0-"));
    TEST_ASSERT_FALSE(t5_version_ok("1.0.0-a..b"));
    TEST_ASSERT_FALSE(t5_version_ok("1.0.0-averyveryverylongtag"));
}

/* ---- installer ------------------------------------------------------------ */

static void test_inspect(void)
{
    t5_manifest_t m;
    char err[160];
    TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_pkg_inspect(FIXTURE(fx_pkg_v1), &m, err, sizeof err), err);
    TEST_ASSERT_EQUAL_STRING("com.example.hello", m.id);
    TEST_ASSERT_EQUAL_STRING("1.0.0", m.version);
    TEST_ASSERT_EQUAL_STRING("icon.png", m.icon);

    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_pkg_inspect(FIXTURE(fx_pkg_noentry), &m, err, sizeof err));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "main.py"), err);
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_pkg_inspect(FIXTURE(fx_pkg_nomanifest), &m, err, sizeof err));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "manifest.toml"), err);
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_pkg_inspect(FIXTURE(fx_pkg_badapi), &m, err, sizeof err));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "API level"), err);
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_pkg_inspect(FIXTURE(fx_zip_traversal), &m, err, sizeof err));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_pkg_inspect("/nonexistent.t5app", &m, err, sizeof err));
}

static void test_install_auto_sd_then_data(void)
{
    t5_pkg_info_t info;
    char err[160];
    TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v1), T5_PKG_DEST_AUTO, &info, err, sizeof err), err);
    TEST_ASSERT_EQUAL(T5_PKG_DEST_SD, info.root);
    char want[256];
    snprintf(want, sizeof want, "%s/com.example.hello", s_sd);
    TEST_ASSERT_EQUAL_STRING(want, info.dir);
    snprintf(want, sizeof want, "%s/com.example.hello/app.elf", s_sd);
    TEST_ASSERT_EQUAL_STRING(want, info.entry_path);
    snprintf(want, sizeof want, "%s/com.example.hello", s_appdata);
    TEST_ASSERT_EQUAL_STRING(want, info.data_dir);
    TEST_ASSERT_TRUE(exists("%s%s", info.data_dir, ""));
    TEST_ASSERT_TRUE(info.install_time > 0);
    uint32_t crc;
    size_t len;
    TEST_ASSERT_TRUE(file_crc(info.entry_path, &crc, &len));
    TEST_ASSERT_EQUAL(FX_BIG_ELF_SIZE, len);
    TEST_ASSERT_EQUAL_HEX32(FX_BIG_ELF_CRC, crc);
    TEST_ASSERT_TRUE(exists("%s/%s", info.dir, "manifest.toml"));
    TEST_ASSERT_TRUE(exists("%s/%s", info.dir, ".install"));
    TEST_ASSERT_TRUE(exists("%s/%s", info.dir, "assets/readme.txt"));
    TEST_ASSERT_FALSE(exists("%s/%s~new", s_sd, "com.example.hello"));

    /* Size is the sum of the extracted files (manifest included, the
     * install record not). */
    char rec[300];
    snprintf(rec, sizeof rec, "%s/.install", info.dir);
    struct stat st;
    TEST_ASSERT_EQUAL(0, stat(rec, &st));
    TEST_ASSERT_EQUAL(tree_bytes(info.dir, 0) - (uint64_t)st.st_size, info.size);

    t5_pkg_info_t f;
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_find("com.example.hello", &f));
    TEST_ASSERT_EQUAL(info.size, f.size);
    TEST_ASSERT_EQUAL(info.install_time, f.install_time);
    TEST_ASSERT_EQUAL_HEX32(T5_PERM_NET | T5_PERM_PORTS, f.manifest.perms);

    /* No SD: AUTO falls back to internal storage. */
    sh("rm -rf '%s/sd'", s_dir);
    TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_other), T5_PKG_DEST_AUTO, &info, err, sizeof err), err);
    TEST_ASSERT_EQUAL(T5_PKG_DEST_DATA, info.root);
    TEST_ASSERT_EQUAL(T5_RUNTIME_MICROPYTHON, info.manifest.runtime);
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_pkg_install(FIXTURE(fx_pkg_v1), T5_PKG_DEST_SD, NULL, err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "SD"));
}

static void write_text(const char *dir, const char *name, const char *text)
{
    char p[300];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
}

static void test_upgrade_keeps_data_and_refuses_downgrade(void)
{
    t5_pkg_info_t info;
    char err[160];
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v1), T5_PKG_DEST_SD, &info, err, sizeof err));
    write_text(info.data_dir, "save.dat", "level 7");
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_grants_save("com.example.hello", T5_PERM_NET, T5_PERM_CAMERA));

    TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v2), T5_PKG_DEST_AUTO, &info, err, sizeof err), err);
    TEST_ASSERT_EQUAL(T5_PKG_DEST_SD, info.root);
    TEST_ASSERT_EQUAL_STRING("1.1.0", info.manifest.version);
    TEST_ASSERT_FALSE(exists("%s/%s", info.dir, "icon.png"));       /* old files gone */
    TEST_ASSERT_FALSE(exists("%s/%s", info.dir, "assets"));
    TEST_ASSERT_TRUE(exists("%s/%s", info.dir, "new.txt"));
    TEST_ASSERT_TRUE(exists("%s/%s", info.data_dir, "save.dat"));   /* data kept */
    uint32_t g, d;
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_grants_load("com.example.hello", &g, &d));
    TEST_ASSERT_EQUAL_HEX32(T5_PERM_NET, g);
    TEST_ASSERT_EQUAL_HEX32(T5_PERM_CAMERA, d);
    TEST_ASSERT_FALSE(exists("%s/%s~old", s_sd, "com.example.hello"));
    TEST_ASSERT_FALSE(exists("%s/%s~new", s_sd, "com.example.hello"));

    /* Same version again is a reinstall. */
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v2), T5_PKG_DEST_AUTO, NULL, err, sizeof err));

    /* Downgrade refused, then allowed with the flag. */
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_pkg_install(FIXTURE(fx_pkg_v0), T5_PKG_DEST_AUTO, &info, err, sizeof err));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "older"), err);
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_find("com.example.hello", &info));
    TEST_ASSERT_EQUAL_STRING("1.1.0", info.manifest.version);
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install_ex(FIXTURE(fx_pkg_v0), T5_PKG_DEST_AUTO, T5_PKG_ALLOW_DOWNGRADE,
                                               &info, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("0.9.0", info.manifest.version);
    TEST_ASSERT_TRUE(exists("%s/%s", info.data_dir, "save.dat"));

    /* Moving to internal storage removes the SD copy. */
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v2), T5_PKG_DEST_DATA, &info, err, sizeof err));
    TEST_ASSERT_EQUAL(T5_PKG_DEST_DATA, info.root);
    TEST_ASSERT_FALSE(exists("%s/%s", s_sd, "com.example.hello"));
    TEST_ASSERT_TRUE(exists("%s/%s", info.data_dir, "save.dat"));
    /* AUTO upgrade stays where the app is. */
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v2), T5_PKG_DEST_AUTO, &info, err, sizeof err));
    TEST_ASSERT_EQUAL(T5_PKG_DEST_DATA, info.root);
}

static void test_failed_install_leaves_old(void)
{
    t5_pkg_info_t info;
    char err[160];
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v1), T5_PKG_DEST_SD, &info, err, sizeof err));
    TEST_ASSERT_NOT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_badapi), T5_PKG_DEST_SD, &info, err, sizeof err));
    TEST_ASSERT_NOT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_zip_traversal), T5_PKG_DEST_SD, &info, err, sizeof err));
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_find("com.example.hello", &info));
    TEST_ASSERT_EQUAL_STRING("1.0.0", info.manifest.version);
}

static void test_remove_deep_data_dir(void)
{
    /* An app can nest its data as deep as paths allow; removing it must not
     * recurse once per level (coding standard rule 1). */
    t5_pkg_info_t info;
    char err[160];
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v1), T5_PKG_DEST_DATA, &info, err, sizeof err));
    char p[512];
    int n = snprintf(p, sizeof p, "%s", info.data_dir);
    int levels = 0;
    while (n + 3 < 400) {
        n += snprintf(p + n, sizeof p - (size_t)n, "/d");
        TEST_ASSERT_EQUAL(0, mkdir(p, 0755));
        if (levels % 10 == 0) {
            char f[520];
            snprintf(f, sizeof f, "%s/f", p);
            FILE *fp = fopen(f, "w");
            TEST_ASSERT_NOT_NULL(fp);
            fclose(fp);
        }
        levels++;
    }
    TEST_ASSERT_GREATER_THAN(100, levels);
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_remove("com.example.hello", false));
    TEST_ASSERT_FALSE(exists("%s%s", info.data_dir, ""));
}

static void test_remove(void)
{
    t5_pkg_info_t info;
    char err[160];
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v1), T5_PKG_DEST_SD, &info, err, sizeof err));
    write_text(info.data_dir, "save.dat", "x");
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_remove("com.example.hello", true));
    TEST_ASSERT_FALSE(exists("%s%s", info.dir, ""));
    TEST_ASSERT_FALSE(exists("%s/%s~del", s_sd, "com.example.hello"));
    TEST_ASSERT_TRUE(exists("%s/%s", info.data_dir, "save.dat"));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_pkg_find("com.example.hello", &info));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_pkg_remove("com.example.hello", true));

    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v1), T5_PKG_DEST_DATA, &info, err, sizeof err));
    TEST_ASSERT_TRUE(exists("%s/%s", info.data_dir, "save.dat"));   /* reinstall finds old data */
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_remove("com.example.hello", false));
    TEST_ASSERT_FALSE(exists("%s%s", info.dir, ""));
    TEST_ASSERT_FALSE(exists("%s%s", info.data_dir, ""));

    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_pkg_remove("../data", false));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_pkg_remove("com.example.hello/..", false));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_pkg_find("..", &info));
}

static void test_list_and_shadowing(void)
{
    t5_pkg_info_t list[8];
    char err[160];
    TEST_ASSERT_EQUAL(0, t5_pkg_list(list, 8));
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v2), T5_PKG_DEST_DATA, NULL, err, sizeof err));
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_other), T5_PKG_DEST_DATA, NULL, err, sizeof err));
    /* The same id copied onto the SD by hand (e.g. from a PC). */
    char cmd[640];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s' && cp -R '%s/com.example.hello' '%s/'", s_sd, s_data, s_sd);
    TEST_ASSERT_EQUAL(0, system(cmd));
    /* Junk that must be ignored. */
    sh("mkdir -p '%s/not_an_id' '%s/com.example.broken'", s_sd, s_sd);

    int n = t5_pkg_list(list, 8);
    TEST_ASSERT_EQUAL(2, n);
    int hello = strcmp(list[0].manifest.id, "com.example.hello") == 0 ? 0 : 1;
    TEST_ASSERT_EQUAL_STRING("com.example.hello", list[hello].manifest.id);
    TEST_ASSERT_EQUAL(T5_PKG_DEST_SD, list[hello].root);
    TEST_ASSERT_TRUE(list[hello].shadowed);
    TEST_ASSERT_EQUAL_STRING("com.example.other", list[1 - hello].manifest.id);
    TEST_ASSERT_EQUAL(T5_PKG_DEST_DATA, list[1 - hello].root);
    TEST_ASSERT_FALSE(list[1 - hello].shadowed);
    TEST_ASSERT_EQUAL(1, t5_pkg_list(list, 1));

    t5_pkg_info_t f;
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_find("com.example.hello", &f));
    TEST_ASSERT_EQUAL(T5_PKG_DEST_SD, f.root);
    TEST_ASSERT_TRUE(f.shadowed);
}

static void test_recover_interrupted(void)
{
    t5_pkg_info_t info;
    char err[160];
    const char *id = "com.example.hello";
    char fin[256], nw[256], old[256], del[256], keep[256];
    snprintf(fin, sizeof fin, "%s/%s", s_sd, id);
    snprintf(nw, sizeof nw, "%s/%s~new", s_sd, id);
    snprintf(old, sizeof old, "%s/%s~old", s_sd, id);
    snprintf(del, sizeof del, "%s/%s~del", s_sd, id);
    snprintf(keep, sizeof keep, "%s/v1copy", s_dir);

    TEST_ASSERT_EQUAL(0, t5_pkg_recover());

    /* a) Cut during extraction: half-written ~new, old install untouched. */
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v1), T5_PKG_DEST_SD, &info, err, sizeof err));
    sh("mkdir -p '%s' && echo partial > '%s/app.elf'", nw, nw);
    TEST_ASSERT_EQUAL(1, t5_pkg_recover());
    TEST_ASSERT_FALSE(exists("%s%s", nw, ""));
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_find(id, &info));
    TEST_ASSERT_EQUAL_STRING("1.0.0", info.manifest.version);

    /* b) Cut after the old copy moved aside, staged copy incomplete: roll back. */
    sh("mv '%s' '%s'", fin, old);
    sh("mkdir -p '%s' && echo partial > '%s/app.elf'", nw, nw);
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_pkg_find(id, &info));
    TEST_ASSERT_EQUAL(2, t5_pkg_recover());
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_find(id, &info));
    TEST_ASSERT_EQUAL_STRING("1.0.0", info.manifest.version);
    TEST_ASSERT_FALSE(exists("%s%s", nw, ""));
    TEST_ASSERT_FALSE(exists("%s%s", old, ""));

    /* c) Same cut but the staged copy was complete: roll forward. */
    sh("cp -R '%s' '%s'", fin, keep);
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v2), T5_PKG_DEST_SD, &info, err, sizeof err));
    sh("mv '%s' '%s'", fin, nw);
    sh("mv '%s' '%s'", keep, old);
    TEST_ASSERT_EQUAL(1 + 1, t5_pkg_recover());
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_find(id, &info));
    TEST_ASSERT_EQUAL_STRING("1.1.0", info.manifest.version);
    TEST_ASSERT_FALSE(exists("%s%s", old, ""));

    /* d) Interrupted remove: ~del is finished off, never resurrected. */
    sh("mv '%s' '%s'", fin, del);
    TEST_ASSERT_EQUAL(1, t5_pkg_recover());
    TEST_ASSERT_FALSE(exists("%s%s", del, ""));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_pkg_find(id, &info));

    /* e) An install also cleans up first. */
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_install(FIXTURE(fx_pkg_v2), T5_PKG_DEST_SD, &info, err, sizeof err));
    sh("mv '%s' '%s'", fin, old);
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_pkg_install(FIXTURE(fx_pkg_v0), T5_PKG_DEST_SD, &info, err, sizeof err));
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_find(id, &info));     /* old copy restored, then downgrade refused */
    TEST_ASSERT_EQUAL_STRING("1.1.0", info.manifest.version);
    TEST_ASSERT_EQUAL(0, t5_pkg_recover());
}

static void test_grants(void)
{
    uint32_t g = 1, d = 1;
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_pkg_grants_load("com.example.hello", &g, &d));
    TEST_ASSERT_EQUAL(0, g);
    TEST_ASSERT_EQUAL(0, d);
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_grants_save("com.example.hello", T5_PERM_CAMERA | T5_PERM_NET, T5_PERM_MIC));
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_grants_load("com.example.hello", &g, &d));
    TEST_ASSERT_EQUAL_HEX32(T5_PERM_CAMERA | T5_PERM_NET, g);
    TEST_ASSERT_EQUAL_HEX32(T5_PERM_MIC, d);
    /* Overwrite; a bit can't be both granted and denied; unknown bits dropped. */
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_grants_save("com.example.hello", T5_PERM_MIC | 0x80000000u, T5_PERM_MIC | T5_PERM_USB));
    TEST_ASSERT_EQUAL(T5_OK, t5_pkg_grants_load("com.example.hello", &g, &d));
    TEST_ASSERT_EQUAL_HEX32(T5_PERM_MIC, g);
    TEST_ASSERT_EQUAL_HEX32(T5_PERM_USB, d);
    /* Saving leaves the grants file and nothing else behind. */
    char dir[300];
    snprintf(dir, sizeof dir, "%s/com.example.hello", s_appdata);
    DIR *dd = opendir(dir);
    TEST_ASSERT_NOT_NULL(dd);
    struct dirent *de;
    while ((de = readdir(dd))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) TEST_ASSERT_EQUAL_STRING(".grants", de->d_name);
    }
    closedir(dd);
    /* Corrupt file. */
    write_text(dir, ".grants", "garbage");
    TEST_ASSERT_EQUAL(T5_ERR_FAIL, t5_pkg_grants_load("com.example.hello", &g, &d));
    TEST_ASSERT_EQUAL(0, g);
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_pkg_grants_save("../etc", 1, 0));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_pkg_grants_load("x", &g, &d));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_inflate_streams);
    RUN_TEST(test_inflate_streaming_big);
    RUN_TEST(test_inflate_errors);
    RUN_TEST(test_inflate_fuzz);
    RUN_TEST(test_zip_list_and_extract_stored);
    RUN_TEST(test_zip_deflate_to_file);
    RUN_TEST(test_zip_crc_and_corruption);
    RUN_TEST(test_zip_rejects_unsafe);
    RUN_TEST(test_zip_fuzz);
    RUN_TEST(test_toml_parse);
    RUN_TEST(test_toml_errors);
    RUN_TEST(test_manifest_design_example);
    RUN_TEST(test_manifest_other_fields);
    RUN_TEST(test_manifest_mbus_ports);
    RUN_TEST(test_manifest_shell);
    RUN_TEST(test_manifest_errors);
    RUN_TEST(test_manifest_rules_match_t5pack);
    RUN_TEST(test_perm_names_and_versions);
    RUN_TEST(test_inspect);
    RUN_TEST(test_install_auto_sd_then_data);
    RUN_TEST(test_upgrade_keeps_data_and_refuses_downgrade);
    RUN_TEST(test_failed_install_leaves_old);
    RUN_TEST(test_remove);
    RUN_TEST(test_remove_deep_data_dir);
    RUN_TEST(test_list_and_shadowing);
    RUN_TEST(test_recover_interrupted);
    RUN_TEST(test_grants);
    return UNITY_END();
}
