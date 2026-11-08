/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_core/err.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_system.h"
#endif

const char *t5_err_str(t5_err_t err)
{
    switch (err) {
    case T5_OK: return "OK";
    case T5_ERR_FAIL: return "FAIL";
    case T5_ERR_BUSY: return "BUSY";
    case T5_ERR_DENIED: return "DENIED";
    case T5_ERR_TIMEOUT: return "TIMEOUT";
    case T5_ERR_NO_MEM: return "NO_MEM";
    case T5_ERR_INVALID_ARG: return "INVALID_ARG";
    case T5_ERR_NOT_FOUND: return "NOT_FOUND";
    case T5_ERR_IO: return "IO";
    case T5_ERR_NOT_SUPPORTED: return "NOT_SUPPORTED";
    case T5_ERR_STATE: return "STATE";
    case T5_ERR_NACK: return "NACK";
    }
    return "UNKNOWN";
}

void t5_assert_fail(const char *file, int line, const char *expr)
{
#ifdef ESP_PLATFORM
    /* The message ends up in the panic output and the core dump. */
    char msg[160];
    snprintf(msg, sizeof msg, "T5_ASSERT(%s) failed at %s:%d", expr, file, line);
    esp_system_abort(msg);
#else
    fprintf(stderr, "T5_ASSERT(%s) failed at %s:%d\n", expr, file, line);
    abort();
#endif
}
