/* SPDX-License-Identifier: Apache-2.0
 * Host tests only.
 */
#pragma once

/* Forget both expanders, so the next t5_xpin_init() adopts them again. */
void t5_xpin_reset_for_test(void);
