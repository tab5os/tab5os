/* SPDX-License-Identifier: Apache-2.0
 * MicroPython configuration for Tab5 OS (plan 8.1, 8.2).
 *
 * Containment is the point of this file. Every switch that could let Python
 * reach memory, registers or peripherals without going through `t5` is set
 * here explicitly, so a change in MicroPython's feature-level defaults can't
 * turn one on. tools/py/check_modules.py checks the resulting module list.
 */
#pragma once
#include <stdint.h>
#include <alloca.h>
#include <sys/types.h>
#include "sdkconfig.h"

#define MICROPY_CONFIG_ROM_LEVEL                (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

/* ---- containment (plan 8.2): never enable these ------------------------------ */
#define MICROPY_EMIT_RV32                       (0)   /* @micropython.native / viper */
#define MICROPY_EMIT_INLINE_RV32                (0)   /* @micropython.asm_rv32 */
#define MICROPY_EMIT_XTENSA                     (0)
#define MICROPY_EMIT_XTENSAWIN                  (0)
#define MICROPY_EMIT_THUMB                      (0)
#define MICROPY_EMIT_INLINE_THUMB               (0)
#define MICROPY_EMIT_ARM                        (0)
#define MICROPY_EMIT_X64                        (0)
#define MICROPY_EMIT_X86                        (0)
#define MICROPY_PERSISTENT_CODE_LOAD            (0)   /* no .mpy: could carry machine code */
#define MICROPY_PY_MACHINE                      (0)   /* machine.mem32, Pin, I2C... */
#define MICROPY_PY_UCTYPES                      (0)   /* uctypes.bytearray_at(addr) */
#define MICROPY_PY_OS                           (0)   /* replaced by the sandboxed os in fs.c */
#define MICROPY_VFS                             (0)   /* no mounts: fs.c checks every path */
#define MICROPY_PY_VFS                          (0)
#define MICROPY_PY_SOCKET                       (0)   /* upstream lwIP socket: off. `socket` is mod_socket.c, on t5 handles */
#define MICROPY_PY_LWIP                         (0)
#define MICROPY_PY_NETWORK                      (0)
#define MICROPY_PY_SSL                          (0)
#define MICROPY_PY_BLUETOOTH                    (0)
#define MICROPY_PY_SELECT                       (1)   /* extmod's: polls stream objects, i.e. t5 sockets */
#define MICROPY_PY_SELECT_SELECT                (1)
#define MICROPY_PY_SELECT_POSIX_OPTIMISATIONS   (0)
#define MICROPY_PY_ONEWIRE                      (0)
#define MICROPY_PY_THREAD                       (0)   /* one VM, one task */
#define MICROPY_PY_SYS_SETTRACE                 (0)
#define MICROPY_PY_MARSHAL                      (0)
#define MICROPY_MODULE_FROZEN_MPY               (0)
#define MICROPY_MODULE_FROZEN_STR               (1)   /* stdlib as source (tools/py/freeze_str.py), compiled on import */
#define MICROPY_PY_BUILTINS_INPUT               (0)   /* no stdin outside the REPL */

/* ---- not needed, and they cost flash ------------------------------------------ */
#define MICROPY_PY_FRAMEBUF                     (0)
#define MICROPY_PY_DEFLATE                      (1)   /* gzip/zlib streams */
#define MICROPY_PY_ASYNCIO                      (1)   /* _asyncio; the package is frozen source */
#define MICROPY_PY_CRYPTOLIB                    (0)
#define MICROPY_PY_HASHLIB_MD5                  (0)
#define MICROPY_PY_HASHLIB_SHA1                 (0)
#define MICROPY_PY_BTREE                        (0)
#define MICROPY_PY_CMATH                        (0)
#define MICROPY_PY_WEBSOCKET                    (0)
#define MICROPY_PY_WEBREPL                      (0)

/* ---- core ---------------------------------------------------------------------- */
#define MICROPY_ENABLE_COMPILER                 (1)
#define MICROPY_ENABLE_GC                       (1)
#define MICROPY_ENABLE_FINALISER                (1)   /* files and ports close when collected */
#define MICROPY_NLR_SETJMP                      (1)
#define MICROPY_OBJ_REPR                        (MICROPY_OBJ_REPR_A)
#define MICROPY_ALLOC_PATH_MAX                  (256)
#define MICROPY_STACK_CHECK                     (1)
#define MICROPY_STACK_CHECK_MARGIN              (2048)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF  (1)
#define MICROPY_EMERGENCY_EXCEPTION_BUF_SIZE    (256)
#define MICROPY_LONGINT_IMPL                    (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_ERROR_REPORTING                 (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_WARNINGS                        (1)
#define MICROPY_USE_INTERNAL_ERRNO              (0)
#define MICROPY_USE_INTERNAL_PRINTF             (0)
#define MICROPY_OPT_COMPUTED_GOTO               (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT          (1)   /* .py modules next to the script and in lib/ */
#define MICROPY_READER_POSIX                    (0)
#define MICROPY_READER_VFS                      (0)
#define MICROPY_MODULE___FILE__                 (1)

/* Stopping: Ctrl-C raises KeyboardInterrupt; app exit aborts the VM in a way
 * Python can't catch. */
#define MICROPY_KBD_EXCEPTION                   (1)
#define MICROPY_ENABLE_SCHEDULER                (1)   /* UI callbacks run on the VM task */
#define MICROPY_SCHEDULER_DEPTH                 (16)
#define MICROPY_ENABLE_VM_ABORT                 (1)
#define MICROPY_PYEXEC_ENABLE_VM_ABORT          (1)
#define MICROPY_PYEXEC_ENABLE_EXIT_CODE_HANDLING (1)

/* REPL (Terminal): driven a character at a time. */
#define MICROPY_HELPER_REPL                     (1)
#define MICROPY_REPL_EVENT_DRIVEN               (1)
#define MICROPY_REPL_AUTO_INDENT                (0)   /* the Terminal pre-fills indentation itself */
#define MICROPY_REPL_EMACS_KEYS                 (0)
#define MICROPY_REPL_INFO                       (0)

/* ---- modules ------------------------------------------------------------------- */
#define MICROPY_PY_SYS                          (1)
#define MICROPY_PY_SYS_PLATFORM                 "tab5"
#define MICROPY_PY_SYS_STDFILES                 (1)
#define MICROPY_PY_SYS_EXIT                     (1)
#define MICROPY_PY_IO                           (1)
#define MICROPY_PY_GC                           (1)
#define MICROPY_PY_MICROPYTHON_MEM_INFO         (1)
#define MICROPY_PY_TIME                         (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS            (1)
#define MICROPY_PY_TIME_INCLUDEFILE             "modtime_t5.h"
#define MICROPY_EPOCH_IS_1970                   (1)
#define MICROPY_PY_RANDOM                       (1)
#define MICROPY_PY_RANDOM_EXTRA_FUNCS           (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC        (t5_py_random_seed())
#define MICROPY_PY_HASHLIB                      (1)   /* sha256 */
#define MICROPY_PY_JSON                         (1)
#define MICROPY_PY_RE                           (1)
#define MICROPY_PY_BINASCII                     (1)
#define MICROPY_PY_HEAPQ                        (1)
#define MICROPY_PY_STRUCT                       (1)
#define MICROPY_PY_ARRAY                        (1)
#define MICROPY_PY_COLLECTIONS                  (1)
#define MICROPY_PY_MATH                         (1)
#define MICROPY_PY_ERRNO                        (1)
#define MICROPY_PY_PLATFORM                     (1)
#define MICROPY_PY_BUILTINS_HELP                (1)
#define MICROPY_PY_BUILTINS_HELP_TEXT           t5_py_help_text
#define MICROPY_PY_BUILTINS_HELP_MODULES        (1)

/* ---- port ------------------------------------------------------------------------ */
#define MICROPY_HW_BOARD_NAME                   "M5Stack Tab5"
#define MICROPY_HW_MCU_NAME                     "ESP32-P4"
#define MICROPY_MPHALPORT_H                     "mphalport.h"
#define MICROPY_MAKE_POINTER_CALLABLE(p)        ((void *)((mp_uint_t)(p)))
#define MP_SSIZE_MAX                            (0x7fffffff)
#define MP_STATE_PORT                           MP_STATE_VM
typedef long mp_off_t;

/* The scheduler's queue is touched by the UI task (widget callbacks) and the
 * VM task, possibly on different cores: a spinlock critical section. */
uint32_t t5_py_atomic_begin(void);
void t5_py_atomic_end(uint32_t state);
#define MICROPY_BEGIN_ATOMIC_SECTION()          t5_py_atomic_begin()
#define MICROPY_END_ATOMIC_SECTION(state)       t5_py_atomic_end(state)

/* Wait for something to happen (a scheduled callback, an interrupt, input). */
void t5_py_wait_event(uint32_t timeout_ms);
#define MICROPY_INTERNAL_WFE(TIMEOUT_MS)        t5_py_wait_event(TIMEOUT_MS)

uint32_t t5_py_random_seed(void);
extern const char t5_py_help_text[];
