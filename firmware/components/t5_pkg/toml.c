/* SPDX-License-Identifier: Apache-2.0
 * TOML subset parser (see toml.h for what is in and out).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t5_pkg/toml.h"

#define KEY_MAX 128
#define STR_MAX 4096
#define ARRAY_MAX 256

typedef struct {
    const char *p, *end;
    int line;
    char *err;
    size_t errlen;
    bool failed;
    t5_toml_doc_t *doc;
} ps_t;

typedef struct {
    char *p;
    size_t n, cap;
} sb_t;

static bool fail(ps_t *s, const char *fmt, ...)
{
    if (!s->failed && s->err && s->errlen) {
        int n = snprintf(s->err, s->errlen, "line %d: ", s->line);
        if (n >= 0 && (size_t)n < s->errlen) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(s->err + n, s->errlen - (size_t)n, fmt, ap);
            va_end(ap);
        }
    }
    s->failed = true;
    return false;
}

static bool sb_put(ps_t *s, sb_t *b, const char *data, size_t len)
{
    if (b->n + len + 1 > STR_MAX) {
        return fail(s, "string too long");
    }
    if (b->n + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 32;
        while (cap < b->n + len + 1) cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) {
            return fail(s, "out of memory");
        }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, len);
    b->n += len;
    b->p[b->n] = 0;
    return true;
}

static void value_free(t5_toml_value_t *v)
{
    free(v->s);
    for (int i = 0; i < v->nitems; i++) {
        value_free(&v->items[i]);
    }
    free(v->items);
    memset(v, 0, sizeof *v);
}

void t5_toml_free(t5_toml_doc_t *doc)
{
    if (!doc) {
        return;
    }
    for (int i = 0; i < doc->n; i++) {
        free(doc->kv[i].key);
        value_free(&doc->kv[i].val);
    }
    free(doc->kv);
    for (int i = 0; i < doc->ntables; i++) {
        free(doc->tables[i]);
    }
    free(doc->tables);
    memset(doc, 0, sizeof *doc);
}

static bool is_bare(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static void skip_ws(ps_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t')) s->p++;
}

static bool skip_comment(ps_t *s)
{
    /* at '#': runs to end of line; control characters other than tab are invalid */
    while (s->p < s->end && *s->p != '\n') {
        unsigned char c = (unsigned char)*s->p;
        if ((c < 0x20 && c != '\t' && c != '\r') || c == 0x7f) {
            return fail(s, "control character in comment");
        }
        if (c == '\r' && (s->p + 1 >= s->end || s->p[1] != '\n')) {
            return fail(s, "bare carriage return");
        }
        s->p++;
    }
    return true;
}

/* Newline at p (LF or CRLF)? Consumes it. */
static bool eat_newline(ps_t *s)
{
    if (s->p < s->end && *s->p == '\n') {
        s->p++;
        s->line++;
        return true;
    }
    if (s->p + 1 < s->end && s->p[0] == '\r' && s->p[1] == '\n') {
        s->p += 2;
        s->line++;
        return true;
    }
    return false;
}

static bool expect_eol(ps_t *s)
{
    skip_ws(s);
    if (s->p < s->end && *s->p == '#' && !skip_comment(s)) {
        return false;
    }
    if (s->p == s->end || eat_newline(s)) {
        return true;
    }
    return fail(s, "expected end of line, found '%c'", *s->p);
}

/* Whitespace, newlines and comments inside arrays. */
static bool skip_ws_nl(ps_t *s)
{
    for (;;) {
        skip_ws(s);
        if (s->p < s->end && *s->p == '#') {
            if (!skip_comment(s)) return false;
            continue;
        }
        if (!eat_newline(s)) return true;
    }
}

static bool parse_key(ps_t *s, char *out)
{
    size_t n = 0;
    for (;;) {
        skip_ws(s);
        if (s->p < s->end && (*s->p == '"' || *s->p == '\'')) {
            return fail(s, "quoted keys are not supported");
        }
        const char *start = s->p;
        while (s->p < s->end && is_bare(*s->p)) s->p++;
        size_t len = (size_t)(s->p - start);
        if (len == 0) {
            return fail(s, "expected a key");
        }
        if (n + len + 2 > KEY_MAX) {
            return fail(s, "key too long");
        }
        if (n) out[n++] = '.';
        memcpy(out + n, start, len);
        n += len;
        out[n] = 0;
        skip_ws(s);
        if (s->p < s->end && *s->p == '.') {
            s->p++;
            continue;
        }
        return true;
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool put_utf8(ps_t *s, sb_t *b, uint32_t cp)
{
    char u[4];
    size_t n;
    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xc0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3f));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xe0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        u[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        u[0] = (char)(0xf0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        u[3] = (char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    return sb_put(s, b, u, n);
}

static bool parse_basic_string(ps_t *s, t5_toml_value_t *v)
{
    sb_t b = {0};
    s->p++;   /* opening quote */
    if (!sb_put(s, &b, "", 0)) goto fail;
    for (;;) {
        if (s->p >= s->end || *s->p == '\n' || *s->p == '\r') {
            fail(s, "unterminated string");
            goto fail;
        }
        unsigned char c = (unsigned char)*s->p++;
        if (c == '"') break;
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            fail(s, "control character in string");
            goto fail;
        }
        if (c != '\\') {
            if (!sb_put(s, &b, (const char *)&c, 1)) goto fail;
            continue;
        }
        if (s->p >= s->end) {
            fail(s, "unterminated string");
            goto fail;
        }
        char e = *s->p++;
        const char *rep = NULL;
        switch (e) {
        case '"': rep = "\""; break;
        case '\\': rep = "\\"; break;
        case 'b': rep = "\b"; break;
        case 't': rep = "\t"; break;
        case 'n': rep = "\n"; break;
        case 'f': rep = "\f"; break;
        case 'r': rep = "\r"; break;
        case 'u':
        case 'U': {
            int nd = e == 'u' ? 4 : 8;
            uint32_t cp = 0;
            for (int i = 0; i < nd; i++) {
                int h = s->p < s->end ? hexval(*s->p) : -1;
                if (h < 0) {
                    fail(s, "bad \\%c escape", e);
                    goto fail;
                }
                cp = (cp << 4) | (uint32_t)h;
                s->p++;
            }
            if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
                fail(s, "invalid unicode escape");
                goto fail;
            }
            if (!put_utf8(s, &b, cp)) goto fail;
            continue;
        }
        default:
            fail(s, "unknown escape \\%c", e);
            goto fail;
        }
        if (!sb_put(s, &b, rep, 1)) goto fail;
    }
    v->type = T5_TOML_STRING;
    v->s = b.p;
    return true;
fail:
    free(b.p);
    return false;
}

static bool parse_literal_string(ps_t *s, t5_toml_value_t *v)
{
    sb_t b = {0};
    s->p++;
    const char *start = s->p;
    while (s->p < s->end && *s->p != '\'') {
        unsigned char c = (unsigned char)*s->p;
        if (c == '\n' || c == '\r') {
            return fail(s, "unterminated string");
        }
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            return fail(s, "control character in string");
        }
        s->p++;
    }
    if (s->p >= s->end) {
        return fail(s, "unterminated string");
    }
    if (!sb_put(s, &b, start, (size_t)(s->p - start))) {
        free(b.p);
        return false;
    }
    if (!b.p) {
        b.p = calloc(1, 1);
        if (!b.p) return fail(s, "out of memory");
    }
    s->p++;
    v->type = T5_TOML_STRING;
    v->s = b.p;
    return true;
}

static bool parse_int(ps_t *s, t5_toml_value_t *v)
{
    bool neg = false;
    if (*s->p == '+' || *s->p == '-') {
        neg = *s->p == '-';
        s->p++;
    }
    const char *start = s->p;
    if (s->p + 1 < s->end && s->p[0] == '0' && (s->p[1] == 'x' || s->p[1] == 'o' || s->p[1] == 'b')) {
        return fail(s, "only decimal integers are supported");
    }
    uint64_t acc = 0;
    int digits = 0;
    bool prev_digit = false;
    while (s->p < s->end && ((*s->p >= '0' && *s->p <= '9') || *s->p == '_')) {
        if (*s->p == '_') {
            if (!prev_digit || s->p + 1 >= s->end || s->p[1] < '0' || s->p[1] > '9') {
                return fail(s, "'_' must be between digits");
            }
            prev_digit = false;
            s->p++;
            continue;
        }
        acc = acc * 10 + (uint64_t)(*s->p - '0');
        if (acc > (uint64_t)INT64_MAX + 1) {
            return fail(s, "integer out of range");
        }
        digits++;
        prev_digit = true;
        s->p++;
    }
    if (digits == 0) {
        return fail(s, "invalid value");
    }
    if (*start == '0' && digits > 1) {
        return fail(s, "leading zeros are not allowed");
    }
    if (s->p < s->end && (*s->p == '.' || *s->p == 'e' || *s->p == 'E')) {
        return fail(s, "floats are not supported");
    }
    if (s->p < s->end && (*s->p == '-' || *s->p == ':')) {
        return fail(s, "dates and times are not supported");
    }
    if (s->p < s->end && is_bare(*s->p)) {
        return fail(s, "invalid value");
    }
    if (!neg && acc > (uint64_t)INT64_MAX) {
        return fail(s, "integer out of range");
    }
    v->type = T5_TOML_INT;
    v->i = neg ? (int64_t)(0 - acc) : (int64_t)acc;
    return true;
}

static bool match_word(ps_t *s, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(s->end - s->p) >= n && memcmp(s->p, w, n) == 0 &&
        (s->p + n == s->end || !is_bare(s->p[n]))) {
        s->p += n;
        return true;
    }
    return false;
}

static bool parse_value(ps_t *s, t5_toml_value_t *v, bool in_array);

static bool parse_array(ps_t *s, t5_toml_value_t *v)
{
    s->p++;   /* '[' */
    v->type = T5_TOML_ARRAY;
    int cap = 0;
    for (;;) {
        if (!skip_ws_nl(s)) return false;
        if (s->p >= s->end) return fail(s, "unterminated array");
        if (*s->p == ']') {
            s->p++;
            return true;
        }
        if (v->nitems == ARRAY_MAX) return fail(s, "array too long");
        if (v->nitems == cap) {
            cap = cap ? cap * 2 : 4;
            t5_toml_value_t *ni = realloc(v->items, (size_t)cap * sizeof *ni);
            if (!ni) return fail(s, "out of memory");
            v->items = ni;
        }
        t5_toml_value_t *it = &v->items[v->nitems];
        memset(it, 0, sizeof *it);
        if (!parse_value(s, it, true)) {
            value_free(it);
            return false;
        }
        v->nitems++;
        if (it->type != v->items[0].type) return fail(s, "mixed types in array");
        if (!skip_ws_nl(s)) return false;
        if (s->p >= s->end) return fail(s, "unterminated array");
        if (*s->p == ',') {
            s->p++;
            continue;
        }
        if (*s->p != ']') return fail(s, "expected ',' or ']' in array");
    }
}

static bool parse_value(ps_t *s, t5_toml_value_t *v, bool in_array)
{
    if (s->p >= s->end || *s->p == '\n' || *s->p == '\r' || *s->p == '#') {
        return fail(s, "missing value");
    }
    char c = *s->p;
    if (c == '"') {
        if (s->end - s->p >= 3 && s->p[1] == '"' && s->p[2] == '"') {
            return fail(s, "multi-line strings are not supported");
        }
        return parse_basic_string(s, v);
    }
    if (c == '\'') {
        if (s->end - s->p >= 3 && s->p[1] == '\'' && s->p[2] == '\'') {
            return fail(s, "multi-line strings are not supported");
        }
        return parse_literal_string(s, v);
    }
    if (c == '[') {
        if (in_array) return fail(s, "nested arrays are not supported");
        return parse_array(s, v);
    }
    if (c == '{') return fail(s, "inline tables are not supported");
    bool t = match_word(s, "true");
    if (t || match_word(s, "false")) {
        if (in_array) return fail(s, "arrays may hold only strings or integers");
        v->type = T5_TOML_BOOL;
        v->b = t;
        return true;
    }
    if ((c >= '0' && c <= '9') || c == '+' || c == '-') {
        return parse_int(s, v);
    }
    if (match_word(s, "inf") || match_word(s, "nan")) return fail(s, "floats are not supported");
    return fail(s, "invalid value");
}

static bool has_prefix_dot(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0 && s[n] == '.';
}

const t5_toml_value_t *t5_toml_get(const t5_toml_doc_t *doc, const char *path)
{
    if (!doc || !path) return NULL;
    for (int i = 0; i < doc->n; i++) {
        if (strcmp(doc->kv[i].key, path) == 0) return &doc->kv[i].val;
    }
    return NULL;
}

bool t5_toml_has_table(const t5_toml_doc_t *doc, const char *name)
{
    if (!doc || !name) return false;
    for (int i = 0; i < doc->ntables; i++) {
        if (strcmp(doc->tables[i], name) == 0 || has_prefix_dot(doc->tables[i], name)) return true;
    }
    for (int i = 0; i < doc->n; i++) {
        if (has_prefix_dot(doc->kv[i].key, name)) return true;
    }
    return false;
}

/* Is some proper dotted prefix of path an existing plain key? */
static bool prefix_is_key(const t5_toml_doc_t *doc, const char *path)
{
    char tmp[KEY_MAX * 2];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *d = strrchr(tmp, '.'); d; d = strrchr(tmp, '.')) {
        *d = 0;
        if (t5_toml_get(doc, tmp)) return true;
    }
    return false;
}

static bool define_table(ps_t *s, const char *name)
{
    t5_toml_doc_t *d = s->doc;
    for (int i = 0; i < d->ntables; i++) {
        if (strcmp(d->tables[i], name) == 0) return fail(s, "table [%s] defined twice", name);
    }
    if (t5_toml_get(d, name) || prefix_is_key(d, name)) {
        return fail(s, "[%s] conflicts with an existing key", name);
    }
    if (d->ntables == d->tcap) {
        int cap = d->tcap ? d->tcap * 2 : 4;
        char **nt = realloc(d->tables, (size_t)cap * sizeof *nt);
        if (!nt) return fail(s, "out of memory");
        d->tables = nt;
        d->tcap = cap;
    }
    char *copy = strdup(name);
    if (!copy) return fail(s, "out of memory");
    d->tables[d->ntables++] = copy;
    return true;
}

static bool define_key(ps_t *s, const char *key, t5_toml_value_t *v, int line)
{
    t5_toml_doc_t *d = s->doc;
    if (t5_toml_get(d, key)) return fail(s, "duplicate key '%s'", key);
    if (t5_toml_has_table(d, key) || prefix_is_key(d, key)) {
        return fail(s, "key '%s' conflicts with a table", key);
    }
    if (d->n == d->cap) {
        int cap = d->cap ? d->cap * 2 : 16;
        t5_toml_kv_t *nk = realloc(d->kv, (size_t)cap * sizeof *nk);
        if (!nk) return fail(s, "out of memory");
        d->kv = nk;
        d->cap = cap;
    }
    char *copy = strdup(key);
    if (!copy) return fail(s, "out of memory");
    d->kv[d->n].key = copy;
    d->kv[d->n].val = *v;
    d->kv[d->n].line = line;
    d->n++;
    memset(v, 0, sizeof *v);
    return true;
}

static bool parse_doc(ps_t *s)
{
    char table[KEY_MAX] = "";
    while (s->p < s->end) {
        skip_ws(s);
        if (s->p >= s->end) break;
        char c = *s->p;
        if (c == '#') {
            if (!skip_comment(s)) return false;
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (!eat_newline(s)) return fail(s, "bare carriage return");
            continue;
        }
        if (c == '[') {
            s->p++;
            if (s->p < s->end && *s->p == '[') return fail(s, "arrays of tables are not supported");
            if (!parse_key(s, table)) return false;
            if (s->p >= s->end || *s->p != ']') return fail(s, "expected ']'");
            s->p++;
            if (!define_table(s, table)) return false;
            if (!expect_eol(s)) return false;
            continue;
        }
        char key[KEY_MAX];
        if (!parse_key(s, key)) return false;
        if (s->p >= s->end || *s->p != '=') return fail(s, "expected '=' after key");
        s->p++;
        skip_ws(s);
        char full[KEY_MAX * 2];
        if (table[0]) {
            snprintf(full, sizeof full, "%s.%s", table, key);
        } else {
            snprintf(full, sizeof full, "%s", key);
        }
        int line = s->line;
        t5_toml_value_t v = {0};
        if (!parse_value(s, &v, false)) {
            value_free(&v);
            return false;
        }
        if (!define_key(s, full, &v, line)) {
            value_free(&v);
            return false;
        }
        if (!expect_eol(s)) return false;
    }
    return true;
}

t5_err_t t5_toml_parse(const char *text, size_t len, t5_toml_doc_t *doc, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    if (!doc || (!text && len)) return T5_ERR_INVALID_ARG;
    memset(doc, 0, sizeof *doc);
    ps_t s = {.p = text, .end = text + len, .line = 1, .err = err, .errlen = errlen, .doc = doc};
    if (len && memchr(text, 0, len)) {
        /* a NUL can only mean a binary file */
        const char *z = memchr(text, 0, len);
        for (const char *q = text; q < z; q++) if (*q == '\n') s.line++;
        fail(&s, "NUL byte in text");
        t5_toml_free(doc);
        return T5_ERR_INVALID_ARG;
    }
    if (!parse_doc(&s)) {
        t5_toml_free(doc);
        return T5_ERR_INVALID_ARG;
    }
    return T5_OK;
}
