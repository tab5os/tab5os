/* SPDX-License-Identifier: Apache-2.0
 * Zip reader: end-of-central-directory, central directory, local headers.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "t5_core/crc.h"
#include "t5_core/log.h"
#include "t5_pkg/zip.h"
#include "pkg_test.h"

static const char *TAG = "t5.zip";

#define SIG_LOCAL 0x04034b50u
#define SIG_CDIR 0x02014b50u
#define SIG_EOCD 0x06054b50u
#define SIG_Z64_LOC 0x07064b50u
#define EOCD_LEN 22
#define CDIR_LEN 46
#define LOCAL_LEN 30
#define FLAG_ENCRYPTED 0x0001u
#define FLAG_STRONG_ENC 0x0040u
#define COPY_CHUNK 4096

struct t5_zip {
    FILE *f;
    bool own;
    uint32_t cd_offset;      /* entry data must lie before this */
    int n;
    t5_zip_entry_t *e;
    char *names;
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool read_at(FILE *f, long off, void *buf, size_t len)
{
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

bool t5_zip_name_ok(const char *name, size_t len)
{
    if (!name || len == 0 || len >= T5_ZIP_NAME_MAX) {
        return false;
    }
    if (name[len - 1] == '/') {
        len--;            /* directory entry */
        if (len == 0) {
            return false;
        }
    }
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || name[i] == '/') {
            size_t clen = i - start;
            if (clen == 0) {
                return false;   /* absolute path or "a//b" */
            }
            if (name[start] == '.' && (clen == 1 || (clen == 2 && name[start + 1] == '.'))) {
                return false;
            }
            start = i + 1;
            continue;
        }
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || c == '\\' || c == ':') {
            return false;
        }
    }
    return true;
}

static bool has_zip64_extra(const uint8_t *x, size_t len)
{
    while (len >= 4) {
        uint16_t id = rd16(x), sz = rd16(x + 2);
        if (id == 0x0001) {
            return true;
        }
        if ((size_t)sz + 4 > len) {
            return false;
        }
        x += 4 + sz;
        len -= 4 + (size_t)sz;
    }
    return false;
}

static t5_err_t parse_cdir(t5_zip_t *z, const uint8_t *cd, size_t cd_size, int n)
{
    z->e = calloc((size_t)(n ? n : 1), sizeof *z->e);
    z->names = malloc(cd_size + 1);
    if (!z->e || !z->names) {
        return T5_ERR_NO_MEM;
    }
    const uint8_t *p = cd, *end = cd + cd_size;
    char *np = z->names;
    for (int i = 0; i < n; i++) {
        if ((size_t)(end - p) < CDIR_LEN || rd32(p) != SIG_CDIR) {
            T5_LOGW(TAG, "bad central directory entry %d", i);
            return T5_ERR_INVALID_ARG;
        }
        uint16_t flags = rd16(p + 8);
        uint16_t nlen = rd16(p + 28), xlen = rd16(p + 30), clen = rd16(p + 32);
        size_t rec = CDIR_LEN + (size_t)nlen + xlen + clen;
        if ((size_t)(end - p) < rec) {
            return T5_ERR_INVALID_ARG;
        }
        t5_zip_entry_t *e = &z->e[i];
        e->flags = flags;
        e->method = rd16(p + 10);
        e->crc32 = rd32(p + 16);
        e->comp_size = rd32(p + 20);
        e->size = rd32(p + 24);
        e->local_offset = rd32(p + 42);
        if (flags & (FLAG_ENCRYPTED | FLAG_STRONG_ENC)) {
            T5_LOGW(TAG, "encrypted entries are not supported");
            return T5_ERR_NOT_SUPPORTED;
        }
        if (e->comp_size == 0xffffffffu || e->size == 0xffffffffu || e->local_offset == 0xffffffffu ||
            has_zip64_extra(p + CDIR_LEN + nlen, xlen)) {
            T5_LOGW(TAG, "zip64 is not supported");
            return T5_ERR_NOT_SUPPORTED;
        }
        const char *name = (const char *)p + CDIR_LEN;
        if (memchr(name, 0, nlen) || !t5_zip_name_ok(name, nlen)) {
            T5_LOGW(TAG, "unsafe entry name rejected");
            return T5_ERR_DENIED;
        }
        memcpy(np, name, nlen);
        np[nlen] = 0;
        e->name = np;
        np += nlen + 1;
        e->is_dir = name[nlen - 1] == '/';
        if (e->local_offset >= z->cd_offset || (e->is_dir && e->size != 0)) {
            return T5_ERR_INVALID_ARG;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(z->e[j].name, e->name) == 0) {
                T5_LOGW(TAG, "duplicate entry %s", e->name);
                return T5_ERR_INVALID_ARG;
            }
        }
        p += rec;
    }
    z->n = n;
    return T5_OK;
}

t5_err_t t5_zip_open_file(FILE *f, bool own_file, t5_zip_t **out)
{
    if (!f || !out) {
        return T5_ERR_INVALID_ARG;
    }
    *out = NULL;
    t5_zip_t *z = calloc(1, sizeof *z);
    if (!z) {
        if (own_file) fclose(f);
        return T5_ERR_NO_MEM;
    }
    z->f = f;
    z->own = own_file;
    t5_err_t err = T5_ERR_INVALID_ARG;
    uint8_t *tail = NULL, *cd = NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        err = T5_ERR_IO;
        goto fail;
    }
    long fsize = ftell(f);
    if (fsize < EOCD_LEN || fsize > 0x7fffffffL) {
        goto fail;
    }
    size_t tlen = (size_t)fsize < EOCD_LEN + 0xffff ? (size_t)fsize : EOCD_LEN + 0xffff;
    long tail_off = fsize - (long)tlen;
    tail = malloc(tlen);
    if (!tail) {
        err = T5_ERR_NO_MEM;
        goto fail;
    }
    if (!read_at(f, tail_off, tail, tlen)) {
        err = T5_ERR_IO;
        goto fail;
    }
    long eocd = -1;
    for (size_t i = tlen - EOCD_LEN + 1; i-- > 0;) {
        if (rd32(tail + i) == SIG_EOCD && rd16(tail + i + 20) <= tlen - i - EOCD_LEN) {
            eocd = (long)i;
            break;
        }
    }
    if (eocd < 0) {
        T5_LOGW(TAG, "no end of central directory");
        goto fail;
    }
    const uint8_t *e = tail + eocd;
    uint16_t disk = rd16(e + 4), cd_disk = rd16(e + 6), n_disk = rd16(e + 8), n_total = rd16(e + 10);
    uint32_t cd_size = rd32(e + 12), cd_off = rd32(e + 16);
    long eocd_pos = tail_off + eocd;
    if (n_total == 0xffff || n_disk == 0xffff || cd_size == 0xffffffffu || cd_off == 0xffffffffu ||
        (eocd >= 20 && rd32(e - 20) == SIG_Z64_LOC)) {
        T5_LOGW(TAG, "zip64 is not supported");
        err = T5_ERR_NOT_SUPPORTED;
        goto fail;
    }
    if (disk != 0 || cd_disk != 0 || n_disk != n_total) {
        err = T5_ERR_NOT_SUPPORTED;
        goto fail;
    }
    if ((uint64_t)cd_off + cd_size > (uint64_t)eocd_pos || cd_size > T5_ZIP_MAX_CDIR ||
        n_total > T5_ZIP_MAX_ENTRIES || (uint64_t)n_total * CDIR_LEN > cd_size) {
        goto fail;
    }
    z->cd_offset = cd_off;
    cd = malloc(cd_size ? cd_size : 1);
    if (!cd) {
        err = T5_ERR_NO_MEM;
        goto fail;
    }
    if (cd_size && !read_at(f, (long)cd_off, cd, cd_size)) {
        err = T5_ERR_IO;
        goto fail;
    }
    err = parse_cdir(z, cd, cd_size, n_total);
    if (err != T5_OK) {
        goto fail;
    }
    free(tail);
    free(cd);
    *out = z;
    return T5_OK;
fail:
    free(tail);
    free(cd);
    t5_zip_close(z);
    return err;
}

t5_err_t t5_zip_open(const char *path, t5_zip_t **out)
{
    if (!path || !out) {
        return T5_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        *out = NULL;
        return T5_ERR_NOT_FOUND;
    }
    return t5_zip_open_file(f, true, out);
}

void t5_zip_close(t5_zip_t *z)
{
    if (!z) {
        return;
    }
    if (z->own && z->f) {
        fclose(z->f);
    }
    free(z->e);
    free(z->names);
    free(z);
}

int t5_zip_count(const t5_zip_t *z)
{
    return z ? z->n : 0;
}

const t5_zip_entry_t *t5_zip_entry(const t5_zip_t *z, int index)
{
    return (z && index >= 0 && index < z->n) ? &z->e[index] : NULL;
}

const t5_zip_entry_t *t5_zip_find(const t5_zip_t *z, const char *name)
{
    if (!z || !name) {
        return NULL;
    }
    for (int i = 0; i < z->n; i++) {
        if (strcmp(z->e[i].name, name) == 0) {
            return &z->e[i];
        }
    }
    return NULL;
}

typedef struct {
    FILE *f;
    uint32_t left;
} file_in_t;

static int file_read(void *ctx, uint8_t *buf, size_t cap)
{
    file_in_t *in = ctx;
    size_t n = in->left < cap ? in->left : cap;
    if (n == 0) {
        return 0;
    }
    size_t got = fread(buf, 1, n, in->f);
    if (got != n) {
        return -1;
    }
    in->left -= (uint32_t)n;
    return (int)n;
}

typedef struct {
    t5_inflate_write_fn wr;
    void *ctx;
    uint32_t crc;
    uint32_t total;
    uint32_t expect;
} check_out_t;

static t5_err_t check_write(void *ctx, const uint8_t *data, size_t len)
{
    check_out_t *o = ctx;
    if (len > o->expect - o->total) {
        return T5_ERR_INVALID_ARG;     /* more data than the directory claims */
    }
    o->crc = t5_crc32(o->crc, data, len);
    o->total += (uint32_t)len;
    return o->wr(o->ctx, data, len);
}

t5_err_t t5_zip_extract_cb(t5_zip_t *z, const t5_zip_entry_t *e, t5_inflate_write_fn wr, void *ctx)
{
    if (!z || !e || !wr) {
        return T5_ERR_INVALID_ARG;
    }
    if (e->is_dir) {
        return T5_OK;
    }
    if (e->method != 0 && e->method != 8) {
        return T5_ERR_NOT_SUPPORTED;
    }
    uint8_t h[LOCAL_LEN];
    if (!read_at(z->f, (long)e->local_offset, h, sizeof h)) {
        return T5_ERR_IO;
    }
    uint16_t nlen = rd16(h + 26), xlen = rd16(h + 28);
    if (rd32(h) != SIG_LOCAL || (rd16(h + 6) & (FLAG_ENCRYPTED | FLAG_STRONG_ENC)) ||
        nlen != strlen(e->name)) {
        return T5_ERR_INVALID_ARG;
    }
    char lname[T5_ZIP_NAME_MAX];
    if (fread(lname, 1, nlen, z->f) != nlen || memcmp(lname, e->name, nlen) != 0) {
        return T5_ERR_INVALID_ARG;
    }
    uint64_t start = (uint64_t)e->local_offset + LOCAL_LEN + nlen + xlen;
    if (start + e->comp_size > z->cd_offset) {
        return T5_ERR_INVALID_ARG;
    }
    if (fseek(z->f, (long)start, SEEK_SET) != 0) {
        return T5_ERR_IO;
    }
    file_in_t in = {z->f, e->comp_size};
    check_out_t out = {wr, ctx, 0, 0, e->size};
    t5_err_t err;
    if (e->method == 0) {
        if (e->comp_size != e->size) {
            return T5_ERR_INVALID_ARG;
        }
        uint8_t *buf = malloc(COPY_CHUNK);
        if (!buf) {
            return T5_ERR_NO_MEM;
        }
        err = T5_OK;
        int n;
        while (err == T5_OK && (n = file_read(&in, buf, COPY_CHUNK)) != 0) {
            err = n < 0 ? T5_ERR_IO : check_write(&out, buf, (size_t)n);
        }
        free(buf);
    } else {
        err = t5_inflate_stream(file_read, &in, check_write, &out, NULL);
    }
    if (err != T5_OK) {
        return err;
    }
    if (out.total != e->size || out.crc != e->crc32) {
        T5_LOGW(TAG, "%s: CRC or size mismatch", e->name);
        return T5_ERR_INVALID_ARG;
    }
    return T5_OK;
}

typedef struct {
    uint8_t *p;
    size_t len;
} buf_out_t;

static t5_err_t buf_write(void *ctx, const uint8_t *data, size_t len)
{
    buf_out_t *b = ctx;
    memcpy(b->p + b->len, data, len);   /* bounded by check_write against e->size */
    b->len += len;
    return T5_OK;
}

t5_err_t t5_zip_extract_mem(t5_zip_t *z, const t5_zip_entry_t *e, void *buf, size_t cap, size_t *out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    if (!z || !e || (!buf && cap)) {
        return T5_ERR_INVALID_ARG;
    }
    if (e->size > cap) {
        return T5_ERR_NO_MEM;
    }
    buf_out_t b = {buf, 0};
    t5_err_t err = t5_zip_extract_cb(z, e, buf_write, &b);
    if (err == T5_OK && out_len) {
        *out_len = b.len;
    }
    return err;
}

t5_err_t t5_zip_extract_alloc(t5_zip_t *z, const t5_zip_entry_t *e, size_t max_size, char **out,
                              size_t *out_len)
{
    if (!out) {
        return T5_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!z || !e) {
        return T5_ERR_INVALID_ARG;
    }
    if (e->size > max_size) {
        return T5_ERR_NO_MEM;
    }
    char *p = malloc((size_t)e->size + 1);
    if (!p) {
        return T5_ERR_NO_MEM;
    }
    size_t n = 0;
    t5_err_t err = t5_zip_extract_mem(z, e, p, e->size, &n);
    if (err != T5_OK) {
        free(p);
        return err;
    }
    p[n] = 0;
    *out = p;
    if (out_len) {
        *out_len = n;
    }
    return T5_OK;
}

static t5_err_t file_write(void *ctx, const uint8_t *data, size_t len)
{
    return fwrite(data, 1, len, (FILE *)ctx) == len ? T5_OK : T5_ERR_IO;
}

t5_err_t t5_zip_extract_file(t5_zip_t *z, const t5_zip_entry_t *e, const char *path)
{
    if (!z || !e || !path || e->is_dir) {
        return T5_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return T5_ERR_IO;
    }
    t5_err_t err = t5_zip_extract_cb(z, e, file_write, f);
    if (err == T5_OK && (fflush(f) != 0 || fsync(fileno(f)) != 0)) {
        err = T5_ERR_IO;
    }
    if (fclose(f) != 0 && err == T5_OK) {
        err = T5_ERR_IO;
    }
    if (err != T5_OK) {
        remove(path);
    }
    return err;
}
