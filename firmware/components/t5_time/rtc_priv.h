/* SPDX-License-Identifier: Apache-2.0
 * Inside t5_time, and for the device tests that check the chip itself.
 */
#pragma once
#include <time.h>
#include "t5_core/err.h"

/* Writes the time, restarts the sub-second divider and clears VLF. */
t5_err_t t5_rtc_write(const struct tm *tm);

/* RX8130 registers (datasheet). */
enum {
    RX8130_SEC = 0x10,
    RX8130_ALARM_MIN = 0x17,   /* 0x17..0x19, bit7 = "don't care" */
    RX8130_TIMER0 = 0x1A,
    RX8130_EXT = 0x1C,         /* TSEL[2:0], WADA bit3, TE bit4 */
    RX8130_FLAG = 0x1D,        /* write-0-to-clear */
    RX8130_CTRL0 = 0x1E,       /* AIE bit3, TIE bit4, UIE bit5, STOP bit6 */
    RX8130_CTRL1 = 0x1F,       /* INIEN bit4, CHGEN bit5 */
    RX8130_RAM = 0x20,         /* 0x20..0x23 user RAM */
};

#define RX8130_EXT_WADA 0x08
#define RX8130_EXT_TE 0x10
#define RX8130_CTRL0_AIE 0x08
#define RX8130_CTRL0_TIE 0x10
#define RX8130_CTRL0_STOP 0x40
