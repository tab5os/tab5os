/* SPDX-License-Identifier: Apache-2.0
 * Videos: the AVI files in the SD card's Videos and DCIM folders, newest
 * first, played in the shared player (video.c). A launch argument naming
 * an .avi plays it.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "t5/storage.h"
#include "t5_gui/t5ui_icons.h"
#include "sysapps_priv.h"

#define MAX_VIDEOS 300

static const char *const s_dirs[] = { T5_SD_PATH "/Videos", T5_DCIM_DIR };

typedef struct {
    char path[96];
    uint32_t size;
    time_t mtime;
} entry_t;

typedef struct {
    entry_t e[MAX_VIDEOS];
    int n;
    t5ui_obj_t *list;
} videos_t;

static t5_app_t *s_app;
static sa_nav_t s_nav;
static videos_t *s_list;

static int newest_first(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return -strcmp(x->path, y->path);
}

static void scan(videos_t *g)
{
    g->n = 0;
    for (size_t d = 0; d < sizeof s_dirs / sizeof s_dirs[0]; d++) {
        DIR *dir = opendir(s_dirs[d]);
        struct dirent *de;
        while (dir && g->n < MAX_VIDEOS && (de = readdir(dir))) {
            if (de->d_name[0] == '.' || !sa_video_can_open(de->d_name)) continue;
            entry_t *e = &g->e[g->n];
            int w = snprintf(e->path, sizeof e->path, "%s/%s", s_dirs[d], de->d_name);
            if (w < 0 || w >= (int)sizeof e->path) continue;
            struct stat st;
            if (stat(e->path, &st) != 0) continue;
            e->size = (uint32_t)st.st_size;
            e->mtime = st.st_mtime;
            g->n++;
        }
        if (dir) closedir(dir);
    }
    qsort(g->e, (size_t)g->n, sizeof g->e[0], newest_first);
}

static void bind(t5ui_event_t *ev)
{
    videos_t *g = t5ui_event_user(ev);
    t5ui_obj_t *r = t5ui_event_target(ev);
    int i = (int)t5ui_event_index(ev);
    if (i < 0 || i >= g->n) return;
    const char *name = strrchr(g->e[i].path, '/') + 1;
    t5ui_set_str(r, T5UI_P_TEXT, name);
    t5ui_set_str(r, T5UI_P_ICON, T5UI_ICON_VIDEOCAM);
    /* Two-line rows: a one-line row has no second line, and this does nothing. */
    char b[24];
    sa_fmt_bytes(b, sizeof b, g->e[i].size);
    bool camera = strncmp(g->e[i].path, T5_DCIM_DIR "/", sizeof T5_DCIM_DIR) == 0;
    t5ui_set_strf(r, T5UI_P_SECONDARY, "%s, %s", camera ? "Camera" : "Videos", b);
}

static void click(t5ui_event_t *ev)
{
    videos_t *g = t5ui_event_user(ev);
    int i = (int)t5ui_event_index(ev);
    if (i >= 0 && i < g->n) sa_video_open(t5ui_root(s_app), g->e[i].path, NULL);
}

static void build_list(void)
{
    t5ui_obj_t *body = sa_body(&s_nav);
    t5ui_clean(body);
    t5ui_set_int(body, T5UI_P_SCROLLABLE, false);
    scan(s_list);
    s_list->list = t5ui_create(body, T5UI_MD_LIST);
    t5ui_on(s_list->list, T5UI_EV_BIND, bind, s_list);
    t5ui_on(s_list->list, T5UI_EV_ITEM, click, s_list);
    t5ui_set_int(s_list->list, T5UI_P_ROW_DP, 64);           /* two-line rows */
    t5ui_set_int(s_list->list, T5UI_P_COUNT, s_list->n);
    t5ui_set_int(s_list->list, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(s_list->list, T5UI_P_GROW, 1);
    if (!s_list->n) {
        t5ui_set_int(s_list->list, T5UI_P_HIDDEN, true);
        sa_note(body, t5_storage_sd_state() == T5_SD_MOUNTED
                          ? "No videos yet. Videos you record with the Camera show up here, and so do AVI files "
                            "(Motion JPEG) in the Videos folder on the SD card."
                          : "Insert an SD card. Videos are played from it.");
    }
}

static void on_create(t5_app_t *app)
{
    s_app = app;
    s_list = calloc(1, sizeof *s_list);
    sa_nav_init(&s_nav, app);
    sa_push(&s_nav, "Videos");
    if (s_list) build_list();
    const char *arg = t5_app_launch_arg(app);
    if (arg[0] == '/' && sa_video_can_open(arg)) sa_video_open(t5ui_root(app), arg, NULL);
}

/* Back from another app (or the card changed): look again. */
static void on_resume(t5_app_t *app)
{
    (void)app;
    if (s_list && !sa_video_is_open()) build_list();
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    sa_video_close();
    sa_nav_destroy(&s_nav);
    free(s_list);
    s_list = NULL;
}

static const t5_app_desc_t s_desc = {
    .api = T5_API_LEVEL, .on_create = on_create, .on_resume = on_resume, .on_destroy = on_destroy,
};

const t5_builtin_app_t sysapp_videos = {
    .id = T5_APP_ID_VIDEOS,
    .name = "Videos",
    .desc = &s_desc,
    .perms = T5_PERM_STORAGE,
    .orientation = T5_ORIENT_ANY,
    .icon = T5UI_ICON_VIDEO_LIBRARY,
};
