/* SPDX-License-Identifier: Apache-2.0
 * Boot control record (plan 9.2, 9.3), shared by the bootloader and the OS.
 *
 * It lives in the last 4 KB sector of the `recovery` partition (the recovery
 * image is smaller than its slot). Erased, every byte is 0xFF.
 *
 *   attempts  64 bytes: each boot the bootloader counts clears the lowest
 *             set bit (flash programs 1 -> 0 without an erase), so the
 *             number of zero bits is the number of boots since the OS last
 *             said it was healthy. Deep-sleep wakes don't count.
 *   request   8 bytes: "RECOVERY" sends the next boot to the recovery
 *             image; "SAFEMODE" asks the OS to start in safe mode.
 *
 * The OS erases the sector once a boot is healthy (services up, shell
 * drawn); the recovery image erases it when it starts. Three failed boots
 * in a row (a fourth attempt) start recovery; two start the OS in safe mode.
 */
#pragma once
#include <stdint.h>

#define T5_BOOTCTL_SECTOR_FROM_END 0x1000u
#define T5_BOOTCTL_ATTEMPT_BYTES 64
#define T5_BOOTCTL_REQ_OFFSET 64
#define T5_BOOTCTL_REQ_LEN 8
#define T5_BOOTCTL_REQ_RECOVERY "RECOVERY"
#define T5_BOOTCTL_REQ_SAFEMODE "SAFEMODE"
#define T5_BOOTCTL_RECOVERY_AFTER 3    /* failed boots */
#define T5_BOOTCTL_SAFEMODE_AFTER 2

static inline int t5_bootctl_count(const uint8_t *attempts)
{
    int n = 0;
    for (int i = 0; i < T5_BOOTCTL_ATTEMPT_BYTES; i++) {
        uint8_t z = (uint8_t)~attempts[i];
        while (z) {
            n += z & 1;
            z >>= 1;
        }
    }
    return n;
}
