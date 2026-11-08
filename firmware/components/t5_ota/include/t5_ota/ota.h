/* SPDX-License-Identifier: Apache-2.0
 * OS updates, boot health and safe mode (plan 9.1, 9.3).
 *
 * An update is a .t5os file: a 160-byte header (t5_ota_hdr_t) followed by
 * the OS image (tab5os.bin). `t5 ota-pack` makes one from a firmware build
 * and signs it. Installing streams the image into the inactive A/B slot
 * while hashing it, then checks, before anything changes what boots:
 *   - the image's SHA-256 against the header,
 *   - the ECDSA P-256 signature against the keys built into this OS
 *     (the .pem files in firmware/ota_keys); unsigned or unknown-key images only with
 *     allow_unsigned (developer mode),
 *   - that it is a Tab5 OS image for this chip's revision (the image
 *     header's min/max chip revision, which is how rev1 and rev3 builds
 *     differ),
 * and only then makes the new slot the boot slot.
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

#ifdef __cplusplus
extern "C" {
#endif

#define T5_OTA_MAGIC "T5OS"
#define T5_OTA_HDR_SIZE 160
#define T5_OTA_HDR_VERSION 1

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

/* Streaming install, for the HTTP endpoint and anything else that gets an
 * update in pieces. hdr is the first 160 bytes. */
typedef struct t5_ota t5_ota_t;
T5_MUST_CHECK t5_err_t t5_ota_begin(const t5_ota_hdr_t *hdr, bool allow_unsigned, t5_ota_t **out, char *err, size_t errlen);
T5_MUST_CHECK t5_err_t t5_ota_write(t5_ota_t *o, const void *data, size_t len);
/* Verify and switch the boot slot. Frees o either way. */
T5_MUST_CHECK t5_err_t t5_ota_finish(t5_ota_t *o, char *err, size_t errlen);
void t5_ota_abort(t5_ota_t *o);

/* Install a .t5os from a file (/sd/...) or a URL (http:// or https://). */
T5_MUST_CHECK t5_err_t t5_ota_install_file(const char *path, bool allow_unsigned, char *err, size_t errlen);
T5_MUST_CHECK t5_err_t t5_ota_install_url(const char *url, bool allow_unsigned, char *err, size_t errlen);
void t5_ota_status(t5_ota_status_t *out);
/* .t5os files in /sd and /sd/update, newest first. Returns the count. */
int t5_ota_find_on_sd(char (*paths)[128], int max);

/* ---- boot health -------------------------------------------------------------- */
/* Early in app_main: read the boot counter and the request word, decide on
 * safe mode, note whether this image is on trial. */
void t5_ota_boot_check(void);
/* The boot reached a usable state (services up, shell drawn): clear the
 * counter, and start the 60 s trial timer for a new image. */
void t5_ota_boot_ok(void);
/* This image is on trial (installed by an update, not yet confirmed). */
bool t5_ota_on_trial(void);
/* Unconfirmed boots before this one (from the boot counter). */
int t5_ota_failed_boots(void);
/* Ask the next boot to start the recovery image, or the OS in safe mode;
 * then restart. */
T5_MUST_CHECK t5_err_t t5_ota_reboot_to_recovery(void);
T5_MUST_CHECK t5_err_t t5_ota_reboot_safe_mode(void);

/* Safe mode (plan 9.3): this boot runs without user apps or auto-launch. */
bool t5_safe_mode(void);

/* Test the update path without moving a file: stream the running image
 * through t5_ota_begin/write/finish into the other slot. sig/key_id (may be
 * NULL) are what `t5ota.py pack --sign` would put in the header for this
 * exact image, so the signed path can be tested too. */
T5_MUST_CHECK t5_err_t t5_ota_selftest(const uint8_t *sig64, const uint8_t *key_id32, char *err, size_t errlen);

/* Called before the restarts above (the OS saves its state). */
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
