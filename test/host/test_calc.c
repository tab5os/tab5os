/* SPDX-License-Identifier: Apache-2.0
 * The Calculator example's engine (apps/examples/calculator/calc.c): typed
 * key by key, as on the device. */
#include <math.h>
#include <string.h>
#include "unity.h"
#include "calc.h"

static calc_t c;

void setUp(void)
{
    calc_reset(&c);
}

void tearDown(void)
{
}

/* Type keys. Digits and . + - * / ^ ( ) ! % = are themselves; letters:
 * s c t sin cos tan, S C T their inverses, l ln, g log, r root, q x²,
 * i 1/x, p pi, e e, a Ans, x eˣ, n ±, b backspace, k clear, d DEG/RAD.
 * Returns false if "=" gave an error. */
static bool type(const char *keys)
{
    bool ok = true;
    for (const char *k = keys; *k; k++) {
        switch (*k) {
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            calc_key(&c, CALC_K_DIGIT, *k - '0');
            break;
        case '.': calc_key(&c, CALC_K_POINT, 0); break;
        case '+': calc_key(&c, CALC_K_OP, CALC_ADD); break;
        case '-': calc_key(&c, CALC_K_OP, CALC_SUB); break;
        case '*': calc_key(&c, CALC_K_OP, CALC_MUL); break;
        case '/': calc_key(&c, CALC_K_OP, CALC_DIV); break;
        case '^': calc_key(&c, CALC_K_OP, CALC_POW); break;
        case '(': calc_key(&c, CALC_K_LP, 0); break;
        case ')': calc_key(&c, CALC_K_RP, 0); break;
        case '!': calc_key(&c, CALC_K_POST, CALC_FACT); break;
        case '%': calc_key(&c, CALC_K_POST, CALC_PCT); break;
        case 'q': calc_key(&c, CALC_K_POST, CALC_SQ); break;
        case 'i': calc_key(&c, CALC_K_POST, CALC_INV); break;
        case 's': calc_key(&c, CALC_K_FUNC, CALC_SIN); break;
        case 'c': calc_key(&c, CALC_K_FUNC, CALC_COS); break;
        case 't': calc_key(&c, CALC_K_FUNC, CALC_TAN); break;
        case 'S': calc_key(&c, CALC_K_FUNC, CALC_ASIN); break;
        case 'C': calc_key(&c, CALC_K_FUNC, CALC_ACOS); break;
        case 'T': calc_key(&c, CALC_K_FUNC, CALC_ATAN); break;
        case 'l': calc_key(&c, CALC_K_FUNC, CALC_LN); break;
        case 'g': calc_key(&c, CALC_K_FUNC, CALC_LOG); break;
        case 'r': calc_key(&c, CALC_K_FUNC, CALC_SQRT); break;
        case 'p': calc_key(&c, CALC_K_CONST, CALC_PI); break;
        case 'e': calc_key(&c, CALC_K_CONST, CALC_E); break;
        case 'a': calc_key(&c, CALC_K_CONST, CALC_ANS); break;
        case 'x': calc_key(&c, CALC_K_EXP, 0); break;
        case 'n': calc_key(&c, CALC_K_SIGN, 0); break;
        case 'b': calc_key(&c, CALC_K_BACK, 0); break;
        case 'k': calc_key(&c, CALC_K_CLEAR, 0); break;
        case 'd': calc_key(&c, CALC_K_ANGLE, 0); break;
        case '=': ok = calc_key(&c, CALC_K_EQUALS, 0) && ok; break;
        default: TEST_FAIL_MESSAGE("unknown key in test");
        }
    }
    return ok;
}

static char s_text[200];

static const char *text(void)
{
    calc_text(&c, s_text, sizeof s_text);
    return s_text;
}

static const char *preview(void)
{
    calc_preview(&c, s_text, sizeof s_text);
    return s_text;
}

/* Type keys ending in "=" and check the shown result. */
static void check(const char *keys, const char *shown)
{
    calc_reset(&c);
    TEST_ASSERT_TRUE_MESSAGE(type(keys), keys);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(shown, text(), keys);
}

static void check_error(const char *keys)
{
    calc_reset(&c);
    TEST_ASSERT_FALSE_MESSAGE(type(keys), keys);
}

static void test_arithmetic(void)
{
    check("2+3*4=", "14");
    check("(2+3)*4=", "20");
    check("10/4=", "2.5");
    check("7-10=", "\xE2\x88\x92" "3");                   /* −3 */
    check("0.1+0.2=", "0.3");                             /* no float noise */
    check("1/3=", "0.333333333333");
    check("2/3*3=", "2");
    check("1234567*1000=", "1,234,567,000");
    check(".5+.5=", "1");
}

static void test_precedence(void)
{
    check("2^3^2=", "512");                               /* right to left */
    check("-2^2=", "\xE2\x88\x92" "4");                    /* −(2²) */
    check("2^-1=", "0.5");
    check("2*-3=", "\xE2\x88\x92" "6");
    check("2p=", "6.28318530718");                        /* implied multiplication */
    check("2(3+4)=", "14");
    check("(1+2)(3+4)=", "21");
    check("3!q=", "36");                                  /* postfix left to right */
    check("(2+3=", "5");                                  /* brackets close themselves */
    check("s(30=", "0.5");
}

static void test_percent(void)
{
    check("50+10%=", "55");
    check("50-10%=", "45");
    check("50*10%=", "5");
    check("10%=", "0.1");
    check("200+5*10%=", "200.5");                          /* only a lone percent term is a share */
}

static void test_scientific_degrees(void)
{
    check("s30=", "0.5");
    check("s180=", "0");
    check("c90=", "0");
    check("c60=", "0.5");
    check("t45=", "1");
    check("S1=", "90");
    check("T1=", "45");
    check_error("t90=");
    check_error("S2=");
}

static void test_scientific_radians(void)
{
    calc_reset(&c);
    TEST_ASSERT_TRUE(type("d"));
    TEST_ASSERT_FALSE(c.deg);
    TEST_ASSERT_TRUE(type("sp="));
    TEST_ASSERT_EQUAL_STRING("0", text());
    calc_reset(&c);
    type("dc0=");
    TEST_ASSERT_EQUAL_STRING("1", text());
    calc_reset(&c);
    type("dC0=");
    TEST_ASSERT_EQUAL_STRING("1.57079632679", text());
}

static void test_functions(void)
{
    check("l" "e" "=", "1");
    check("g1000=", "3");
    check("r16=", "4");
    check("x1=", "2.71828182846");                        /* eˣ: e^(1 */
    check("5!=", "120");
    check("0!=", "1");
    check("4i=", "0.25");
    check("9q=", "81");
    check_error("l0=");
    check_error("r-4=");
    check_error("1/0=");
    check_error("2.5!=");
    check_error("171!=");
}

static void test_format(void)
{
    char out[64];
    TEST_ASSERT_TRUE(calc_format(1e20, out, sizeof out, true));
    TEST_ASSERT_EQUAL_STRING("1" "\xC3\x97" "10" "\xC2\xB2" "\xE2\x81\xB0", out);   /* 1×10²⁰ */
    TEST_ASSERT_TRUE(calc_format(-1.5e-9, out, sizeof out, true));
    TEST_ASSERT_EQUAL_STRING("\xE2\x88\x92" "1.5" "\xC3\x97" "10" "\xE2\x81\xBB" "\xE2\x81\xB9", out);
    TEST_ASSERT_TRUE(calc_format(9.99999999999999e19, out, sizeof out, true));   /* rounds up a power */
    TEST_ASSERT_EQUAL_STRING("1" "\xC3\x97" "10" "\xC2\xB2" "\xE2\x81\xB0", out);
    TEST_ASSERT_TRUE(calc_format(-0.0, out, sizeof out, true));
    TEST_ASSERT_EQUAL_STRING("0", out);
    TEST_ASSERT_TRUE(calc_format(123456.75, out, sizeof out, false));
    TEST_ASSERT_EQUAL_STRING("123456.75", out);
    TEST_ASSERT_TRUE(calc_format(0.000001, out, sizeof out, true));
    TEST_ASSERT_EQUAL_STRING("0.000001", out);
    TEST_ASSERT_FALSE(calc_format(NAN, out, sizeof out, true));
    TEST_ASSERT_FALSE(calc_format(INFINITY, out, sizeof out, true));
}

static void test_editing(void)
{
    type("12+");
    TEST_ASSERT_EQUAL_STRING("12+", text());
    type("*");                                            /* a new operator replaces the old */
    TEST_ASSERT_EQUAL_STRING("12" "\xC3\x97", text());
    type("-");                                            /* × − starts a negative operand */
    TEST_ASSERT_EQUAL_STRING("12" "\xC3\x97" "\xE2\x88\x92", text());
    type("+");                                            /* and goes with the × */
    TEST_ASSERT_EQUAL_STRING("12+", text());
    calc_reset(&c);
    type("1.2.3");                                        /* one point per number */
    TEST_ASSERT_EQUAL_STRING("1.23", text());
    calc_reset(&c);
    type(".");
    TEST_ASSERT_EQUAL_STRING("0.", text());
    calc_reset(&c);
    type("s30b");                                         /* backspace takes a digit */
    TEST_ASSERT_EQUAL_STRING("sin(3", text());
    type("bb");                                           /* and a whole function */
    TEST_ASSERT_EQUAL_STRING("", text());
    calc_reset(&c);
    type(")");                                            /* nothing to close */
    TEST_ASSERT_EQUAL_STRING("", text());
    type("*");                                            /* nothing to multiply */
    TEST_ASSERT_EQUAL_STRING("", text());
    calc_reset(&c);
    type("5n");
    TEST_ASSERT_EQUAL_STRING("\xE2\x88\x92" "5", text());
    type("n");
    TEST_ASSERT_EQUAL_STRING("5", text());
    type("+3n");
    TEST_ASSERT_EQUAL_STRING("5+" "\xE2\x88\x92" "3", text());
    TEST_ASSERT_TRUE(type("="));
    TEST_ASSERT_EQUAL_STRING("2", text());
}

static void test_results_carry_on(void)
{
    type("12*3=");
    TEST_ASSERT_EQUAL_STRING("36", text());
    TEST_ASSERT_EQUAL_STRING("12" "\xC3\x97" "3 =", c.hist);
    type("+4");                                           /* an operator carries on from the result */
    TEST_ASSERT_EQUAL_STRING("36+4", text());
    TEST_ASSERT_EQUAL_STRING("12" "\xC3\x97" "3 = 36", c.hist);
    TEST_ASSERT_EQUAL_STRING("= 40", preview());
    type("=");
    type("7");                                            /* a digit starts afresh */
    TEST_ASSERT_EQUAL_STRING("7", text());
    type("*a=");                                          /* Ans is the last result */
    TEST_ASSERT_EQUAL_STRING("280", text());
    type("b");
    TEST_ASSERT_EQUAL_STRING("", text());
}

static void test_preview(void)
{
    type("12");
    TEST_ASSERT_EQUAL_STRING("", preview());               /* a lone number: nothing more to show */
    type("+3");
    TEST_ASSERT_EQUAL_STRING("= 15", preview());
    type("*");
    TEST_ASSERT_EQUAL_STRING("", preview());               /* incomplete */
    type("(2");
    TEST_ASSERT_EQUAL_STRING("= 18", preview());           /* open brackets close themselves */
}

static void test_full(void)
{
    for (int i = 0; i < CALC_MAX_TOK + 10; i++) type("1");   /* the table fills, nothing breaks */
    TEST_ASSERT_EQUAL_INT(CALC_MAX_TOK, c.ntok);
    TEST_ASSERT_TRUE(type("="));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_arithmetic);
    RUN_TEST(test_precedence);
    RUN_TEST(test_percent);
    RUN_TEST(test_scientific_degrees);
    RUN_TEST(test_scientific_radians);
    RUN_TEST(test_functions);
    RUN_TEST(test_format);
    RUN_TEST(test_editing);
    RUN_TEST(test_results_carry_on);
    RUN_TEST(test_preview);
    RUN_TEST(test_full);
    return UNITY_END();
}
