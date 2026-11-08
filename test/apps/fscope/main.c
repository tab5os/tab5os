/* File isolation test (t5_app/fspolicy.h). Prints PASS/FAIL lines.
 *
 *   (none)  the fixed rules: own folders, other apps, OS files, path tricks, quota
 *   pick    opens the file picker, then checks what the chosen file allows
 *   pickw   the same with T5_FILE_PICK_WRITE
 *   shared  asks for shared_read (fsshared only), then reads and tries to write
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <t5/app.h>
#include <t5/sys.h>
#include <t5/ui.h>

static const char *s_mode = "";
static t5_app_t *s_app;
static char s_file[160];

static void result(const char *what, bool ok)
{
    printf("fscope: %s %s\n", ok ? "PASS" : "FAIL", what);
}

static bool can_open(const char *p, const char *mode)
{
    FILE *f = fopen(p, mode);
    if (f) fclose(f);
    return f != NULL;
}

static void fixed_rules(void)
{
    char own[128], own2[128];
    snprintf(own, sizeof own, "%s/t.txt", t5_app_data_dir(s_app));
    result("write own data", can_open(own, "w"));
    result("read own data", can_open(own, "r"));
    snprintf(own2, sizeof own2, "%s/manifest.toml", t5_app_install_dir(s_app));
    result("read install dir", can_open(own2, "r"));
    result("write install dir refused", !can_open(own2, "a"));
    result("other app refused", !can_open("/data/appdata/com.t5.test.crasher/x", "w"));
    result("OS file read refused", !can_open("/data/sys/crashes.bin", "r"));
    result("delete OS file refused", remove("/data/sys/crashes.bin") != 0);
    result("shared folder refused without permission", !can_open("/sd/Documents/fs.txt", "r"));
    result("SD root refused", !can_open("/sd/fs.txt", "w"));
    result(".. refused", !can_open("/data/appdata/com.t5.test.fscope/../com.t5.test.crasher/x", "w"));
    result("case trick refused", !can_open("/SD/APPS/com.t5.test.crasher/app.elf", "r"));
    result("trailing dot refused", !can_open("/sd/apps./com.t5.test.crasher/app.elf", "r"));
    struct stat st;
    result("stat of OS file refused", stat("/data/sys/crashes.bin", &st) != 0);
    result("listing SD root refused", opendir("/sd") == NULL);
    DIR *d = opendir(t5_app_data_dir(s_app));
    result("listing own data", d != NULL);
    if (d) closedir(d);

    /* Quota: 1 MB, in 32 KB writes. */
    snprintf(own2, sizeof own2, "%s/big.bin", t5_app_data_dir(s_app));
    FILE *f = fopen(own2, "w");
    static char block[32 * 1024];
    size_t total = 0;
    if (f) {
        while (total < 3 * 1024 * 1024 && fwrite(block, 1, sizeof block, f) == sizeof block) total += sizeof block;
        fclose(f);
    }
    printf("fscope: wrote %u KB before refusal\n", (unsigned)(total / 1024));
    result("quota stops writes", total >= 512 * 1024 && total <= 1024 * 1024);
    result("data folder works again after delete", remove(own2) == 0 && can_open(own, "w"));
    printf("fscope: done\n");
}

static void picked(t5_app_t *app, const char *path, void *user)
{
    if (!path) {
        printf("fscope: picker cancelled\n");
        return;
    }
    snprintf(s_file, sizeof s_file, "%s", path);
    printf("fscope: picked %s\n", s_file);
    result("read picked file", can_open(s_file, "r"));
    bool w = can_open(s_file, "a");
    result(s_mode[4] ? "write picked file" : "write picked file refused", s_mode[4] ? w : !w);
    char sib[180];
    snprintf(sib, sizeof sib, "%s.other", s_file);
    result("sibling refused", !can_open(sib, "r"));
}

static void granted(t5_app_t *app, uint32_t perm, bool ok, void *user)
{
    printf("fscope: shared_read %s\n", ok ? "granted" : "denied");
    result("read shared after answer", can_open("/sd/Documents/fs.txt", "r") == ok);
    result("write shared refused", !can_open("/sd/Documents/fs.txt", "a"));
    result("outside shared refused", !can_open("/sd/fs.txt", "r"));
}

static void go(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (!strncmp(s_mode, "pick", 4)) {
        t5_err_t e = t5_file_pick(s_app, "Choose a test file", "txt", s_mode[4] ? T5_FILE_PICK_WRITE : 0, picked, NULL);
        printf("fscope: t5_file_pick -> %d\n", (int)e);
    } else if (!strcmp(s_mode, "shared")) {
        printf("fscope: request -> %d\n", (int)t5_app_request_permission(s_app, T5_PERM_SHARED_READ, granted, NULL));
    } else {
        fixed_rules();
    }
}

static void on_create(t5_app_t *app)
{
    s_app = app;
    s_mode = t5_app_launch_arg(app);
    lv_obj_t *l = md_label_create(t5_app_screen(app), "File scope test", MD_TYPE_HEADLINE5);
    lv_obj_center(l);
    lv_timer_create(go, 300, NULL);
}

static void on_destroy(t5_app_t *app)
{
    printf("fscope: on_destroy\n");
}

T5_APP(.on_create = on_create, .on_destroy = on_destroy);
