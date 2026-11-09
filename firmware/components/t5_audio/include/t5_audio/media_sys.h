/* SPDX-License-Identifier: Apache-2.0
 * Media sessions, the system's side (not in the SDK): read the active
 * session and send it commands (the standby screen, the console).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_audio/media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool active;              /* a session is open */
    char app_id[64];          /* T5_APP_ID_LEN */
    t5_media_info_t info;
    t5_media_state_t state;
    uint32_t position_ms;     /* now, moved on while playing */
} t5_media_now_t;

/* The active session; false (and active false) if there is none. */
bool t5_media_now(t5_media_now_t *out);
/* Queue a command for the active session. T5_ERR_STATE if there is none,
 * T5_ERR_BUSY if its queue is full. */
t5_err_t t5_media_command(const t5_media_cmd_t *cmd);

#ifdef __cplusplus
}
#endif
