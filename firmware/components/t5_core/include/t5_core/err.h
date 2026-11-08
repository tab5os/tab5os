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

/* Rule 7 (docs/coding-standard.md): the caller has to look at the result.
 * The strict components mark their t5_err_t APIs with it. */
#define T5_MUST_CHECK __attribute__((warn_unused_result))

/* Ignore a T5_MUST_CHECK result on purpose, with a comment saying why.
 * GCC doesn't take a (void) cast as ignoring one. */
#define T5_IGNORE(expr)                         \
    do {                                        \
        t5_err_t _t5_ignored = (expr);          \
        (void)_t5_ignored;                      \
    } while (0)

/* Rule 5 (docs/coding-standard.md): an invariant that must hold. Kept in
 * release builds: a failure aborts with the expression and where, which
 * the panic output and core dump keep, and the device restarts. Use it for "can't
 * happen" states, not to validate input (return T5_ERR_INVALID_ARG for that). */
#define T5_ASSERT(cond)                                         \
    do {                                                        \
        if (__builtin_expect(!(cond), 0)) {                     \
            t5_assert_fail(__FILE__, __LINE__, #cond);          \
        }                                                       \
    } while (0)

__attribute__((noreturn, cold)) void t5_assert_fail(const char *file, int line, const char *expr);

#define T5_RETURN_ON_ERR(expr)          \
    do {                                \
        t5_err_t _e = (expr);           \
        if (_e != T5_OK) return _e;     \
    } while (0)

#ifdef __cplusplus
}
#endif
