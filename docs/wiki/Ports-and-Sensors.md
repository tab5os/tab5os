# Ports and sensors

Apps address connectors by name, and the ports service knows the pins. Include `<t5/port.h>`. List each port in the manifest: `ports = ["grove"]`. Fragments on this page leave out error handling. The [recipes](Cookbook.md) don't.

## Ports

| Name | Connector | Modes | Notes |
| --- | --- | --- | --- |
| `grove` | HY2.0-4P | I2C, UART, GPIO, PWM, ADC | The usual choice |
| `mbus.spi` | M5-Bus SPI | SPI | Pins shared with `stamp` |
| `mbus.uart` | M5-Bus UART | UART | G37/G38: risky |
| `mbus.debug` | M5-Bus debug UART | UART | G6 shared with `stamp` |
| `mbus.i2c` | M5-Bus I2C | I2C | The internal system bus. Internal addresses refused, no pin claim |
| `mbus.g2`... | M5-Bus single pins | GPIO, PWM, ADC where available | |
| `ext` | 2.54 mm 10-pin | GPIO, UART, I2C, PWM, ADC | |
| `rs485` | HC1.25-6P | RS-485 half duplex | With Modbus |
| `stamp` | Stamp pads | SPI, UART, GPIO | Pins shared with `mbus.spi` |

Claims are per pin and exclusive, and everything is released when the app exits.

```c
t5_port_t *p = NULL;
t5_port_i2c_cfg_t cfg = { .hz = 400000 };
char why[T5_PORT_WHY_LEN] = "";
t5_err_t e = t5_port_open_ex("grove", T5_MODE_I2C, &cfg, 0, &p, why, sizeof why);
if (e != T5_OK) md_snackbar_show(why[0] ? why : t5_err_str(e), NULL, NULL, NULL);
```

`t5_port_open_ex` gives a reason such as "G18 held by com.x.app (mbus.spi)", which is worth showing. `cfg` can be `NULL` for the defaults.

Port operations block, so do bus work on a task. A short I2C read from the UI task is tolerable if it's rare.

## Power for modules

**Opening a port doesn't power it.** The 5 V on Grove, the M5-Bus and the header is a separate switched rail:

```c
t5_rail_hold_t *rail;
if (t5_rail_acquire(T5_RAIL_EXT5V, "myapp", 0, &rail) == T5_OK) {
    t5_delay_ms(50);                     /* give the module time to start */
}
```

Rails are reference-counted and need the `ports` permission. In Sleep the OS drops rails unless you acquired with `T5_RAIL_KEEP_IN_SLEEP` or a `T5_LOCK_RAIL` lock is held, and restores them on wake. A sensor may need re-initialising after that.

Rails: `T5_RAIL_EXT5V`, `T5_RAIL_USB5V`, and others the OS mostly manages for itself.

## I2C

```c
uint8_t v[2];
t5_err_t e = t5_port_i2c_read_reg(p, 0x44, 0x00, v, 2);   /* T5_ERR_NACK: nothing there */
t5_port_i2c_write_reg(p, 0x44, 0x01, v, 1);
t5_port_i2c_xfer(p, 0x44, tx, txlen, rx, rxlen);          /* repeated start; either side may be empty */
uint8_t found[16];
int n = t5_port_i2c_scan(p, found, sizeof found);
```

Addresses are 7-bit, and the default clock is 100 kHz. On `mbus.i2c` the OS refuses the internal chips' addresses, which are 0x10, 0x14, 0x28, 0x32, 0x36, 0x40, 0x41, 0x43, 0x44, 0x55, 0x5D and 0x68. Grove is a separate bus and has no such restriction.

## UART and RS-485

```c
t5_port_uart_cfg_t cfg = { .baud = 115200, .rx_buf = 4096 };
t5_port_open("grove", T5_MODE_UART, &cfg, &p);
int n = t5_port_uart_read(p, buf, sizeof buf, 100);     /* bytes, or a negative t5_err_t */
t5_port_uart_write(p, data, len);
t5_port_uart_wait_tx(p, 100);                           /* before RS-485 turnaround */
```

The default receive ring is 1 KB. Raise `rx_buf` at high baud rates or for bursts. A reader that doesn't keep up loses data.

## Modbus RTU

```c
t5_modbus_t mb;
t5_modbus_init(&mb, bus);
uint16_t regs[2];
t5_err_t e = t5_modbus_read_holding(&mb, 1, 0x0000, 2, regs);
```

| Failure | Error |
| --- | --- |
| No reply | `T5_ERR_TIMEOUT` |
| Bad CRC, malformed | `T5_ERR_IO` |
| Exception 1 | `T5_ERR_NOT_SUPPORTED` |
| Exception 2, 3 | `T5_ERR_INVALID_ARG` |
| Exception 6 | `T5_ERR_BUSY` |

`mb.exception` holds the raw code. Slave 0 is broadcast, for writes only. Full recipe: [Modbus poller](Cookbook.md#modbus-poller). The RS-485 Console example is a complete tool.

## SPI, GPIO, PWM, ADC

```c
t5_port_spi_cfg_t spi = { .hz = 1000000, .mode = 0, .cs_pin_index = -1 };
t5_port_spi_transfer(p, tx, rx, n);                     /* tx or rx may be NULL */

t5_port_gpio_cfg_t gpio = { .pin_index = 0, .output = true };
t5_port_gpio_set(p, true);

t5_port_pwm_cfg_t pwm = { .pin_index = 0, .hz = 1000 };
t5_port_pwm_set(p, 500);                                /* permille: 500 is 50% */

t5_port_adc_cfg_t adc = { .pin_index = 0 };
int mv;
t5_port_adc_read_mv(p, &mv);                            /* calibrated, 0 to about 3300 mV */
```

GPIO, PWM and ADC claim one pin, chosen by `pin_index`. There's no GPIO interrupt API: poll from a task.

## Risky pins

G35 is the power key and input-only. G37 and G38 are strapping pins, and a module that pulls them at boot can stop the device booting. They need the `T5_PORT_FLAG_RISKY` flag, else `T5_ERR_DENIED`.

## Inspecting

The Port Inspector system app shows each pin's owner, level and mode. On the console:

```text
port                    owners and levels
port scan grove         I2C devices
rail                    rails and holders
```

## IMU

```c
t5_imu_accel_t a;
if (t5_imu_status() == T5_IMU_READY && t5_imu_read_accel(&a) == T5_OK) { /* a.x, a.y, a.z in milli-g */ }
```

Include `<t5/sys.h>`. Axes are the chip's, so they don't follow screen rotation. The OS uses this sensor for auto-rotate. Poll it from a task or a timer at the rate you need.

## Time

| Call | Returns |
| --- | --- |
| `t5_time_now()`, `time(NULL)` | UTC seconds |
| `t5_time_valid()` | Whether the wall clock is trustworthy |
| `t5_time_local(&tm)` | Local time, in the zone from Settings |
| `t5_time_us()` | Monotonic microseconds, for intervals |
| `t5_delay_ms(ms)` | Sleep this task |

`gettimeofday`, `localtime_r`, `gmtime_r`, `mktime` and `strftime` work too. Don't use the wall clock for intervals: NTP can move it.

## Bluetooth

The `ble` permission group exists, but the C app ABI doesn't publish a Bluetooth API at API level 1. Check [the API reference](../api-reference.md) before you plan around it.
