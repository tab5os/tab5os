/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_app/fspolicy.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include "t5_pkg/manifest.h"

static const char *const s_protected[] = { "/data/sys", "/data/apps", "/data/appdata", "/sd/apps", "/sd/appdata" };
static const char *const s_shared[] = { "/data/user",  "/sd/DCIM",  "/sd/Photos",    "/sd/Pictures",
                                        "/sd/Music",   "/sd/Videos", "/sd/Documents", "/sd/Downloads" };
static const char *const s_all_roots[] = { "/sd", "/usb", "/data/user" };

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

bool t5_fs_under(const char *abs, const char *root)
{
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    return n && !strncasecmp(abs, root, n) && (abs[n] == 0 || abs[n] == '/');
}

static bool under_any(const char *abs, const char *const *roots, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (t5_fs_under(abs, roots[i])) return true;
    }
    return false;
}

bool t5_fs_protected(const char *abs)
{
    return under_any(abs, s_protected, COUNT(s_protected));
}

static bool bad_segment(const char *seg, size_t len)
{
    if (len == 2 && seg[0] == '.' && seg[1] == '.') return true;
    if (seg[len - 1] == '.' || seg[len - 1] == ' ') return true;
    /* An 8.3 alias: APPDAT~1 is another name for appdata on FAT. */
    if (len <= 12) {
        for (size_t i = 0; i + 1 < len; i++) {
            if (seg[i] == '~' && isdigit((unsigned char)seg[i + 1])) return true;
        }
    }
    return false;
}

bool t5_fs_normalise(const char *in, char *out, size_t n)
{
    if (!in || in[0] != '/' || n < 2) return false;
    size_t w = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        if (!(len == 1 && p[0] == '.')) {
            if (bad_segment(p, len) || w + 1 + len + 1 > n) return false;
            out[w++] = '/';
            memcpy(out + w, p, len);
            w += len;
        }
        p = e;
    }
    if (!w) out[w++] = '/';
    out[w] = 0;
    return true;
}

t5_fs_verdict_t t5_fs_check(const t5_fs_scope_t *s, const char *abs, bool write, uint32_t *need)
{
    if (s->data_dir && s->data_dir[0] && t5_fs_under(abs, s->data_dir)) return T5_FS_ALLOW;
    if (s->install_dir && s->install_dir[0] && t5_fs_under(abs, s->install_dir)) return write ? T5_FS_DENY : T5_FS_ALLOW;
    for (int i = 0; i < s->npicks; i++) {
        if (!strcasecmp(abs, s->picks[i].path) && (!write || s->picks[i].write)) return T5_FS_ALLOW;
    }
    if (t5_fs_protected(abs)) return T5_FS_DENY;
    if (under_any(abs, s_shared, COUNT(s_shared))) {
        *need = (write ? 0 : T5_PERM_SHARED_READ) | T5_PERM_SHARED_WRITE | T5_PERM_ALL_FILES;
        return T5_FS_NEEDS_PERM;
    }
    if (under_any(abs, s_all_roots, COUNT(s_all_roots))) {
        *need = T5_PERM_ALL_FILES;
        return T5_FS_NEEDS_PERM;
    }
    return T5_FS_DENY;
}

bool t5_fs_quota_ok(uint64_t used, size_t n)
{
    return used + n <= T5_FS_QUOTA_BYTES;
}

bool t5_fs_ext_matches(const char *name, const char *exts)
{
    if (!exts || !exts[0]) return true;
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return false;
    dot++;
    size_t dl = strlen(dot);
    for (const char *p = exts; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == dl && !strncasecmp(p, dot, len)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}
