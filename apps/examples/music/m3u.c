/* M3U playlists (m3u.h). */
#include "m3u.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

void m3u_clean(char *path)
{
    for (char *p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    bool abs = path[0] == '/';
    char out[M3U_PATH_MAX];
    size_t o = 0;
    size_t starts[64];
    int depth = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - s);
        if (!n || (n == 1 && s[0] == '.')) continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            if (depth > 0) o = starts[--depth];
            else if (!abs && o + 3 < sizeof out) {       /* a relative path keeps leading ".." */
                memcpy(out + o, o ? "/.." : "..", o ? 3 : 2);
                o += o ? 3 : 2;
            }
            continue;
        }
        if (depth < 64) starts[depth++] = o;
        if (o + n + 2 >= sizeof out) break;
        if (o || abs) out[o++] = '/';
        memcpy(out + o, s, n);
        o += n;
    }
    if (!o && abs) out[o++] = '/';
    out[o] = 0;
    snprintf(path, M3U_PATH_MAX, "%s", out);
}

int m3u_parse(const char *text, size_t len, const char *dir, m3u_entry_fn fn, void *user)
{
    if (len >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf) {
        text += 3, len -= 3;
    }
    char title[128] = "";
    int count = 0;
    size_t i = 0;
    while (i < len) {
        size_t s = i;
        while (i < len && text[i] != '\n' && text[i] != '\r') i++;
        size_t e = i;
        while (i < len && (text[i] == '\n' || text[i] == '\r')) i++;
        while (s < e && (text[s] == ' ' || text[s] == '\t')) s++;
        while (e > s && (text[e - 1] == ' ' || text[e - 1] == '\t')) e--;
        if (s == e) continue;
        size_t n = e - s;
        const char *l = text + s;
        if (l[0] == '#') {
            if (n > 8 && !strncmp(l, "#EXTINF:", 8)) {
                const char *c = memchr(l, ',', n);
                size_t tl = c ? n - (size_t)(c + 1 - l) : 0;
                if (tl >= sizeof title) tl = sizeof title - 1;
                if (c) memcpy(title, c + 1, tl);
                title[tl] = 0;
            }
            continue;
        }
        char path[M3U_PATH_MAX];
        if (n >= sizeof path) n = sizeof path - 1;
        memcpy(path, l, n);
        path[n] = 0;
        char *p = path;
        if (!strncmp(p, "file://", 7)) {
            p += 7;
            /* %20 and friends */
            char *w = p;
            for (char *r = p; *r; r++) {
                int hi, lo;
                if (r[0] == '%' && r[1] && r[2] &&
                    (hi = r[1] <= '9' ? r[1] - '0' : (lower(r[1]) - 'a' + 10)) >= 0 && hi < 16 &&
                    (lo = r[2] <= '9' ? r[2] - '0' : (lower(r[2]) - 'a' + 10)) >= 0 && lo < 16) {
                    *w++ = (char)(hi << 4 | lo);
                    r += 2;
                } else {
                    *w++ = *r;
                }
            }
            *w = 0;
        }
        char full[M3U_PATH_MAX];
        bool drive = p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/');   /* "E:\Music\..." */
        if (p[0] == '/' || drive) snprintf(full, sizeof full, "%s", drive ? p + 2 : p);
        else if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8)) continue;    /* streams: not ours */
        else {
            size_t dl = strlen(dir), pl = strlen(p);
            if (dl + 1 + pl >= sizeof full) continue;     /* too long to be one of ours */
            memcpy(full, dir, dl);
            full[dl] = '/';
            memcpy(full + dl + 1, p, pl + 1);
        }
        m3u_clean(full);
        fn(full, title, user);
        title[0] = 0;
        count++;
    }
    return count;
}

/* The next component of p: its start and length; p moves past it. */
static const char *comp(const char **p, size_t *n)
{
    while (**p == '/') (*p)++;
    const char *s = *p;
    while (**p && **p != '/') (*p)++;
    *n = (size_t)(*p - s);
    return s;
}

static bool same(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

void m3u_relative(char *out, size_t cap, const char *dir, const char *path)
{
    const char *d = dir, *p = path;
    /* skip the folders both share */
    for (;;) {
        const char *d0 = d, *p0 = p;
        size_t dn, pn;
        const char *dc = comp(&d, &dn), *pc = comp(&p, &pn);
        if (!dn || !pn || dn != pn || !same(dc, pc, dn) || !*p) {
            d = d0, p = p0;
            break;
        }
    }
    size_t o = 0;
    out[0] = 0;
    size_t n;
    while (comp(&d, &n), n) {                            /* one ".." per folder left in dir */
        if (o + 4 >= cap) return;
        memcpy(out + o, "../", 4);
        o += 3;
    }
    while (*p == '/') p++;
    snprintf(out + o, cap - o, "%s", p);
}
