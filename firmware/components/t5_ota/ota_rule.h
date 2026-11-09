/* SPDX-License-Identifier: Apache-2.0
 * Which images a source may install (t5_ota_source_t in ota.h). Pure, for
 * host tests.
 */
#pragma once
#include <stdbool.h>
#include "t5_ota/ota.h"

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
