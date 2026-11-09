/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_camera/camera.h"
#include "t5_ui/t5_ui.h"
#include "board_media.h"

static int rotation_deg(void)
{
    return (int)t5_ui_rotation() * 90;
}

static void ui_call(void (*fn)(void *arg), void *arg)
{
    if (t5_ui_on_ui_task()) {
        fn(arg);
        return;
    }
    t5_ui_lock();
    fn(arg);
    t5_ui_unlock();
}

void board_media_wire(void)
{
    const t5_camera_host_t host = {
        .rotation_deg = rotation_deg,
        .on_ui_task = t5_ui_on_ui_task,
        .lend_ram = t5_ui_lend_internal,
        .reclaim_ram = t5_ui_reclaim_internal,
        .ui_call = ui_call,
    };
    t5_camera_set_host(&host);
    /* The UI starts last; by then internal RAM has no 100 KB piece left for
     * its draw block (which the encoder borrows), so take it now. */
    t5_ui_reserve_internal();
}
