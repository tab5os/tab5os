/* SPDX-License-Identifier: Apache-2.0
 * Shutdown record and boot wake reason: pure, host-tested.
 */
#include "t5_core/crc.h"
#include "t5_time/rtc.h"
#include "priv.h"

void t5_offrec_encode(const t5_offrec_t *r, uint8_t out[4])
{
    out[0] = T5_OFFREC_MAGIC;
    out[1] = r->reason;
    out[2] = r->flags;
    out[3] = t5_crc8(out, 3);
}

bool t5_offrec_decode(const uint8_t in[4], t5_offrec_t *out)
{
    if (in[0] != T5_OFFREC_MAGIC || t5_crc8(in, 3) != in[3]) {
        return false;
    }
    out->reason = in[1];
    out->flags = in[2];
    return true;
}

t5_wake_reason_t t5_power_classify_wake(const t5_wake_in_t *in)
{
    switch (in->reset) {
    case T5_RST_SW: return T5_WAKE_REBOOT;
    case T5_RST_PANIC: return T5_WAKE_CRASH;
    case T5_RST_BROWNOUT: return T5_WAKE_BROWNOUT;
    case T5_RST_DEEPSLEEP:
        return (in->have_rec && (in->rec.flags & T5_OFFREC_FLAG_CHARGE)) || in->deep_sleep_timer
                   ? T5_WAKE_CHARGE_TIMER : T5_WAKE_UNKNOWN;
    default:
        break;
    }
    /* Power-on paths: the PMS150G switched main power on. A record means we
     * cut power on purpose; the RTC alarm flag says whether the alarm (rather
     * than the button) brought it back. VLF means the RTC lost power too, so
     * its RAM is noise. */
    bool vlf = in->rtc_ok && (in->rtc_flags & T5_RTC_FLAG_VLF);
    if (in->have_rec && !vlf) {
        if ((in->rec.flags & T5_OFFREC_FLAG_ALARM) && (in->rtc_flags & T5_RTC_FLAG_AF)) {
            return T5_WAKE_RTC_ALARM;
        }
        return T5_WAKE_BUTTON;
    }
    if (in->reset == T5_RST_POWERON || vlf) {
        return T5_WAKE_COLD;
    }
    if (in->reset == T5_RST_WDT) {
        /* No record: power went without us asking. A short press of the
         * button power-cycles a running board (bench 0.6). */
        return T5_WAKE_POWER_CYCLE;
    }
    return T5_WAKE_UNKNOWN;
}
