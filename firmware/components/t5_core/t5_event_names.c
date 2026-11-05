/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_core/event.h"

const char *t5_event_name(t5_event_id_t id)
{
    switch (id) {
    case T5_EV_ANY: return "any";
    case T5_EV_I2C_DEVICE_FAULT: return "i2c.device_fault";
    case T5_EV_I2C_DEVICE_OK: return "i2c.device_ok";
    case T5_EV_I2C_BUS_RECOVERED: return "i2c.bus_recovered";
    case T5_EV_SD_INSERTED: return "sd.inserted";
    case T5_EV_SD_REMOVED: return "sd.removed";
    case T5_EV_SD_MOUNT_FAILED: return "sd.mount_failed";
    case T5_EV_TIME_SET: return "time.set";
    case T5_EV_RTC_ALARM: return "rtc.alarm";
    case T5_EV_IMU_FAILED: return "imu.failed";
    case T5_EV_IMU_MOTION: return "imu.motion";
    case T5_EV_DISPLAY_READY: return "display.ready";
    case T5_EV_TOUCH_FAILED: return "touch.failed";
    }
    return "unknown";
}
