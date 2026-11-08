/* SPDX-License-Identifier: Apache-2.0
 * Developer mode (plan 8.5): an authenticated HTTP endpoint on the Wi-Fi
 * network for pushing, launching and watching apps from a computer (the
 * `t5` CLI). Off by default; nothing listens on the network while it is off
 * (plan 9.4).
 *
 * Pairing: the computer asks to pair (POST /api/pair?client=NAME); the
 * Tab5 shows a six-digit code; the computer sends it back
 * (POST /api/pair/confirm?request=ID&code=CODE) and receives a random
 * 256-bit token. Only its SHA-256 is stored. Every other call needs
 * "Authorization: Bearer <token>". Three wrong codes or two minutes end the
 * attempt.
 *
 * Endpoints (all under http://<ip>:8350/api/):
 *   GET  info                      model, OS version, API level (no token)
 *   GET  apps                      installed apps
 *   POST install?dest=auto|sd|data body: a .t5app; replaces a running copy
 *   POST launch?id=ID[&arg=A]      POST exit       POST remove?id=ID[&keep=1]
 *   GET  logs?pos=N&wait=MS        log bytes after N (long poll); X-Log-Pos: next N
 *   GET  screenshot?div=N          RGB565, native portrait; X-Width, X-Height, X-Rotation
 *   POST py                        body: Python source; runs headless, returns its output
 *   POST ota                       body: a signed OS image (plan 9.1), see t5_ota
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_DEV_PORT 8350
#define T5_DEV_MAX_CLIENTS 8
#define T5_DEV_NAME_LEN 32

/* Read the setting and start the server if developer mode is on. */
T5_MUST_CHECK t5_err_t t5_dev_init(void);
bool t5_dev_enabled(void);
/* Persisted; starts or stops the server. */
T5_MUST_CHECK t5_err_t t5_dev_set_enabled(bool on);

typedef struct {
    bool enabled, listening;
    uint16_t port;
    int clients;
    bool pairing;               /* a pairing code is on screen */
    uint32_t requests, denied;  /* since boot */
} t5_dev_status_t;
void t5_dev_status(t5_dev_status_t *out);
/* Paired computers' names; returns the count. */
int t5_dev_clients(char (*names)[T5_DEV_NAME_LEN], int max);
T5_MUST_CHECK t5_err_t t5_dev_forget_all(void);

/* Handlers other components add (t5_ota adds /api/ota). Called before or
 * after the server starts; the token check is done before fn runs. */
struct httpd_req;
typedef int (*t5_dev_handler_t)(struct httpd_req *req);
T5_MUST_CHECK t5_err_t t5_dev_add_handler(const char *uri, int method, t5_dev_handler_t fn);

#ifdef __cplusplus
}
#endif
