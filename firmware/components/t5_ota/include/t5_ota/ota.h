/* SPDX-License-Identifier: Apache-2.0
 * OS updates, boot health and safe mode (plan 9.1, 9.3).
 *
 * An update is a .t5os file: a 160-byte header (t5_ota_hdr_t) followed by
 * the OS image (tab5os.bin). `t5 ota-pack` makes one from a firmware build
 * and signs it. Installing streams the image into the update slot while
 * hashing it, then checks, before anything changes what boots:
 *   - the image's SHA-256 against the header,
 *   - the ECDSA P-256 signature against the keys built into this OS
 *     (the .pem files in firmware/ota_keys); an image no built-in key
 *     signed only as t5_ota_source_t allows,
 *   - that it is a Tab5 OS image for this chip's revision (the image
 *     header's min/max chip revision),
 * and only then makes the new slot the boot slot.
 *
 * The update slot: running from an A/B slot, the other one. Running from
 * the recovery image, the slot the device is failing to boot (the current
 * boot slot), so the other one, which may hold the last OS that worked,
 * stays as it is.
 *
 * The new image boots "on trial" (ESP-IDF app rollback). t5_ota_boot_ok()
 * marks it valid once the services are up, the shell has drawn, and it has
 * run 60 s; a crash or reset before that boots the previous slot again.
 *
 * Boot health (bootctl.h): every boot clears a bit of the boot counter; a
 * healthy boot erases it. Two unconfirmed boots in a row start the OS in
 * safe mode, three start the recovery image.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_bootfmt/bootctl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_OTA_MAGIC "T5OS"
#define T5_OTA_HDR_SIZE 160
#define T5_OTA_HDR_VERSION 1
#define T5_OTA_SIG_LEN 64
/* The image's project name (esp_app_desc_t): anything else isn't Tab5 OS. */
#define T5_OTA_PROJECT "tab5os"

typedef struct __attribute__((packed)) {
    char magic[4];              /* "T5OS" */
    uint16_t version;           /* T5_OTA_HDR_VERSION */
    uint16_t sig_len;           /* 64 (raw r||s), or 0 when unsigned */
    uint32_t image_len;
    uint8_t key_id[32];         /* SHA-256 of the signer's public key (DER), zeros if unsigned */
    uint8_t image_sha256[32];
    uint8_t sig[64];            /* ECDSA P-256 over image_sha256 */
    char os_version[16];        /* informational: the image's version string */
    uint8_t reserved[4];
} t5_ota_hdr_t;
_Static_assert(sizeof(t5_ota_hdr_t) == T5_OTA_HDR_SIZE, "t5_ota_hdr_t");

typedef enum {
    T5_OTA_IDLE,
    T5_OTA_RECEIVING,
    T5_OTA_VERIFYING,
    T5_OTA_READY,         /* installed; restart to run it */
    T5_OTA_FAILED,
} t5_ota_state_t;

typedef struct {
    t5_ota_state_t state;
    uint32_t done, total;       /* bytes */
    char version[16];           /* the incoming image's */
    char error[96];
    bool signed_ok;             /* signed by a built-in key */
    char signer[17];            /* the key's id, first 16 hex digits */
} t5_ota_status_t;

/* "idle", "receiving", "verifying", "ready (restart)", "failed". */
const char *t5_ota_state_name(t5_ota_state_t s);

/* Who asks for an install. It decides whether an image that no built-in key
 * signed (unsigned, or signed by another key) may go in; consent is the
 * source's own say-so where it has one. */
typedef enum {
    T5_OTA_FROM_SETTINGS,     /* Settings > Updates: consent = developer mode is on */
    T5_OTA_FROM_CONSOLE,      /* the `ota` command: consent = it said `unsigned` */
    T5_OTA_FROM_DEV,          /* the developer server, which runs only in developer mode: always */
    T5_OTA_FROM_RECOVERY,     /* the recovery image, which asks the person first: unsigned
                               * images, never one signed by an unknown key */
} t5_ota_source_t;

/* A .t5os arriving in pieces: up to n bytes into buf, returning the count,
 * 0 at the end, or a negative t5_err_t. */
typedef int (*t5_ota_read_fn)(void *ctx, void *buf, size_t n);
/* The whole install from a source: header, image, verify, switch the boot
 * slot. hdr (may be NULL) gets the header once it has arrived. */
T5_MUST_CHECK t5_err_t t5_ota_install_stream(t5_ota_read_fn rd, void *ctx, t5_ota_source_t from, bool consent,
                                             t5_ota_hdr_t *hdr, char *err, size_t errlen);

/* Install a .t5os from a file (/sd/...). URLs: t5_ota_net/ota_url.h. */
T5_MUST_CHECK t5_err_t t5_ota_install_file(const char *path, t5_ota_source_t from, bool consent, char *err,
                                           size_t errlen);
/* Read a .t5os file's header and apply the source's rule, installing
 * nothing: T5_OK when the header is one the install would take. trusted:
 * a built-in key signed it (the signature itself is checked on install). */
T5_MUST_CHECK t5_err_t t5_ota_peek(const char *path, t5_ota_source_t from, bool consent, t5_ota_hdr_t *hdr,
                                   bool *trusted, char *err, size_t errlen);
void t5_ota_status(t5_ota_status_t *out);
/* .t5os files in the SD card's top folder and /update, newest first (by
 * modification time). Returns the count. */
int t5_ota_find_on_sd(char (*paths)[128], int max);

/* ---- the C6 radio's firmware -------------------------------------------------- */
/* A .t5c6 is the same 160-byte header with magic "T5C6" (os_version holds
 * the C6 image's version, such as "t5-c6-0.2.0"), then an ESP32-C6 app
 * image. A release publishes one beside its .t5os, signed with the same
 * key; it isn't inside the .t5os, because the installers in 0.2.0 and
 * earlier stop reading at the end of the OS image. The C6 firmware has its
 * own version (c6/VERSION, raised only when the image changes), and each OS
 * expects the one it was built with: T5_OTA_C6_PREFIX and that version.
 * t5_ota_net's c6 sync fetches it and has t5_net move the C6 onto it. */
#define T5_OTA_C6_MAGIC "T5C6"
#define T5_OTA_C6_PREFIX "t5-c6-"
#define T5_OTA_C6_PART "c6fw"

/* Read a whole .t5c6 into PSRAM, check it (header, an ESP32-C6 image of
 * version want_version, or any when NULL; its hash; the signature, by the
 * source's rule as for the OS), and only then write the image to the c6fw
 * partition. The C6 itself isn't touched: t5_net_c6_update() does that.
 * Writes nothing when c6fw already holds this image. */
T5_MUST_CHECK t5_err_t t5_ota_c6_install_stream(t5_ota_read_fn rd, void *ctx, t5_ota_source_t from, bool consent,
                                                const char *want_version, char *err, size_t errlen);
T5_MUST_CHECK t5_err_t t5_ota_c6_install_file(const char *path, t5_ota_source_t from, bool consent,
                                              const char *want_version, char *err, size_t errlen);

/* ---- boot health -------------------------------------------------------------- */
/* Early in app_main (the OS's or recovery's): read the boot counter and the
 * request word, decide on safe mode, note whether this image is on trial. */
void t5_ota_boot_check(void);
/* The boot reached a usable state (services up, shell drawn): clear the
 * counter, and start the 60 s trial timer for a new image. */
void t5_ota_boot_ok(void);
/* This image is on trial (installed by an update, not yet confirmed). */
bool t5_ota_on_trial(void);
/* Unconfirmed boots before this one (from the boot counter). */
int t5_ota_failed_boots(void);
/* The request this boot found (t5_ota_boot_check). */
t5_boot_req_t t5_ota_boot_request(void);
/* Ask the next boot to start the recovery image, or the OS in safe mode;
 * then restart. */
T5_MUST_CHECK t5_err_t t5_ota_reboot_to_recovery(void);
T5_MUST_CHECK t5_err_t t5_ota_reboot_safe_mode(void);

/* Safe mode (plan 9.3): this boot runs without user apps or auto-launch. */
bool t5_safe_mode(void);

/* Test the update path without moving a file: stream the running image
 * through the install core into the update slot. sig/key_id (may be
 * NULL) are what `t5ota.py pack --sign` would put in the header for this
 * exact image, so the signed path can be tested too. */
T5_MUST_CHECK t5_err_t t5_ota_selftest(const uint8_t *sig64, const uint8_t *key_id32, char *err, size_t errlen);

/* How the restarts above restart: the OS's own restart, which saves its
 * state first (t5_power_restart). Without one, a plain restart. */
void t5_ota_set_restart_hook(void (*fn)(void));
/* A recovery flag file on the SD card (RECOVERY, recovery.txt or
 * tab5/recovery in its top folder): renamed to *.done, then restart into
 * recovery. Call once storage is up. */
T5_MUST_CHECK t5_err_t t5_ota_check_sd_flag(void);
/* Erase the boot counter and any request (recovery does this at start). */
void t5_ota_bootctl_clear(void);

#ifdef __cplusplus
}
#endif
