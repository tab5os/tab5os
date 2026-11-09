/* SPDX-License-Identifier: Apache-2.0
 * Manifest validation on top of the TOML subset parser.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t5_pkg/manifest.h"
#include "t5_pkg/toml.h"
#include "t5_pkg/zip.h"

const t5_perm_info_t t5_perms[] = {
    {T5_PERM_CAMERA, "camera", true},
    {T5_PERM_MIC, "mic", true},
    {T5_PERM_NET, "net", false},
    {T5_PERM_BLE, "ble", false},
    {T5_PERM_PORTS, "ports", false},
    {T5_PERM_USB, "usb", false},
    {T5_PERM_STORAGE, "storage", false},
    {T5_PERM_SHARED_READ, "shared_read", true},
    {T5_PERM_SHARED_WRITE, "shared_write", true},
    {T5_PERM_ALL_FILES, "all_files", true},
    {T5_PERM_BACKGROUND_WAKE, "background_wake", false},
    {T5_PERM_SYSTEM, "system", false},
};
const size_t t5_perm_count = sizeof t5_perms / sizeof t5_perms[0];

static const char *const s_runtimes[T5_RUNTIME_COUNT] = {
    [T5_RUNTIME_NATIVE] = "native",
    [T5_RUNTIME_MICROPYTHON] = "micropython",
};

/* The keys a manifest may have (tools/t5/t5pack.py reads these tables). */
static const char *const s_top_keys[] = {"id", "name", "version", "api", "runtime", "entry", "icon",
                                         "min_heap_mb", "min_heap_kb", "orientation"};
/* Top-level keys accepted but not stored (informational). */
static const char *const s_info_keys[] = {"description", "author", "homepage", "license"};
static const char *const s_bg_keys[] = {"entry", "max_seconds"};
static const char *const s_orients[] = {
    [T5_ORIENT_ANY] = "any",
    [T5_ORIENT_LANDSCAPE] = "landscape",
    [T5_ORIENT_PORTRAIT] = "portrait",
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static bool listed(const char *k, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(k, list[i]) == 0) return true;
    }
    return false;
}

const char *t5_perm_name(uint32_t perm_bit)
{
    for (size_t i = 0; i < t5_perm_count; i++) {
        if (t5_perms[i].bit == perm_bit) return t5_perms[i].name;
    }
    return NULL;
}

uint32_t t5_perm_from_name(const char *name)
{
    if (!name) return 0;
    for (size_t i = 0; i < t5_perm_count; i++) {
        if (strcmp(t5_perms[i].name, name) == 0) return t5_perms[i].bit;
    }
    return 0;
}

uint32_t t5_perm_prompted(void)
{
    uint32_t m = 0;
    for (size_t i = 0; i < t5_perm_count; i++) {
        if (t5_perms[i].prompted) m |= t5_perms[i].bit;
    }
    return m;
}

const char *t5_runtime_name(t5_runtime_t rt)
{
    return ((unsigned)rt < T5_RUNTIME_COUNT) ? s_runtimes[rt] : "?";
}

bool t5_manifest_id_ok(const char *id)
{
    if (!id) return false;
    size_t len = strlen(id);
    if (len == 0 || len >= T5_APP_ID_LEN) return false;
    int segs = 1;
    size_t seglen = 0;
    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        if (c == '.') {
            if (seglen == 0) return false;
            segs++;
            seglen = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            seglen++;
        } else {
            return false;
        }
    }
    return seglen > 0 && segs >= 2;
}

typedef struct {
    uint32_t num[3];
    const char *pre;      /* prerelease, not terminated: ends at '+' or NUL */
    size_t prelen;
} semver_t;

static bool parse_num(const char **pp, uint32_t *out)
{
    const char *p = *pp;
    if (*p < '0' || *p > '9') return false;
    if (p[0] == '0' && p[1] >= '0' && p[1] <= '9') return false;   /* leading zero */
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xffffffffu) return false;
        p++;
    }
    *out = (uint32_t)v;
    *pp = p;
    return true;
}

static bool ident_ok(const char *p, size_t len)
{
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool idents_ok(const char *p, size_t len)
{
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || p[i] == '.') {
            if (!ident_ok(p + start, i - start)) return false;
            start = i + 1;
        }
    }
    return true;
}

static bool semver_parse(const char *v, semver_t *sv)
{
    if (!v || strlen(v) >= T5_APP_VERSION_LEN) return false;
    const char *p = v;
    for (int i = 0; i < 3; i++) {
        if (!parse_num(&p, &sv->num[i])) return false;
        if (i < 2) {
            if (*p != '.') return false;
            p++;
        }
    }
    sv->pre = NULL;
    sv->prelen = 0;
    if (*p == '-') {
        p++;
        sv->pre = p;
        while (*p && *p != '+') p++;
        sv->prelen = (size_t)(p - sv->pre);
        if (!idents_ok(sv->pre, sv->prelen)) return false;
    }
    if (*p == '+') {
        p++;
        if (!idents_ok(p, strlen(p))) return false;
        p += strlen(p);
    }
    return *p == 0;
}

bool t5_version_ok(const char *v)
{
    semver_t sv;
    return semver_parse(v, &sv);
}

static bool all_digits(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
    }
    return n > 0;
}

static int cmp_ident(const char *a, size_t an, const char *b, size_t bn)
{
    bool ad = all_digits(a, an), bd = all_digits(b, bn);
    if (ad && bd) {
        if (an != bn) return an < bn ? -1 : 1;    /* no leading zeros: longer is bigger */
        int c = memcmp(a, b, an);
        return c < 0 ? -1 : c > 0;
    }
    if (ad != bd) return ad ? -1 : 1;             /* numeric < alphanumeric */
    int c = memcmp(a, b, an < bn ? an : bn);
    if (c) return c < 0 ? -1 : 1;
    return an == bn ? 0 : (an < bn ? -1 : 1);
}

int t5_version_cmp(const char *a, const char *b)
{
    semver_t x, y;
    bool xa = semver_parse(a, &x), yb = semver_parse(b, &y);
    if (!xa || !yb) {
        if (xa != yb) return xa ? 1 : -1;
        int c = strcmp(a ? a : "", b ? b : "");
        return c < 0 ? -1 : c > 0;
    }
    for (int i = 0; i < 3; i++) {
        if (x.num[i] != y.num[i]) return x.num[i] < y.num[i] ? -1 : 1;
    }
    if (!x.pre || !y.pre) {
        if (!x.pre && !y.pre) return 0;
        return x.pre ? -1 : 1;                    /* 1.0.0-rc < 1.0.0 */
    }
    const char *p = x.pre, *q = y.pre, *pe = x.pre + x.prelen, *qe = y.pre + y.prelen;
    bool pdone = false, qdone = false;
    for (;;) {
        if (pdone || qdone) {
            if (pdone && qdone) return 0;
            return pdone ? -1 : 1;                /* fewer fields sorts first */
        }
        const char *pd = memchr(p, '.', (size_t)(pe - p)), *qd = memchr(q, '.', (size_t)(qe - q));
        size_t pn = (size_t)((pd ? pd : pe) - p), qn = (size_t)((qd ? qd : qe) - q);
        int c = cmp_ident(p, pn, q, qn);
        if (c) return c;
        if (pd) p = pd + 1; else pdone = true;
        if (qd) q = qd + 1; else qdone = true;
    }
}

typedef struct {
    char *err;
    size_t errlen;
} ctx_t;

static t5_err_t bad(ctx_t *c, const char *fmt, ...)
{
    if (c->err && c->errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err, c->errlen, fmt, ap);
        va_end(ap);
    }
    return T5_ERR_INVALID_ARG;
}

static bool printable(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || *s == 0x7f) return false;
    }
    return true;
}

static t5_err_t get_str(ctx_t *c, const t5_toml_doc_t *d, const char *key, bool required, char *dst,
                        size_t cap, bool *present)
{
    const t5_toml_value_t *v = t5_toml_get(d, key);
    if (present) *present = v != NULL;
    if (!v) {
        return required ? bad(c, "%s: missing", key) : T5_OK;
    }
    if (v->type != T5_TOML_STRING) return bad(c, "%s: must be a string", key);
    size_t len = strlen(v->s);
    if (len == 0) return bad(c, "%s: must not be empty", key);
    if (len >= cap) return bad(c, "%s: longer than %u bytes", key, (unsigned)(cap - 1));
    if (!printable(v->s)) return bad(c, "%s: control characters", key);
    memcpy(dst, v->s, len + 1);
    return T5_OK;
}

static t5_err_t get_int(ctx_t *c, const t5_toml_doc_t *d, const char *key, int64_t lo, int64_t hi,
                        int64_t *out, bool *present)
{
    const t5_toml_value_t *v = t5_toml_get(d, key);
    *present = v != NULL;
    if (!v) return T5_OK;
    if (v->type != T5_TOML_INT) return bad(c, "%s: must be an integer", key);
    if (v->i < lo || v->i > hi) {
        return bad(c, "%s: %lld out of range %lld..%lld", key, (long long)v->i, (long long)lo, (long long)hi);
    }
    *out = v->i;
    return T5_OK;
}

static bool path_ok(const char *p)
{
    size_t n = strlen(p);
    return t5_zip_name_ok(p, n) && p[n - 1] != '/';
}

static bool port_name_ok(const char *p)
{
    if (!*p) return false;
    for (; *p; p++) {
        /* '.': the M5-Bus ports are "mbus.i2c", "mbus.g2"... */
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '.')) return false;
    }
    return true;
}

static bool c_ident_ok(const char *p)
{
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) return false;
    for (p++; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            return false;
        }
    }
    return true;
}

static t5_err_t check_keys(ctx_t *c, const t5_toml_doc_t *d)
{
    for (int i = 0; i < d->n; i++) {
        const char *k = d->kv[i].key;
        bool ok;
        if (strncmp(k, "permissions.", 12) == 0) {
            ok = strcmp(k + 12, "ports") == 0 || t5_perm_from_name(k + 12);
        } else if (strncmp(k, "background.", 11) == 0) {
            ok = listed(k + 11, s_bg_keys, COUNT(s_bg_keys));
        } else {
            ok = listed(k, s_top_keys, COUNT(s_top_keys)) || listed(k, s_info_keys, COUNT(s_info_keys));
        }
        if (!ok) return bad(c, "line %d: unknown key '%s'", d->kv[i].line, k);
    }
    for (int i = 0; i < d->ntables; i++) {
        if (strcmp(d->tables[i], "permissions") != 0 && strcmp(d->tables[i], "background") != 0) {
            return bad(c, "unknown table [%s]", d->tables[i]);
        }
    }
    return T5_OK;
}

static t5_err_t validate(ctx_t *c, const t5_toml_doc_t *d, t5_manifest_t *m)
{
    T5_RETURN_ON_ERR(check_keys(c, d));

    T5_RETURN_ON_ERR(get_str(c, d, "id", true, m->id, sizeof m->id, NULL));
    if (!t5_manifest_id_ok(m->id)) {
        return bad(c, "id: '%s' is not reverse-DNS (lowercase a-z, 0-9, _; at least two parts)", m->id);
    }
    T5_RETURN_ON_ERR(get_str(c, d, "name", true, m->name, sizeof m->name, NULL));
    T5_RETURN_ON_ERR(get_str(c, d, "version", true, m->version, sizeof m->version, NULL));
    if (!t5_version_ok(m->version)) return bad(c, "version: '%s' is not MAJOR.MINOR.PATCH", m->version);

    int64_t api = 0;
    bool present;
    const t5_toml_value_t *av = t5_toml_get(d, "api");
    if (!av) return bad(c, "api: missing");
    if (av->type != T5_TOML_INT) return bad(c, "api: must be an integer");
    if (av->i < 1) return bad(c, "api: must be at least 1");
    if (av->i > T5_API_LEVEL) {
        return bad(c, "api: app needs API level %lld, this firmware has %d", (long long)av->i, T5_API_LEVEL);
    }
    api = av->i;
    m->api = (int)api;

    char rt[16];
    T5_RETURN_ON_ERR(get_str(c, d, "runtime", true, rt, sizeof rt, NULL));
    m->runtime = T5_RUNTIME_COUNT;
    for (int i = 0; i < T5_RUNTIME_COUNT; i++) {
        if (strcmp(rt, s_runtimes[i]) == 0) m->runtime = (t5_runtime_t)i;
    }
    if (m->runtime == T5_RUNTIME_COUNT) return bad(c, "runtime: unknown '%s' (native or micropython)", rt);

    T5_RETURN_ON_ERR(get_str(c, d, "entry", true, m->entry, sizeof m->entry, NULL));
    if (!path_ok(m->entry)) return bad(c, "entry: '%s' is not a relative file path", m->entry);
    T5_RETURN_ON_ERR(get_str(c, d, "icon", false, m->icon, sizeof m->icon, &present));
    if (present && !path_ok(m->icon)) return bad(c, "icon: '%s' is not a relative file path", m->icon);

    int64_t mb = 0, kb = 0;
    bool has_mb, has_kb;
    T5_RETURN_ON_ERR(get_int(c, d, "min_heap_mb", 0, T5_MANIFEST_MAX_HEAP_KB / 1024, &mb, &has_mb));
    T5_RETURN_ON_ERR(get_int(c, d, "min_heap_kb", 0, T5_MANIFEST_MAX_HEAP_KB, &kb, &has_kb));
    if (has_mb && has_kb) return bad(c, "min_heap_mb and min_heap_kb are exclusive");
    m->min_heap_kb = has_mb ? (uint32_t)mb * 1024u : (uint32_t)kb;

    char orient[16];
    m->orientation = T5_ORIENT_LANDSCAPE;
    T5_RETURN_ON_ERR(get_str(c, d, "orientation", false, orient, sizeof orient, &present));
    if (present) {
        size_t i = 0;
        while (i < COUNT(s_orients) && strcmp(orient, s_orients[i])) i++;
        if (i == COUNT(s_orients)) return bad(c, "orientation: unknown '%s' (any, landscape or portrait)", orient);
        m->orientation = (t5_orient_pref_t)i;
    }

    for (size_t i = 0; i < t5_perm_count; i++) {
        if (t5_perms[i].bit == T5_PERM_PORTS) continue;
        char key[48];
        snprintf(key, sizeof key, "permissions.%s", t5_perms[i].name);
        const t5_toml_value_t *v = t5_toml_get(d, key);
        if (!v) continue;
        if (v->type != T5_TOML_BOOL) return bad(c, "%s: must be true or false", key);
        if (v->b) m->perms |= t5_perms[i].bit;
    }
    const t5_toml_value_t *pv = t5_toml_get(d, "permissions.ports");
    if (pv) {
        if (pv->type != T5_TOML_ARRAY) return bad(c, "permissions.ports: must be an array of port names");
        if (pv->nitems > T5_MANIFEST_MAX_PORTS) {
            return bad(c, "permissions.ports: at most %d ports", T5_MANIFEST_MAX_PORTS);
        }
        for (int i = 0; i < pv->nitems; i++) {
            const t5_toml_value_t *it = &pv->items[i];
            if (it->type != T5_TOML_STRING) return bad(c, "permissions.ports: must be an array of port names");
            if (strlen(it->s) >= sizeof m->ports[0] || !port_name_ok(it->s)) {
                return bad(c, "permissions.ports: bad port name '%s'", it->s);
            }
            for (int j = 0; j < m->nports; j++) {
                if (strcmp(m->ports[j], it->s) == 0) return bad(c, "permissions.ports: '%s' listed twice", it->s);
            }
            strcpy(m->ports[m->nports++], it->s);
        }
        if (m->nports) m->perms |= T5_PERM_PORTS;
    }

    m->bg_max_seconds = T5_MANIFEST_BG_DEFAULT_SECONDS;
    if (t5_toml_has_table(d, "background")) {
        m->background = true;
        if (!(m->perms & T5_PERM_BACKGROUND_WAKE)) {
            return bad(c, "[background] needs background_wake = true in [permissions]");
        }
        strcpy(m->bg_entry, T5_MANIFEST_BG_DEFAULT_ENTRY);
        T5_RETURN_ON_ERR(get_str(c, d, "background.entry", false, m->bg_entry, sizeof m->bg_entry, NULL));
        if (!c_ident_ok(m->bg_entry)) return bad(c, "background.entry: '%s' is not a function name", m->bg_entry);
        int64_t secs = 0;
        T5_RETURN_ON_ERR(get_int(c, d, "background.max_seconds", 1, T5_MANIFEST_BG_MAX_SECONDS, &secs, &present));
        if (present) m->bg_max_seconds = (uint32_t)secs;
    }
    return T5_OK;
}

t5_err_t t5_manifest_parse(const char *text, size_t len, t5_manifest_t *out, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    if (!out || (!text && len)) return T5_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    t5_toml_doc_t doc;
    t5_err_t e = t5_toml_parse(text, len, &doc, err, errlen);
    if (e != T5_OK) {
        t5_toml_free(&doc);
        return e;
    }
    ctx_t c = {err, errlen};
    e = validate(&c, &doc, out);
    t5_toml_free(&doc);
    if (e != T5_OK) {
        memset(out, 0, sizeof *out);
    }
    return e;
}
