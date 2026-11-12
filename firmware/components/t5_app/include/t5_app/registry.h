/* SPDX-License-Identifier: Apache-2.0
 * The app registry's limits and the built-in apps' ids. Macros only, so the
 * shell, the simulator and the tools can use them without the runtime.
 */
#pragma once

#include "t5_pkg/manifest.h"     /* T5_APP_ID_LEN, T5_APP_NAME_LEN, T5_APP_VERSION_LEN */

#define T5_APPS_MAX 48           /* built-in and installed apps listed at once */
#define T5_APPS_BUILTIN_MAX 24
#define T5_APP_ICON_LEN 8        /* an MD_ICON_* glyph, with its NUL */

#define T5_APP_ID_SETTINGS "com.t5.settings"
#define T5_APP_ID_FILES "com.t5.files"
#define T5_APP_ID_MANAGER "com.t5.appmanager"
#define T5_APP_ID_POWER "com.t5.power"
#define T5_APP_ID_PORTS "com.t5.ports"
#define T5_APP_ID_CAMERA "com.t5.camera"
#define T5_APP_ID_TERMINAL "com.t5.terminal"
#define T5_APP_ID_EDITOR "com.t5.editor"
#define T5_APP_ID_VIDEOS "com.t5.videos"

/* The Settings pages, by the launch argument that opens each one (the
 * quick settings tiles link to some). */
#define T5_SETTINGS_PAGE_WIFI "wifi"
#define T5_SETTINGS_PAGE_BLE "ble"
#define T5_SETTINGS_PAGE_DISPLAY "display"
#define T5_SETTINGS_PAGE_SOUND "sound"
#define T5_SETTINGS_PAGE_POWER "power"
#define T5_SETTINGS_PAGE_TIME "time"
#define T5_SETTINGS_PAGE_USB "usb"
#define T5_SETTINGS_PAGE_UPDATES "updates"
#define T5_SETTINGS_PAGE_DEVELOPER "developer"
#define T5_SETTINGS_PAGE_ABOUT "about"
