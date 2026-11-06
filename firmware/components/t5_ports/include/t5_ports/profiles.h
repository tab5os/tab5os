/* SPDX-License-Identifier: Apache-2.0
 * Module profiles (plan 5.3): what a Grove unit or M5-Bus module is, which
 * port and bus it uses, its I2C addresses and its power needs. Part of the
 * app SDK.
 *
 * A profile file is JSON, one object or an array of them:
 *
 *   {
 *     "name": "ENV III",
 *     "ports": ["grove"],
 *     "bus": "i2c",                       // i2c uart spi gpio pwm adc rs485
 *     "i2c": ["0x44", "0x70"],            // numbers or hex strings
 *     "power": ["ext5v"],
 *     "driver": "sht3x+qmp6988",
 *     "notes": "SHT30 humidity, QMP6988 pressure"
 *   }
 *
 * Unknown keys are ignored. Files ending in .json under /data/profiles and
 * /sd/profiles are loaded by t5_profiles_reload(). A few common M5 units are
 * built in (marked best-effort: addresses from vendor docs, not tested here).
 *
 * Scanning is manual (probing can upset some devices) and only touches the
 * port the caller names. Pass "mbus.i2c" to scan the internal bus where it
 * reaches the M5-Bus; its reserved addresses are skipped.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_ports/ports.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_PROFILE_MAX_ADDRS   8
#define T5_PROFILE_MAX_PORTS   4
#define T5_PROFILE_MAX         48     /* built-in plus loaded */

#define T5_PROFILE_POWER_EXT5V 0x1u   /* switched 5 V (Grove, M5-Bus, ext) */
#define T5_PROFILE_POWER_USB5V 0x2u
#define T5_PROFILE_POWER_HVIN  0x4u   /* M5-Bus HVIN / rs485 6-24 V input */

typedef struct {
    char name[32];
    char driver[24];
    char notes[96];
    char ports[T5_PROFILE_MAX_PORTS][12];
    uint8_t nports;
    t5_port_mode_t bus;
    uint8_t addrs[T5_PROFILE_MAX_ADDRS];
    uint8_t naddrs;
    uint32_t power;             /* T5_PROFILE_POWER_* */
    bool builtin;               /* built in, best-effort */
} t5_profile_t;

typedef struct {
    const t5_profile_t *profile;
    uint8_t matched;            /* profile addresses found in the scan */
    bool complete;              /* every profile address was found */
    bool port_ok;               /* the profile lists the scanned port */
} t5_profile_match_t;

/* Parse JSON text into up to max profiles. Returns the count parsed, or a
 * negative t5_err_t (T5_ERR_INVALID_ARG on malformed JSON or a profile
 * without a name). */
int t5_profile_parse(const char *json, size_t len, t5_profile_t *out, int max);

/* Built-ins plus every .json under dir (adds to what is loaded). Returns the
 * number of profiles added from dir. */
int t5_profiles_load_dir(const char *dir);
/* Drop loaded profiles, keep built-ins, load /data/profiles and /sd/profiles. */
int t5_profiles_reload(void);
int t5_profiles_count(void);
const t5_profile_t *t5_profiles_get(int i);

/* Open name in I2C mode, scan, close. BUSY if someone holds the port.
 * Returns the number of addresses found (fills up to max) or a negative t5_err_t. */
int t5_profiles_scan(const char *port, uint8_t *found, int max);

/* Rank profiles against a scan of port: complete matches first, then more
 * matched addresses, then ones that list the port, then fewer missing.
 * Returns the number of suggestions written (at most max). */
int t5_profiles_suggest(const char *port, const uint8_t *found, int nfound, t5_profile_match_t *out, int max);

/* For tests: forget loaded profiles. */
void t5_profiles_reset(void);

#ifdef __cplusplus
}
#endif
