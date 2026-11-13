/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "unity.h"
#include "t5_core/excl_sys.h"
#include "t5_core/io_hold.h"
#include "t5_mocks.h"

void setUp(void) { mock_reset_all(); }
void tearDown(void) {}

static int s_hits;
static t5_event_id_t s_last;
static uint8_t s_payload;
static const void *s_data;
static size_t s_len;
static uint8_t s_copy[T5_EVENT_MAX_DATA];

static void handler(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    s_hits += (int)(intptr_t)arg;
    s_last = id;
    s_data = data;
    s_len = len;
    if (data) {
        s_payload = *(const uint8_t *)data;
        memcpy(s_copy, data, sizeof s_copy);
    }
}

static void test_err_strings(void)
{
    TEST_ASSERT_EQUAL_STRING("OK", t5_err_str(T5_OK));
    TEST_ASSERT_EQUAL_STRING("NACK", t5_err_str(T5_ERR_NACK));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", t5_err_str((t5_err_t)-99));
}

static void test_event_subscribe_dispatch_unsubscribe(void)
{
    s_hits = 0;
    TEST_ASSERT_EQUAL(T5_OK, t5_event_subscribe(T5_EV_SD_INSERTED, handler, (void *)1));
    TEST_ASSERT_EQUAL(T5_OK, t5_event_subscribe(T5_EV_ANY, handler, (void *)10));
    uint8_t p = 42;
    t5_event_post(T5_EV_SD_INSERTED, &p, 1);
    TEST_ASSERT_EQUAL(11, s_hits);
    TEST_ASSERT_EQUAL(42, s_payload);
    t5_event_post(T5_EV_SD_REMOVED, NULL, 0);
    TEST_ASSERT_EQUAL(21, s_hits);   /* only the ANY subscriber */
    TEST_ASSERT_EQUAL(T5_OK, t5_event_unsubscribe(T5_EV_ANY, handler, (void *)10));
    t5_event_post(T5_EV_SD_REMOVED, NULL, 0);
    TEST_ASSERT_EQUAL(21, s_hits);
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_event_unsubscribe(T5_EV_ANY, handler, (void *)10));
}

static void test_event_payload_limit(void)
{
    uint8_t big[T5_EVENT_MAX_DATA + 1] = {0};
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_event_post(T5_EV_TIME_SET, big, sizeof big));
}

static void test_event_payload_is_a_padded_copy_with_its_length(void)
{
    TEST_ASSERT_EQUAL(T5_OK, t5_event_subscribe(T5_EV_I2C_DEVICE_FAULT, handler, (void *)1));
    t5_ev_i2c_t ev = { .addr = 0x41, .err = T5_ERR_NACK, .errors = 8 };
    TEST_ASSERT_EQUAL(T5_OK, t5_event_post(T5_EV_I2C_DEVICE_FAULT, &ev, sizeof ev));
    TEST_ASSERT_EQUAL(sizeof ev, s_len);
    TEST_ASSERT_TRUE(s_data != (const void *)&ev);
    TEST_ASSERT_EQUAL_MEMORY(&ev, s_copy, sizeof ev);
    for (size_t i = sizeof ev; i < T5_EVENT_MAX_DATA; i++) TEST_ASSERT_EQUAL_HEX8(0, s_copy[i]);
    TEST_ASSERT_EQUAL(T5_OK, t5_event_post(T5_EV_I2C_DEVICE_FAULT, NULL, 0));
    TEST_ASSERT_NULL(s_data);
    TEST_ASSERT_EQUAL(0, s_len);
}

static void test_event_names(void)
{
    TEST_ASSERT_EQUAL_STRING("i2c.device_fault", t5_event_name(T5_EV_I2C_DEVICE_FAULT));
    TEST_ASSERT_EQUAL_STRING("unknown", t5_event_name((t5_event_id_t)12345));
}

static int s_takes, s_gives;
static t5_err_t s_take_result;
static t5_err_t prov_take(void) { s_takes++; return s_take_result; }
static void prov_give(void) { s_gives++; }

static void test_excl_claims(void)
{
    t5_excl_t *h = NULL, *h2 = NULL;
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_excl_claim(T5_EXCL_BT, &h));   /* no provider */
    const t5_excl_provider_t p = { prov_take, prov_give };
    t5_excl_set_provider(T5_EXCL_BT, &p);
    s_take_result = T5_ERR_BUSY;
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_excl_claim(T5_EXCL_BT, &h));          /* the service refused */
    TEST_ASSERT_FALSE(t5_excl_claimed(T5_EXCL_BT));
    s_take_result = T5_OK;
    TEST_ASSERT_EQUAL(T5_OK, t5_excl_claim(T5_EXCL_BT, &h));
    TEST_ASSERT_TRUE(t5_excl_holds(h, T5_EXCL_BT));
    TEST_ASSERT_FALSE(t5_excl_holds(h, T5_EXCL_WIFI));
    TEST_ASSERT_TRUE(t5_io_hold_get(T5_IO_EXCL_BT, NULL, 0));
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_excl_claim(T5_EXCL_BT, &h2));
    TEST_ASSERT_EQUAL(T5_OK, t5_excl_release(h));
    TEST_ASSERT_EQUAL(1, s_gives);
    TEST_ASSERT_FALSE(t5_io_hold_get(T5_IO_EXCL_BT, NULL, 0));
    TEST_ASSERT_EQUAL(T5_OK, t5_excl_claim(T5_EXCL_BT, &h2));                /* free again */
    TEST_ASSERT_EQUAL(T5_OK, t5_excl_release(h2));
    TEST_ASSERT_EQUAL(2, s_gives);
    TEST_ASSERT_EQUAL(3, s_takes);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_err_strings);
    RUN_TEST(test_event_subscribe_dispatch_unsubscribe);
    RUN_TEST(test_event_payload_limit);
    RUN_TEST(test_event_payload_is_a_padded_copy_with_its_length);
    RUN_TEST(test_event_names);
    RUN_TEST(test_excl_claims);
    return UNITY_END();
}
