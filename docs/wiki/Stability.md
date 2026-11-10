# Stability

## The failure model

A misbehaving installed app is closed, and the OS keeps running.

| What the app did | What the OS does | App callbacks |
| --- | --- | --- |
| Bad pointer or illegal instruction, on any app task or in an OS function it called | Closes the app, shows a toast, records the crash | None |
| Stack overflow | Same | None |
| Blocks the UI task for 2 s | "App not responding" dialog. Wait gives 5 s more; Close app ends it | None |
| Event handler hasn't returned in 3 s | Handler stopped, app closed | None |
| Task keeps a core over 90% busy for 3 s | Task moved to priority 1 | Keeps running |
| `on_alarm` runs past `max_seconds` | Stopped, recorded | None |
| Memory critically low for 1.5 s | App closed | **Runs them** |

Two in a row and the app stops being auto-launched (session restore, alarms). The person can still open it, and a clean exit clears the count.

### When the device still reboots

Containment isn't total. These still reboot, after which the crash guard names the app and saves a core dump to `/data/sys/dumps/<id>.elf`:

- A fault inside a critical section or an interrupt handler.
- A fault while a finished frame is being handed to the panel.
- A fault in a callback an OS driver runs on its own task, in some cases.
- A second fault within 30 s with no app frame on the stack.

And nothing catches a stray write that silently corrupts OS memory, since native code shares the address space. Bounds-check everything that came from outside. Python apps don't have this exposure.

## Handling errors

Almost every API returns `t5_err_t`, or a count that's negative on error.

| Error | Means | Usual response |
| --- | --- | --- |
| `T5_ERR_DENIED` | Permission, reserved address, risky pin | A bug: fix the manifest |
| `T5_ERR_BUSY` | Someone else owns it | Tell the person, using the reason from `t5_port_open_ex` |
| `T5_ERR_TIMEOUT` | Too slow | Retry with a limit |
| `T5_ERR_NO_MEM` | Out of memory or handles | Degrade, or report it |
| `T5_ERR_INVALID_ARG` | Bad argument | A bug |
| `T5_ERR_NOT_FOUND` | Device or file absent | Normal: show that state |
| `T5_ERR_IO` | Bus, device or connection failed | Retry with backoff |
| `T5_ERR_NOT_SUPPORTED` | No such feature | Disable it in the UI |
| `T5_ERR_STATE` | Wrong state: closed, or the wrong task | A bug, usually |
| `T5_ERR_NACK` | An I2C device didn't answer | Normal for an absent sensor |
| `T5_ERR_FAIL` | Generic | Report it |

`t5_err_str(e)` gives text. In Python, these are `OSError`s with an errno.

Many APIs are marked `T5_MUST_CHECK`, so ignoring one is a compiler warning. When it really doesn't matter, say so:

```c
T5_IGNORE(t5_port_close(p));   /* exiting anyway; nothing to do if this fails */
```

A `(void)` cast doesn't silence it under GCC. `T5_RETURN_ON_ERR(expr)` is handy in helpers. Build your own code with `-Werror` so nothing slips through:

```cmake
target_compile_options(myapp.elf PRIVATE -Werror)
```

**Tell the person.** A snackbar or an inline state beats silence. Keep the app usable: an absent sensor means an empty panel with a reason, not a crash.

## Design for what goes away

| Thing | How it goes away | Do |
| --- | --- | --- |
| Wi-Fi | Drops any time; radio off in Sleep with no net lock | Check `t5_net_status`, retry with backoff, show offline |
| SD card | Pulled at any moment; no card-detect line, polled once a second | Check `t5_storage_sd_state`, handle write errors, subscribe to `T5_EV_SD_REMOVED` |
| USB device | Unplugged; re-enumerates after Sleep | Handle errors, reopen on `T5_EV_USB_ATTACHED` |
| Camera | Closed by the OS at sleep | Reopen in `on_resume` |
| Grove module | Unplugged | Expect `T5_ERR_NACK` |
| Power | Button cuts power with no warning; battery critical skips callbacks | [Save early](Storage.md#durable-writes) |
| Wall clock | Unset after the RTC loses power | Check `t5_time_valid()` |
| The app itself | Exit or crash at any line | Keep on-disk state consistent |

Use timeouts everywhere. Bound every retry.

## Input safety

Native code shares memory with the OS. Treat as hostile everything from the network, files, or a port.

```c
int n = t5_http_fetch(&req, buf, sizeof buf - 1, &status);
if (n < 0) return n;
buf[n] = 0;                               /* now it's a string */
```

- Use `snprintf`, never `sprintf`. Use `strnlen`, and check sizes before `memcpy`.
- Validate lengths in binary protocols before indexing.
- Parse JSON with a bounded parser. [Weather](https://github.com/tab5os/weather)'s `json.c` is a small example.
- Keep recursion out of parsers, or give it a depth limit.
- Don't disable TLS verification outside testing.
- Don't build paths from untrusted strings. The OS's file rules guard against mistakes in native apps. They aren't a sandbox.

## Hardening checklist

Before you call an app done:

- [ ] No blocking calls on the UI task; `ui` shows a sensible longest loop
- [ ] LVGL touched only from the UI task, or under the lock
- [ ] Every result checked or explicitly ignored; builds with `-Werror`
- [ ] Every `malloc` checked
- [ ] Worker loops check `t5_app_running` and wait in short slices
- [ ] No shared state freed in `on_destroy`
- [ ] Late results can't touch deleted widgets
- [ ] Works with no Wi-Fi, no SD card, no sensor, and a denied permission
- [ ] Survives rotation if `orientation = "any"`
- [ ] Survives screen off and Sleep, then wake
- [ ] `app stress <id> 50` returns to baseline
- [ ] Settings written with `t5_storage_write_atomic`, and an old or corrupt file is handled
- [ ] All imports exist in `api_v1.sym`; only needed permissions declared
- [ ] Holds no wake lock when idle
- [ ] Reads OK in portrait and landscape; touch targets are at least 48 dp
- [ ] No text is cut off with long strings

See [Debugging and testing](Debugging-and-Testing.md) for how to run these.
