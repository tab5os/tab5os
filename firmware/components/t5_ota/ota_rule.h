/* SPDX-License-Identifier: Apache-2.0
 * Which images a source may install (t5_ota_source_t in ota.h), and which
 * published release is an update. Pure, for host tests.
 */
#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "t5_bootfmt/espimg.h"
#include "t5_ota/ota.h"

/* <0, 0 or >0 as version a is older than, the same as or newer than b.
 * Dotted numbers ("0.1.10" is newer than "0.1.9"); a missing part counts
 * as 0, and anything after the numbers ("-dirty") is ignored. */
static inline int t5_ota_version_cmp(const char *a, const char *b)
{
    for (int i = 0; i < 4; i++) {
        unsigned long x = 0, y = 0;
        while (*a >= '0' && *a <= '9') x = x * 10 + (unsigned long)(*a++ - '0');
        while (*b >= '0' && *b <= '9') y = y * 10 + (unsigned long)(*b++ - '0');
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* NULL when an image of version `incoming` may replace `running` from this
 * source, else why not. From Settings without developer mode only the same
 * or a newer version goes in, so an old signed release (and its old bugs)
 * can't be put back from a card or a download. The version is the one in
 * the image's app description, which the signature covers. */
static inline const char *t5_ota_downgrade_refusal(const char *incoming, const char *running, t5_ota_source_t from,
                                                   bool consent)
{
    if (from != T5_OTA_FROM_SETTINGS || consent) return NULL;
    return t5_ota_version_cmp(incoming, running) < 0 ? "older than this system: install it in developer mode" : NULL;
}

/* Where releases are published, and the update file in each one. */
#define T5_OTA_RELEASES_API "https://api.github.com/repos/tab5os/tab5os/releases?per_page=10"
#define T5_OTA_ASSET_PREFIX "https://github.com/tab5os/tab5os/releases/download/v"

/* The newest release in a GitHub releases list (JSON) that is newer than
 * `running`, by its update asset's URL: T5_OTA_ASSET_PREFIX <v>/tab5os-<v>.t5os.
 * Fills url and version and returns true, or false when there is none.
 * Text inside a release's notes can't match: its quotes are escaped. */
static inline bool t5_ota_pick_release(const char *json, const char *running, char *url, size_t url_len, char *version,
                                       size_t version_len)
{
    static const char key[] = "\"browser_download_url\"";
    const size_t plen = sizeof T5_OTA_ASSET_PREFIX - 1;
    bool found = false;
    char best[32] = "";
    for (const char *p = strstr(json, key); p; p = strstr(p + 1, key)) {
        const char *u = p + sizeof key - 1;     /* then `: "` with or without spaces */
        while (*u == ' ' || *u == '\t' || *u == '\n' || *u == '\r') u++;
        if (*u++ != ':') continue;
        while (*u == ' ' || *u == '\t' || *u == '\n' || *u == '\r') u++;
        if (*u++ != '"') continue;
        const char *end = strchr(u, '"');
        if (!end || strncmp(u, T5_OTA_ASSET_PREFIX, plen)) continue;
        const char *v = u + plen, *slash = memchr(v, '/', (size_t)(end - v));
        if (!slash || slash == v || (size_t)(slash - v) >= sizeof best) continue;
        char ver[32], want[64];
        snprintf(ver, sizeof ver, "%.*s", (int)(slash - v), v);
        snprintf(want, sizeof want, "/tab5os-%s.t5os", ver);
        if ((size_t)(end - slash) != strlen(want) || strncmp(slash, want, strlen(want))) continue;
        if (t5_ota_version_cmp(ver, running) <= 0 || (found && t5_ota_version_cmp(ver, best) <= 0)) continue;
        if ((size_t)(end - u) >= url_len) continue;
        snprintf(best, sizeof best, "%s", ver);
        snprintf(url, url_len, "%.*s", (int)(end - u), u);
        found = true;
    }
    if (found) snprintf(version, version_len, "%s", best);
    return found;
}

/* The C6 image an OS of this version expects: "t5-c6-<version>". */
static inline void t5_ota_c6_version(const char *os_version, char *out, size_t n)
{
    snprintf(out, n, "%s%s", T5_OTA_C6_PREFIX, os_version);
}

/* Where the release of this OS version publishes its .t5c6, and the file's
 * name on an SD card (top folder or /update). */
static inline void t5_ota_c6_url(const char *os_version, char *out, size_t n)
{
    snprintf(out, n, T5_OTA_ASSET_PREFIX "%s/tab5os-c6-%s.t5c6", os_version, os_version);
}

static inline void t5_ota_c6_file(const char *os_version, char *out, size_t n)
{
    snprintf(out, n, "tab5os-c6-%s.t5c6", os_version);
}

#define T5_OTA_C6_HEAD_LEN (T5_IMG_DESC_OFF + T5_IMG_DESC_VERSION_OFF + T5_IMG_DESC_VERSION_LEN)

/* NULL when head (an image's first T5_OTA_C6_HEAD_LEN bytes) starts an
 * ESP32-C6 app image of version want (NULL: any version), else why not. */
static inline const char *t5_ota_c6_head_refusal(const uint8_t *head, size_t len, const char *want)
{
    if (len < T5_OTA_C6_HEAD_LEN || head[0] != T5_IMG_MAGIC) return "not a firmware image";
    if ((head[T5_IMG_CHIP_ID_OFF] | head[T5_IMG_CHIP_ID_OFF + 1] << 8) != T5_IMG_CHIP_ESP32C6) {
        return "not an ESP32-C6 image";
    }
    const uint8_t *d = head + T5_IMG_DESC_OFF;
    uint32_t magic = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
    if (magic != T5_IMG_DESC_MAGIC) return "not an app image";
    if (want && strncmp((const char *)d + T5_IMG_DESC_VERSION_OFF, want, T5_IMG_DESC_VERSION_LEN)) {
        return "not the radio firmware this system expects";
    }
    return NULL;
}

/* NULL when an image with this header may go in from this source, else why
 * not. trusted: a built-in key signed it. */
static inline const char *t5_ota_refusal(const t5_ota_hdr_t *h, bool trusted, t5_ota_source_t from, bool consent)
{
    if (trusted) return NULL;
    switch (from) {
    case T5_OTA_FROM_SETTINGS:
    case T5_OTA_FROM_CONSOLE:
        if (consent) return NULL;
        break;
    case T5_OTA_FROM_DEV:
        return NULL;
    case T5_OTA_FROM_RECOVERY:
        if (!h->sig_len) return NULL;
        break;
    }
    if (h->sig_len) return "signed by a key this OS doesn't trust";
    return from == T5_OTA_FROM_CONSOLE ? "unsigned: add 'unsigned' to install it"
                                       : "unsigned: install it in developer mode";
}
