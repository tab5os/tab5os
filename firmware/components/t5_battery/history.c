/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_battery/history.h"

void t5_bat_hist_init(t5_bat_hist_t *h)
{
    memset(h, 0, sizeof *h);
    h->version = T5_BAT_HIST_VERSION;
}

void t5_bat_hist_add(t5_bat_hist_t *h, const t5_bat_hist_pt_t *p)
{
    h->pt[h->head] = *p;
    h->head = (uint16_t)((h->head + 1) % T5_BAT_HIST_LEN);
    if (h->count < T5_BAT_HIST_LEN) h->count++;
}

int t5_bat_hist_read(const t5_bat_hist_t *h, t5_bat_hist_pt_t *out, int max)
{
    int n = h->count < max ? h->count : max;
    int start = (h->head + T5_BAT_HIST_LEN - h->count) % T5_BAT_HIST_LEN;
    int skip = h->count - n;                 /* keep the newest when out is short */
    for (int i = 0; i < n; i++) out[i] = h->pt[(start + skip + i) % T5_BAT_HIST_LEN];
    return n;
}

bool t5_bat_hist_valid(const t5_bat_hist_t *h)
{
    return h->version == T5_BAT_HIST_VERSION && h->head < T5_BAT_HIST_LEN && h->count <= T5_BAT_HIST_LEN;
}
