/* SPDX-License-Identifier: Apache-2.0
 * FreeRTOS thread-local storage slots the OS uses (device only). IDF's
 * pthread layer uses slot 0; ours are counted down from the top.
 */
#pragma once
#include "sdkconfig.h"

#define T5_TASK_TLS_APP_CTX    (CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS - 1)   /* t5_app_ctx_get() */
#define T5_TASK_TLS_NET_CANCEL (CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS - 2)   /* t5_net's cancel flag */

_Static_assert(CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 3, "need TLS slots for the app context and net");
