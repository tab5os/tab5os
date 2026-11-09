/* SPDX-License-Identifier: Apache-2.0
 * Who may open which port. Apps may open only the ports their manifest
 * lists (plan 5.9); the app runtime knows the manifests and installs the
 * check, which t5_port_open_ex() asks for every open. Not part of the SDK.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* May the caller open port? If not, false with the reason in why. */
typedef bool (*t5_ports_access_fn_t)(const char *port, char *why, size_t whylen);
void t5_ports_set_access_check(t5_ports_access_fn_t fn);

#ifdef __cplusplus
}
#endif
