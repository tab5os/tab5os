/* SPDX-License-Identifier: Apache-2.0 */
#include "unity.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_mocks.h"
#include "t5_pins/pins.h"

void setUp(void) { mock_reset_all(); }
void tearDown(void) {}

static void test_register_read_write(void)
{
    mock_i2c_dev_t *m = mock_i2c_add(0x50, false);
    t5_i2c_dev_t *d;
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_open(0x50, 400000, "x", &d));
    const uint8_t w[3] = { 1, 2, 3 };
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_write(d, 0x10, w, 3));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(w, &m->regs[0x10], 3);
    uint8_t r[3];
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_read(d, 0x10, r, 3));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(w, r, 3);
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_update_u8(d, 0x11, 0xF0, 0xA5));
    TEST_ASSERT_EQUAL_HEX8(0xA2, m->regs[0x11]);
}

static void test_16bit_registers(void)
{
    mock_i2c_dev_t *m = mock_i2c_add(0x55, true);
    m->regs[0x1234] = 0x77;
    t5_i2c_dev_t *d;
    t5_i2c_open(0x55, 400000, "t", &d);
    uint8_t v = 0;
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_read16(d, 0x1234, &v, 1));
    TEST_ASSERT_EQUAL_HEX8(0x77, v);
}

static void test_reserved_addresses(void)
{
    TEST_ASSERT_TRUE(t5_i2c_addr_reserved(T5_I2C_ADDR_INA226));
    TEST_ASSERT_TRUE(t5_i2c_addr_reserved(T5_I2C_ADDR_SC2356));
    TEST_ASSERT_TRUE(t5_i2c_addr_reserved(T5_I2C_ADDR_UNKNOWN28));
    TEST_ASSERT_FALSE(t5_i2c_addr_reserved(0x3C));
}

static void test_bad_args(void)
{
    t5_i2c_dev_t *d;
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_i2c_open(0x03, 0, "x", &d));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_i2c_open(0x78, 0, "x", &d));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_i2c_xfer(NULL, NULL, 0, NULL, 0));
}

static void test_timeout_triggers_recovery_and_retry(void)
{
    mock_i2c_dev_t *m = mock_i2c_add(0x50, false);
    m->regs[0] = 0x42;
    t5_i2c_dev_t *d;
    t5_i2c_open(0x50, 0, "x", &d);
    mock_i2c_fail_next(0x50, 1, T5_ERR_TIMEOUT);
    uint8_t v = 0;
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_read_u8(d, 0, &v));
    TEST_ASSERT_EQUAL_HEX8(0x42, v);
    TEST_ASSERT_EQUAL(1, mock_i2c_recover_count());
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_I2C_BUS_RECOVERED));
}

static void test_nack_does_not_recover(void)
{
    mock_i2c_add(0x50, false);
    t5_i2c_dev_t *d;
    t5_i2c_open(0x50, 0, "x", &d);
    mock_i2c_fail_next(0x50, 1, T5_ERR_NACK);
    uint8_t v;
    TEST_ASSERT_EQUAL(T5_ERR_NACK, t5_i2c_read_u8(d, 0, &v));
    TEST_ASSERT_EQUAL(0, mock_i2c_recover_count());
}

static int s_recover_cb_hits;
static void on_recover(void *arg) { (void)arg; s_recover_cb_hits++; }

static void test_recover_callbacks(void)
{
    s_recover_cb_hits = 0;
    t5_i2c_on_recover(on_recover, NULL);
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_recover());
    TEST_ASSERT_EQUAL(1, s_recover_cb_hits);
}

static void test_fault_isolation_and_recovery(void)
{
    mock_i2c_add(0x50, false);
    t5_i2c_dev_t *d;
    t5_i2c_open(0x50, 0, "flaky", &d);
    uint8_t v;
    mock_i2c_fail_next(0x50, 1000, T5_ERR_NACK);
    for (int i = 0; i < T5_I2C_FAULT_THRESHOLD; i++) {
        TEST_ASSERT_EQUAL(T5_ERR_NACK, t5_i2c_read_u8(d, 0, &v));
    }
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_I2C_DEVICE_FAULT));
    /* Faulted: fails fast without touching the bus. */
    uint32_t before = mock_i2c_get(0x50)->fail_count;
    TEST_ASSERT_EQUAL(T5_ERR_IO, t5_i2c_read_u8(d, 0, &v));
    TEST_ASSERT_EQUAL(before, (uint32_t)mock_i2c_get(0x50)->fail_count);
    /* After the retry interval one real attempt goes through; success clears the fault. */
    mock_i2c_get(0x50)->fail_count = 0;
    mock_time_advance_ms(T5_I2C_FAULT_RETRY_MS);
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_read_u8(d, 0, &v));
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_I2C_DEVICE_OK));
    t5_i2c_dev_stats_t st[4];
    TEST_ASSERT_EQUAL(1, t5_i2c_dev_stats(st, 4));
    TEST_ASSERT_FALSE(st[0].faulted);
    TEST_ASSERT_EQUAL(T5_I2C_FAULT_THRESHOLD, st[0].nacks);
}

static void test_trace_ring_keeps_newest(void)
{
    mock_i2c_add(0x50, false);
    t5_i2c_dev_t *d;
    t5_i2c_open(0x50, 0, "x", &d);
    t5_i2c_trace_enable(true);
    for (int i = 0; i < T5_I2C_TRACE_LEN + 10; i++) {
        t5_i2c_write_u8(d, (uint8_t)i, 0);
    }
    static t5_i2c_trace_t t[T5_I2C_TRACE_LEN];
    int n = t5_i2c_trace_read(t, T5_I2C_TRACE_LEN);
    TEST_ASSERT_EQUAL(T5_I2C_TRACE_LEN, n);
    TEST_ASSERT_EQUAL_HEX8(10, t[0].first);                       /* oldest kept */
    TEST_ASSERT_EQUAL_HEX8((T5_I2C_TRACE_LEN + 9) & 0xFF, t[n - 1].first);
}

static void test_lock_nests_and_no_handoff_when_idle(void)
{
    mock_i2c_add(0x50, false);
    t5_i2c_dev_t *d;
    t5_i2c_open(0x50, 0, "x", &d);
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_lock(10));
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_lock(10));            /* recursive */
    uint8_t v;
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_read_u8(d, 0, &v));   /* nested lock inside */
    t5_i2c_unlock();
    t5_i2c_unlock();
    int64_t before = t5_time_us();
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_read_u8(d, 0, &v));
    TEST_ASSERT_EQUAL(0, t5_i2c_handoffs());              /* nobody waiting: no step-aside */
    TEST_ASSERT_EQUAL_INT64(before, t5_time_us());        /* and no delay */
}

static void test_probe(void)
{
    mock_i2c_add(0x50, false);
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_probe(0x50));
    TEST_ASSERT_EQUAL(T5_ERR_NACK, t5_i2c_probe(0x51));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_register_read_write);
    RUN_TEST(test_16bit_registers);
    RUN_TEST(test_reserved_addresses);
    RUN_TEST(test_bad_args);
    RUN_TEST(test_timeout_triggers_recovery_and_retry);
    RUN_TEST(test_nack_does_not_recover);
    RUN_TEST(test_recover_callbacks);
    RUN_TEST(test_fault_isolation_and_recovery);
    RUN_TEST(test_trace_ring_keeps_newest);
    RUN_TEST(test_lock_nests_and_no_handoff_when_idle);
    RUN_TEST(test_probe);
    return UNITY_END();
}
