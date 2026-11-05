/* SPDX-License-Identifier: Apache-2.0
 * Host-test mocks: fake clock, synchronous events, simulated I2C chips,
 * and the reset hooks each service exposes for tests.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "t5_core/err.h"
#include "t5_core/event.h"
#include "t5_core/os.h"
#include "t5_i2c/t5_i2c.h"

/* ---- clock and tasks ---------------------------------------------------- */
void mock_time_set_us(int64_t us);
void mock_time_advance_ms(uint32_t ms);
typedef struct {
    const char *name;
    t5_task_fn_t fn;
    void *arg;
    void *ctx;
} mock_task_t;
/* Tasks are recorded, never run. */
int mock_tasks_count(void);
const mock_task_t *mock_task(int i);
void mock_tasks_reset(void);

/* ---- events ------------------------------------------------------------- */
#define MOCK_EV_MAX 64
int mock_events_count(t5_event_id_t id);
void mock_events_reset(void);

/* ---- simulated I2C ------------------------------------------------------ */
typedef struct mock_i2c_dev mock_i2c_dev_t;
/* Return true if handled. Default: bytes go to/come from regs[] with auto-increment. */
typedef bool (*mock_i2c_write_hook_t)(mock_i2c_dev_t *d, uint32_t reg, const uint8_t *data, size_t len);
typedef bool (*mock_i2c_read_hook_t)(mock_i2c_dev_t *d, uint32_t reg, uint8_t *out, size_t len);

struct mock_i2c_dev {
    uint8_t addr;
    bool present;
    bool reg16;                 /* 16-bit big-endian register address */
    uint8_t regs[65536];
    mock_i2c_write_hook_t on_write;
    mock_i2c_read_hook_t on_read;
    void *user;
    uint32_t writes, reads;
    /* fault injection */
    int fail_count;
    t5_err_t fail_err;
    /* write log: (reg << 8 | value) for single-byte writes, for ordering checks */
    uint32_t wlog[256];
    int wlog_n;
};

mock_i2c_dev_t *mock_i2c_add(uint8_t addr, bool reg16);
mock_i2c_dev_t *mock_i2c_get(uint8_t addr);
void mock_i2c_reset(void);                      /* remove all simulated chips */
void mock_i2c_fail_next(uint8_t addr, int count, t5_err_t err);
int mock_i2c_recover_count(void);

/* ---- service reset hooks (defined in firmware sources) ------------------ */
void t5_event_reset_for_test(void);
void t5_i2c_reset_for_test(void);
void t5_xpin_reset_for_test(void);
void t5_time_reset_for_test(void);
void t5_imu_reset_for_test(void);
void t5_storage_reset_for_test(void);
void t5_display_reset_for_test(void);

/* ---- other hardware stubs ----------------------------------------------- */
extern time_t mock_system_time;
/* storage */
typedef struct {
    t5_err_t mount_result;      /* what the next sd mount returns */
    bool alive;
    int mounts, unmounts;
} mock_sd_t;
extern mock_sd_t mock_sd;
/* display */
extern int mock_backlight;
extern bool mock_panel_asleep;

void mock_i2c_handles_reset(void);

/* Everything back to a clean state; call from setUp(). */
static inline void mock_reset_all(void)
{
    t5_event_reset_for_test();
    mock_events_reset();
    t5_i2c_reset_for_test();
    mock_i2c_reset();
    mock_i2c_handles_reset();
    mock_tasks_reset();
    mock_time_set_us(1000000);
    t5_event_init();
    t5_i2c_init();
}
