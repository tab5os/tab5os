# Bench results

Answers from real hardware. Each entry says which unit it came from and how it was measured.

## Unit A (MAC 80:f1:b2:d1:43:f4)

### 0.0 Chip revision — 2026-11-05

Measured with `esptool chip-id` and `flash-id` (esptool v5.6.0) over USB-C.

| Item | Result |
| --- | --- |
| Chip | ESP32-P4, **revision v1.3** |
| Build variant | **`rev1`** (`ESP32P4_SELECTS_REV_LESS_V3=y`, minimum Rev v1.0) |
| esptool features line | Dual Core + LP Core, 400MHz. That is the chip family's rating; plan for 360 MHz on v1.x until 0.2 measures it |
| Crystal | 40 MHz |
| USB on USB-C | USB-Serial/JTAG, port `/dev/cu.usbmodem1101` |
| Flash | 16 MB (manufacturer 0x46, device 0x4018) |

### Factory firmware backup

The full 16 MB flash is saved at `../../backups/tab5-factory-80f1b2d143f4.bin`, outside the repo.
SHA-256 `eee8d49d5b7a39ac0a8f00e438774d510fc1dcb8ffd1a5106f5eead19edd6da8`.
Restore with `esptool --port <port> write-flash 0 tab5-factory-80f1b2d143f4.bin`.

Factory partition table at 0x8000:

| Name | Type | Subtype | Offset | Size |
| --- | --- | --- | --- | --- |
| nvs | data | nvs | 0x9000 | 24K |
| otadata | data | ota | 0xf000 | 8K |
| phy_init | data | phy | 0x11000 | 4K |
| app0 | app | ota_0 | 0x20000 | 6M |
| app1 | app | ota_1 | 0x620000 | 6M |
| coredump | data | coredump | 0xc20000 | 64K |

About 3.8 MB at the end of flash is unused by the factory layout. The factory app slots are 6 MB, larger than our planned 4 MB, which is a hint about how big a full LVGL and media image gets. Check our image size against 4 MB early.

### 0.2 Minimal boot — 2026-11-05

Tab5 OS 0.0.1 (`rev1`, ESP-IDF v6.1) boots from `ota_0` with the console on USB-C. Measured with `sys` and `psram` in the bring-up console.

| Item | Result |
| --- | --- |
| CPU | 360 MHz. This is the ceiling: IDF v6.1 only offers 360 MHz when `ESP32P4_SELECTS_REV_LESS_V3` is set |
| PSRAM | 32 MB, stable at **200 MHz**. Pattern test over 24 MB found 0 bad words |
| PSRAM, CPU memset / memcpy | 200 MHz: 88 / 57 MB/s. 80 MHz: 45 / 37 MB/s. 200 MHz is about twice as fast, so we keep it. CPU copies go through the cache and are far below what DMA2D/PPA should reach. Measure that in Phase 4.2 |
| Internal heap after boot | 581 KB free, 352 KB largest block |
| PSRAM heap after boot | 33.5 MB free |

### Flash layout change

The partition table moved from 0x8000 to **0x10000**. At 0x8000 the IDF v6.1 bootloader (24.3 KB with rollback on) had only 208 bytes left, so the Phase 9 recovery hook and secure boot would not fit. At 0x10000 it has 32 KB free. NVS shrank to 188 KB (0x11000–0x40000). Every other offset matches the design. When flashing over the factory image, erase NVS (0x11000, 0x2F000) and the core dump (0xB50000, 0x40000) once, because the old factory app left bytes there. Both regions have been erased on Unit A.

### 0.3 I2C scan, bench check 1 — 2026-11-05

The system bus on G31/G32 at 400 kHz answers at 9 addresses:

| Address | Device |
| --- | --- |
| 0x10 | ES8388 audio DAC |
| **0x28** | **A second address of the ST7123 display and touch chip.** It ignores the register address (reads `0xfc` then `0x50` repeated). It disappears together with 0x55 when either `TP_RST` or `LCD_RST` is held low (see 0.4). Reserved |
| 0x32 | RX8130CE RTC |
| 0x40 | ES7210 mic ADC |
| 0x41 | INA226 battery monitor |
| 0x43, 0x44 | PI4IOE5V6408 expanders (ID register 0xA0) |
| 0x55 | ST7123/ST7121 touch |
| 0x68 | BMI270 IMU |

- **Bench check 1 (panel):** Unit A has the **ST712x** variant. 0x55 answers and GT911 (0x14/0x5D) doesn't. Telling ST7123 from ST7121 needs the panel ID over DSI (Phase 1.4).
- **Camera:** **0x36** answers as soon as `CAM_RST` (0x43 P6) is released (0.4). Confirmed, and reserved.
- **Panel ID:** ST7123 vs ST7121 is read from the touch controller's firmware-version register (16-bit address 0x0000): 3 = ST7123, 1 = ST7121. Unit A reads **3: ST7123**. The panel answers DCS ID `80 a0 fb` over DSI.

### 0.4 Expander state as left by the factory firmware — 2026-11-05

Read with `exp dump` after flashing. The expanders keep power across P4 resets, so this is the factory firmware's configuration, not the power-on default.

| Pin | Signal | State | Notes |
| --- | --- | --- | --- |
| 0x43 P0 | RF_ANT_SEL | output 0 | |
| 0x43 P1 | SPK_EN | output 0 | |
| 0x43 P2 | EXT5V_EN | input, pull-up, reads 1 | Enabled by the pull-up rather than driven |
| 0x43 P3 | — | output hi-z, pull-down | Unused |
| 0x43 P4 | LCD_RST | input, pull-up, reads 1 | Same as the BSP's "LCD enable" (open drain released) |
| 0x43 P5 | TP_RST | output 1 | |
| 0x43 P6 | CAM_RST | output hi-z, pull-down | Camera held in reset |
| 0x43 P7 | HP_DET | input, reads 0 | No headphones plugged in |
| 0x44 P0 | WLAN_PWR_EN | output 0 | C6 off |
| 0x44 P1–P2 | — | output hi-z, pull-down | Unused |
| 0x44 P3 | USB5V_EN | output 0 | |
| 0x44 P4 | PWROFF_PULSE? | output 0 | Not toggled. `exp out` refuses this pin without `--force` |
| 0x44 P5 | nCHG_QC_EN | output 0 | Fast charge enabled (active low) |
| 0x44 P6 | CHG_STAT | input, reads 1 | |
| 0x44 P7 | CHG_EN | output 1 | Charging enabled |

#### Toggling each output (`test/hil/expander_map.py`)

Each output was changed on its own while the script watched the I2C bus and the INA226.

| Change | Effect seen | Pin status |
| --- | --- | --- |
| `CAM_RST` = 1 | 0x36 (SC2356) appears | **verified** |
| `TP_RST` = 0 | 0x55 and 0x28 vanish | **verified** |
| `LCD_RST` = 0 (driven) | 0x55 and 0x28 vanish (ST7123 is one chip) | **verified** |
| `nCHG_QC_EN`, `CHG_EN` | No visible change: pack full (8.38 V), board on USB | names match schematic and M5Unified; effect unverified |
| `SPK_EN`, `EXT5V_EN`, `USB5V_EN`, `WLAN_PWR_EN`, `RF_ANT_SEL` | No change in battery current: on USB the load isn't on the battery path | names match the BSP and M5Unified; effect needs a meter or a load |
| `PWROFF_PLUSE` | Not toggled | 0x44 P4 per the schematic netlist (U7 pin 6 = P4, ADDR tied to 3.3 V) |
| `HP_DET`, `CHG_STAT` | Inputs; read 0 (no headphones) and 1 | not exercised |

M5Unified's Tab5 init table (`Power_Class.inl`) labels every pin the same way as `pins.h`.

**INA226 surprise.** With USB attached and the pack full, the INA226 bus voltage register steps between 0x0DA7 (4.37 V) and 0x1A32 (8.38 V) over a few seconds. The I2C data is consistent (no errors, stable values within each step), so the measured node really moves. Find out what the charger does here in Phase 2 before trusting the gauge on USB.


### 0.5 `PWROFF_PLUSE` — partly answered

The schematic netlist puts `PWROFF_PLUSE` on U7 pin 6 (P4); U7's ADDR pin is tied to 3.3 V, which makes it the 0x44 expander. So it's **0x44 P4**, as M5Unified also labels it. Pulsing it on battery to confirm it cuts power is still to do (procedure in `bench-procedures.md`). `exp out`/`exp pulse` refuse this pin without `--force`.

### 0.6 Power button, bench check 3 — 2026-11-05

Watched G35 and G23 with `gpio watch ... log` (edges written to `/data` with fsync) while a person pressed the button.

| Action | Result |
| --- | --- |
| Single short press while on | **Power-cycles the board**: off, then straight back on. Next boot reports `ESP_RST_WDT` |
| Double press while on | **Powers off and stays off** |
| Single press while off | Powers on |
| G35 during any press | No edge recorded before power went |

**Answer:** the PMS150G handles the button by itself. The OS gets no warning and can't intercept a press, so "short press toggles screen off or sleep" isn't possible. One caveat: if power drops within a millisecond or two of the press, the fsync'd log may not have survived. The practical result is the same either way. Autosave and atomic writes (2.12) are what protect data.

### 0.7 Interrupt routing, bench checks 2 and 9 — 2026-11-05

**RTC `/IRQ` does not reach the P4; it toggles main power.** Method: `rtc timer 3` arms the RX8130 countdown interrupt, then `gpio watch 8000 log` samples 21 candidate GPIOs (0-4, 14, 16, 17, 23, 33, 35, 45-54) every 1 ms and writes each edge to `/data` with `fsync`.

- When the timer fired, USB disappeared: the board lost main power. The edge log held only its header, so no watched GPIO changed before power went.
- The board came back on its own 10-50 s later. The next boot showed RTC flag TF set and reset reason `ESP_RST_WDT` ("other watchdog").
- Control run: the same `gpio watch` with no timer armed doesn't affect power.
- The RX8130 timer reloads itself, so a running timer keeps toggling power until boot stops it (`t5_rtc_init` does, about 1.2 s into boot).

Consequences, now in the design: RTC interrupts stay off while the OS runs; RTC alarm wake works only from soft off; the PMS150G power-up shows as `ESP_RST_WDT`.

**IMU interrupt (supervised test):** a latched any-motion interrupt on INT1+INT2, then a tap:

- Active-high push-pull: no power change, and no GPIO edge.
- **Active-low: the tap cut main power**, with no GPIO edge first. One button press brought the board back, and it stayed on; boot's IMU soft reset releases the latched line.

So the IMU interrupt joins the RTC's active-low trigger into the PMS150G and never reaches the P4. Motion wake works only from soft off, with INT configured active-low. While the OS runs, IMU interrupts must stay off or active-high.

**Bench check 9 (SD detect):** a person pulled the card and reinserted it while 21 GPIOs were watched. No GPIO moved, so no card-detect line reaches the P4. The storage poller saw the removal within 2 s and remounted after reinsertion. Polling is the only mechanism, and it works.

### 0.8 Light sleep, bench check 4 — 2026-11-05

Measured with `sleep light <ms> <MB> [g<pin>...]`: fill PSRAM with a pattern, enter light sleep with the DSI panel running, check the pattern and read the panel's power mode after waking.

| Run | Result |
| --- | --- |
| 3 s, timer wake, 8 MB pattern | Woke by timer at 3004 ms; PSRAM intact; panel answers, power mode 0x9C; USB console survived |
| 5 s, G23 (touch) + G14 (C6) armed, 16 MB | Woke by timer (no false GPIO wake); PSRAM intact; panel OK |
| 8 s, G23 + G35 + G14 armed (IDF default pad isolation) | **Board reset after a few seconds** (`ESP_RST_WDT`, ~10 s outage), no wake reported |
| 4 s, G35 armed with `gpio_sleep_sel_dis(35)` | Woke by timer; PSRAM intact; panel OK; no reset |

| 30 s, G23 armed, person touching during sleep (first attempt) | **Board hung**, screen dark, USB dead; needed a button press. Cause: `gpio_wakeup_enable()` turns G23's interrupt into a level one, and the touch service's ISR then fired non-stop while the finger was down. Fixed: mask the pin's ISR during sleep, restore its falling-edge interrupt afterwards |
| 30 s, G23 armed, person touching (after the fix) | Survived, but **woke by timer, not touch**. Screen held grey; queued touches drew once video resumed. Touch works normally afterwards |

**Answer:** light sleep keeps PSRAM and the DSI panel on this revision, and wakes cleanly from the timer. Two board rules: G35 must keep its pad config through sleep (`t5_bsp_board_init()` does this), and a pin with a live ISR must have it masked while armed as a wake source.

**Touch can't wake the board from light sleep.** The ST7123 scans touch during display blanking (see the display findings), and the video stream stops in light sleep, so the touch interrupt never fires. The button can't serve as a gentle wake either (0.6). **Phase 2 decision (plan's decision rule):** the Sleep state becomes "screen off, video stream kept alive, CPU idling at low clock". Deep standby is soft off, which wakes with a reboot and a session restore.

### 0.9 Baseline power draw — not yet measured

Needs an external meter in series with the pack (procedure in `bench-procedures.md`).

### 0.10 ELF from PSRAM, bench check 11 — 2026-11-05

`elf bench`: a 61 KB position-independent ELF (built by the firmware build with the same compiler flags) is written to `/sd`, read back, loaded by `elf_loader` 2.1.0 into PSRAM (entry 0x481e2526) and run. The same two kernels are compiled into the firmware, which runs from flash via cache.

| Kernel | Built in (flash, 80 MHz QIO) | ELF in PSRAM (200 MHz) |
| --- | --- | --- |
| Small: 16x16 matrix loop, fits in cache | 199.97 ms | 200.05 ms (1.00x) |
| Big: 64 KB straight-line code, misses I-cache | 17.19 ms | 15.46 ms (0.90x, faster) |

300 load-run-unload rounds: 0 relocation failures, 0 wrong results.

**Answer:** ELF code runs reliably from PSRAM on v1.3, at built-in speed or better. Native apps stay the main runtime, and the "internal RAM region" fallback isn't needed.

### 0.11 Connector pins — not yet measured

Needs a meter on `ext`, `stamp` and the M5-Bus BAT pin.

## Phase 1 results — 2026-11-05

| Service | Result on Unit A |
| --- | --- |
| `t5_i2c` | 9 devices at 400 kHz. Device test: 20 forced recoveries in a row, every chip still answering. **Stress** (5 tasks on both cores reading fixed ID registers at ~5,600 reads/s): stopped at 2,870 s of the planned hour with 16.2 million good reads, **0 wrong data, 0 recoveries**. A 2-minute rerun gave 604,216 transfers with **0 bus errors**; every reported "error" was a **lock timeout** (44, spread across all tasks) |
| Bus lock fairness | Under 100% bus load a task could wait over 1 s for the lock: the releasing task on one core re-took the FreeRTOS mutex before a waiter on the other core ran. **Fixed** (`e7e1c58`): a task releasing the bus while others wait steps aside for one tick. 10-minute stress after the fix: **3,080,792 reads, 0 errors, 0 wrong data, 0 recoveries, 0 lock timeouts**, and even shares (600k-647k per task); all 9 devices answer afterwards. Throughput ~5,100 reads/s (was ~5,600) |
| Expanders | State adopted at boot without writes; boot defaults applied (rails off, camera in reset, charging on) |
| Display | ST7123 detected; DSI 2 lanes at 1000 Mbps, 70 MHz pixel clock; test pattern confirmed on screen; DCS read works; sleep and wake |
| Display shimmer | With Espressif's BSP timing (70 MHz, vertical front porch 220, **57.8 Hz**) the whole screen shimmered, worst on flat mid-grey, at any brightness including constant-on backlight. So not PWM; it's LCD frame-rate flicker. Shortening the porch to 40 lines (65.6 Hz) fixed the shimmer but **stopped touch**: the ST7123 scans touch during vertical blanking, and the touch interrupt never fired. Final setting: keep the 220-line porch and raise the pixel clock to **78 MHz (64.4 Hz)**. Steady, and touch works. `disp timing <pclk> <vfp>` tries other timings without rebuilding |
| Backlight PWM | 20 kHz needed a fractional LEDC divider (80 MHz / 3.9), which jitters. Now 19531 Hz (80 MHz / 1024 / 4, integer) and true constant-on at 100% |
| Touch mapping | The ST7123 init table's MADCTL (0x36 = 0x03) flips the image in X and Y but not touch: a touch at top right drew at bottom left. Touch is now mirrored in both axes for ST7123; dots confirmed under the finger at all corners |
| Touch | ST7123 firmware 3, 720x1280, interrupt on G23 |
| RTC | Time set from the host survives a chip reset, and a real power-off by double press (within 1 s of the host afterwards) |
| IMU | BMI270 config upload succeeds on the first attempt; 1 g at rest (-995 mg on Z, flat); die temperature 37 °C |
| `/data` | LittleFS, 4.4 MB. Atomic-write stress interrupted by 23 chip resets mid-write: every read-back intact, sequence never went backwards |
| `/sd` | 32 GB card mounts 4-bit at 40 MHz. Raw reads reach 14.6 MB/s with 64 KB transfers, but this card is FAT32 with **512-byte clusters**, so file I/O through FatFs is ~1.2 MB/s read, 0.4 MB/s write. Reformat with 32 KB clusters; Phase 6's Files app should format that way |

### Problems found

- **eFuse boot loop (IDF v6.1, v1.3 silicon).** After one of many quick USB resets during flash writes, every boot asserted in IDF's PVT init (`get_lp_hp_gap pmu_pvt.c:47`). The real eFuse value (`LP_DCDC_DBIAS_VOL_GAP` = 4, from `espefuse summary`) passes the check, so the app was reading stale eFuse shadow registers. The panic reboot is a CPU-only reset, which doesn't reload eFuses, so the board looped until a reset through the ROM bootloader (`esptool chip-id`) fixed it. A real power loss reloads eFuses, so users wouldn't hit it this way, but the crash guard can't catch it (it fires before `app_main`). Phase 9: add a bootloader check that forces a full system reset if those registers look invalid.
- **Real power-cut test stopped after 1 cycle.** `test/hil/fs_powerloss.py --mode powercut` cuts power through the RTC interrupt. The first cut and restore worked (data intact); after the second cut the board stayed off and needs the power button. The PMS150G's behaviour on repeated pulses isn't understood yet (see 0.6/0.7).

## Phase 2 results — 2026-11-05

Unit A on USB (pack full, 8.38 V). Battery-only measurements are still to do (see "Still to answer").

| Item | Result |
| --- | --- |
| State machine | Active → Dim → Screen off → Sleep and back, on the board, with the handheld timers and with 5/10/5 s test timers. Touch wakes from Sleep because the panel keeps its video (Screen off and Sleep only turn the backlight off) |
| Sleep and wake cycles (2.13) | `test/hil/power_cycle.py`, 5 minutes: **179 cycles, 179 resumes, 0 failures**; the panel answered its DCS ID and power mode 0x9C after every wake. The C6 was powered off and on in each cycle. The plan's 1,000 cycles weren't run (test runs are kept to 5 minutes); about 30 minutes reaches it |
| Wake to first frame | 0–1 ms from the wake trigger to backlight on. The panel never stops receiving video, so the first frame is the one already on screen. The 300 ms target holds trivially |
| Suspend sequence | 89 ms into Sleep with the C6 powered off; 0–1 ms resume (the C6 restarts in the background) |
| **CPU clock in Sleep** | **Stays at 360 MHz.** With `CONFIG_PM_ENABLE`, `esp_pm_dump_locks` shows IDF's DSI DPI driver holding a `CPU_FREQ_MAX` lock for as long as the panel runs, and Sleep must keep the panel running for touch. DFS could never lower the clock, so power management is off again; idle cores still wait in WFI. Sleep saves power by suspending services, dropping rails and powering the C6 off |
| INA226 | IDs 0x5449/0x2260, configured for 16× averaging and 1.1 ms conversions. On USB with the full pack: 8386–8388 mV, −0.5 to 0 mA. The bus-voltage anomaly from 0.4 shows up constantly (10 of the first 12 samples read ~4.4 V); the gauge drops those samples as implausible |
| **Charge temperature proxy** | The BMI270 reads **42–43 °C** idle on USB at room temperature, and the P4 die 39 °C. The design's 40 °C pause would stop charging almost all the time (it did, on the first boot). The pause is now 50 °C on the IMU, resume at 47 °C |
| Rails | `rail on usb5v` / `rail off` switch EXP1 P3 and read back; refcounting and owners shown by `rail` |
| Wake reason | Reads the RTC flags and the shutdown record in RX8130 RAM (0x20–0x23). RTS resets classify as `reboot`, the panic below as `crash` |
| Autosave | `/data/sys/power.bin`, `battery.bin`, `charge.bin` and `session.bin`, each with a CRC header, written atomically every 30 s and on each state change |

### Problems found

- **NimBLE stopped mid-start.** Sleep powered the C6 off while NimBLE was still in its start-up sync, and the host task then used freed timers (panic in `ble_hs_timer_sched`, from the core dump). `t5_ble` now waits up to 3 s for the sync before stopping.
- **Console commands don't count as input.** A bench session drifts into Sleep after the normal timers. Scripts hold `power lock screen`.

## Phase 3 results — 2026-11-05

| Item | Result |
| --- | --- |
| C6 firmware | The factory C6 runs **esp_hosted 1.4.1** (capabilities: Wi-Fi and BLE over SDIO). It works with the esp_hosted 3.1.0 host as is, so no C6 update was needed to start |
| SDIO link | 4-bit, 40 MHz on CLK 12, CMD 13, D0–D3 11/10/9/8, reset G15 (esp_hosted's own Tab5 board option agrees with M5Unified). Shares the SDMMC controller with the SD card (slot 0): both drivers tear down only their own slot, and `/sd` keeps working |
| C6 power-up to link up | **1.5 s** (2.2 s before the reset settle time was cut from 1500 to 800 ms) |
| Scan | 16 access points |
| Connect | First join 4.4–7.5 s to an address (WPA2/3, −40 dBm) |
| SNTP | Time set from `pool.ntp.org` about 2 s after the address; written to the RTC |
| **C6 reset to connected** | 4.8, 4.8, 4.9, 6.3, 4.8 s, and in `test/hil/net_check.py` 7.8, 4.8, 4.8 s: **usually under the 5 s target, but not always** (2 of 8 over). The 5 s target needed: rejoining the last network directly by channel and BSSID (no scan), restoring the last DHCP lease, no ARP probe on the offered address, and ignoring the C6's stale disconnect at boot. Was 10–12 s |
| **Sleep with a TCP connection open** | `net echo 7777` (holds a net lock), device forced into Sleep, packets from a Mac on the LAN: every reply tagged `[sleep]`, **8 ms** for the first, under 1 ms after. Target 1 s |
| Sleep without a net lock | The C6 is powered off (`WLAN_PWR_EN` low, 89 ms suspend) and comes back on wake |
| BLE | NimBLE on the P4, controller on the C6 over hosted HCI. The 1.4.1 C6 firmware doesn't answer the controller init RPC (it enables its controller at boot), so `t5_ble` skips that step for C6 firmware before 2.x. Scan: 32 devices. Connect and GATT discovery to a nearby phone: 16 characteristics |
| C6 firmware build (3.1) | `c6/` builds esp_hosted 3.1.0 for the C6 with the Tab5 board option, BLE, host wake on IO2 and rollback: **1.46 MB**, 95% of the 1.5 MB `c6fw` partition. Not yet installed on Unit A (see below) |
| Device tests | `t5 device-test`: 18 tests, 0 failures (13 Phase 1, plus INA226, rails, RTC RAM, wake reason, C6 link and scan) |
| Image size | `tab5os.bin` 1.22 MB (rev1) and 1.23 MB (rev3), with Wi-Fi, lwIP and NimBLE, against a 3.6 MB budget |

## Phase 4 results — 2026-11-05

First boot of the LVGL pipeline on Unit A (`rev1`): the launcher draws in landscape (1280 × 720) through the PPA into three framebuffers. Image size 3.05 MB (rev1) and 3.06 MB (rev3) against a 3.6 MB budget, after the baked shadows went to A8 with fewer variants (from 5.1 MB).

| Check | Result |
| --- | --- |
| Rendering gate, scroll, landscape (`ui gate scroll 5`) | **FAIL: 29–35 fps** (target 57). LVGL render 26.6 ms per frame, PPA rotate and copy 17.1 ms per frame (about 23 MB/s, far below what the PPA should do) |
| Transition and dialog modes, portrait | Not run yet |
| Rotation in four directions, touch mapping, IMU axes | Not checked yet (needs eyes on the screen) |

**Next:** the copy path first (blocking per-rectangle PPA calls, burst length, cache handling), then rendering (draw with two cores, PPA fills). The plan's rule stands: no Phase 6 until the gate passes.

## Phase 5 results — 2026-11-05

Test apps built with the SDK (`t5 apps`), downloaded and installed with `app fetch` over Wi-Fi, which runs the install-time ABI check on each.

| Exit criterion | Result |
| --- | --- |
| Leaky app opened and closed 500 times, heap, claims and locks back to baseline every time | **PASS**: `app stress com.t5.test.leaky 500`, 218 s, 0 cycles off baseline, worst free-heap drift 900 bytes (threshold 4 KB). Each cycle Leaky leaks 92 KB in 34 blocks, claims grove (I2C), holds ext5v and a CPU wake lock, starts two tasks (one that never stops), two LVGL timers, an animation, a top-layer dialog, an event subscription, an open file, a mutex and a semaphore; exit releases 12 handles and the heap |
| A deliberately crashing app triggers the crash guard | **PASS**: two crashes in a row, each attributed to the app from RTC memory, core dump saved to `/data/sys/dumps/com.t5.test.crasher.elf` (20 KB), boot back to the launcher, then "auto-launch blocked" |
| An app built against API 1 still loads after unrelated firmware changes | **PASS**: apps installed on the first Phase 5 image kept loading across five later rebuilds (console, UI and runtime changes) without reinstalling |
| UI watchdog (5.12) | **Works**: Hang showed "App not responding" after 2.1 s; Wait hid it and it came back 5 s later; Close app restarted to the launcher and recorded "not responding" (exercised from the touch screen by hand) |

### Problems found and fixed

- **A use-after-free in `t5_ui_sync()`.** On a timeout the queued call still ran later, with the caller's argument (often on its stack). It now cancels the queued call under the UI lock. This is the likely cause of a `realloc() pointer is outside heap areas` assert seen once during an early stress run; the crash guard recorded it against Leaky (`/data/sys/dumps/com.t5.test.leaky.elf`), and it hasn't recurred in 730 cycles since.
- **Session restore relaunched an app that had just hung or crashed.** After a watchdog restart the device reopened Hang and hung again, until the two-in-a-row block. An app that was running when the device reset is now never restored on the next boot.
- **A crash showed blue, then a test pattern, then flashing cyan before the launcher** (seen by hand). The colours are the panel underrunning: the panic handler writes the core dump to flash, and at boot the crash guard copied and erased it, both while the display DMA was running. Now the panic handler turns the backlight off first (`--wrap=esp_panic_handler`, plus a shutdown handler for `esp_restart`), the crash check runs before the display starts, the framebuffers start black, and the backlight stays off until the launcher's first frame. The Phase 1 boot test pattern is gone.
- **Launching an app with the screen off timed out.** The UI doesn't run in Screen off and Sleep; launching now counts as user activity and wakes it. `app stress` holds a screen wake lock for the same reason.

## Phases 6 and 7 — 2026-11-06

Unit A (`rev1`), driven from the console. Screens were captured with `t5 shot` (the scanned-out framebuffer, base64 over USB) since nobody was watching the panel; the simulator renders the same shell code (`t5 sim --shell`, five new screenshot goldens).

### Shell and system apps (Phase 6)

| Check | Result |
| --- | --- |
| Launcher, status bar, quick settings, drawer, recents | Draw on the device and in the simulator, landscape and portrait. Time, battery, Wi-Fi and the mic, camera, USB and keyboard indicators come from the services once a second; an unchanged status bar redraws nothing |
| System apps | Settings (root, Wi-Fi with a live scan, Display, Sound, USB, About), Files, App Manager (list and one app), Power, Port Inspector and Camera open and draw on Unit A. Settings' Bluetooth, Power, Time, Updates and Developer pages weren't captured |
| Phase 5 stress with the shell | `app stress com.t5.test.leaky 100`: 0 cycles off baseline, worst heap drift 0 bytes |
| Permission prompt | Shown on first use of the camera by an installed app (UVC Monitor); closes with its app if the app exits first |
| Touch | Someone took photos with the Camera app's shutter during the tests, so touch reaches the system apps; gestures, the on-screen keyboard and long presses haven't been tried by hand yet |
| Per-app power | Accounting runs every 5 s, on battery only (on USB the pack current says nothing about the load); no figures yet, as the unit stayed on USB |

### Media and USB (Phase 7)

| Check | Result |
| --- | --- |
| Codecs | ES8388 and ES7210 found through the I2C broker; both stay powered down (by register) until used |
| Playback with the screen off (exit: 1 h, no underruns) | `audio play 240` after `power screenoff`: **240 s, 0 underruns**, worst mixer loop 382 µs, the audio wake lock kept the device in Screen off (no Sleep). The full hour is left for a longer run |
| Microphones | Both mics, 48 kHz stereo; ambient −43 to −54 dBFS at 30 dB gain. Recording delivers the full byte count (96,640 bytes for 3 s at 16 kHz mono) |
| Echo cancellation | `audio echo 5` (noise from the speaker while recording at 16 kHz): echo −36.7 dBFS RMS without, about −54 dBFS with once converged, **ERLE 17.5 dB**. The first second is worse than no canceller while it converges |
| Camera | SC202CS (SC2356) detected at 0x36; 1280x720 YUV420 at 29–31 fps, 0 dropped |
| Photo | JPEG 1280x720, about 135 KB, also from the Camera app's shutter |
| Recording (exit: 10 min of 1080p, no drops) | `cam rec` **270 s: 7,889 frames (29.2 fps), 0 dropped, 67 MB at 6 Mbit/s target**, write buffer peak 65 KB of 2 MB. 1280x720: the sensor has no 1080p mode (its 2 MP modes are 1600x1200 and 1600x900). One encode call took 1.6 s once without a dropped frame; to watch |
| Preview | **11 fps**, capture stays at 30. The preview is an LVGL image: render 59 ms and copy 39 ms a frame for 998x562, the same slow full-frame path that fails the Phase 4 gate (full screen was 8 fps). A video plane written by the PPA straight into the panel framebuffer would bypass both |
| USB-A host | Installed on the high-speed controller with hub support, USB 5V held as a rail. **USB drive: works** (SanDisk 3.2Gen1, 0781:5591, high speed, FAT mounted at `/usb` and listed in Files; formatted from Files, by hand, in about 15 s). Keyboard, mouse, serial, MIDI and UVC not tried yet |
| USB-C mass storage (bench check 6) | **Works, until the computer ejects it**: Settings > USB > "SD card to a computer" (or `usbc storage`) hands the card over with no timer; `diskutil eject` on the Mac brings it back in about 1 s (`/sd` remounted with its files, console back). Pulling the cable without ejecting keeps the card reserved for the computer (not tried by hand); "Take it back" in Settings ends it early, with a warning |
| UVC Monitor | Builds with the SDK, installs over Wi-Fi with the ABI check, starts and waits for a device |

### Problems found and fixed

- **Camera, JPEG and H.264 setup failed from the UI task.** The CSI controller's DMA, the JPEG engines and the encoder allocate their interrupts on the calling core, and core 1 (the UI task's) has none left ("alloc interrupt failed"). Camera setup, encoder setup and teardown, stills and JPEG decoding now run on tasks pinned to core 0.
- **`esp_video` can't be brought up twice.** `esp_video_deinit()` leaves the CSI interrupt allocated. It stays initialised after the first open; a closed camera is streamed off (sensor standby) rather than held in reset.
- **H.264 needs 92 KB of contiguous internal RAM** for its reference frame, which a running system no longer has in one piece. The camera service reserves it at boot and lends it to the encoder while recording (since 2026-11-06 the UI's band block serves as that reserve; see the rendering section).
- **Recording from the mics stalled after 0.3 s, and opening them crashed** (a double free in the heap). In full-duplex I2S the receive side runs on the transmit side's clock, and `esp_codec_dev` reconfigures the pair on every open and close. Output now keeps running (silent, amp off) while a mic is open, and all codec opens and closes happen on the output task between its writes.
- **The console died after every hand-back** once the eject code went in, though the chip kept running (an RTC breadcrumb and heartbeat showed it): USB-Serial/JTAG only answers again if something is written to it right after it gets its PHY back. The two log lines around the PHY swap do that and are now commented as required. Remounting `/sd` on the esp_timer task also hung; the hand-back runs on its own task.
- **USB-C mass storage didn't enumerate**: the PHY swap ran after TinyUSB had set up its PHY, so its pads and pull-up were on the other one. It now swaps first.
- **Menus were clipped** at the right edge: their rows were a percentage of a content-sized menu. Rows are content-sized now, then widened to the widest.
- **USB devices failed every root-port reset** when the host came up after boot: the stack powered the root port before USB 5V, so the device powered up mid-reset. 5V now comes first (200 ms), and turning the host off and on power-cycles the root port.
- **The end of a stream counted as an underrun.** An underrun is now counted only when data comes back after running dry.
- **The echo canceller barely adapted.** The mics hear the speaker louder than the digital reference, so a 0.5 Geigel threshold called everything double talk; the threshold is now 4, and the step is regularised while the window fills.
- **A permission prompt outlived its app** (it belongs to the system), so answering it touched freed memory. Prompts now close with their app.
- **MD2 switch handle drawn inside the track** (since Phase 4): LVGL grows the knob by its padding, and the theme's was negative. **Checkbox check mark missing**: it is `LV_SYMBOL_OK`, which Roboto lacks.
- **Image size**: 3.89 MB with Phase 7, over CI's 3.77 MB (90% of the slot). The build is now size-first (`-Os`) with `-O2` kept for LVGL, MD2, the display pipeline and audio: 3.72 MB.
- **Internal RAM**: 96 KB free with everything running. The new long-lived service tasks (audio, USB, JPEG decoding) now have PSRAM stacks: 137 KB free at boot, 256 KB with the camera closed after a recording.

### Not solved

- ~~**Wi-Fi drops data after a while**~~ Fixed 2026-11-07: an esp_hosted host/C6 firmware mismatch; see "Phases 6 and 7 follow-up".
- **One unexplained panic** during a 270 s recording with the previous (`-O2`) image; its core dump didn't match the next build, so it couldn't be decoded. The same run on the current image completed cleanly.

## Rendering pipeline, 60 fps work — 2026-11-06

Goal: the Phase 4 gate (57 fps scrolling). **Scrolling now renders at 59–60 fps; the gate still reads 55 fps on average** (below). It was 29–32 fps at the start. Measured on Unit A (rev1, 360 MHz), landscape.

What changed:
- LVGL renders bands (partial mode) instead of a full-frame canvas. Each band is rotated into the back framebuffer by an async PPA copy while the next band renders. At frame start, the PPA copies whatever the back buffer is missing from the newest frame, minus what this frame redraws anyway (`t5_fbt_subtract`). The frame is presented on vsync once the PPA is idle.
- **The bands are in internal RAM**: two 20-line bands, one 100 KB block. CPU writes there don't pull PSRAM lines into the cache before writing, need no cache write-back, and the PPA reads them off the PSRAM bus. This is the main gain (37 to 55 fps). tab5emu found the same for its blit: about 100 MB/s for cached PSRAM writes.
- **Espressif's PPA hang fix** (`firmware/components/esp_driver_ppa`, an override of IDF's driver; see its README.t5.md). LVGL's PPA fills now run beside the band copies (30 to 37 fps). The CPU-fill workaround is gone.
- Code and fonts run from PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM`, +3 fps).
- The H.264 encoder's 92 KB boot reservation is gone. The encoder borrows the band block while recording: `t5_ui` wraps `esp_h264_aligned_calloc` to hand it out and `heap_caps_free` to take it back. Meanwhile the UI draws in 40-line PSRAM bands.

| Check | Result |
| --- | --- |
| Gate, scroll (`ui gate scroll`) | **avg 55 fps, 59–60 in most seconds**; render 15 ms a frame (max 16.7), loop max 17 ms. The gate's eased animation reverses every 4 s and barely moves near each end, so no frame is drawn then; those seconds read 42–44 fps. Every frame that was drawn made the 64 Hz vsync |
| Gate, transition | 9–10 fps (was 6–7): both screens redraw in full every frame |
| Gate, dialog | 6 fps (was 5): the scrim fade redraws the whole screen every frame |
| Camera preview | 13 fps (was 10) |
| Recording | 3 x 3 s records: block lent and returned each time, 0 dropped frames |
| Stress | 3 rounds of scroll, transition, dialog and camera open/close: no hangs, heap integrity clean throughout |
| Internal RAM | About 107–120 KB free with everything running, min-ever 55 KB |
| Where scroll time went, before internal bands (`prof`, core 1) | 23% software fills (`lv_draw_sw_blend_color_to_rgb565`, about 90 MB/s into PSRAM), 11% cache write-back before each band copy, 9% waiting for a band copy, 9% idle at frame end, 8% unpacking 4 bpp glyphs |

| Tried | Result |
| --- | --- |
| LVGL draw threads on both cores (`LV_OS_FREERTOS`, 2 units) | Slower (render 30 ms against 23) |
| PSRAM band height 12, 20, 40, 60 lines | 33, 33, 33, 30 fps; 40 kept for the PSRAM fallback |
| **Internal RAM headroom** | The UI collapses when internal RAM runs short. With PSRAM bands, reserving 100 KB of internal RAM (86 KB left free) took scroll from 37 to 20 fps. Internal bands with 55–68 KB left free gave 9–11 fps; with 130 KB free, 55 fps. The profile then is LVGL's tree walk (events, style lookups), not drawing. Leave ~120 KB of internal RAM free |
| CPU write speed, 24 KB buffer | Internal RAM about 1.1 GB/s. The earlier "fills ran 7x slower in internal RAM" was the starvation above, not the RAM |
| 128-byte L2 cache lines | +1–2 fps, but breaks the 64-byte buffer alignment the PPA and camera rely on; dropped |
| LVGL PPA unit off | Camera preview fell to 12 fps with render at 83 ms (software image blits); kept on |

**PPA hang:** an SRM rotation in flight while LVGL's PPA unit fills hung the PPA on Unit A: the SRM transaction never completed and the UI task watchdog fired. IDF v6.1 still has the size-based DIG-734 workaround in `ppa_srm.c`. Espressif's esp_lvgl_adapter documents the same freeze for partial rendering with 90/270 rotation, and patches it to always bypass the macro-block order. With that patch, stress runs saw no hangs.

**Recording crash fixed along the way:** stopping a recording waited 3 s for the SD writer, then freed the ring under it (assert in `xStreamBufferReceive`). The writer now owns the ring and file, drains them after stop, and frees them itself. The card in Unit A writes at ~210 KB/s (FAT32, 512-byte clusters), below the ~535 KB/s the encoder can produce, so long busy scenes fill the 2 MB ring and drop frames. Reformat with 32 KB clusters.

Next steps, by expected gain:
1. Snapshot-based transitions: draw both screens once and slide the snapshots, as PPA blits. Same for dialogs and the quick settings scrim: freeze the backdrop. Transitions and dialogs would go from 6–10 fps to an estimated 30+.
2. A video plane for the camera, with the PPA writing straight into the framebuffer.
3. More internal RAM headroom (task stacks to PSRAM) before the UI grows.

Tools: `prof start [ms] [delay]` / `prof dump [n]` on the console sample core 1's PC from core 0. `tools/t5/t5prof.py <elf> <log>` maps the samples to functions.

## Rendering pipeline, round 2 — 2026-11-07

Unit A (rev1, 360 MHz), landscape (`ui rot 270`). Gate runs are 8 s each.

| | Before | After |
| --- | --- | --- |
| Scroll (`ui gate scroll`) | avg 56, min 42 fps; render max 17 ms | **avg 57–58**, min 44–51 fps; render avg 12.3 ms, max 13–14 ms |
| Transition | 9 fps; ~74–100 ms a frame | **17–18 fps; 40 ms a frame**. Snapshots delay the start by ~190 ms (loop max 190 ms) |
| Dialog | 6 fps | 6 fps (render ~118 ms a frame); no change |
| Camera preview | 13 fps, render 66 ms | 13 fps, render 57 ms |
| Recording, 3 x 3 s | lent and returned, 0 dropped | lent and returned, 0 dropped |
| Internal RAM, steady | ~128 KB free; min-ever ~70 KB | 92 KB free, **plus 61 KB free in LVGL's own 88 KB pool**; min-ever 69 KB (camera app open) |
| Static DIRAM (`idf.py size`) | 239,108 B | 201,138 B |
| Image | 3,734,592 B | 3,741,440 B (budget 3.77 MB) |

These fps figures include the gate's idle time between animations. At 64 Hz, the transition gate can't read more than ~38 fps (it animates 300 ms of every 500 ms), and the dialog gate can't read more than ~18 fps (225 ms of every 800 ms).

### Kept

- **LVGL's own internal-RAM pool** (`t5_ui/lv_mem_t5.c`, `LV_USE_CUSTOM_MALLOC`). Requests up to 4 KB come from an 88 KB pool in L2MEM. It shrinks in 8 KB steps if there isn't room, down to 32 KB. Everything else goes to PSRAM, never plain malloc. `ui mem` shows where allocations landed; `ui mem check` checks the pool's integrity.
- **Software fills** instead of the PPA for LVGL fills (`T5_UI_PPA_FILLS false`; `ui ppafill on|off`). Scroll render max fell from 17 to 14 ms and the transition frame from 100 to 83 ms. PPA image blits stay on (`ui ppaimg`).
- **Snapshot transitions** (`md/md_freeze.c`). The linker wraps `lv_screen_load_anim`, so apps get it too. Both screens are rendered once into RGB565 PSRAM snapshots and hidden. The display's bottom layer then draws the snapshots where LVGL's animation puts the screens. The top and sys layers stay live. If another screen load interrupts, the snapshots are dropped. `ui freeze [on|off|<mask>]` switches this and reports stats.
- **Cold static buffers in PSRAM** (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`, `EXT_RAM_BSS_ATTR`). This moved 38 KB: the dmesg ring (16 KB), the scratch buffers for the `fs`, `audio` and `i2c` test commands, and the loaded port profiles. That room let the pool grow from 72 to 88 KB; it no longer overflows during the gates.
- **Camera close/reopen fix** (found by the stress pass, below).

### Tried and reverted

| Tried | Result |
| --- | --- |
| Hypothesis: LVGL's small allocations fall back to PSRAM | **Wrong.** With 66 KB free, LVGL made 0 PSRAM allocations: 7,314 went to LP SRAM, 356 to the 8 KB TCM, and 3,044 to L2MEM. The heap counts LP SRAM (32 KB at 0x50108000) and the TCM as internal memory, and both are uncached on the slow LP bus. LVGL there ran ~6x slower than in PSRAM; with 37 KB free (PSRAM fallback) scroll was 43 fps. The "internal free" figure overstates fast RAM by ~38 KB. That's why the pool takes `INTERNAL\|DMA` (L2MEM only) and its overflow goes to PSRAM. With the pool, scroll held 54 fps even with 12.6 KB internal free |
| Wider bands | Not testable: there's no fast RAM left for them (the main 362 KB L2MEM region is fully allocated) |
| Two LVGL draw threads (`LV_OS_FREERTOS`, 2 SW units), tried twice | Slower: scroll 49, transition 150 ms a frame (121 with software fills), dialog 5, camera 12, and 17 KB more internal RAM. Both threads on core 1: 158 ms. The per-task handoff costs ~60% |
| Drawing snapshots straight into the framebuffer (compositor that bypasses LVGL) | Slower (55 ms a frame) and hung the PPA ("exceed maximum pending transactions", band flush timeouts) |
| Banded snapshots | No faster than full-screen ones |
| Dialog snapshots (box as ARGB8888, backdrop as RGB565) | Render ~118 ms a frame unfrozen. 103 with the backdrop snapshot, 126 with the box, 103 with both, all still 6 fps. A dialog fade is about 2 frames, so a 78 ms snapshot costs more than it saves. The code stays (`ui freeze 2/4/6`), off by default |
| LVGL asserts and logging off | Not worth it: only the cheap malloc and NULL asserts are on (object, style and integrity asserts are off) and logging is at WARN |
| esp_lvgl_port SIMD blend routines | Skipped: a new dependency built against LVGL internals, and the remaining cost is PSRAM bandwidth, not blend arithmetic |
| Task stacks to PSRAM | Not done. Stacks total ~137 KB; audio, USB (`usb.*`), `jpeg.dec` and `cam.write` are already in PSRAM, leaving ~105 KB internal: UI 16 KB, console 12, power 6, camera 6, net 5, the esp_hosted SDIO/RPC tasks, UVC/CDC class tasks and ~15 small ones. The console and power tasks write flash (littlefs, NVS), which IDF forbids from a PSRAM stack; the camera and esp_hosted tasks are on hot I/O paths |

**PSRAM bandwidth is the ceiling.** With the panel scanning out, a full-screen (1.8 MB) PSRAM-to-PSRAM copy takes ~46 ms (~39 MB/s), whether done by the PPA (rotated or not) or by CPU memcpy. `psram 2` reads "write 62 MB/s copy 40 MB/s". Any full-screen animation is therefore bound to roughly 22–28 fps. The transition frame (40 ms) is already near it; 30 fps would need fewer bytes moved per frame, e.g. drawing only the moving strip.

### Camera crash found by the stress pass

After step 3 the stress pass rebooted once. A camera loop (`shell launch com.t5.camera`, `shell home`, `cam open`, `cam close`) reproduced it within 1–11 cycles. The core dumps showed heap corruption: a fresh camera's semaphore, then its `call_fn`, overwritten with allocator metadata.

- **Root cause: a deadlock between the capture task and the UI task.** An app's exit releases its camera on the UI task, which holds the UI lock. `t5_camera_close` then waits for the capture task to stop. The capture task meanwhile blocks in `t5_ui_async`, which takes the UI lock to queue the preview swap. Close gave up after 1 s and freed the camera under the still-running task. A queued `preview_swap` could also write into a freed camera.
- **Fixes in `t5_camera/camera.c`:**
  - The capture task waits for the UI lock in 10 ms steps and gives up once asked to stop.
  - `preview_swap` gets the open's serial number, not the pointer.
  - Close waits up to 5 s and never frees the camera while its task runs; it logs and leaks instead.
- **After the fix:** 35 loop cycles (69 opens) with no stall, no ANR and no reboot. The stress pass was clean too: no reboot, integrity OK, internal free steady at 92 KB.
- The snapshot transitions made the deadlock likelier (going home blocks the UI task ~190 ms), but it reproduced with them off too.

## Phases 6 and 7 follow-up — 2026-11-07

Unit A, working through the open Phase 6/7 items. Five real bugs found and fixed; most of them were hiding behind each other.

| Problem | Cause | Fix |
| --- | --- | --- |
| **Every installed app crashed on its first instruction** (instruction access fault at the ELF entry, `hello` included) | `CONFIG_SPIRAM_XIP_FROM_PSRAM` (rendering work, 2026-11-06) maps PSRAM before memory protection is set up, so IDF then locks the whole PSRAM heap read/write and never executable. The ELF loader puts `.text` in the PSRAM heap. Without XIP the protected range came out empty, which is why apps worked before | `CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION` off (`sdkconfig.defaults`). Internal RAM keeps the PMP IDRAM split |
| **Installing an app could corrupt LVGL's heap** (`tlsf` assert in `lv_realloc`, from `launcher_reload` on the console task) | `t5_apps_install`/`remove` rebuilt the launcher from the calling task, racing the UI task | `apps_home_refresh` queues the rebuild on the UI task (`lifecycle.c`) |
| **Wi-Fi data and RPCs stop** ("SDIO RX read (2048 bytes) exceeds staging target (1536 bytes)", then `rx_get_buffer failed`, timeouts until `net reset`) | esp_hosted 3.1 sizes its SDIO streaming RX buffers from what the C6 advertises at INIT and refuses larger reads. The factory C6 firmware (1.4.1) advertises neither its mode nor its queue depth, so the host fixes one 1536-byte frame while the C6 streams bigger reads; the first one desyncs the link | `t5_net/hosted_compat.c` wraps `eh_host_bus_sdio_set_rx_staging_target`: a one-frame target keeps the buffers dynamic (the host's own behaviour when the depth is unknown). An updated C6 that advertises its capacity passes through. After: 5 installs and 8 x 2 MB downloads, 0 errors, 0 health failures (~160 KB/s, the SD card's write speed) |
| **Camera close/reopen corrupted the heap** (found 2026-11-07, round 2) | Capture task and UI task deadlocked on the UI lock; close freed the camera after 1 s anyway | See "Camera crash found by the stress pass" above |
| **Audio input/output freed under their tasks** | `t5_audio_in_close`/`out_close` slept 25 ms for "the task to finish its block", but a read after a resume, or a codec open, takes longer | The tasks mark the stream they hold; close waits for them to let go (up to 1 s, else logs and leaks) |
| **Double free deleting stream buffers** (mic close, camera writer: `tlsf_free` "block already marked as free", intermittent) | **An ESP-IDF v6.1 bug**: `vStreamBufferGenericDeleteWithCaps()` deletes the stream buffer with `vSemaphoreDelete()`, i.e. `vQueueDelete()` on a stream buffer, which frees the struct whenever the byte where a queue keeps its "statically allocated" flag reads 0; IDF then frees it again | `t5_core/t5_sbfix_idf.c` wraps it: `vStreamBufferDelete()`, then free both blocks once. Covers audio, camera and USB |
| **On-screen keyboard never appeared** (Wi-Fi password, time fields) | Its slide-in animated an alignment offset to the keyboard's absolute y, leaving it a keyboard's height below the screen. Its special keys used LVGL symbols, which Roboto lacks | Animate the offset from its height to 0 (`keyboard.c`); the keys' font falls back to the Material icons for those symbols (`md_theme.c`). `ui kbtest` shows it |
| **Recording from the Camera app crashed** (and video was choppy) | The record button ran on the UI task; the camera service needs the UI task (it lends the encoder the draw buffer, and posted preview frames under the UI lock). `cam_call` gave up after 5 s but left the call queued with a pointer into a dead stack frame, which crashed later | Record start/stop refuse the UI task (`T5_ERR_STATE`, as the SDK docs already said) and the Camera app records from a worker; a timed-out call is cancelled atomically; the capture task never takes the UI lock (an LVGL timer shows new preview frames); preview conversion is capped at 15 fps, 5 fps while recording. Recording with the preview on: ~11 fps before, ~22 fps after |
| Date and time page showed the old zone until reopened | The zone row was filled once | Refreshes with the page; fields refill when the zone changes (unless being edited); new "Sync with the network now" row (`t5_net_sync_time`) |
| `app stress` leak check off by 1.8 MB | The baseline was taken while the exit transition still held its screen snapshots | `stress` waits for snapshot transitions to finish before measuring (`diag_app.c`) |
| Console `cam` used a freed camera after Sleep | The camera's suspend hook closes it behind the console's back | `cam` drops its handle when the camera isn't open |

| Check | Result |
| --- | --- |
| SDK apps rebuilt against today's image (`t5 apps`, `t5 examples`) | All five install over Wi-Fi. `hello` and UVC Monitor run; `crasher` crashes and is recorded; `hang` brings up the "App not responding" dialog, which waits for a tap (by design) |
| `app stress com.t5.test.leaky` | 100 cycles, then 50 with the fixed baseline: 0 off baseline, worst drift 68 bytes |
| Media through 100 sleep cycles (camera open and a tone playing, Sleep, wake, camera reopened with frames, a 1 s mic recording every 10th cycle) | **250 cycles over five runs after the fixes: 0 failures, 0 resume failures, 0 underruns, 0 codec errors**; suspend 60–150 ms, every hook within budget |
| PPA alpha path (`ui ppatest`) | PPA vs software within 2 RGB565 steps for faded RGB565 and ARGB8888 |

The intermittent mic double free seen during the sleep cycles was the IDF stream-buffer bug above; 50 more cycles after that fix: clean.

| Exit check | Result |
| --- | --- |
| 10 min recording, no drops | **Not met on Unit A's card.** First run (6 Mbit/s fixed): 2,053 of 17,571 frames dropped, the 2 MB ring full from minute 2. The card writes ~300 KB/s (`fs sdbench`: 0.3 MB/s write, 1.0 MB/s read; FAT32 with 512-byte clusters on 30 GB). With adaptive bitrate (down a quarter while the ring is over 40% full, to 1.5 Mbit/s at least; back up under 10%): **14 dropped in 10 min**, all in one stall near the end where the ring filled even at 1.5 Mbit/s, i.e. the card stops for seconds (FAT free-cluster scans). Reformatting the card with 32 KB clusters should fix it; not done (it erases the card) |
| Touch run-through (by a person) | Installing from SD, the camera permission prompt, gestures, per-app power (camera 1,841 mW over a minute, on battery) worked. Missing keyboard, time setting and Camera-app recording were broken; fixed above. Wi-Fi rejoined with the on-screen keyboard and NTP fixed the clock |
| Early-boot camera stall | A recording started 16 s after boot timed out: the camera had delivered 2 frames. Recordings after a few seconds work; not investigated further |

### Rendering targets, measured while animating

`ui gate transition` and `ui gate dialog` now report the frame rate while animating (frame intervals with an animation running, leaving out each step's set-up), which is what plan 4.8 asks for ("30 fps during full-screen transitions"); the old per-second figure included the idle time between steps.

| | Before today | After |
| --- | --- | --- |
| Transition | 40 ms a frame | **26.7 fps while animating** (37 ms a frame); 27.4 fps in portrait. Start latency ~190 ms (the two snapshots) |
| Dialog | 118 ms a frame, 6.6 fps | Unchanged by default. Snapshotting backdrop and box (`ui freeze 7`) gives 16.7 fps but ~260 ms before it moves, for a 150 ms fade, so it stays off |

What changed:
- **The bottom layer's background isn't drawn under snapshots** (`md_freeze.c`): LVGL filled the whole screen in software every transition frame, only for the snapshots to cover it. Saves ~3 ms a frame.
- **Faded and ARGB8888 images on the PPA** (`t5_ui/ppa_img_t5.c`, wrapping `lv_draw_ppa_img`). LVGL's PPA unit only took opaque RGB565/RGB888 images; anything with opacity was blended in software. The PPA's blender now does them (band as background, the image as foreground with fixed or scaled alpha). `ui ppaalpha on|off` for A/B; `ui ppatest` checks it against software.
- **Dimmed snapshots** (`md_freeze_dim`): a frozen screen under a scrim is drawn as a fill plus the snapshot at 255 − scrim, the same pixels as a blended scrim.

Why transitions stop at ~27 fps: a frame reads a full-screen snapshot from PSRAM and writes a full frame back (~3.6 MB) while the panel scans out ~118 MB/s; rotation isn't the cost (portrait is the same). 30 fps needs ~10% less traffic per frame. The way to get it is not to copy at all: lay the two screens' rotated snapshots out back to back and slide the panel's scan-out start address on vsync (a landscape slide is a row offset in the panel's memory), leaving only the status bar to composite. That is a display-pipeline change, not done.

## Phases 8 and 9 — 2026-11-07

Unit A (ESP32-P4 v1.3, rev1). Everything below was run on the device unless it says otherwise.

### Flash budget first

MicroPython needed ~360 KB and the app slot had 26 KB left under CI's 90% limit. The MD2 fonts and baked shadows (850 KB of `.rodata`) now go into the image deflated (`tools/assets/zpack.py` at build time) and are inflated into PSRAM once at boot by the ROM's `tinfl` (55 ms). Rendering is unchanged: with `SPIRAM_XIP_FROM_PSRAM` the originals were in PSRAM anyway. `tab5os.bin`: 3.75 MB before, 3.06 MB after, **3.48 MB with all of Phases 8 and 9** (83% of the slot). The simulator still builds the plain files.

### Phase 8

| Check | Result |
| --- | --- |
| MicroPython 1.29 (`third_party/micropython`), one VM per app, GC heap from PSRAM (manifest `min_heap_kb`, 2 MB default) | Runs; `t5` bindings return live values (power, battery, time, IMU, rotation, Wi-Fi, port names) |
| Containment (`py test`): `machine`, `uctypes`, `socket`, `network`, `esp`, `esp32`, `_thread`, `ffi`, `select`, `vfs`, `bluetooth`, `ssl`, `framebuf` don't import; `@micropython.native`/`viper`/`asm_rv32` are syntax errors; no `.mpy` loading; files outside the app's roots and `..` paths refused; internal I2C addresses and rails refused | **All pass.** The build fails if any module outside `modules_allowed.txt` registers (21 do, all listed) |
| Python app with UI, from a `.t5app` | Taps, switch and slider reach Python callbacks; the chart updates from the script's loop; back ends the loop and the app (`t5.app.running()` turns false, a script that ignores it gets a KeyboardInterrupt after 500 ms) |
| Editor | Syntax colouring, typing with the on-screen keyboard (backspace, Enter with auto-indent) and with USB-keyboard keys (`ui type`, `ui key`); Run shows output below the code; a script's own UI covers the Editor until its back arrow |
| Terminal | REPL with history, multi-line blocks (the Terminal pre-fills indentation), tracebacks; Log and System tabs |
| Developer mode | Pairing with the on-screen code (a wrong code is refused, three end the attempt); every endpoint refuses requests without the token; the token works from `t5` |
| **Exit: `t5 new` to the app on screen in under 30 s, log streaming** | **2.7 s** for a C app over Wi-Fi (new, build, pack, install, launch); a Python app 1.1 s from `t5 run` |
| Exit: a Python app using Grove I2C runs from the on-device editor | The bundled `hello.py` opens Grove I2C and scans it from the Editor; nothing was plugged into Grove, so it found no devices |
| USB fallback (`t5 run --usb`) | 200 KB app sent, installed and launched in 11 s (~20 KB/s). The console drops bytes when its 256-byte receive ring fills, so lines are 150 bytes with a CRC each and resent on error; a 3.4 MB file still stalled once at ~3 MB, so the USB path is for apps, not OS images |

Bugs found on the way: `t5_apps_exit_wait` timed out with the screen off (the UI task pauses; it now wakes the screen first); developer-mode uploads died when the device went to Sleep mid-transfer (requests now hold net and CPU wake locks); query parameters weren't URL-decoded (a pairing code with a space failed); no LVGL default group existed until a USB keyboard was plugged in; the install-time ABI check let `__extendsfdf2` through and the loader then failed on it (the libgcc helpers apps need are now published in `api_v1.sym` and the check is strict).

### Phase 9

| Check | Result |
| --- | --- |
| Signed update (`ota selftest` with a signature over the image, firmware built trusting the test key) | Hash, ECDSA P-256 signature, chip revision and project checked; slot switched |
| Tampered signature (one bit) | Refused: "the signature doesn't verify" |
| A failed attempt after a good one | Found a real bug: the slot already chosen for the next boot was being rewritten. The boot slot now points back at the running system before an install touches the other one |
| New image on trial, confirmed after 60 s | Confirmed at 64 s |
| Crash during the trial (the crasher app) | Next boot is the previous slot (rollback) |
| **Update over Wi-Fi** (`t5 ota`, 3.4 MB) | 21 s, restarts on trial. Needs paced writes: a flood of TCP data runs esp_hosted's SDIO receive buffers dry, the C6 health check fails and the link resets mid-upload |
| `test/hil/ota_cycles.py`: interrupt mid-install, crash during trial, normal update | 3 of 3 cycles pass (the plan's 1,000 is for an unattended run) |
| Boot counter (custom bootloader, last sector of `recovery`) | One bit per counted boot; a healthy boot erases it. 2 unconfirmed boots → safe mode, 3 → recovery (both reached by resetting during boot) |
| Safe mode | Installed apps refused with a toast; banner on the home screen names the last crash; Restart leaves it |
| Recovery image (810 KB) | Starts on request and after three failed boots; "Save logs to the SD card" wrote 3 files; "Restart Tab5 OS" boots the system with a cleared counter. Reinstall from SD not run (the Mac can't write the card here; see below) |
| Wi-Fi credentials sealed (AES-256-GCM, chip-bound key) | Existing plaintext networks migrated on first boot; reconnects from the sealed copy after a restart |
| Example apps | RS-485 Console opens the port and shows traffic (nothing on the bus); Logger reads the battery (no Grove sensor) and charts it |
| Soak smoke test (`test/hil/soak.py --minutes 4`) | 51 rounds of apps, Python, rotation, idle and Wi-Fi checks: 0 resets, internal RAM 77 KB free throughout (low point 47 KB), PSRAM steady. (One Python round hit its 512 KB console heap: the script asked for 500 KB; it asks for 300 KB now) |

## Smoothness: dialogs and transitions — 2026-11-07

Goal: dialogs and screen transitions at 30 fps or better while they animate (plan 4.8). Unit A (rev1, 360 MHz), portrait (`ui rot 180`). New tools: `ui frames` logs each of the last 32 frames (render time, time blocked on PPA draws, waiting for a band, the frame's band and catch-up copies, the area redrawn); `ui dialog open|close` steps one dialog.

| Gate | Before | After |
| --- | --- | --- |
| Dialog, while animating | 8.8 fps (one full-screen frame of ~113 ms per step) | **33 fps, PASS**; every fade frame ~28 ms |
| App launch (fade, the OS's only screen transition) | ~19 fps (3 frames of 52 ms) | **~31 fps** (5–6 frames of 30–33 ms) |
| Transition gate (full-screen slide, MOVE_LEFT) | 27.2 fps | 27.3 fps: still FAIL, see below |
| Scroll | min 40, avg 53 fps | min 49, avg 53 fps (unchanged in effect) |
| Dialog start latency (tap to first fade frame) | ~110 ms | ~335 ms; the dim shows on the next frame, ~100 ms |

**What the time was.** Every animated frame that changes the whole screen moves ~3.7 MB through PSRAM, which runs at 40–60 MB/s with the panel scanning out, so ~35 ms is the floor. A dialog's fading scrim made every dialog frame full-screen, and the CPU blended it (~80 ns a pixel). The fix is to stop changing the whole screen:

- **Translucent fills on the PPA** (`t5_ui/ppa_fill_t5.c`): scrims and other translucent rectangles go to the PPA's blender; rounded ones send the rows between their corners. `ui ppafilltest` checks it against software (within 1–2 RGB565 steps).
- **The scrim no longer fades**: it shows at once (on the PPA, the next frame), and only the dialog box fades. The box's shadow moves to the scrim so it doesn't fade with the box.
- **Frozen backdrop** (`md_freeze_draw_into`): after the scrim's first frame, the screen behind is frozen with the scrim and shadow drawn into the snapshot, with invalidation off (same pixels, no extra frame). Each fade frame is then the box's area only (258 kpx, not 921). The backdrop stays frozen while the dialog is open: a tap on the dialog redraws the dialog's area, and closing costs one full-screen frame. Objects on the top layer below the dialog (the status bar) are left out of the bake and dimmed live. A rotation, or a transition behind the dialog, thaws it back to live drawing.
- **Wait for the framebuffers**: a full-screen frame must reach all three framebuffers, and those catch-up copies landed in the fade's first two frames (~40 ms each). The fade now starts once `t5_ui_settled()` reports them done; it copies on request only (done blindly, it copies whole screens the next full-screen frame redraws anyway).
- **Fade through** for crossfade transitions (`md_freeze.c`): the old screen fades out to the background, then the new one fades in, so each frame blends one snapshot instead of blitting one and blending another. Nearly opaque steps (≥ 240) are drawn opaque.
- **Snapshots written back from the cache** before the PPA reads them (correctness; it could read stale lines).
- Fixed along the way: setting the scrim's style to its current value redrew the whole screen (LVGL invalidates on any set).

**Tried and dropped:**

| Tried | Result |
| --- | --- |
| Box snapshot pre-composited over the backdrop (`MD_FREEZE_DIALOG_BOX`) | Prep 339 ms (two snapshots, CPU bake and compose); no faster per frame. Off by default |
| Half-resolution transition snapshots, scaled 2x by the PPA's SRM | The SRM's scaling is slow: 42 ms of PPA time a frame, worse than full size. LVGL's own scaled-image path misdrew them too. Removed |
| Snapshot blits as SRM copies instead of LVGL's blend copy | 11 fps against 27: the SRM reads PSRAM slowly. Removed |
| Widening redraw areas to whole rows | No difference. Removed |
| Rotation 0 against 180 for transitions | Same (27.2 fps): the band copies' rotation isn't the cost |
| Catching up the framebuffers between every frame | Worse: it copied screens that the next full-screen frame redrew |

**Follow-up the same day: two artefacts, and the drawer.** Found with `ui slowmo N` (LVGL's clock N times slower) and screenshots mid-animation:

- *App launch ghosting*: the fade through drew a snapshot below its screen's opacity, but the bottom layer judged coverage by the screen's opacity and skipped its background fill, so the snapshot blended with whatever the band held last: copies of the launcher one band apart. Coverage now follows the opacity drawn.
- *Rounded translucent fills drawn as stacked pills* (a chip, the drawer's selected row, the dialog box while it fades): LVGL's PPA unit passes a fill already clipped to the band, and the split rounded every band's slice. The corners now come from the whole rectangle. The dialog gate went from 33 to 40 fps with it.
- *Navigation drawer*: was ~11 fps (each frame redrew the launcher under a fading scrim, ~80 ms). Now like a dialog: the scrim shows at once, the backdrop freezes with the scrim baked in, and the panel slides over it: 10 frames of 16–38 ms opening, 6 of ~30 ms closing.

**Still open.** The transition gate's full-screen slide is at the PSRAM floor (two full-screen snapshot reads and a full-screen write each frame, ~36 ms). The OS doesn't use a slide; if one is added, a fade-through or a partial slide stays under 33 ms. Dialog start latency is mostly the backdrop's software snapshot (~77 ms) and bake (~46 ms); copying the framebuffer with the PPA instead would roughly halve it.

## App isolation — 2026-11-08

Unit A, rev1 build. Each case launched from the console (`app run <id> <mode>`, `app anr close` for the dialog's Close app). "Survived" means no reset: uptime kept counting, the launcher came back, `app crashes` showed the record.

| Case | What the app did | Result |
| --- | --- | --- |
| crasher (none) | store through NULL in a UI timer | survived: pc app+0x730, cause 7 |
| crasher call | `strlen(NULL)` from a UI timer | survived: pc in ROM, ra app+0x716 |
| crasher lvgl | `lv_label_set_text((lv_obj_t *)0x10, …)` as a tail call | first build panicked (no app frame left on the stack); now survived as a "guess" (data fault, UI task working for the app, none in the last 30 s) |
| crasher task | store through NULL in its own task | survived: task ended, app closed |
| crasher hog | prio-4 task spinning on core 0 | task moved to priority 1 after 3 s; app kept running until closed |
| crasher mem | malloc 256 KB blocks until refused, then leak 512 KB LVGL buffers every 50 ms | malloc refused at the 2 MB PSRAM reserve (about 19 MB in); LVGL took PSRAM to 603 KB; app closed 1.5 s later; PSRAM back to 21.2 MB |
| hang (none) | busy loop in a UI timer | dialog at 2.1 s; Close app stopped it (pc app+0x374), no restart |
| hang sleep | `vTaskDelay` forever in a UI timer | dialog at 2.0 s; Close app stopped it (pc in the kernel, app found on the stack), no restart |

`app stress com.t5.test.leaky 40` after the change: 0 cycles off baseline, worst heap drift 336 bytes (PASS). A screenshot after a stopped hang shows the home screen drawn normally.

Not reclaimed: LVGL memory the app allocates outside objects it owns (the 1.6 MB of draw buffers `crasher mem` leaked stayed allocated after it closed). Not run: a fault in a custom draw callback (still reboots by design), stack overflow (still reboots).

### Containment, second round (same day)

Same method, `crasher` modes added. No reset in any case; `heap dma` and `heap psram` read after each. Tested with `test/apps/crasher` on Unit A.

| Case | What the app did | Result |
| --- | --- | --- |
| stack | unbounded recursion in its own task | survived: stack guard, task ended, app closed |
| uistack | unbounded recursion on the UI task | survived: unwound to the UI loop |
| draw, drawbad | NULL store in a draw callback, mid-frame | survived: frame abandoned, next one drawn |
| badfree | `free((void *)0x20)`, faulting under the apps lock | survived: lock released |
| dfree | double free | logged and ignored |
| tasks | start tasks until refused | stopped at 16 |
| objs | queues and mutexes until refused | stopped at 255 (the handle table) |
| post | 5000 `t5_ui_async` | 512 queued, 4488 refused with `T5_ERR_BUSY` |
| lvleak | leak 4 MB through `lv_malloc` | all back after exit |
| mem | as before | PSRAM back to its pre-test value after close (1.6 MB used to stay lost) |
| evcrash, evhang | an event handler faults, or never returns | survived; dispatcher carries on |
| `l2eat 76` (console) | internal RAM held down to 9 KB | reserve released, app closed; reserve retaken after |

Internal RAM free after boot: `heap dma` 82.2 KB before, 95.1 KB after, with the 16 KB reserve held (30 KB of idle stacks moved to PSRAM: console, USB class drivers). 28 repeated crashes in a loop: `heap dma` drift about 6 bytes each. `app stress com.t5.test.leaky 25`: 0 off baseline, worst drift 1000 bytes (PASS). `test/hil/net_stack.py` PASS, including after Sleep and wake.

Bugs this turned up: the stack guard interrupt reaches both cores and IDF parks the second forever; the exception return does not restore SP from the frame; the guard's saved frame overwrote the heap block below the stack (hence `T5_STACK_MARGIN`); the port rewrites a TCB's stack pointer on delete; a semaphore's "storage" is its own control block, which `vQueueDeleteWithCaps` frees twice.

Third round, same firmware line. `wcode` and `wrodata` (writes over OS code and read-only data in PSRAM) both fault with cause 7 and only the app closes: IDF's PMP setup already covers those with XIP on. `uilock` (a task keeps the UI lock): Close app ended the task, no restart. `late` (calls queued in `on_destroy`): dropped, neither call executed. `socks`: stopped at 8. `syswrite`: both refused. `hog2` (spinners on both cores): demoted after 3 s, 14 s with no task watchdog message. Regression after: `net_stack` PASS, `app stress com.t5.test.leaky 20` worst drift 108 bytes (PASS). The USB MIDI and UVC callback guards were not run (no such device attached).

### File isolation (same day)

`test/apps/fscope`, `fsshared` and `fspy` on Unit A, all PASS: own data read and write; install folder read but not write; other apps, `/data/sys`, shared folders without permission and the SD root refused; `..`, upper-case and trailing-dot paths refused; quota stopped writes at 1024 KB and space came back after a delete. Python app: same refusals. `shared_read`: Don't allow, Allow, and a second run with no prompt.

File picker (`ui tap` and `ui swipe` on the device): Cancel, close button, Back at the top level, tap outside and the bottom swipe each gave the callback NULL and left the app running. Back inside a folder went up one level. `app exit` and launching another app closed the picker without a callback. Filter hid `skip.jpg`; `apps` was hidden on the SD card. Landscape and portrait checked by screenshot. Not run: removing the SD card with the picker open, and the picker with a real tap by a person.

Seen and not explained: `net reset` while the device was in Sleep left the radio at "starting" until a reboot. Not run: USB class drivers with a device attached on PSRAM stacks.

## Still to answer

| Item | Needs | Status |
| --- | --- | --- |
| 0.5 confirm `PWROFF_PLUSE` cuts power | Battery only, a person | Soft off is implemented (`power off --force`, M5Unified's 10 × 50 ms toggle). Not yet run on battery |
| 0.9 power per state | External meter | `test/hil/power_cycle.py --meter-pause`; not measured |
| Charge mode, bench checks 7 and 8 | USB attached, a person | Implemented (deep sleep with a 60 s timer check); not yet run |
| `CHG_STAT` polarity | Battery only | Reads 1 on USB with a full pack. `bat` counts its level against current direction; the gauge doesn't use it yet |
| USB-power detection on battery | Battery only | Assumed from battery current (under 50 mA means USB). Confirm the running draw on battery is well above that |
| Gauge accuracy (exit: under 5%) | A full discharge trace | Host-tested on synthetic traces only. Start `bat log start` on battery |
| Soft-off wake by button and RTC alarm | Battery only, a person | Implemented; the alarm path arms an RTC interrupt, so it needs a person ready to press the button |
| C6 reset to connected under 5 s every time | Board time | The slow cases spend their time in association and DHCP; look at the AP side, or a static lease |
| C6 update round trip with forced rollback | Permission to update the C6 | The factory C6 bootloader's rollback support is unknown. `t5 c6-stage` then `net update` when agreed |
| 0.11 connector pins | Meter | The ports broker exists (`port`); ext and stamp pin roles still need a meter |
| Phase 4 rendering gate | Board time | Fails at 29–35 fps scrolling; see Phase 4 results |
| Rail switch effects (USB-A 5V, ext 5V, speaker, Wi-Fi power) | A load on each rail | Deferred to Phase 2 (rails) |
| 1.10 full 1 h stress | Board time | 48 min before the fix (data and bus clean) + 10 min after it with 0 errors of any kind. A full uninterrupted hour is optional |
| 1.10 SDA short during stress | A probe and a person | Not done |
| Phase 6 exit: a fresh device end to end without a PC | A person | Every piece runs from the console; join Wi-Fi, set the time, install from SD, grant, run, see the app's power and remove it, all by touch, not yet done |
| Phase 7 exit: 1 h playback, screen off | Board time | 240 s with 0 underruns; run `audio play 3600` after `power screenoff` |
| Phase 7 exit: 10 min recording | Board time | 270 s with 0 dropped at 720p (the sensor's 1080p-class modes are 1600x1200/1600x900, not wired up) |
| Phase 7 exit: keyboard, flash drive and UVC dongle through a hub | Those devices and a hub on USB-A | Host and drivers run; nothing plugged in yet |
| Phase 7 exit: media through 100 sleep cycles | Board time, nobody using the unit | Suspend hooks close the camera and power the codecs down; not cycled yet |
| Camera orientation and colour | Eyes on the screen | Preview and stills are drawn without rotation in landscape (`CAM_LANDSCAPE_ROT` 0); the corners show a magenta cast (lens shading in the ISP tuning) |
| `HP_DET` polarity | Headphones in the jack | Assumed high with a plug in |
| Camera preview frame rate | Phase 4 pipeline work | 11 fps; see Phases 6 and 7 |
| Phase 8: Python app with a real Grove I2C sensor | A sensor on Grove | Code path runs (scan returns nothing with no device); read a real register next time one is plugged in |
| Phase 9 exit: power pulled during an update | A person or a relay | Resets during the install pass (`ota_cycles.py --mix interrupt`); a real power cut is `--mode powercut` (RTC trick; may need the power button) |
| Phase 9: 1,000 OTA cycles with forced failures | Unattended board time (~3 days) | `test/hil/ota_cycles.py --cycles 1000 --mix ok,crash,interrupt` |
| Phase 9: 7-day soak | Unattended week | `test/hil/soak.py --hours 168` |
| Phase 9 exit: the logger for a week on one charge, draw measured | Battery, a sensor, a meter | The background wake arms the RTC, which can leave the board off until the power button; start it with someone nearby |
| Recovery "Reinstall from SD" and the SD `RECOVERY` flag | Writing to the SD card from a computer | macOS refused this session access to removable volumes; put a `.t5os` and a `RECOVERY` file on the card by hand |
| USB fallback for large files | Board time | 3.4 MB stalled once at ~3 MB over the console; apps are fine |

Procedures for each are in [bench-procedures.md](bench-procedures.md).
