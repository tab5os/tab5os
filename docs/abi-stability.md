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
| Macro constants compiled into apps, and the LVGL configuration | `sdk/abi/layout_v1.lock` | `tools/abi/layout.py check` |
| Every public header compiles by itself | | `tools/abi/layout.py check` |
| Each exported prototype matches its header | `sdk/abi/api_v1.sym` | The firmware build (`abi_table.c`) |

`t5 abi-check` runs both checks locally. CI runs them on every push and pull request. The check compares against the lock files in the tree, so it works on direct pushes to `main` too.

Python apps depend on the `t5` module rather than this ABI.

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
python tools/abi/layout.py gen firmware/build-rev1
```

**Releasing level 1 is the point of no return.** Do it when the first public SDK ships:

```sh
python3 tools/abi/abi.py freeze sdk/abi/api_v1.sym --release
```

After that, `freeze` refuses to rewrite the lock.

## Adding to a released ABI

Additions after release raise the API level, because an app built against a new symbol can't run on an older OS at the same level.

1. Raise `T5_API_LEVEL` (`t5_app/app.h` and `t5_pkg/manifest.h`) and `API_LEVEL` in `tools/t5/t5pack.py`.
2. Add the symbol to `api_v1.sym` with `since=2`.
3. A new callback in `t5_app_desc_t` is read only when `desc->api` is at least the level that added it. Older apps' descriptors are shorter, so reading past the end is a bug.

The app's manifest `api` key says the level it needs, and an OS with a lower level refuses it at install.

## What isn't covered

- **Functions declared in headers but not exported.** The headers come from the firmware and show more than the ABI, so calling one of these compiles and is then refused at install. Only `api_v1.sym` counts.
- **Internals of opaque types** such as `lv_obj_t` and `t5_app_t`. Apps hold pointers and never see inside them.
- **`rev3` builds.** The check runs on `rev1`.
