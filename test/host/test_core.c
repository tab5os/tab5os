/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "unity.h"
#include "t5_mocks.h"

void setUp(void) { mock_reset_all(); }
void tearDown(void) {}

static int s_hits;
static t5_event_id_t s_last;
static uint8_t s_payload;

static void handler(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)len;
    s_hits += (int)(intptr_t)arg;
    s_last = id;
    if (data) s_payload = *(const uint8_t *)data;
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

static void dummy(void *a) { (void)a; }
static void *s_hook_ctx;
static void hook(t5_task_t t, void *ctx) { (void)t; s_hook_ctx = ctx; }

static void test_task_inherits_app_context(void)
{
    int app;
    t5_core_set_task_hook(hook);
    t5_app_ctx_set(&app);
    TEST_ASSERT_EQUAL(T5_OK, t5_task_create("x", dummy, NULL, 4096, 1, T5_CORE_ANY, NULL));
    TEST_ASSERT_EQUAL_PTR(&app, mock_task(0)->ctx);
    TEST_ASSERT_EQUAL_PTR(&app, s_hook_ctx);
    t5_app_ctx_set(NULL);
}

static void test_event_names(void)
{
    TEST_ASSERT_EQUAL_STRING("i2c.device_fault", t5_event_name(T5_EV_I2C_DEVICE_FAULT));
    TEST_ASSERT_EQUAL_STRING("unknown", t5_event_name((t5_event_id_t)12345));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_err_strings);
    RUN_TEST(test_event_subscribe_dispatch_unsubscribe);
    RUN_TEST(test_event_payload_limit);
    RUN_TEST(test_task_inherits_app_context);
    RUN_TEST(test_event_names);
    return UNITY_END();
}
