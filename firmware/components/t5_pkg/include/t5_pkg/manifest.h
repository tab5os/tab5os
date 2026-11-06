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

#define T5_API_LEVEL 1

typedef enum { T5_RUNTIME_NATIVE, T5_RUNTIME_MICROPYTHON } t5_runtime_t;
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
};
#define T5_PERM_ALL 0x1ffu

#define T5_MANIFEST_MAX_PORTS 8
#define T5_MANIFEST_MAX_HEAP_KB (32u * 1024u)
#define T5_MANIFEST_BG_DEFAULT_SECONDS 30
#define T5_MANIFEST_BG_MAX_SECONDS 300

typedef struct {
    char id[64];              /* reverse-DNS: [a-z0-9_]+(\.[a-z0-9_]+)+, max 63 */
    char name[48];
    char version[16];         /* MAJOR.MINOR.PATCH, optional -prerelease */
    int api;                  /* 1..T5_API_LEVEL */
    t5_runtime_t runtime;     /* "native" | "micropython" */
    char entry[64];           /* file inside the package, e.g. "app.elf" or "main.py" */
    char icon[64];            /* optional, e.g. "icon.png"; "" if absent */
    uint32_t min_heap_kb;     /* min_heap_mb * 1024 or min_heap_kb; 0 if absent */
    uint32_t perms;           /* T5_PERM_* bits requested */
    char ports[T5_MANIFEST_MAX_PORTS][16];  /* [permissions] ports = ["grove"] */
    int nports;
    t5_orient_pref_t orientation;  /* default landscape */
    bool background;          /* [background] table present (needs background_wake = true) */
    char bg_entry[32];        /* [background] entry, default "on_alarm" */
    uint32_t bg_max_seconds;  /* [background] max_seconds, default 30, max 300 */
} t5_manifest_t;

/* Parse and validate. On failure err holds a human-readable reason
 * ("line 3: ..." for syntax errors, "id: ..." for validation). */
t5_err_t t5_manifest_parse(const char *text, size_t len, t5_manifest_t *out, char *err, size_t errlen);

const char *t5_perm_name(uint32_t perm_bit);       /* "camera", ...; NULL if not a single known bit */
uint32_t t5_perm_from_name(const char *name);      /* 0 if unknown */

/* Valid reverse-DNS app id? Also used to reject path tricks in API calls. */
bool t5_manifest_id_ok(const char *id);
bool t5_version_ok(const char *v);
/* Semver precedence: <0, 0, >0. Invalid versions sort before valid ones. */
int t5_version_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
