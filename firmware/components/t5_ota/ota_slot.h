/* SPDX-License-Identifier: Apache-2.0
 * Which A/B slot an install writes (see ota.h). Pure, for host tests.
 */
#pragma once

/* running: the running image's OTA slot, or -1 when it isn't in one (the
 * recovery image). boot: the slot the bootloader picks next, -1 unknown.
 * nslots: OTA slots in the partition table. Returns a slot, or -1. */
static inline int t5_ota_target_slot(int running, int boot, int nslots)
{
    if (nslots < 1) return -1;
    if (running >= 0) return nslots >= 2 ? (running + 1) % nslots : -1;
    return boot >= 0 && boot < nslots ? boot : 0;
}
