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
    case T5_EV_POWER_STATE: return "power.state";
    case T5_EV_POWER_SUSPEND_OVERRUN: return "power.suspend_overrun";
    case T5_EV_POWER_SHUTDOWN: return "power.shutdown";
    case T5_EV_BATTERY_UPDATE: return "battery.update";
    case T5_EV_BATTERY_LOW: return "battery.low";
    case T5_EV_BATTERY_LOW_POWER: return "battery.low_power";
    case T5_EV_BATTERY_LOW_POWER_OFF: return "battery.low_power_off";
    case T5_EV_BATTERY_CRITICAL: return "battery.critical";
    case T5_EV_CHARGE_STATE: return "battery.charge_state";
    case T5_EV_USB_POWER: return "battery.usb_power";
    case T5_EV_BATTERY_SWAPPED: return "battery.swapped";
    case T5_EV_NET_C6_UP: return "net.c6_up";
    case T5_EV_NET_C6_DOWN: return "net.c6_down";
    case T5_EV_NET_SCAN_DONE: return "net.scan_done";
    case T5_EV_NET_CONNECTED: return "net.connected";
    case T5_EV_NET_GOT_IP: return "net.got_ip";
    case T5_EV_NET_DISCONNECTED: return "net.disconnected";
    case T5_EV_NET_WAKE: return "net.wake";
    case T5_EV_NET_C6_UPDATE: return "net.c6_update";
    case T5_EV_BLE_READY: return "ble.ready";
    case T5_EV_BLE_SCAN_DONE: return "ble.scan_done";
    case T5_EV_BLE_CONNECTED: return "ble.connected";
    case T5_EV_BLE_DISCONNECTED: return "ble.disconnected";
    case T5_EV_UI_ROTATE: return "ui.rotate";
    case T5_EV_UI_STALL: return "ui.stall";
    case T5_EV_APP_STARTED: return "app.started";
    case T5_EV_APP_STOPPED: return "app.stopped";
    case T5_EV_APP_CRASHED: return "app.crashed";
    case T5_EV_APP_INSTALLED: return "app.installed";
    case T5_EV_APP_REMOVED: return "app.removed";
    case T5_EV_PORT_CLAIMED: return "port.claimed";
    case T5_EV_PORT_RELEASED: return "port.released";
    }
    return "unknown";
}
