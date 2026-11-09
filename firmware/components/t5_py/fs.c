/* SPDX-License-Identifier: Apache-2.0
 * Files for Python (plan 8.2): open(), a small `os`, and module import.
 *
 *   an app's files follow t5_app/fspolicy.h, as for native apps
 *   the console's VM (no app)          t5_fs_roots[] read-write, its working dir read-only
 *
 * Every path is made absolute against the working dir and normalised by
 * t5_fs_normalise(), which refuses "..": there is no way to walk out of a
 * root. There are no symlinks on LittleFS or FAT.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "py/builtin.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "t5_app/app.h"
#include "t5_app/apps.h"
#include "t5_app/fspolicy.h"
#include "py_priv.h"

static bool join(const char *in, const char *cwd, char *out, size_t n)
{
    char tmp[T5_FS_PATH_MAX * 2];
    if (!in || !in[0]) in = ".";
    if (in[0] == '/') snprintf(tmp, sizeof tmp, "%s", in);
    else snprintf(tmp, sizeof tmp, "%s/%s", cwd, in);
    return t5_fs_normalise(tmp, out, n);
}

static bool allowed(const char *abs, bool write)
{
    t5_py_t *vm = py_vm;
    if (!vm) return false;
    if (vm->app) return t5_app_fs_check(vm->app, abs, write);
    for (size_t i = 0; i < t5_fs_nroots; i++) {
        if (t5_fs_under(abs, t5_fs_roots[i].path)) return true;
    }
    return !write && t5_fs_under(abs, vm->cwd);
}

void py_resolve_path(const char *in, bool write, char *out, size_t n)
{
    if (!join(in, py_vm ? py_vm->cwd : "/", out, n)) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("bad path: %s"), in);
    }
    if (!allowed(out, write)) {
        char msg[T5_FS_PATH_MAX + 32];
        snprintf(msg, sizeof msg, "%s is outside this app's files", out);
        py_raise_perm(msg);
    }
}

typedef struct {
    mp_obj_base_t base;
    int fd;
    bool quota;         /* writes count against the app's data folder */
} py_file_t;

static void check_open(const py_file_t *f)
{
    if (f->fd < 0) mp_raise_ValueError(MP_ERROR_TEXT("I/O operation on closed file"));
}

static void file_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    py_file_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<io.%s %d>", mp_obj_get_type_str(self_in), self->fd);
}

static mp_uint_t file_read(mp_obj_t o_in, void *buf, mp_uint_t size, int *errcode)
{
    py_file_t *o = MP_OBJ_TO_PTR(o_in);
    check_open(o);
    ssize_t r = read(o->fd, buf, size);
    if (r < 0) {
        *errcode = errno;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)r;
}

static mp_uint_t file_write(mp_obj_t o_in, const void *buf, mp_uint_t size, int *errcode)
{
    py_file_t *o = MP_OBJ_TO_PTR(o_in);
    check_open(o);
    if (o->quota && py_vm && py_vm->app && !t5_app_fs_room(py_vm->app, size)) {
        *errcode = MP_ENOSPC;
        return MP_STREAM_ERROR;
    }
    ssize_t r = write(o->fd, buf, size);
    if (r < 0) {
        *errcode = errno;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)r;
}

static mp_uint_t file_ioctl(mp_obj_t o_in, mp_uint_t request, uintptr_t arg, int *errcode)
{
    py_file_t *o = MP_OBJ_TO_PTR(o_in);
    switch (request) {
    case MP_STREAM_FLUSH:
        check_open(o);
        if (fsync(o->fd) != 0 && errno != EINVAL) {
            *errcode = errno;
            return MP_STREAM_ERROR;
        }
        return 0;
    case MP_STREAM_SEEK: {
        check_open(o);
        struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)arg;
        off_t off = lseek(o->fd, s->offset, s->whence);
        if (off == (off_t)-1) {
            *errcode = errno;
            return MP_STREAM_ERROR;
        }
        s->offset = off;
        return 0;
    }
    case MP_STREAM_CLOSE:
        if (o->fd >= 0) close(o->fd);
        o->fd = -1;
        return 0;
    default:
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
}

static const mp_rom_map_elem_t file_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(file_locals, file_locals_table);

static const mp_stream_p_t fileio_stream = { .read = file_read, .write = file_write, .ioctl = file_ioctl };
static const mp_stream_p_t textio_stream = {
    .read = file_read, .write = file_write, .ioctl = file_ioctl, .is_text = true,
};

MP_DEFINE_CONST_OBJ_TYPE(py_type_fileio, MP_QSTR_FileIO, MP_TYPE_FLAG_ITER_IS_STREAM, print, file_print, protocol,
                         &fileio_stream, locals_dict, &file_locals);
MP_DEFINE_CONST_OBJ_TYPE(py_type_textio, MP_QSTR_TextIOWrapper, MP_TYPE_FLAG_ITER_IS_STREAM, print, file_print,
                         protocol, &textio_stream, locals_dict, &file_locals);

static mp_obj_t py_open(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { ARG_file, ARG_mode, ARG_encoding };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_file, MP_ARG_OBJ | MP_ARG_REQUIRED, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_mode, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_r) } },
        { MP_QSTR_encoding, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    const char *mode = mp_obj_str_get_str(a[ARG_mode].u_obj);
    int rw = O_RDONLY, x = 0;
    bool writes = false;
    const mp_obj_type_t *type = &py_type_textio;
    for (const char *m = mode; *m; m++) {
        switch (*m) {
        case 'r': rw = O_RDONLY; break;
        case 'w': rw = O_WRONLY; x = O_CREAT | O_TRUNC; writes = true; break;
        case 'a': rw = O_WRONLY; x = O_CREAT | O_APPEND; writes = true; break;
        case 'x': rw = O_WRONLY; x = O_CREAT | O_EXCL; writes = true; break;
        case '+': rw = O_RDWR; writes = true; break;
        case 'b': type = &py_type_fileio; break;
        case 't': type = &py_type_textio; break;
        default: mp_raise_ValueError(MP_ERROR_TEXT("bad mode"));
        }
    }
    char path[T5_FS_PATH_MAX];
    py_resolve_path(mp_obj_str_get_str(a[ARG_file].u_obj), writes, path, sizeof path);
    t5_app_t *app = py_vm ? py_vm->app : NULL;
    bool quota = writes && app && t5_app_fs_quota_applies(app, path);
    if (quota && !t5_app_fs_room(app, 0)) mp_raise_OSError(MP_ENOSPC);
    py_file_t *o = mp_obj_malloc_with_finaliser(py_file_t, type);
    o->fd = -1;
    int fd = open(path, rw | x, 0644);
    if (fd < 0) mp_raise_OSError(errno);
    o->fd = fd;
    o->quota = quota;
    return MP_OBJ_FROM_PTR(o);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, py_open);

mp_import_stat_t mp_import_stat(const char *path)
{
    char abs[T5_FS_PATH_MAX];
    if (!py_vm || !join(path, py_vm->cwd, abs, sizeof abs) || !allowed(abs, false)) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    struct stat st;
    if (stat(abs, &st) != 0) return MP_IMPORT_STAT_NO_EXIST;
    return S_ISDIR(st.st_mode) ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
}

#define PY_SOURCE_MAX (256 * 1024)

mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
    char abs[T5_FS_PATH_MAX];
    py_resolve_path(qstr_str(filename), false, abs, sizeof abs);
    int fd = open(abs, O_RDONLY);
    if (fd < 0) mp_raise_OSError(errno);
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size > PY_SOURCE_MAX) {
        close(fd);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("source file too big"));
    }
    size_t len = (size_t)st.st_size;
    char *buf = m_new(char, len ? len : 1);
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return mp_lexer_new_from_str_len(filename, buf, got, len ? len : 1);
}

static mp_obj_t os_listdir(size_t n_args, const mp_obj_t *args)
{
    char path[T5_FS_PATH_MAX];
    py_resolve_path(n_args ? mp_obj_str_get_str(args[0]) : ".", false, path, sizeof path);
    DIR *d = opendir(path);
    if (!d) mp_raise_OSError(errno);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        mp_obj_list_append(list, mp_obj_new_str_from_cstr(e->d_name));
    }
    closedir(d);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(os_listdir_obj, 0, 1, os_listdir);

static mp_obj_t os_stat(mp_obj_t p)
{
    char path[T5_FS_PATH_MAX];
    py_resolve_path(mp_obj_str_get_str(p), false, path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) mp_raise_OSError(errno);
    mp_obj_t t[10] = {
        mp_obj_new_int_from_uint(st.st_mode), MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0),
        MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0), mp_obj_new_int_from_uint((mp_uint_t)st.st_size),
        mp_obj_new_int((mp_int_t)st.st_atime), mp_obj_new_int((mp_int_t)st.st_mtime), mp_obj_new_int((mp_int_t)st.st_ctime),
    };
    return mp_obj_new_tuple(10, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_stat_obj, os_stat);

#define OS_PATH_FN(name, call)                                          \
    static mp_obj_t os_##name(mp_obj_t p)                               \
    {                                                                   \
        char path[T5_FS_PATH_MAX];                                         \
        py_resolve_path(mp_obj_str_get_str(p), true, path, sizeof path); \
        if (call != 0) mp_raise_OSError(errno);                         \
        return mp_const_none;                                           \
    }                                                                   \
    static MP_DEFINE_CONST_FUN_OBJ_1(os_##name##_obj, os_##name)

OS_PATH_FN(remove, unlink(path));
OS_PATH_FN(rmdir, rmdir(path));
OS_PATH_FN(mkdir, mkdir(path, 0755));

static mp_obj_t os_rename(mp_obj_t a, mp_obj_t b)
{
    char pa[T5_FS_PATH_MAX], pb[T5_FS_PATH_MAX];
    py_resolve_path(mp_obj_str_get_str(a), true, pa, sizeof pa);
    py_resolve_path(mp_obj_str_get_str(b), true, pb, sizeof pb);
    if (rename(pa, pb) != 0) mp_raise_OSError(errno);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(os_rename_obj, os_rename);

static mp_obj_t os_getcwd(void)
{
    return mp_obj_new_str_from_cstr(py_vm ? py_vm->cwd : "/");
}
static MP_DEFINE_CONST_FUN_OBJ_0(os_getcwd_obj, os_getcwd);

static mp_obj_t os_uname(void)
{
    return mp_obj_new_str_from_cstr("Tab5 OS " MICROPY_HW_MCU_NAME);
}
static MP_DEFINE_CONST_FUN_OBJ_0(os_uname_obj, os_uname);

static const mp_rom_map_elem_t os_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_os) },
    { MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&os_listdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&os_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&os_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&os_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&os_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&os_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_uname), MP_ROM_PTR(&os_uname_obj) },
    { MP_ROM_QSTR(MP_QSTR_sep), MP_ROM_QSTR(MP_QSTR__slash_) },
};
static MP_DEFINE_CONST_DICT(os_globals, os_globals_table);

const mp_obj_module_t py_module_os = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&os_globals,
};
MP_REGISTER_MODULE(MP_QSTR_os, py_module_os);
