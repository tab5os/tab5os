# App ABI stability

Nov 8, 2026 · [SDK guide](sdk.md) · [API reference](api-reference.md)

An app is compiled once and has to keep working as the OS changes. This page says what's promised, and how the promise is enforced.

## The promise

An app built for API level N runs on any OS at level N or higher, unmodified, without a rebuild.

An app depends on more than function names. All of this is covered:

| Part of the ABI | Where it's recorded | Checked by |
| --- | --- | --- |
| Which functions and data exist, with their prototypes and permissions | `sdk/abi/api_v1.lock` | `tools/abi/abi.py check` |
| Struct layouts, enum values, the types behind every exported function | `sdk/abi/layout_v1.lock` | `tools/abi/layout.py check` |
| Macro constants and enumerators compiled into apps, from the OS and from the libraries, and the LVGL configuration | `sdk/abi/layout_v1.lock` | `tools/abi/layout.py check` |
| Every public header compiles by itself, against the headers the SDK release ships | | `tools/abi/layout.py check` |
| Each exported prototype matches its header | `sdk/abi/api_v1.sym` | The firmware build (`abi_table.c`) |

`t5 abi-check` runs both checks locally. CI runs them on every push and pull request. The check compares against the lock files in the tree, so it works on direct pushes to `main` too.

Python apps depend on the `t5` module rather than this ABI.

### What the layout lock records

`layout.py` compiles the public headers the way an app does, against the staged SDK headers (`sdk/cmake/sysroot.py`), and records:

- **Types:** every struct, union, enum and typedef named in an exported prototype, the types they reach, and a few an app reaches another way (`EXTRA_ROOTS`: event payloads, `t5_app_desc_t`, `lv_image_dsc_t`, the BSD socket structs, and `lv_obj_class_t` for custom widgets).
- **Enumerators:** every enumeration defined in the OS's headers and in the libraries' headers, whether or not a prototype names it: an app can write `LV_OPA_COVER` or `LV_ALIGN_CENTER` and the value is compiled in.
- **The OS's macros:** every macro defined in a Tab5 OS header the SDK reaches, as written.
- **Library macros:** for the libraries apps call into (picolibc and GCC's own headers, lwIP, FreeRTOS, LVGL: the `ext` symbols in `api_v1.sym`), every macro their headers define that an app could compile in as a value. Nobody lists them by hand: the compiler evaluates each candidate as an integer or floating constant, a string literal, or for a function-like macro at sample arguments (`LV_PCT(1)`, `LV_PCT(100)`), and the value and its type are recorded, such as `macro EACCES = 13 int4` or `macro LV_SYMBOL_OK = "\xef\x80\x8c" str`. Candidates that don't evaluate to a constant (function aliases, statements, pointers) drop out. Left out on purpose: names with a leading underscore (reserved for the implementation), macros built from `__FILE__`/`__LINE__`, and the libraries' tuning headers (`lv_conf*.h`, `lwipopts.h`, `lwip/opt.h`, `FreeRTOSConfig.h`). Their options reach apps through values that are recorded (`FD_SETSIZE`, `pdMS_TO_TICKS(x)`, `LV_DPX(x)`) and the `CONFIG_LV_*` settings.

That covers errno values, socket and `fcntl` constants (`AF_*`, `SOCK_*`, `SOL_SOCKET`, `SO_*`, `IPPROTO_*`, `MSG_*`, `O_*`, `F_*`), `SEEK_*`, `FD_SETSIZE`, `<limits.h>`, `<math.h>`, `LV_SIZE_CONTENT`, `LV_COORD_MAX`, `LV_RADIUS_CIRCLE`, `LV_ANIM_REPEAT_INFINITE`, `LV_OPA_*`, `LV_SYMBOL_*` and the rest: about 3,600 items in all.

### Custom widgets

`lv_obj_class_create_obj` and `lv_obj_class_init_obj` are exported and `lv_obj_class_t`'s layout is locked, so an app can define its own widget class ([UI guide](wiki/UI-Guide.md#custom-widgets)). `lv_obj_t`'s layout is **not** part of the ABI, so a class must leave `instance_size` at 0 (the base class's size) and keep its own data outside the object, for example in a block it allocates in `constructor_cb`, points to with `lv_obj_set_user_data` and frees in `destructor_cb`. A widget that embeds `lv_obj_t` in a bigger struct compiles, but isn't promised to run on a later OS.

## What you can and can't change

**Always allowed:** adding a new type, enumerator or macro. Fixing a bug without changing how a function is called. Changing anything not in the locks.

**Never, once a symbol or type is locked:**

- Removing or renaming a function, or changing its prototype.
- Changing a function's permission group.
- Changing a struct's size, or a member's name, type or offset. That includes adding a member at the end: a struct an app allocated is the old size, and the OS would write past it.
- Changing an enum value or a recorded macro.
- Changing an LVGL feature setting that the locks record, or upgrading LVGL within a level.

To extend a config or result struct, add a new function that takes a new struct, such as `t5_foo_open_ex()`.

**Behaviour** is part of the contract even though no tool can check it. A function that returned quickly can't start blocking, and a call that worked on any task can't become UI-task only.

## Candidate and released

The first line of `api_v1.lock` gives the status.

| Status | New symbols | Locked symbols |
| --- | --- | --- |
| `candidate` (now) | Allowed in level 1 | Can't change without re-freezing |
| `released` | Need `since=2` and a new API level | Can't change at all |

While level 1 is a candidate, a deliberate change is possible and visible: edit the symbol, run the commands below, and the diff of the lock file shows exactly what broke.

```sh
python3 tools/abi/abi.py freeze sdk/abi/api_v1.sym
python tools/abi/layout.py gen firmware/build
```

**Releasing level 1 is the point of no return.** Do it when the first public SDK ships:

```sh
python3 tools/abi/abi.py freeze sdk/abi/api_v1.sym --release
```

After that, `freeze` refuses to rewrite the lock.

## Adding to a released ABI

Additions after release raise the API level, because an app built against a new symbol can't run on an older OS at the same level.

1. Raise `T5_API_LEVEL` in `t5_pkg/manifest.h` (the only definition: `t5_app/app.h` includes it, and `tools/t5/t5pack.py` reads it).
2. Add the symbol to `api_v1.sym` with `since=2`.
3. A new callback in `t5_app_desc_t` is read only when `desc->api` is at least the level that added it. Older apps' descriptors are shorter, so reading past the end is a bug.

The app's manifest `api` key says the level it needs, and an OS with a lower level refuses it at install.

## What isn't covered

- **Functions declared in headers but not exported.** The headers come from the firmware and show more than the ABI, so calling one of these compiles; `t5 pack` then refuses the app (it checks the ELF's imports against `api_v1.sym`), as the installer would. Only `api_v1.sym` counts.
- **Internals of opaque types** such as `lv_obj_t`, `lv_event_t` and `t5_app_t`. Apps hold pointers and never see inside them. That includes custom widget classes, which must not embed `lv_obj_t` (see above).
