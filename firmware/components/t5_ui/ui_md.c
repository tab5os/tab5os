/* SPDX-License-Identifier: Apache-2.0
 * md's view of the display: when a full ripple fits the frame budget, and
 * when a full-screen change has reached every framebuffer. Built on the
 * device and in the simulator, over t5_ui.h.
 */
#include "md/md.h"
#include "t5_ui/t5_ui.h"
#include "ui_priv.h"

/* Plan 4.7: a full ripple only when the frame budget allows; a plain fade in
 * low-power mode or after a frame that took longer than one refresh. */
static bool ripple_affordable(void)
{
    if (t5_ui_low_power()) return false;
    t5_ui_stats_t s;
    t5_ui_get_stats(&s);
    return s.render_us_max + s.copy_us_avg < T5_UI_REFRESH_MS * 1000u;
}

void ui_md_probes_init(void)
{
    md_ripple_set_probe(ripple_affordable);
    md_freeze_set_settled_probe(t5_ui_settled);
}
