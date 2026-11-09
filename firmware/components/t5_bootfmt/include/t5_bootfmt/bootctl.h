/* SPDX-License-Identifier: Apache-2.0
 * Boot control record (plan 9.2, 9.3), shared by the bootloader, the OS and
 * the recovery image. Pure: callers read and write the flash.
 *
 * It lives in the last 4 KB sector of the app partition of subtype "test"
 * (`recovery`; the recovery image is smaller than its slot): the bootloader
 * finds it by subtype, so everyone does. Erased, every byte is 0xFF.
 *
 *   attempts  64 bytes: each boot the bootloader counts clears the lowest
 *             set bit (flash programs 1 -> 0 without an erase), so the
 *             number of zero bits is the number of counted boots since the
 *             OS last said it was healthy.
 *   request   8 bytes: "RECOVERY" sends the next boot to the recovery
 *             image; "SAFEMODE" asks the OS to start in safe mode.
 *
 * The bootloader counts every boot except a deep-sleep wake and a requested
 * recovery, before the app starts; so an app that was counted finds itself
 * in the count too (t5_bootctl_failed_before() takes it out). The OS erases
 * the sector once a boot is healthy (services up, shell drawn); the recovery
 * image erases it when it starts. Three failed boots in a row (a fourth
 * attempt) start recovery; two start the OS in safe mode.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define T5_BOOTCTL_SECTOR_FROM_END 0x1000u
#define T5_BOOTCTL_ATTEMPT_BYTES 64
#define T5_BOOTCTL_REQ_OFFSET T5_BOOTCTL_ATTEMPT_BYTES
#define T5_BOOTCTL_REQ_LEN 8
#define T5_BOOTCTL_REC_LEN (T5_BOOTCTL_REQ_OFFSET + T5_BOOTCTL_REQ_LEN)
#define T5_BOOTCTL_REQ_RECOVERY "RECOVERY"
#define T5_BOOTCTL_REQ_SAFEMODE "SAFEMODE"
#define T5_BOOTCTL_RECOVERY_AFTER 3    /* failed boots */
#define T5_BOOTCTL_SAFEMODE_AFTER 2

/* Flash writes are whole aligned words. */
_Static_assert(T5_BOOTCTL_ATTEMPT_BYTES % 4 == 0, "attempt words");
_Static_assert(T5_BOOTCTL_REQ_OFFSET % 4 == 0 && T5_BOOTCTL_REQ_LEN % 4 == 0, "request words");
_Static_assert(sizeof T5_BOOTCTL_REQ_RECOVERY - 1 == T5_BOOTCTL_REQ_LEN, "request length");
_Static_assert(sizeof T5_BOOTCTL_REQ_SAFEMODE - 1 == T5_BOOTCTL_REQ_LEN, "request length");

typedef enum {
    T5_BOOT_REQ_NONE,
    T5_BOOT_REQ_RECOVERY,
    T5_BOOT_REQ_SAFEMODE,
} t5_boot_req_t;

static inline t5_boot_req_t t5_bootctl_request(const uint8_t rec[T5_BOOTCTL_REC_LEN])
{
    const uint8_t *r = rec + T5_BOOTCTL_REQ_OFFSET;
    if (!memcmp(r, T5_BOOTCTL_REQ_RECOVERY, T5_BOOTCTL_REQ_LEN)) return T5_BOOT_REQ_RECOVERY;
    if (!memcmp(r, T5_BOOTCTL_REQ_SAFEMODE, T5_BOOTCTL_REQ_LEN)) return T5_BOOT_REQ_SAFEMODE;
    return T5_BOOT_REQ_NONE;
}

/* Counted boots in the record. */
static inline int t5_bootctl_count(const uint8_t rec[T5_BOOTCTL_REC_LEN])
{
    int n = 0;
    for (int i = 0; i < T5_BOOTCTL_ATTEMPT_BYTES; i++) {
        uint8_t z = (uint8_t)~rec[i];
        while (z) {
            n += z & 1;
            z >>= 1;
        }
    }
    return n;
}

/* Does the bootloader count this boot? */
static inline bool t5_bootctl_counts(const uint8_t rec[T5_BOOTCTL_REC_LEN], bool deep_sleep_wake)
{
    return !deep_sleep_wake && t5_bootctl_request(rec) != T5_BOOT_REQ_RECOVERY;
}

/* The bootloader's count for this boot: which aligned word of the record to
 * program, and its new value (one more bit cleared). False when every bit
 * is used up already. */
static inline bool t5_bootctl_mark(const uint8_t rec[T5_BOOTCTL_REC_LEN], uint32_t *word_off, uint32_t *word)
{
    for (int i = 0; i < T5_BOOTCTL_ATTEMPT_BYTES; i++) {
        if (!rec[i]) continue;
        int w = i & ~3;
        uint8_t b[4];
        memcpy(b, rec + w, 4);
        b[i - w] = rec[i] & (uint8_t)(rec[i] - 1);
        memcpy(word, b, 4);
        *word_off = (uint32_t)w;
        return true;
    }
    return false;
}

/* The bootloader's decision for one boot. */
typedef struct {
    bool recovery;               /* start the recovery image */
    bool requested;              /* ... because it was asked for */
    int failed;                  /* counted boots before this one */
    bool mark;                   /* program word at word_off to count this boot */
    uint32_t word_off, word;
} t5_bootctl_boot_t;

static inline t5_bootctl_boot_t t5_bootctl_boot(const uint8_t rec[T5_BOOTCTL_REC_LEN], bool deep_sleep_wake)
{
    t5_bootctl_boot_t b = { 0 };
    b.requested = t5_bootctl_request(rec) == T5_BOOT_REQ_RECOVERY;
    b.failed = t5_bootctl_count(rec);
    b.recovery = b.requested || b.failed >= T5_BOOTCTL_RECOVERY_AFTER;
    b.mark = t5_bootctl_counts(rec, deep_sleep_wake) && t5_bootctl_mark(rec, &b.word_off, &b.word);
    return b;
}

/* Failed boots before the running one, read by the app (OS or recovery)
 * after the bootloader counted, or didn't count, its boot. */
static inline int t5_bootctl_failed_before(const uint8_t rec[T5_BOOTCTL_REC_LEN], bool deep_sleep_wake)
{
    int n = t5_bootctl_count(rec);
    if (t5_bootctl_counts(rec, deep_sleep_wake) && n > 0) n--;
    return n;
}

/* The OS's decision: start in safe mode? */
static inline bool t5_bootctl_safe_mode(t5_boot_req_t req, int failed_before)
{
    return req == T5_BOOT_REQ_SAFEMODE || failed_before >= T5_BOOTCTL_SAFEMODE_AFTER;
}
