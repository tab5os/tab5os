/* SPDX-License-Identifier: Apache-2.0
 * Exclusive claims (t5_core/excl.h, excl_sys.h): one holder per kind, the
 * person's question, the Quick Settings hold, and the app's cleanup.
 */
#include <stdio.h>
#include <stdlib.h>
#include "t5_core/excl_sys.h"
#include "t5_core/io_hold.h"
#include "t5_core/os.h"
#include "t5_core/res.h"

#define MAGIC 0x4558434cu      /* "EXCL" */

struct t5_excl {
    uint32_t magic;
    t5_excl_kind_t kind;
    volatile bool active;
};

static t5_excl_provider_t s_prov[T5_EXCL_COUNT];
static t5_excl_t *s_holder[T5_EXCL_COUNT];
static t5_mutex_t s_mx;

static t5_mutex_t mx(void)
{
    return t5_mutex_once(&s_mx);
}

void t5_excl_set_provider(t5_excl_kind_t kind, const t5_excl_provider_t *p)
{
    if ((unsigned)kind >= T5_EXCL_COUNT || !p) return;
    t5_mutex_lock(mx());
    s_prov[kind] = *p;
    t5_mutex_unlock(mx());
}

bool t5_excl_holds(const t5_excl_t *h, t5_excl_kind_t kind)
{
    return h && h->magic == MAGIC && h->kind == kind && h->active;
}

bool t5_excl_claimed(t5_excl_kind_t kind)
{
    if ((unsigned)kind >= T5_EXCL_COUNT) return false;
    t5_mutex_lock(mx());
    bool c = s_holder[kind] != NULL;
    t5_mutex_unlock(mx());
    return c;
}

bool t5_excl_active(t5_excl_t *h)
{
    return h && h->magic == MAGIC && h->active;
}

static const uint32_t CAP[T5_EXCL_COUNT] = { T5_CORE_CAP_EXCL_USBC, T5_CORE_CAP_EXCL_USBA, T5_CORE_CAP_EXCL_WIFI,
                                             T5_CORE_CAP_EXCL_BT };
static const t5_io_kind_t IO[T5_EXCL_COUNT] = { T5_IO_EXCL_USBC, T5_IO_EXCL_USBA, T5_IO_EXCL_WIFI, T5_IO_EXCL_BT };

/* Give kind back (the holder's handle stays, inactive). Once per claim. */
static void end(t5_excl_kind_t kind)
{
    t5_mutex_lock(mx());
    t5_excl_t *h = s_holder[kind];
    void (*give)(void) = s_prov[kind].give;
    if (!h || !h->active) {                    /* not yet taken, or someone else is ending it */
        t5_mutex_unlock(mx());
        return;
    }
    h->active = false;
    t5_mutex_unlock(mx());
    if (give) give();
    t5_io_hold_set(IO[kind], NULL, NULL);
    t5_mutex_lock(mx());
    if (s_holder[kind] == h) s_holder[kind] = NULL;
    t5_mutex_unlock(mx());
}

static void stop_usbc(void) { end(T5_EXCL_USBC); }
static void stop_usba(void) { end(T5_EXCL_USBA); }
static void stop_wifi(void) { end(T5_EXCL_WIFI); }
static void stop_bt(void) { end(T5_EXCL_BT); }
static const t5_io_stop_fn_t STOP[T5_EXCL_COUNT] = { stop_usbc, stop_usba, stop_wifi, stop_bt };

static void release_handle(void *h)
{
    T5_IGNORE(t5_excl_release(h));
}

t5_err_t t5_excl_claim(t5_excl_kind_t what, t5_excl_t **out)
{
    if ((unsigned)what >= T5_EXCL_COUNT || !out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    int pe = t5_core_perm_check(CAP[what]);
    if (pe != T5_OK) return (t5_err_t)pe;
    t5_excl_t *h = calloc(1, sizeof *h);
    if (!h) return T5_ERR_NO_MEM;
    h->magic = MAGIC;
    h->kind = what;
    t5_mutex_lock(mx());
    t5_err_t e = !s_prov[what].take ? T5_ERR_NOT_SUPPORTED : s_holder[what] ? T5_ERR_BUSY : T5_OK;
    t5_err_t (*take)(void) = s_prov[what].take;
    if (e == T5_OK) s_holder[what] = h;      /* reserve it while the OS lets go */
    t5_mutex_unlock(mx());
    if (e == T5_OK) e = take();
    if (e != T5_OK) {
        t5_mutex_lock(mx());
        if (s_holder[what] == h) s_holder[what] = NULL;
        t5_mutex_unlock(mx());
        h->magic = 0;
        free(h);
        return e;
    }
    h->active = true;
    const t5_app_ctx_hdr_t *app = t5_app_ctx_get();
    char who[32] = "";
    if (app && app->magic == T5_APP_CTX_MAGIC) snprintf(who, sizeof who, "%s", app->name && app->name[0] ? app->name : app->id);
    t5_io_hold_set(IO[what], who, STOP[what]);
    t5_res_add(T5_RES_OTHER, h, release_handle);
    *out = h;
    return T5_OK;
}

t5_err_t t5_excl_release(t5_excl_t *h)
{
    if (!h || h->magic != MAGIC) return T5_ERR_INVALID_ARG;
    t5_res_remove(h);
    t5_mutex_lock(mx());
    bool mine = s_holder[h->kind] == h;
    t5_mutex_unlock(mx());
    if (mine) end(h->kind);
    h->magic = 0;
    free(h);
    return T5_OK;
}
