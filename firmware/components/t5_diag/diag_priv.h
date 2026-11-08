/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void diag_register_sys(void);
void diag_register_i2c(void);
void diag_register_exp(void);
void diag_register_dev(void);
void diag_register_bench(void);
void diag_register_log(void);
void diag_register_elf(void);
void diag_register_power(void);
void diag_register_net(void);
void diag_register_ports(void);
void diag_register_ui(void);
void diag_register_app(void);
void diag_register_media(void);
void diag_register_prof(void);
void diag_register_py(void);
void diag_print_boot_banner(void);

/* Parse "0x43", "67" etc. Returns false if not a number in [min, max]. */
bool diag_parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out);
