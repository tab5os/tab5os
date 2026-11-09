/* SPDX-License-Identifier: Apache-2.0
 * App manifest (manifest.toml at the root of a .t5app): parsing and
 * validation. Unknown keys are errors, so a typo in [permissions] is caught
 * at pack/install time rather than silently ignored.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* API level this firmware implements. An app built against level N runs on
 * any OS whose level is >= N (sdk/abi/api_v<N>.sym never loses or changes a
 * symbol). */
#define T5_API_LEVEL 1

typedef enum { T5_RUNTIME_NATIVE, T5_RUNTIME_MICROPYTHON, T5_RUNTIME_COUNT } t5_runtime_t;
typedef enum { T5_ORIENT_ANY, T5_ORIENT_LANDSCAPE, T5_ORIENT_PORTRAIT } t5_orient_pref_t;

enum { /* permission groups, bit flags */
    T5_PERM_CAMERA = 1u << 0,
    T5_PERM_MIC = 1u << 1,
    T5_PERM_NET = 1u << 2,
    T5_PERM_BLE = 1u << 3,
    T5_PERM_PORTS = 1u << 4,
    T5_PERM_USB = 1u << 5,
    T5_PERM_STORAGE = 1u << 6,
    T5_PERM_BACKGROUND_WAKE = 1u << 7,
    T5_PERM_SYSTEM = 1u << 8,
    T5_PERM_SHARED_READ = 1u << 9,      /* the shared folders, asked at first use */
    T5_PERM_SHARED_WRITE = 1u << 10,    /* ...and changing them */
    T5_PERM_ALL_FILES = 1u << 11,       /* all of /sd and /usb, never OS or other apps' files */
    T5_PERM_AMBIENT = 1u << 12,         /* a card on the standby screen (t5_ambient_*) */
};
#define T5_PERM_ALL 0x1fffu

/* One row per permission group: its [permissions] key and whether the person
 * is asked on first use (the rest are granted by installing the app). */
typedef struct {
    uint32_t bit;
    const char *name;
    bool prompted;
} t5_perm_info_t;
extern const t5_perm_info_t t5_perms[];
extern const size_t t5_perm_count;
/* The groups that are asked for on first use. */
uint32_t t5_perm_prompted(void);

#define T5_APP_ID_LEN 64          /* reverse-DNS id, with its NUL */
#define T5_APP_NAME_LEN 48
#define T5_APP_VERSION_LEN 16
#define T5_APP_PATH_MAX 192       /* an app's install or data dir, or a file in it */

#define T5_MANIFEST_MAX_PORTS 8
#define T5_MANIFEST_FILE_LEN 64       /* entry and icon: a path inside the package */
#define T5_MANIFEST_PORT_LEN 16
#define T5_MANIFEST_FUNC_LEN 32       /* [background] entry */
#define T5_MANIFEST_MAX_HEAP_KB (32u * 1024u)
#define T5_MANIFEST_BG_DEFAULT_ENTRY "on_alarm"
#define T5_MANIFEST_BG_DEFAULT_SECONDS 30
#define T5_MANIFEST_BG_MAX_SECONDS 300

typedef struct {
    char id[T5_APP_ID_LEN];   /* reverse-DNS: [a-z0-9_]+(\.[a-z0-9_]+)+ */
    char name[T5_APP_NAME_LEN];
    char version[T5_APP_VERSION_LEN];   /* MAJOR.MINOR.PATCH, optional -prerelease */
    int api;                  /* 1..T5_API_LEVEL */
    t5_runtime_t runtime;     /* "native" | "micropython" */
    char entry[T5_MANIFEST_FILE_LEN];   /* file inside the package, e.g. "app.elf" or "main.py" */
    char icon[T5_MANIFEST_FILE_LEN];    /* optional, e.g. "icon.png"; "" if absent */
    uint32_t min_heap_kb;     /* min_heap_mb * 1024 or min_heap_kb; 0 if absent */
    uint32_t perms;           /* T5_PERM_* bits requested */
    char ports[T5_MANIFEST_MAX_PORTS][T5_MANIFEST_PORT_LEN];  /* [permissions] ports = ["grove"] */
    int nports;
    t5_orient_pref_t orientation;  /* default landscape */
    bool background;          /* [background] table present (needs background_wake = true) */
    char bg_entry[T5_MANIFEST_FUNC_LEN];   /* [background] entry */
    uint32_t bg_max_seconds;  /* [background] max_seconds */
    /* [shell]: the bars the shell draws around the app's screen. */
    bool fullscreen;          /* fullscreen = true: start without the bars (t5_shell_set_fullscreen) */
} t5_manifest_t;

/* Parse and validate. On failure err holds a human-readable reason
 * ("line 3: ..." for syntax errors, "id: ..." for validation). */
T5_MUST_CHECK t5_err_t t5_manifest_parse(const char *text, size_t len, t5_manifest_t *out, char *err, size_t errlen);

const char *t5_perm_name(uint32_t perm_bit);       /* "camera", ...; NULL if not a single known bit */
uint32_t t5_perm_from_name(const char *name);      /* 0 if unknown */
/* "native", "micropython"; "?" for anything else. */
const char *t5_runtime_name(t5_runtime_t rt);

/* Valid reverse-DNS app id? Also used to reject path tricks in API calls. */
bool t5_manifest_id_ok(const char *id);
bool t5_version_ok(const char *v);
/* Semver precedence: <0, 0, >0. Invalid versions sort before valid ones. */
int t5_version_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
