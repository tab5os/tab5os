/* SPDX-License-Identifier: Apache-2.0
 * The system apps (plan 6.5-6.9, 7.6): Settings, Files, App Manager, Power,
 * Port Inspector and Camera. Built into the image but written against the
 * public SDK (sdk/include/t5) like any installed app.
 */
#pragma once
#include "t5_core/err.h"

/* Register them with the app runtime. Call before t5_apps_init(). */
t5_err_t t5_sysapps_register(void);
