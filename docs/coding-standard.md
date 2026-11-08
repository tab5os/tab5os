# Coding standard

Nov 7, 2026 · based on Holzmann's [Power of 10](https://en.wikipedia.org/wiki/The_Power_of_10:_Rules_for_Developing_Safety-Critical_Code)

The Power of 10 targets the bugs that hurt firmware like this: hangs, runaway loops, a heap that runs out, ignored errors. Applied word for word, some rules don't fit an OS that loads apps and runs LVGL and MicroPython, so Tab5 OS uses two profiles. The compiler, the linter and CI enforce as much of it as they can.

## Which profile

| Profile | Components | Rules |
| --- | --- | --- |
| **Strict**: must never fail, or reads untrusted input | `t5_power`, `t5_battery`, `t5_i2c`, `t5_app` (lifecycle, crash guard), `t5_ota`, `t5_pkg` (zip, inflate, TOML, manifests), `t5_dev` (the HTTP API), `t5_ports` (Modbus, profile JSON), and the recovery image | All ten, as below |
| **Standard**: UI, diagnostics, everything else we wrote | `md`, `t5_ui`, `t5_shell`, `t5_diag`, `t5_py`, the system apps, `main`, the other services | 1, 4, 6 and 10, and rule 7 for every call into a strict API |
| Vendored | `esp_driver_ppa`, everything in `managed_components/`, `third_party/` | ESP-IDF's own warning set |

The strict list lives in one place: `STRICT` in `firmware/CMakeLists.txt` (and in `recovery/CMakeLists.txt` for the recovery build). `tools/lint_std.py` reads it from there.

## The rules

**1. Simple control flow.** No `setjmp`/`longjmp`, except the recovery points that let the OS survive an app's crash (`t5_core/fault.h`; the files are listed in `JMP_OK` in `tools/lint_std.py`). `goto` only jumps forward to a single cleanup label. In strict code, no recursion unless it has an explicit depth limit; the few cases are listed in `RECURSION_OK` in `tools/lint_std.py`. Deleting a directory tree is iterative (`rm_rf_buf` in `t5_pkg/pkg.c`), because an app can nest its data far deeper than any task's stack allows. Strict code also builds with `-Wvla` and `-Wstack-usage=3072`, so every frame's size is known and small. A function that needs more puts its buffers on the heap (see `t5_pkg_install_ex`).

**2. Every loop has a bound.** Task main loops (`for (;;)` around a blocking wait) are the exception. Everything else ends after a known number of passes: waits on hardware time out, retries are counted, and loops over input stop at the input's length. Examples: the power state machine takes at most `T5_PWR_SLEEP` steps each way, the dev-mode HTTP API allows a stalled client three receive timeouts (`recv_some`), and the INA226 boot sample gets three tries.

**3. No allocation where running out would hurt.** Apps, LVGL and MicroPython allocate at run time, so this can't be a global rule. In strict code, keep state in static tables sized at compile time (wake locks, rail holds, suspend hooks, I2C devices), and allocate only at init or for a bounded job (an install, an upload), checking every result.

**4. Functions fit on a page.** About 60 lines. This is a review guideline, not a hard check. UI construction functions are the usual offenders, and they're low risk.

**5. Assert invariants, and keep the assertions in release builds.** `T5_ASSERT(cond)` (`t5_core/err.h`) aborts with the expression and its location, which the panic output and core dump keep, and the device restarts. Use it for "can't happen" states: a state machine that didn't move, an index a check upstream already bounded, an unlock by a task that doesn't own the lock. Don't use it for input checks: bad arguments get `T5_ERR_INVALID_ARG`. Current assertions: power state stepping, rail and wake-lock table indexes, one foreground app at a time, I2C lock ownership, the tree walk in `rm_rf_buf`.

**6. Smallest scope.** Declare variables where they're used. Strict code builds with `-Wshadow`.

**7. Check every result.** Strict components mark their `t5_err_t` APIs with `T5_MUST_CHECK` (GCC's `warn_unused_result`), and `-Werror` makes ignoring one a build error everywhere, apps included (for apps it's a warning unless they build with `-Werror`). `tools/lint_std.py` fails if a new strict API leaves the mark off. When you call one:

| Situation | Write |
| --- | --- |
| The caller can handle it | `T5_RETURN_ON_ERR(...)`, or check it and act |
| A user action failed (system apps) | `sa_report(e, "open the app")`, which shows a snackbar |
| Nothing to do but report it | `T5_LOG_ON_ERR(TAG, ...)` (`t5_core/log.h`) |
| Failure really doesn't matter | `T5_IGNORE(...)` with a comment saying why. GCC doesn't accept a `(void)` cast for this |

Parameters coming from outside the module are validated and refused with an error code.

**8. Keep the preprocessor simple.** Includes, constants, and small statement macros (`T5_RETURN_ON_ERR`, `T5_ASSERT`, logging). Token pasting and variadic macros only where an interface needs them: `T5_APP(...)` is part of the app ABI, and the logging macros forward to ESP-IDF's.

**9. Pointers.** The Power of 10's "no function pointers" doesn't fit: callbacks, app lifecycle hooks and ELF symbol binding all rely on them. Instead, callbacks are registered into fixed tables and checked for NULL before the call. Avoid more than one level of dereference in one expression where it hurts reading.

**10. Zero warnings, and the analyzer.** Every component we wrote builds with ESP-IDF's `-Wall -Wextra -Werror`, with the warnings IDF downgrades (`-Wextra`, unused functions and variables, deprecated declarations) turned back into errors (`firmware/cmake/t5_standard.cmake`). Strict code adds `-Wshadow -Wformat=2 -Wvla -Wstack-usage=3072 -Werror=unused-result`. Host tests build with `-Wall -Wextra -Werror`. GCC's static analyzer (`-fanalyzer`) runs on the strict components in CI, and any finding fails the build.

## Running the checks

```sh
t5 build                  # zero warnings or it fails
t5 analyze                # -fanalyzer on the strict components (build-analyze-<rev>/)
t5 lint                   # tools/lint_std.py with the UI density lint and the ABI check
t5 host-test
```

CI runs all of them (the `analyze` and `lint` jobs in `.github/workflows/ci.yml`), plus the analyzer on the recovery image.

## What the first pass found

Turning the rules on (Nov 7, 2026) found these, all fixed:

- `t5_battery_init` ignored a failed first INA226 read and booted the fuel gauge at 0 mV. That reads as an empty battery, which can power the Tab5 off. The read now gets three tries and init fails without it; the charge-mode check boots normally instead of turning off. Host test: `test_service_init_needs_a_first_sample`.
- The dev-mode HTTP API retried receive timeouts forever, so a stalled client held the server. `/py` also ran a script that was cut off partway through the upload. Now: three timeouts, and a short body is refused.
- `rm_rf_buf` recursed once per directory level. A deeply nested app data folder could overflow the stack on uninstall. Now iterative; host test `test_remove_deep_data_dir`.
- The power state machine's step loops had no bound: if the arbiter ever returned a state the steps don't cover, the power task would spin forever. Now bounded and asserted.
- Installing over a running app went ahead if the app didn't stop within 3 s. Now refused.
- Modbus didn't check that the frame had finished sending before listening on RS-485, and didn't check the length of the write echo. Both fixed.
- Recovery's `rm_tree` had no depth limit, and `cmd_shot` leaked its buffer when there was no framebuffer.
- The I2C recovery hook registration in `t5_xpin_init` could fail silently, leaving the expander outputs unrestored after a bus glitch. It's now an init error.
- 51 other ignored results: registrations that only fail when a table is too small (now logged), user actions in the system apps that failed silently (now a snackbar), and best-effort calls (now `T5_IGNORE` with a reason).
