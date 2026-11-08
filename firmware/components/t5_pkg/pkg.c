/* SPDX-License-Identifier: Apache-2.0
 * Package installer and registry. Plain POSIX file APIs only, so it runs on
 * the host against a temp directory exactly as it does on FAT on the device.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "t5_core/log.h"
#include "t5_pkg/pkg.h"
#include "t5_pkg/zip.h"

static const char *TAG = "t5.pkg";

#define ROOT_MAX 96
#define PATH_BUF 512
#define MANIFEST_MAX (64 * 1024)
#define MANIFEST_NAME "manifest.toml"
#define INSTALL_NAME ".install"
#define GRANTS_NAME ".grants"

static char s_sd[ROOT_MAX] = "/sd/apps";
static char s_data[ROOT_MAX] = "/data/apps";
static char s_appdata[ROOT_MAX] = "/data/appdata";

static const char *const s_suffix[] = {"~new", "~old", "~del"};
enum { SUF_NEW, SUF_OLD, SUF_DEL };

static void set_root(char *dst, const char *src)
{
    if (!src) return;
    size_t n = strlen(src);
    while (n > 1 && src[n - 1] == '/') n--;      /* no trailing slash */
    if (n >= ROOT_MAX) n = ROOT_MAX - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void t5_pkg_set_roots(const char *sd_apps, const char *data_apps, const char *data_appdata)
{
    set_root(s_sd, sd_apps);
    set_root(s_data, data_apps);
    set_root(s_appdata, data_appdata);
}

static const char *root_path(t5_pkg_dest_t r)
{
    return r == T5_PKG_DEST_SD ? s_sd : s_data;
}

static const char *root_str(t5_pkg_dest_t r)
{
    return r == T5_PKG_DEST_SD ? "sd" : "data";
}

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static bool fmt_path(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n >= 0 && (size_t)n < cap;
}

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static bool mkdir_p(const char *path)
{
    char tmp[PATH_BUF];
    if (!fmt_path(tmp, sizeof tmp, "%s", path)) return false;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST && !is_dir(tmp)) return false;
        *p = '/';
    }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) && is_dir(tmp);
}

/* Delete a tree using one shared path buffer, so deep trees don't cost
 * stack. Restarts the directory scan after each removal rather than deleting
 * under an open readdir (FAT's VFS doesn't promise that is safe). */
static bool rm_rf_buf(char *path, size_t cap)
{
    struct stat st;
    if (stat(path, &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    /* Depth-first without recursion (docs/coding-standard.md, rule 1): an
     * app's data dir can nest as deep as the path buffer allows, which would
     * be far more stack than any task has. path is always the directory
     * being emptied: step into its first entry, or remove it once it's
     * empty and step back out. Each pass removes an entry or goes one level
     * down, so the walk ends. */
    const size_t base = strlen(path);
    for (;;) {
        size_t len = strlen(path);
        DIR *d = opendir(path);
        if (!d) return false;
        struct dirent *de;
        bool found = false;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            found = len + 1 + strlen(de->d_name) < cap;
            if (!found) {
                closedir(d);
                return false;
            }
            path[len] = '/';
            strcpy(path + len + 1, de->d_name);
            break;
        }
        closedir(d);
        if (found) {
            if (stat(path, &st) != 0) return false;
            if (!S_ISDIR(st.st_mode)) {
                if (unlink(path) != 0) return false;
                path[len] = 0;
            }
            continue;   /* a directory: empty it on the next pass */
        }
        if (rmdir(path) != 0) return false;
        if (len == base) return true;
        char *slash = strrchr(path, '/');   /* back out to the parent */
        T5_ASSERT(slash && (size_t)(slash - path) >= base);
        *slash = 0;
    }
}

static bool rm_rf(const char *path)
{
    char buf[PATH_BUF];
    if (!fmt_path(buf, sizeof buf, "%s", path)) return false;
    return rm_rf_buf(buf, sizeof buf);
}

static t5_err_t write_file_sync(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return T5_ERR_IO;
    const uint8_t *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            close(fd);
            unlink(path);
            return errno == ENOSPC ? T5_ERR_NO_MEM : T5_ERR_IO;
        }
        p += n;
        len -= (size_t)n;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(path);
        return T5_ERR_IO;
    }
    if (close(fd) != 0) {
        unlink(path);
        return T5_ERR_IO;
    }
    return T5_OK;
}

static t5_err_t write_atomic(const char *path, const void *data, size_t len)
{
    char tmp[PATH_BUF];
    if (!fmt_path(tmp, sizeof tmp, "%s.tmp", path)) return T5_ERR_INVALID_ARG;
    T5_RETURN_ON_ERR(write_file_sync(tmp, data, len));
    if (rename(tmp, path) != 0) {
        /* FAT won't rename over an existing file. */
        if (unlink(path) != 0 || rename(tmp, path) != 0) {
            unlink(tmp);
            return T5_ERR_IO;
        }
    }
    return T5_OK;
}

static char *read_text(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(max + 1);
    size_t n = buf ? fread(buf, 1, max + 1, f) : 0;
    fclose(f);
    if (!buf || n > max) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

/* --- inspect --------------------------------------------------------------- */

static t5_err_t inspect_zip(t5_zip_t *z, t5_manifest_t *m, char *err, size_t errlen)
{
    const t5_zip_entry_t *me = t5_zip_find(z, MANIFEST_NAME);
    if (!me) {
        seterr(err, errlen, "no manifest.toml at the package root");
        return T5_ERR_NOT_FOUND;
    }
    if (t5_zip_find(z, INSTALL_NAME)) {
        seterr(err, errlen, "package contains the reserved file .install");
        return T5_ERR_DENIED;
    }
    char *text = NULL;
    size_t len = 0;
    t5_err_t e = t5_zip_extract_alloc(z, me, MANIFEST_MAX, &text, &len);
    if (e != T5_OK) {
        seterr(err, errlen, "manifest.toml unreadable (%s)", t5_err_str(e));
        return e;
    }
    char why[160];
    e = t5_manifest_parse(text, len, m, why, sizeof why);
    free(text);
    if (e != T5_OK) {
        seterr(err, errlen, "manifest.toml: %s", why);
        return e;
    }
    const t5_zip_entry_t *ee = t5_zip_find(z, m->entry);
    if (!ee || ee->is_dir) {
        seterr(err, errlen, "entry '%s' is not in the package", m->entry);
        return T5_ERR_NOT_FOUND;
    }
    if (m->icon[0] && !t5_zip_find(z, m->icon)) {
        seterr(err, errlen, "icon '%s' is not in the package", m->icon);
        return T5_ERR_NOT_FOUND;
    }
    return T5_OK;
}

static t5_err_t open_pkg(const char *path, t5_zip_t **z, char *err, size_t errlen)
{
    t5_err_t e = t5_zip_open(path, z);
    if (e == T5_ERR_NOT_FOUND) {
        seterr(err, errlen, "cannot open %s", path);
    } else if (e != T5_OK) {
        seterr(err, errlen, "not a valid package (%s)", t5_err_str(e));
    }
    return e;
}

t5_err_t t5_pkg_inspect(const char *path, t5_manifest_t *out, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    if (!path || !out) return T5_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    t5_zip_t *z;
    T5_RETURN_ON_ERR(open_pkg(path, &z, err, errlen));
    t5_err_t e = inspect_zip(z, out, err, errlen);
    t5_zip_close(z);
    return e;
}

/* --- registry -------------------------------------------------------------- */

typedef struct {
    uint64_t size;
    int64_t time;
} install_rec_t;

static bool read_install_rec(const char *dir, install_rec_t *r)
{
    char p[PATH_BUF];
    if (!fmt_path(p, sizeof p, "%s/" INSTALL_NAME, dir)) return false;
    char *text = read_text(p, 1024, NULL);
    if (!text) return false;
    bool ok = strncmp(text, "t5pkg 1\n", 8) == 0;
    bool got_size = false, got_time = false;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); ok && line; line = strtok_r(NULL, "\n", &save)) {
        unsigned long long u;
        long long t;
        if (sscanf(line, "size=%llu", &u) == 1) {
            r->size = u;
            got_size = true;
        } else if (sscanf(line, "time=%lld", &t) == 1) {
            r->time = t;
            got_time = true;
        }
    }
    free(text);
    return ok && got_size && got_time;
}

static t5_err_t load_info(t5_pkg_dest_t root, const char *id, t5_pkg_info_t *out)
{
    memset(out, 0, sizeof *out);
    out->root = root;
    if (!fmt_path(out->dir, sizeof out->dir, "%s/%s", root_path(root), id) ||
        !fmt_path(out->data_dir, sizeof out->data_dir, "%s/%s", s_appdata, id)) {
        return T5_ERR_INVALID_ARG;
    }
    if (!is_dir(out->dir)) return T5_ERR_NOT_FOUND;
    install_rec_t rec = {0};
    if (!read_install_rec(out->dir, &rec)) {
        T5_LOGW(TAG, "%s: no install record", out->dir);
        return T5_ERR_NOT_FOUND;
    }
    char p[PATH_BUF];
    size_t len;
    if (!fmt_path(p, sizeof p, "%s/" MANIFEST_NAME, out->dir)) return T5_ERR_INVALID_ARG;
    char *text = read_text(p, MANIFEST_MAX, &len);
    if (!text) return T5_ERR_NOT_FOUND;
    char why[160];
    t5_err_t e = t5_manifest_parse(text, len, &out->manifest, why, sizeof why);
    free(text);
    if (e != T5_OK) {
        T5_LOGW(TAG, "%s: bad manifest: %s", out->dir, why);
        return e;
    }
    if (strcmp(out->manifest.id, id) != 0) {
        T5_LOGW(TAG, "%s: manifest id %s does not match", out->dir, out->manifest.id);
        return T5_ERR_INVALID_ARG;
    }
    if (!fmt_path(out->entry_path, sizeof out->entry_path, "%s/%s", out->dir, out->manifest.entry)) {
        return T5_ERR_INVALID_ARG;
    }
    out->size = rec.size;
    out->install_time = rec.time;
    return T5_OK;
}

t5_err_t t5_pkg_find(const char *id, t5_pkg_info_t *out)
{
    if (!t5_manifest_id_ok(id) || !out) return T5_ERR_INVALID_ARG;
    t5_err_t e = load_info(T5_PKG_DEST_SD, id, out);
    if (e == T5_OK) {
        t5_pkg_info_t other;
        out->shadowed = load_info(T5_PKG_DEST_DATA, id, &other) == T5_OK;
        return T5_OK;
    }
    return load_info(T5_PKG_DEST_DATA, id, out);
}

int t5_pkg_list(t5_pkg_info_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    const t5_pkg_dest_t roots[2] = {T5_PKG_DEST_SD, T5_PKG_DEST_DATA};
    for (int r = 0; r < 2 && n < max; r++) {
        DIR *d = opendir(root_path(roots[r]));
        if (!d) continue;
        struct dirent *de;
        while (n < max && (de = readdir(d)) != NULL) {
            if (!t5_manifest_id_ok(de->d_name)) continue;   /* also skips ~new/~old/~del */
            bool dup = false;
            for (int i = 0; i < n; i++) {
                if (out[i].root != roots[r] && strcmp(out[i].manifest.id, de->d_name) == 0) {
                    out[i].shadowed = true;
                    dup = true;
                    T5_LOGW(TAG, "%s installed on both SD and internal storage; using SD", de->d_name);
                }
            }
            if (dup) continue;
            if (load_info(roots[r], de->d_name, &out[n]) == T5_OK) n++;
        }
        closedir(d);
    }
    return n;
}

/* --- recovery -------------------------------------------------------------- */

static int recover_id(const char *root, const char *id)
{
    char fin[PATH_BUF], nw[PATH_BUF], old[PATH_BUF], del[PATH_BUF], rec[PATH_BUF];
    if (!fmt_path(fin, sizeof fin, "%s/%s", root, id) ||
        !fmt_path(nw, sizeof nw, "%s/%s%s", root, id, s_suffix[SUF_NEW]) ||
        !fmt_path(old, sizeof old, "%s/%s%s", root, id, s_suffix[SUF_OLD]) ||
        !fmt_path(del, sizeof del, "%s/%s%s", root, id, s_suffix[SUF_DEL]) ||
        !fmt_path(rec, sizeof rec, "%s/" INSTALL_NAME, nw)) {
        return 0;
    }
    int fixed = 0;
    bool have_new = is_dir(nw), new_complete = have_new && is_file(rec);
    if (!is_dir(fin) && (is_dir(old) || new_complete)) {
        /* Cut during the swap: the staged copy is only complete once its
         * install record exists; otherwise put the old version back. */
        const char *from = new_complete ? nw : old;
        if (rename(from, fin) == 0) {
            T5_LOGI(TAG, "%s: recovered %s install", id, new_complete ? "new" : "previous");
            fixed++;
        }
    }
    const char *const leftovers[] = {nw, old, del};
    for (int i = 0; i < 3; i++) {
        if (is_dir(leftovers[i])) {
            rm_rf(leftovers[i]);
            fixed++;
        }
    }
    return fixed;
}

static int recover_root(const char *root)
{
    DIR *d = opendir(root);
    if (!d) return 0;
    char **ids = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        for (int s = 0; s < 3; s++) {
            size_t sl = strlen(s_suffix[s]);
            if (len > sl && strcmp(de->d_name + len - sl, s_suffix[s]) == 0) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 8;
                    char **ni = realloc(ids, (size_t)cap * sizeof *ni);
                    if (!ni) break;
                    ids = ni;
                }
                ids[n] = strndup(de->d_name, len - sl);
                if (ids[n]) n++;
                break;
            }
        }
    }
    closedir(d);
    int fixed = 0;
    for (int i = 0; i < n; i++) {
        if (t5_manifest_id_ok(ids[i])) {
            fixed += recover_id(root, ids[i]);
        }
        free(ids[i]);
    }
    free(ids);
    return fixed;
}

int t5_pkg_recover(void)
{
    return recover_root(s_sd) + recover_root(s_data);
}

/* --- install / remove ------------------------------------------------------ */

static bool sd_usable(void)
{
    /* "Mounted-ish": the apps dir exists or can be made, i.e. /sd is there. */
    return is_dir(s_sd) || (mkdir(s_sd, 0755) == 0 && is_dir(s_sd));
}

static bool remove_dir_safely(const char *root, const char *id)
{
    char fin[PATH_BUF], del[PATH_BUF];
    if (!fmt_path(fin, sizeof fin, "%s/%s", root, id) ||
        !fmt_path(del, sizeof del, "%s/%s%s", root, id, s_suffix[SUF_DEL])) {
        return false;
    }
    if (!is_dir(fin)) return false;
    rm_rf(del);
    /* Rename first so a cut mid-delete can't leave a half app that looks installed. */
    if (rename(fin, del) != 0) {
        return rm_rf(fin);
    }
    rm_rf(del);
    return true;
}

static t5_err_t extract_all(t5_zip_t *z, const char *stage, uint64_t *total, char *err, size_t errlen)
{
    char p[PATH_BUF];
    *total = 0;
    for (int i = 0; i < t5_zip_count(z); i++) {
        const t5_zip_entry_t *e = t5_zip_entry(z, i);
        if (!fmt_path(p, sizeof p, "%s/%s", stage, e->name)) {
            seterr(err, errlen, "path too long: %s", e->name);
            return T5_ERR_INVALID_ARG;
        }
        if (e->is_dir) {
            p[strlen(p) - 1] = 0;
            if (!mkdir_p(p)) {
                seterr(err, errlen, "cannot create %s", e->name);
                return T5_ERR_IO;
            }
            continue;
        }
        char *slash = strrchr(p, '/');
        *slash = 0;
        bool ok = mkdir_p(p);
        *slash = '/';
        if (!ok) {
            seterr(err, errlen, "cannot create directory for %s", e->name);
            return T5_ERR_IO;
        }
        t5_err_t r = t5_zip_extract_file(z, e, p);
        if (r != T5_OK) {
            seterr(err, errlen, "%s: extract failed (%s)", e->name, t5_err_str(r));
            return r;
        }
        *total += e->size;
    }
    return T5_OK;
}

t5_err_t t5_pkg_install_ex(const char *path, t5_pkg_dest_t dest, uint32_t flags, t5_pkg_info_t *out,
                           char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    if (!path || dest > T5_PKG_DEST_DATA) return T5_ERR_INVALID_ARG;
    t5_zip_t *z;
    T5_RETURN_ON_ERR(open_pkg(path, &z, err, errlen));
    /* The paths live on the heap: five PATH_BUFs would take this frame past
     * the strict components' stack-usage limit (docs/coding-standard.md). */
    struct {
        char fin[PATH_BUF], stage[PATH_BUF], oldp[PATH_BUF], rec[PATH_BUF], data[PATH_BUF];
    } *pb = NULL;
    t5_manifest_t m;
    t5_err_t e = inspect_zip(z, &m, err, errlen);
    if (e != T5_OK) goto done;
    const char *id = m.id;

    /* Finish any interrupted operation on this id before looking at it. */
    recover_id(s_sd, id);
    recover_id(s_data, id);

    t5_pkg_info_t *old = malloc(sizeof *old);
    if (!old) {
        e = T5_ERR_NO_MEM;
        goto done;
    }
    bool have_old = t5_pkg_find(id, old) == T5_OK;
    if (have_old && !(flags & T5_PKG_ALLOW_DOWNGRADE) && t5_version_cmp(m.version, old->manifest.version) < 0) {
        seterr(err, errlen, "%s %s is older than the installed %s", id, m.version, old->manifest.version);
        free(old);
        e = T5_ERR_DENIED;
        goto done;
    }
    t5_pkg_dest_t old_root = have_old ? old->root : T5_PKG_DEST_DATA;
    free(old);

    if (dest == T5_PKG_DEST_AUTO) {
        if (have_old && (old_root == T5_PKG_DEST_DATA || sd_usable())) {
            dest = old_root;
        } else {
            dest = sd_usable() ? T5_PKG_DEST_SD : T5_PKG_DEST_DATA;
        }
    }
    if (dest == T5_PKG_DEST_SD ? !sd_usable() : !mkdir_p(s_data)) {
        seterr(err, errlen, "%s storage is not available", dest == T5_PKG_DEST_SD ? "SD card" : "internal");
        e = T5_ERR_NOT_FOUND;
        goto done;
    }
    const char *root = root_path(dest);
    pb = malloc(sizeof *pb);
    if (!pb) {
        e = T5_ERR_NO_MEM;
        goto done;
    }
    char *fin = pb->fin, *stage = pb->stage, *oldp = pb->oldp, *rec = pb->rec;
    if (!fmt_path(fin, PATH_BUF, "%s/%s", root, id) ||
        !fmt_path(stage, PATH_BUF, "%s/%s%s", root, id, s_suffix[SUF_NEW]) ||
        !fmt_path(oldp, PATH_BUF, "%s/%s%s", root, id, s_suffix[SUF_OLD]) ||
        !fmt_path(rec, PATH_BUF, "%s/" INSTALL_NAME, stage)) {
        seterr(err, errlen, "install path too long");
        e = T5_ERR_INVALID_ARG;
        goto done;
    }

    rm_rf(stage);
    if (mkdir(stage, 0755) != 0) {
        seterr(err, errlen, "cannot create %s", stage);
        e = T5_ERR_IO;
        goto done;
    }
    uint64_t total = 0;
    e = extract_all(z, stage, &total, err, errlen);
    if (e != T5_OK) {
        rm_rf(stage);
        goto done;
    }
    char record[256];
    int rn = snprintf(record, sizeof record, "t5pkg 1\ndest=%s\nsize=%" PRIu64 "\ntime=%lld\nversion=%s\n",
                      root_str(dest), total, (long long)time(NULL), m.version);
    /* Written last: its presence marks the staged copy as complete. */
    e = write_file_sync(rec, record, (size_t)rn);
    if (e != T5_OK) {
        seterr(err, errlen, "cannot write install record");
        rm_rf(stage);
        goto done;
    }

    /* Swap: old -> ~old, ~new -> final, drop ~old. */
    bool had_fin = is_dir(fin);
    if (had_fin && rename(fin, oldp) != 0) {
        seterr(err, errlen, "cannot move the previous version aside");
        rm_rf(stage);
        e = T5_ERR_IO;
        goto done;
    }
    if (rename(stage, fin) != 0) {
        if (had_fin) rename(oldp, fin);
        rm_rf(stage);
        seterr(err, errlen, "cannot activate the new version");
        e = T5_ERR_IO;
        goto done;
    }
    rm_rf(oldp);
    /* Moved between roots: drop the copy on the other one. */
    remove_dir_safely(root_path(dest == T5_PKG_DEST_SD ? T5_PKG_DEST_DATA : T5_PKG_DEST_SD), id);

    if (fmt_path(pb->data, sizeof pb->data, "%s/%s", s_appdata, id) && !mkdir_p(pb->data)) {
        T5_LOGW(TAG, "%s: cannot create data dir %s", id, pb->data);
    }
    T5_LOGI(TAG, "installed %s %s on %s (%" PRIu64 " bytes)%s", id, m.version, root_str(dest), total,
            have_old ? ", upgrade" : "");
    if (out) {
        e = load_info(dest, id, out);
        if (e != T5_OK) seterr(err, errlen, "installed but unreadable");
    }
done:
    free(pb);
    t5_zip_close(z);
    return e;
}

t5_err_t t5_pkg_install(const char *path, t5_pkg_dest_t dest, t5_pkg_info_t *out, char *err, size_t errlen)
{
    return t5_pkg_install_ex(path, dest, 0, out, err, errlen);
}

t5_err_t t5_pkg_remove(const char *id, bool keep_data)
{
    if (!t5_manifest_id_ok(id)) return T5_ERR_INVALID_ARG;
    bool found = remove_dir_safely(s_sd, id);
    found = remove_dir_safely(s_data, id) || found;
    if (!keep_data) {
        char data[PATH_BUF];
        if (fmt_path(data, sizeof data, "%s/%s", s_appdata, id)) rm_rf(data);
    }
    if (found) T5_LOGI(TAG, "removed %s%s", id, keep_data ? " (data kept)" : "");
    return found ? T5_OK : T5_ERR_NOT_FOUND;
}

/* --- grants ---------------------------------------------------------------- */

t5_err_t t5_pkg_grants_load(const char *id, uint32_t *granted, uint32_t *denied)
{
    if (granted) *granted = 0;
    if (denied) *denied = 0;
    if (!t5_manifest_id_ok(id) || !granted || !denied) return T5_ERR_INVALID_ARG;
    char p[PATH_BUF];
    if (!fmt_path(p, sizeof p, "%s/%s/" GRANTS_NAME, s_appdata, id)) return T5_ERR_INVALID_ARG;
    char *text = read_text(p, 256, NULL);
    if (!text) return T5_ERR_NOT_FOUND;
    unsigned g, d;
    int n = sscanf(text, "t5grants 1\ngranted=%x\ndenied=%x", &g, &d);
    free(text);
    if (n != 2) return T5_ERR_FAIL;
    *granted = g & T5_PERM_ALL;
    *denied = d & T5_PERM_ALL & ~*granted;
    return T5_OK;
}

t5_err_t t5_pkg_grants_save(const char *id, uint32_t granted, uint32_t denied)
{
    if (!t5_manifest_id_ok(id)) return T5_ERR_INVALID_ARG;
    char dir[PATH_BUF], p[PATH_BUF];
    if (!fmt_path(dir, sizeof dir, "%s/%s", s_appdata, id) ||
        !fmt_path(p, sizeof p, "%s/" GRANTS_NAME, dir)) {
        return T5_ERR_INVALID_ARG;
    }
    if (!mkdir_p(dir)) return T5_ERR_IO;
    granted &= T5_PERM_ALL;
    denied &= T5_PERM_ALL & ~granted;
    char text[64];
    int n = snprintf(text, sizeof text, "t5grants 1\ngranted=%x\ndenied=%x\n", (unsigned)granted, (unsigned)denied);
    return write_atomic(p, text, (size_t)n);
}
