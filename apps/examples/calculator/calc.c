/* SPDX-License-Identifier: Apache-2.0
 * The Calculator's engine (calc.h).
 *
 * What's typed is kept as tokens (a digit, an operator, "sin(" ...), so
 * backspace takes a whole function off and the display is built from them.
 * A recursive-descent parser evaluates them with the usual precedence:
 * postfix (! ² ⁻¹ %), then powers (right to left: 2^3^2 is 2^9), then unary
 * minus (−2² is −4), then × ÷ and implied multiplication (2π, 3sin(30)),
 * then + −. Open brackets close themselves at the end. As on phone
 * calculators, a lone percent after + or − is a share of the left side:
 * 50 + 10% is 55.
 */
#include "calc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

#define SIG_DIGITS 12

/* UTF-8 for the symbols on the display. */
#define S_MINUS "\xE2\x88\x92"     /* − */
#define S_TIMES "\xC3\x97"         /* × */
#define S_DIVIDE "\xC3\xB7"        /* ÷ */
#define S_ROOT "\xE2\x88\x9A"      /* √ */
#define S_PI "\xCF\x80"            /* π */
#define S_SUP_MINUS "\xE2\x81\xBB" /* ⁻ */
#define S_SUP_1 "\xC2\xB9"         /* ¹ */
#define S_SUP_2 "\xC2\xB2"         /* ² */

static const char *const s_op_text[] = { "+", S_MINUS, S_TIMES, S_DIVIDE, "^" };
static const char *const s_fn_text[] = {
    "sin(", "cos(", "tan(", "sin" S_SUP_MINUS S_SUP_1 "(", "cos" S_SUP_MINUS S_SUP_1 "(",
    "tan" S_SUP_MINUS S_SUP_1 "(", "ln(", "log(", S_ROOT "(",
};
static const char *const s_post_text[] = { "!", S_SUP_2, S_SUP_MINUS S_SUP_1, "%" };

/* ---- numbers --------------------------------------------------------------- */
static void append(char *out, size_t n, const char *text)
{
    size_t l = strlen(out);
    if (l < n) snprintf(out + l, n - l, "%s", text);
}

/* Superscript digits for an exponent: ⁰ ¹ ² ³ ⁴ ... ⁹. */
static const char *sup_digit(int d)
{
    static const char *const t[] = {
        "\xE2\x81\xB0", S_SUP_1, S_SUP_2, "\xC2\xB3", "\xE2\x81\xB4",
        "\xE2\x81\xB5", "\xE2\x81\xB6", "\xE2\x81\xB7", "\xE2\x81\xB8", "\xE2\x81\xB9",
    };
    return t[d];
}

/* Drop trailing zeros after a decimal point, and the point itself. */
static void trim_zeros(char *t)
{
    if (!strchr(t, '.')) return;
    size_t l = strlen(t);
    while (l && t[l - 1] == '0') t[--l] = 0;
    if (l && t[l - 1] == '.') t[--l] = 0;
}

bool calc_format(double v, char *out, size_t n, bool group)
{
    out[0] = 0;
    if (isnan(v) || isinf(v)) return false;
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.*g", SIG_DIGITS, v);   /* round away float noise: 0.1+0.2 */
    v = strtod(tmp, NULL);
    if (v == 0) v = 0;                                    /* no "−0" */
    double av = fabs(v);
    if (v < 0) append(out, n, S_MINUS);
    if (av == 0) {
        append(out, n, "0");
        return true;
    }
    int e = (int)floor(log10(av));
    if (e >= 15 || e < -7) {
        double m = av / pow(10, e);
        snprintf(tmp, sizeof tmp, "%.*f", SIG_DIGITS - 1, m);
        if (tmp[0] == '1' && tmp[1] == '0') {             /* rounding carried: 9.99… became 10 */
            e++;
            snprintf(tmp, sizeof tmp, "%.*f", SIG_DIGITS - 1, av / pow(10, e));
        }
        trim_zeros(tmp);
        append(out, n, tmp);
        append(out, n, S_TIMES "10");
        if (e < 0) append(out, n, S_SUP_MINUS);
        char digits[12];
        snprintf(digits, sizeof digits, "%d", abs(e));
        for (char *d = digits; *d; d++) append(out, n, sup_digit(*d - '0'));
        return true;
    }
    int decimals = SIG_DIGITS - 1 - e;
    if (decimals < 0) decimals = 0;
    if (decimals > 20) decimals = 20;
    snprintf(tmp, sizeof tmp, "%.*f", decimals, av);
    trim_zeros(tmp);
    char *dot = strchr(tmp, '.');
    size_t int_len = dot ? (size_t)(dot - tmp) : strlen(tmp);
    for (size_t i = 0; i < int_len; i++) {
        char c[2] = { tmp[i], 0 };
        append(out, n, c);
        size_t left = int_len - i - 1;
        if (group && left && left % 3 == 0) append(out, n, ",");
    }
    if (dot) append(out, n, dot);
    return true;
}

/* ---- the parser ------------------------------------------------------------ */
typedef struct {
    const calc_t *c;
    int i;
    bool err;
} parser_t;

static double p_expr(parser_t *p);
static double p_unary(parser_t *p, bool *pct);

static bool peek(const parser_t *p, calc_tok_kind_t kind, int v)
{
    return p->i < p->c->ntok && p->c->tok[p->i].kind == kind && (v < 0 || p->c->tok[p->i].v == v);
}

/* Could the next token start an operand (for implied multiplication)? */
static bool starts_operand(const parser_t *p)
{
    if (p->i >= p->c->ntok) return false;
    calc_tok_kind_t k = (calc_tok_kind_t)p->c->tok[p->i].kind;
    return k == CALC_T_DIGIT || k == CALC_T_POINT || k == CALC_T_FUNC || k == CALC_T_LP || k == CALC_T_CONST;
}

/* Trig in degrees lands exactly on the right angles: sin 180° is 0. */
static double trig(calc_fn_t f, double x, bool deg)
{
    if (deg && x == floor(x) && fmod(x, 90) == 0) {
        int q = (int)fmod(fmod(x / 90, 4) + 4, 4);       /* quadrant 0..3 */
        static const double sn[4] = { 0, 1, 0, -1 }, cs[4] = { 1, 0, -1, 0 };
        if (f == CALC_SIN) return sn[q];
        if (f == CALC_COS) return cs[q];
        return (q & 1) ? NAN : 0;                          /* tan 90° has no value */
    }
    double r = deg ? x * M_PI / 180.0 : x;
    double v = f == CALC_SIN ? sin(r) : f == CALC_COS ? cos(r) : tan(r);
    return fabs(v) < 1e-15 ? 0 : v;
}

static double apply(calc_fn_t f, double x, bool deg)
{
    double k = deg ? 180.0 / M_PI : 1.0;
    switch (f) {
    case CALC_SIN: case CALC_COS: case CALC_TAN: return trig(f, x, deg);
    case CALC_ASIN: return x < -1 || x > 1 ? NAN : asin(x) * k;
    case CALC_ACOS: return x < -1 || x > 1 ? NAN : acos(x) * k;
    case CALC_ATAN: return atan(x) * k;
    case CALC_LN: return x <= 0 ? NAN : log(x);
    case CALC_LOG: return x <= 0 ? NAN : log10(x);
    case CALC_SQRT: return x < 0 ? NAN : sqrt(x);
    }
    return NAN;
}

static double factorial(double x)
{
    if (x < 0 || x != floor(x) || x > 170) return NAN;
    double r = 1;
    for (int i = 2; i <= (int)x; i++) r *= i;
    return r;
}

static double p_number(parser_t *p)
{
    char buf[48];
    size_t l = 0;
    int points = 0;
    while (peek(p, CALC_T_DIGIT, -1) || peek(p, CALC_T_POINT, -1)) {
        const calc_tok_t *t = &p->c->tok[p->i++];
        if (t->kind == CALC_T_POINT) points++;
        if (l + 1 < sizeof buf) buf[l++] = t->kind == CALC_T_POINT ? '.' : (char)('0' + t->v);
    }
    buf[l] = 0;
    if (points > 1 || !strcmp(buf, ".")) p->err = true;
    return strtod(buf, NULL);
}

/* A closing bracket, or the end (brackets close themselves). */
static void close_bracket(parser_t *p)
{
    if (peek(p, CALC_T_RP, -1)) p->i++;
    else if (p->i < p->c->ntok) p->err = true;
}

static double p_primary(parser_t *p)
{
    if (p->i >= p->c->ntok) {
        p->err = true;
        return NAN;
    }
    const calc_tok_t *t = &p->c->tok[p->i];
    switch ((calc_tok_kind_t)t->kind) {
    case CALC_T_DIGIT: case CALC_T_POINT: return p_number(p);
    case CALC_T_CONST:
        p->i++;
        return t->v == CALC_PI ? M_PI : t->v == CALC_E ? M_E : p->c->ans;
    case CALC_T_LP: {
        p->i++;
        double v = p_expr(p);
        close_bracket(p);
        return v;
    }
    case CALC_T_FUNC: {
        p->i++;
        double v = p_expr(p);
        close_bracket(p);
        return apply((calc_fn_t)t->v, v, p->c->deg);
    }
    default:
        p->err = true;
        return NAN;
    }
}

static double p_postfix(parser_t *p, bool *pct)
{
    double v = p_primary(p);
    *pct = false;
    while (peek(p, CALC_T_POST, -1)) {
        calc_post_t op = (calc_post_t)p->c->tok[p->i++].v;
        *pct = op == CALC_PCT;
        v = op == CALC_FACT ? factorial(v) : op == CALC_SQ ? v * v : op == CALC_INV ? 1.0 / v : v / 100.0;
    }
    return v;
}

static double p_power(parser_t *p, bool *pct)
{
    double b = p_postfix(p, pct);
    if (!peek(p, CALC_T_OP, CALC_POW)) return b;
    p->i++;
    bool unused;
    double e = p_unary(p, &unused);                     /* right to left: 2^3^2 = 2^9 */
    *pct = false;
    return pow(b, e);
}

static double p_unary(parser_t *p, bool *pct)
{
    if (peek(p, CALC_T_OP, CALC_SUB)) {
        p->i++;
        return -p_unary(p, pct);
    }
    if (peek(p, CALC_T_OP, CALC_ADD)) {
        p->i++;
        return p_unary(p, pct);
    }
    return p_power(p, pct);
}

/* × ÷ and implied multiplication. *pct: the term was a lone "x%". */
static double p_term(parser_t *p, bool *pct)
{
    bool last_pct;
    double v = p_unary(p, &last_pct);
    int factors = 1;
    for (;;) {
        if (peek(p, CALC_T_OP, CALC_MUL) || peek(p, CALC_T_OP, CALC_DIV)) {
            bool div = p->c->tok[p->i++].v == CALC_DIV;
            double r = p_unary(p, &last_pct);
            v = div ? v / r : v * r;
        } else if (starts_operand(p)) {
            v *= p_unary(p, &last_pct);
        } else {
            break;
        }
        factors++;
    }
    *pct = factors == 1 && last_pct;
    return v;
}

static double p_expr(parser_t *p)
{
    bool pct;
    double v = p_term(p, &pct);
    while (peek(p, CALC_T_OP, CALC_ADD) || peek(p, CALC_T_OP, CALC_SUB)) {
        bool sub = p->c->tok[p->i++].v == CALC_SUB;
        double r = p_term(p, &pct);
        if (pct) r *= v;                                  /* 50 + 10% = 55 */
        v = sub ? v - r : v + r;
    }
    return v;
}

bool calc_eval(const calc_t *c, double *out)
{
    if (!c->ntok) return false;
    parser_t p = { .c = c };
    double v = p_expr(&p);
    if (p.err || p.i != c->ntok || isnan(v) || isinf(v)) return false;
    *out = v;
    return true;
}

/* ---- editing --------------------------------------------------------------- */
void calc_reset(calc_t *c)
{
    memset(c, 0, sizeof *c);
    c->deg = true;
}

static calc_tok_t *last_tok(calc_t *c)
{
    return c->ntok ? &c->tok[c->ntok - 1] : NULL;
}

static void push(calc_t *c, calc_tok_kind_t kind, int v)
{
    if (c->ntok < CALC_MAX_TOK) c->tok[c->ntok++] = (calc_tok_t){ (uint8_t)kind, (uint8_t)v };
}

/* Does what's typed end in something an operator can follow? */
static bool ends_operand(calc_t *c)
{
    const calc_tok_t *t = last_tok(c);
    return t && (t->kind == CALC_T_DIGIT || t->kind == CALC_T_POINT || t->kind == CALC_T_RP ||
                 t->kind == CALC_T_CONST || t->kind == CALC_T_POST);
}

static int open_brackets(const calc_t *c)
{
    int depth = 0;
    for (int i = 0; i < c->ntok; i++) {
        if (c->tok[i].kind == CALC_T_LP || c->tok[i].kind == CALC_T_FUNC) depth++;
        if (c->tok[i].kind == CALC_T_RP) depth--;
    }
    return depth;
}

/* After "=": a number starts afresh, an operator carries on from the result. */
static void after_result(calc_t *c, bool carry_on)
{
    if (!c->done) return;
    c->done = false;
    char num[64];                                         /* "12×3 =" becomes "12×3 = 36" */
    if (calc_format(c->ans, num, sizeof num, true)) {
        size_t l = strlen(c->hist);
        snprintf(c->hist + l, sizeof c->hist - l, " %s", num);
    }
    c->ntok = 0;
    if (carry_on) push(c, CALC_T_CONST, CALC_LAST);
}

/* Where the number at the end starts, or -1. */
static int trailing_number(const calc_t *c)
{
    int i = c->ntok;
    while (i > 0 && (c->tok[i - 1].kind == CALC_T_DIGIT || c->tok[i - 1].kind == CALC_T_POINT)) i--;
    return i == c->ntok ? -1 : i;
}

/* ±: negate the number (or constant) at the end, or start a negative one. */
static void toggle_sign(calc_t *c)
{
    after_result(c, true);
    int at = trailing_number(c);
    if (at < 0 && last_tok(c) && last_tok(c)->kind == CALC_T_CONST) at = c->ntok - 1;
    if (at < 0) {
        if (!ends_operand(c)) push(c, CALC_T_OP, CALC_SUB);   /* after ")" or "!": nothing to do */
        return;
    }
    bool unary = at > 0 && c->tok[at - 1].kind == CALC_T_OP && c->tok[at - 1].v == CALC_SUB &&
                 (at == 1 || c->tok[at - 2].kind == CALC_T_OP || c->tok[at - 2].kind == CALC_T_LP ||
                  c->tok[at - 2].kind == CALC_T_FUNC);
    if (unary) {
        memmove(&c->tok[at - 1], &c->tok[at], (size_t)(c->ntok - at) * sizeof(calc_tok_t));
        c->ntok--;
    } else if (c->ntok < CALC_MAX_TOK) {
        memmove(&c->tok[at + 1], &c->tok[at], (size_t)(c->ntok - at) * sizeof(calc_tok_t));
        c->tok[at] = (calc_tok_t){ CALC_T_OP, CALC_SUB };
        c->ntok++;
    }
}

static void press_operator(calc_t *c, calc_op_t op)
{
    after_result(c, true);
    calc_tok_t *t = last_tok(c);
    if (!t) {
        if (op == CALC_SUB) push(c, CALC_T_OP, CALC_SUB);    /* a negative number to start */
        return;
    }
    if (t->kind == CALC_T_OP) {
        if (op == CALC_SUB && t->v != CALC_ADD && t->v != CALC_SUB) {
            push(c, CALC_T_OP, CALC_SUB);                  /* × − starts a negative operand */
            return;
        }
        /* A minus that made the next operand negative goes too: 5×− then + is 5+. */
        if (c->ntok >= 2 && t->v == CALC_SUB && c->tok[c->ntok - 2].kind == CALC_T_OP) {
            c->ntok--;
            t = last_tok(c);
        }
        if (c->ntok == 1 && op != CALC_SUB) c->ntok = 0;   /* only a leading minus: drop it */
        else t->v = (uint8_t)op;                           /* otherwise the new one replaces it */
        return;
    }
    if (t->kind == CALC_T_LP || t->kind == CALC_T_FUNC) {
        if (op == CALC_SUB) push(c, CALC_T_OP, CALC_SUB);
        return;
    }
    push(c, CALC_T_OP, op);
}

bool calc_key(calc_t *c, calc_key_t key, int v)
{
    switch (key) {
    case CALC_K_DIGIT:
        after_result(c, false);
        push(c, CALC_T_DIGIT, v);
        break;
    case CALC_K_POINT: {
        after_result(c, false);
        int at = trailing_number(c);
        for (int i = at; at >= 0 && i < c->ntok; i++) {
            if (c->tok[i].kind == CALC_T_POINT) return true;   /* one point per number */
        }
        if (at < 0) push(c, CALC_T_DIGIT, 0);               /* "." starts "0." */
        push(c, CALC_T_POINT, 0);
        break;
    }
    case CALC_K_OP: press_operator(c, (calc_op_t)v); break;
    case CALC_K_FUNC:
        after_result(c, false);
        push(c, CALC_T_FUNC, v);
        break;
    case CALC_K_LP:
        after_result(c, false);
        push(c, CALC_T_LP, 0);
        break;
    case CALC_K_RP:
        if (!c->done && open_brackets(c) > 0 && ends_operand(c)) push(c, CALC_T_RP, 0);
        break;
    case CALC_K_CONST:
        after_result(c, false);
        push(c, CALC_T_CONST, v);
        break;
    case CALC_K_POST:
        after_result(c, true);
        if (ends_operand(c)) push(c, CALC_T_POST, v);
        break;
    case CALC_K_EXP:                                       /* eˣ types e^( */
        after_result(c, false);
        push(c, CALC_T_CONST, CALC_E);
        push(c, CALC_T_OP, CALC_POW);
        push(c, CALC_T_LP, 0);
        break;
    case CALC_K_CLEAR:
        c->ntok = 0;
        c->done = false;
        break;
    case CALC_K_BACK:
        if (c->done) {
            c->ntok = 0;
            c->done = false;
        } else if (c->ntok) {
            c->ntok--;
        }
        break;
    case CALC_K_SIGN: toggle_sign(c); break;
    case CALC_K_ANGLE: c->deg = !c->deg; break;
    case CALC_K_EQUALS: {
        double r;
        if (c->done || !c->ntok) break;
        if (!calc_eval(c, &r)) return false;
        char e[CALC_HIST_LEN];
        calc_text(c, e, sizeof e);
        snprintf(c->hist, sizeof c->hist, "%.140s =", e);   /* room for " <result>" later */
        c->ans = r;
        c->ntok = 0;
        push(c, CALC_T_CONST, CALC_LAST);
        c->done = true;
        break;
    }
    }
    return true;
}

/* ---- text ------------------------------------------------------------------ */
void calc_text(const calc_t *c, char *out, size_t n)
{
    out[0] = 0;
    for (int i = 0; i < c->ntok; i++) {
        const calc_tok_t *t = &c->tok[i];
        char num[64];
        switch ((calc_tok_kind_t)t->kind) {
        case CALC_T_DIGIT: snprintf(num, sizeof num, "%d", t->v); append(out, n, num); break;
        case CALC_T_POINT: append(out, n, "."); break;
        case CALC_T_OP: append(out, n, s_op_text[t->v]); break;
        case CALC_T_FUNC: append(out, n, s_fn_text[t->v]); break;
        case CALC_T_LP: append(out, n, "("); break;
        case CALC_T_RP: append(out, n, ")"); break;
        case CALC_T_POST: append(out, n, s_post_text[t->v]); break;
        case CALC_T_CONST:
            if (t->v == CALC_PI) append(out, n, S_PI);
            else if (t->v == CALC_E) append(out, n, "e");
            else if (t->v == CALC_ANS) append(out, n, "Ans");
            else if (calc_format(c->ans, num, sizeof num, true)) append(out, n, num);
            break;
        }
    }
}

void calc_preview(const calc_t *c, char *out, size_t n)
{
    out[0] = 0;
    if (c->done) return;
    /* Only once there's more than a lone number to work out. */
    bool worth = false;
    for (int i = 0; i < c->ntok; i++) {
        calc_tok_kind_t k = (calc_tok_kind_t)c->tok[i].kind;
        if (k != CALC_T_DIGIT && k != CALC_T_POINT && !(k == CALC_T_CONST && c->tok[i].v == CALC_LAST)) {
            worth = true;
        }
    }
    double v;
    char num[64];
    if (worth && calc_eval(c, &v) && calc_format(v, num, sizeof num, true)) snprintf(out, n, "= %s", num);
}
