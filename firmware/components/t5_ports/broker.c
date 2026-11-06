/* SPDX-License-Identifier: Apache-2.0
 * Pin broker: per-pin exclusive claims. Pure logic; ports.c holds the lock.
 */
#include <stdio.h>
#include <string.h>
#include "ports_priv.h"

static broker_pin_t s_pins[PORTS_GPIO_MAX];

void broker_reset(void)
{
    memset(s_pins, 0, sizeof s_pins);
}

const broker_pin_t *broker_pin(uint8_t gpio)
{
    return gpio < PORTS_GPIO_MAX ? &s_pins[gpio] : NULL;
}

t5_err_t broker_claim(const uint8_t *gpio, int n, uint32_t flags, const char *owner, const char *port,
                      t5_port_mode_t mode, const void *handle, char *why, size_t whylen)
{
    if (!gpio || n <= 0 || !handle) {
        return T5_ERR_INVALID_ARG;
    }
    for (int i = 0; i < n; i++) {
        if (gpio[i] >= PORTS_GPIO_MAX) {
            return T5_ERR_INVALID_ARG;
        }
    }
    /* Risky first, so the answer doesn't depend on who else holds what. */
    if (!(flags & T5_PORT_FLAG_RISKY)) {
        for (int i = 0; i < n; i++) {
            if (ports_pin_risky(gpio[i])) {
                if (why && whylen) {
                    snprintf(why, whylen, "G%u is %s: needs T5_PORT_FLAG_RISKY", gpio[i],
                             ports_pin_risk(gpio[i]));
                }
                return T5_ERR_DENIED;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        const broker_pin_t *p = &s_pins[gpio[i]];
        if (p->claimed) {
            if (why && whylen) {
                snprintf(why, whylen, "G%u held by %s (%s)", gpio[i], p->owner, p->port ? p->port : "?");
            }
            return T5_ERR_BUSY;
        }
    }
    for (int i = 0; i < n; i++) {
        broker_pin_t *p = &s_pins[gpio[i]];
        p->claimed = true;
        snprintf(p->owner, sizeof p->owner, "%s", owner ? owner : "?");
        p->port = port;
        p->mode = mode;
        p->handle = handle;
    }
    return T5_OK;
}

int broker_release(const void *handle)
{
    int n = 0;
    if (!handle) {
        return 0;
    }
    for (int i = 0; i < PORTS_GPIO_MAX; i++) {
        if (s_pins[i].claimed && s_pins[i].handle == handle) {
            memset(&s_pins[i], 0, sizeof s_pins[i]);
            n++;
        }
    }
    return n;
}
