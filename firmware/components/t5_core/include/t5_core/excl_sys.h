/* SPDX-License-Identifier: Apache-2.0
 * The service side of exclusive control (t5_core/excl.h): each service
 * says how to hand its hardware over and take it back, and checks a handle
 * before serving an exclusive call. Not for apps.
 */
#pragma once
#include <stdbool.h>
#include "t5_core/excl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Let go of the hardware (blocking is fine). An error refuses the claim. */
    t5_err_t (*take)(void);
    /* Reset it and start the OS's own use again. Undo whatever the app set
     * up under the claim. */
    void (*give)(void);
} t5_excl_provider_t;

/* Once, at the service's init. */
void t5_excl_set_provider(t5_excl_kind_t kind, const t5_excl_provider_t *p);
/* Does h hold kind right now? */
bool t5_excl_holds(const t5_excl_t *h, t5_excl_kind_t kind);
/* Is kind claimed by anyone (the OS's drivers stay away meanwhile)? */
bool t5_excl_claimed(t5_excl_kind_t kind);

#ifdef __cplusplus
}
#endif
