/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_core/err.h"

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
