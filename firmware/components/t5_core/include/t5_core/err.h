/* SPDX-License-Identifier: Apache-2.0
 * Error codes shared by every service and, later, the public SDK.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_OK = 0,
    T5_ERR_FAIL = -1,          /* generic failure */
    T5_ERR_BUSY = -2,          /* resource owned by someone else */
    T5_ERR_DENIED = -3,        /* not permitted (manifest, reserved address, risky pin) */
    T5_ERR_TIMEOUT = -4,
    T5_ERR_NO_MEM = -5,
    T5_ERR_INVALID_ARG = -6,
    T5_ERR_NOT_FOUND = -7,     /* device or file absent */
    T5_ERR_IO = -8,            /* bus or device error */
    T5_ERR_NOT_SUPPORTED = -9,
    T5_ERR_STATE = -10,        /* wrong state for this call (not initialised, asleep...) */
    T5_ERR_NACK = -11,         /* I2C device did not acknowledge */
} t5_err_t;

const char *t5_err_str(t5_err_t err);

#define T5_RETURN_ON_ERR(expr)          \
    do {                                \
        t5_err_t _e = (expr);           \
        if (_e != T5_OK) return _e;     \
    } while (0)

#ifdef __cplusplus
}
#endif
