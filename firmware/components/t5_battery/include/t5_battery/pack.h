/* SPDX-License-Identifier: Apache-2.0
 * The Tab5's battery pack: 2S Li-ion, NP-F550 style (vendor: 2000 mAh,
 * 7.4 V, 14.8 Wh). The one place its voltages and currents are written
 * down; the gauge, the policies and the battery service all use these.
 * Currents in 0.1 mA, positive = discharging (as the INA226 reports them).
 */
#pragma once

#define T5_PACK_NOMINAL_MAH 2000
#define T5_PACK_FULL_MV 8400          /* 4.2 V a cell, open circuit: 100% */
#define T5_PACK_EMPTY_MV 6500         /* 3.25 V a cell, the design's floor: 0% */
#define T5_PACK_CRITICAL_MV 6300      /* under any load: off at once */
/* Charging and at least this, with the current tapered off: full. */
#define T5_PACK_CHARGED_MV 8300
#define T5_PACK_TAPER_MA10 1000       /* 100 mA */
/* Outside this a reading can't come from the pack (the INA226's bus node
 * jumps to ~4.4 V on USB with a full pack, bench 0.4). */
#define T5_PACK_MIN_MV 5000
#define T5_PACK_MAX_MV 9000
/* Under this load the floor voltage means empty; above it, it's sag. */
#define T5_PACK_LIGHT_LOAD_MA10 5000  /* 500 mA */
/* The running P4 and panel always draw well over this from the pack: a
 * smaller current, either way, means something else powers the board. */
#define T5_PACK_IDLE_MA10 500         /* 50 mA */
#define T5_PACK_RINT_MOHM 150         /* pack + wiring, for load compensation */
#define T5_PACK_STANDBY_MA10 20       /* soft-off draw until measured */
