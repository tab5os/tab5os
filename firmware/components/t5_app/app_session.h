/* SPDX-License-Identifier: Apache-2.0
 * The session: the app in front, opened again after a soft off. Kept in the
 * system directory and written by autosave, so a power cut loses at most one
 * autosave period.
 */
#pragma once

#include "t5_core/err.h"

/* Read what the last run saved (the app runtime's init). */
void app_session_load(void);
/* The app the last run had in front; "" if none. */
const char *app_session_restored(void);
/* The app now in front (NULL or "" for none), written at the next autosave. */
void app_session_set(const char *id);
/* The autosave saver. */
t5_err_t app_session_save(void *arg);
