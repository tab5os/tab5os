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

Procedures for each are in [bench-procedures.md](bench-procedures.md).
