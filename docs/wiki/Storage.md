# Storage

## Filesystems

| Path | Medium | Size | Character |
| --- | --- | --- | --- |
| `/data` | Internal flash, LittleFS | 4.35 MB total, shared with system apps | Power-safe, always there, limited write endurance |
| `/sd` | microSD, FAT | The card | Large and removable; not power-safe |
| `/usb` | USB drive, FAT | The drive | Removable |

Plain stdio works: `fopen`, `fread`, `fwrite`, `fclose`, `fseek`, `ftell`, `fgets`, `fputs`, `fprintf`, `stat`, `mkdir`, `remove`, `rename`, `opendir`, `readdir`, `closedir`. Paths must be absolute.

## Which files an app can use

| Where | Access | Needs |
| --- | --- | --- |
| Its data folder, `t5_app_data_dir(app)` | Read and write, up to 1 MB | Nothing |
| Its install folder, `t5_app_install_dir(app)` | Read only | Nothing |
| [A file the person picks](#letting-the-person-choose) | That file, until the app exits | Nothing |
| Shared folders: `/sd/DCIM`, `/sd/Photos`, `/sd/Pictures`, `/sd/Music`, `/sd/Videos`, `/sd/Documents`, `/sd/Downloads`, `/data/user` | Read, or read and write | `shared_read`, or `shared_write` |
| The rest of `/sd` and `/usb` | Read and write | `all_files` |
| OS files and other apps' folders: `/data/sys`, `/data/apps`, `/data/appdata`, `/sd/apps`, `/sd/appdata` | Never | |

The rules apply to `fopen`, `opendir`, `stat`, `mkdir`, `remove`, `rename`, `t5_storage_write_atomic`, and to the paths given to the camera. A refused call returns `NULL` or -1 with `errno` set to `EACCES`, or `T5_ERR_DENIED`.

A path is refused if it's relative, contains `..`, or uses a name FAT would quietly read as another (a trailing dot or space, or an 8.3 alias like `APPDAT~1`).

`shared_read`, `shared_write` and `all_files` also ask the person at first use, and the answer is remembered. Where you make the call matters:

| Caller | First use |
| --- | --- |
| A worker task | The call waits for the answer, up to 60 s |
| The UI task | The prompt appears and **this call is denied**, because the UI task can't wait for a dialog |

So on the UI task, ask first:

```c
if (!t5_app_has_permission(app, T5_PERM_SHARED_READ)) {
    T5_IGNORE(t5_app_request_permission(app, T5_PERM_SHARED_READ, on_answer, s));   /* on_answer does the work */
}
```

Ask for the least you need. For native apps these rules stop mistakes, not a determined app, since it shares the OS's address space. For Python apps they're a real boundary.

## Letting the person choose

`t5_file_pick` opens a large system picker over your app, covering the SD card, USB drive and the internal shared folder. No permission is needed: choosing the file is the consent.

```c
static void picked(t5_app_t *app, const char *path, void *user)
{
    if (path) open_it(user, path);     /* NULL: the person backed out */
}

T5_IGNORE(t5_file_pick(app, "Open a track", "mp3,wav", 0, picked, s));
```

| Argument | Meaning |
| --- | --- |
| `exts` | `"jpg,png"`, or `NULL` for any file |
| `flags` | `T5_FILE_PICK_WRITE` also allows changing the file |
| Callback | On the UI task. Not called if the app exits first |

Call it on the UI task, one picker at a time. The grant lasts until the app exits, and up to 8 files can be open this way in one run. There's a [recipe](Cookbook.md#open-a-file-the-person-chooses).

Prefer this over `all_files`. A person understands "choose a file" much better than "manage all files".

## Where to put things

| Data | Where |
| --- | --- |
| Settings, small state | The data folder |
| Session state | `t5_app_state_save` (8 KB at most) |
| Read-only assets | The package, read from `t5_app_install_dir` |
| Photos, music, documents, downloads | The matching shared folder |
| Your own large caches or logs | `/sd/<your folder>`, with `all_files` |

The data folder is `/data/appdata/<id>`, is created for you, and is kept across upgrades. The install folder is replaced on upgrade.

Past the 1 MB quota, or when `/data` has under 128 KB free, writes fail: `fopen` for writing or `fwrite` with `ENOSPC`, or `t5_storage_write_atomic` with `T5_ERR_NO_MEM`. Handle that, and rotate anything that grows. A log in the data folder needs a cap.

## Durable writes

The power button can cut power **with no warning to the OS**. Battery-critical shutdown skips app callbacks. So save when the data changes. `on_suspend` isn't a guarantee.

```c
char path[200];
snprintf(path, sizeof path, "%s/settings.bin", t5_app_data_dir(app));
t5_err_t e = t5_storage_write_atomic(path, &cfg, sizeof cfg);
```

`t5_storage_write_atomic` writes `<path>.tmp`, fsyncs and renames. A reader sees the old or the new contents, never a mix. On `/data` that's truly atomic. On FAT the old file is removed first, so a power cut in that moment can lose it.

Write a version field at the start of your struct, so a later app version can recognise an old file. Debounce: flash has limited write cycles and `/data` is small. The [settings recipe](Cookbook.md#settings-that-survive) writes one second after the last change.

A file written with `fopen` and `fclose` may not have reached the medium if power is lost soon after. Use that for logs and streams, where losing the tail is acceptable.

## Appending logs

```c
FILE *f = fopen(path, "a");
if (f) {
    fputs(line, f);
    fclose(f);
}
```

Opening and closing for each line is simple and durable enough for slow logs. For fast logs, buffer in RAM and write in blocks of 4 KB or more on a worker task.

## What the C library is missing

The ABI exports a subset. Things you may reach for:

| Missing | Do this instead |
| --- | --- |
| `fsync` | `t5_storage_write_atomic` |
| `unlink` | `remove` |
| `rmdir` | `remove` may work on an empty directory, depending on the filesystem. Avoid needing it |
| `access` | `stat` |
| `ftruncate` | Rewrite the file |
| `open`, `read`, `write`, `lseek` | The `FILE *` calls |

## The SD card

```c
if (t5_storage_sd_state() != T5_SD_MOUNTED) {
    show_no_card();
    return;
}
t5_sd_info_t info;
if (t5_storage_sd_info(&info) == T5_OK) { /* info.free_bytes, info.total_bytes */ }
```

| State | Means |
| --- | --- |
| `T5_SD_ABSENT` | No card |
| `T5_SD_MOUNTED` | Ready |
| `T5_SD_FAILED` | A card that won't mount |
| `T5_SD_EJECTED` | Unmounted on request |

There's no card-detect line, so the OS polls once a second. A card can vanish between your check and your write, and that shows up as an I/O error, so handle it. Subscribe to `T5_EV_SD_INSERTED` and `T5_EV_SD_REMOVED` if you show the card's contents.

Shared folders may not exist yet on a given card. `mkdir` them first, and ignore a failure if the folder's already there.

Card speed varies enormously and is usually the bottleneck. A FAT32 volume with small clusters was measured at about 210 KB/s for sequential writes. Advise 32 KB clusters for anything like video.

`t5_storage_sd_eject` and `t5_storage_sd_format` need the `storage` permission. `format` is slow, so never call it on the UI task.

## Which task?

Small `/data` reads and atomic writes are fine on the UI task. The examples do this, and they take milliseconds. Anything big, and almost anything on `/sd`, belongs on a worker.

One caution for workers. App task stacks are in PSRAM, and ESP-IDF forbids operations that disable the cache from a PSRAM stack. In this build (XIP from PSRAM, nothing mapped from flash) writing `/data` shouldn't need to disable the cache, so it should work. That comes from reading the code. Logger's `on_alarm` relies on it, but it hasn't been measured under load. If you ever see an abort in `spi_flash_disable_interrupts_caches_and_other_cpu`, do the `/data` write on the UI task with `t5_ui_sync`.

## Filename limits

Paths apps can use are at most 191 bytes. `char path[200]` is the usual buffer. Keep names plain: FAT ignores case, so `/sd/Music` and `/sd/MUSIC` are the same folder.
