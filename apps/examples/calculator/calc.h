/* SPDX-License-Identifier: Apache-2.0
 * The Calculator's engine: what's typed, as tokens, and its value. Plain C,
 * no UI, so test/host/test_calc.c checks it on a computer.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CALC_MAX_TOK 96
#define CALC_HIST_LEN 160

typedef enum { CALC_T_DIGIT, CALC_T_POINT, CALC_T_OP, CALC_T_FUNC, CALC_T_LP, CALC_T_RP, CALC_T_CONST,
               CALC_T_POST } calc_tok_kind_t;
typedef enum { CALC_ADD, CALC_SUB, CALC_MUL, CALC_DIV, CALC_POW } calc_op_t;
typedef enum { CALC_SIN, CALC_COS, CALC_TAN, CALC_ASIN, CALC_ACOS, CALC_ATAN, CALC_LN, CALC_LOG,
               CALC_SQRT } calc_fn_t;
typedef enum { CALC_PI, CALC_E, CALC_ANS, CALC_LAST } calc_const_t;   /* LAST: the result, shown as its value */
typedef enum { CALC_FACT, CALC_SQ, CALC_INV, CALC_PCT } calc_post_t;

/* What a key does; v is the digit, operator, function, constant or postfix. */
typedef enum {
    CALC_K_DIGIT, CALC_K_POINT, CALC_K_OP, CALC_K_FUNC, CALC_K_LP, CALC_K_RP, CALC_K_CONST, CALC_K_POST,
    CALC_K_CLEAR, CALC_K_BACK, CALC_K_SIGN, CALC_K_EQUALS, CALC_K_ANGLE, CALC_K_EXP,   /* EXP: eˣ */
} calc_key_t;

typedef struct {
    uint8_t kind, v;
} calc_tok_t;

/* All of it is plain data, so the app saves it as it is. */
typedef struct {
    calc_tok_t tok[CALC_MAX_TOK];
    uint8_t ntok;
    bool deg;                   /* angles in degrees, else radians */
    bool done;                  /* "=" was pressed: the result is showing */
    double ans;                 /* the last result */
    char hist[CALC_HIST_LEN];   /* the last calculation, "12×3 =" or "12×3 = 36" */
} calc_t;

void calc_reset(calc_t *c);
/* Press a key. false only for "=" on something that has no value (Error). */
bool calc_key(calc_t *c, calc_key_t key, int v);
/* The value of what's typed: false if it's incomplete or has none. */
bool calc_eval(const calc_t *c, double *out);
/* What's typed, for the display ("" if nothing). */
void calc_text(const calc_t *c, char *out, size_t n);
/* The live result ("= 42"), or "" while there's nothing more to show. */
void calc_preview(const calc_t *c, char *out, size_t n);
/* A number for people: 12 significant digits, "1,234" if group, "1.5×10⁻⁹"
 * when very large or small, a real minus sign. false for no value. */
bool calc_format(double v, char *out, size_t n, bool group);
