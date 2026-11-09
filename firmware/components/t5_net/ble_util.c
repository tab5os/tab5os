/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include "t5_net/ble.h"

bool t5_ble_parse_addr(const char *s, uint8_t out[6])
{
    unsigned b[6];
    char tail;
    if (!s || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[5 - i];
    return true;
}

void t5_ble_format_addr(const uint8_t a[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

bool t5_ble_adv_name(const uint8_t *adv, size_t len, char *out, size_t cap)
{
    bool found = false;
    for (size_t i = 0; i + 1 < len;) {
        uint8_t l = adv[i];
        if (l == 0 || i + 1 + l > len) break;
        uint8_t type = adv[i + 1];
        if ((type == 0x09 || (type == 0x08 && !found)) && cap) {
            size_t name_len = (size_t)l - 1;          /* l >= 1: the type byte */
            size_t n = name_len < cap - 1 ? name_len : cap - 1;
            memcpy(out, adv + i + 2, n);
            out[n] = 0;
            found = true;
            if (type == 0x09) return true;
        }
        i += 1 + l;
    }
    return found;
}
