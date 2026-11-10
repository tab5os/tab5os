/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_app/isolate.h"

t5_iso_verdict_t t5_iso_blame(const t5_iso_fault_t *f)
{
    if (!f || f->text_hi <= f->text_lo) return T5_ISO_NONE;   /* built-ins and scripts: OS code */
    /* A task the app created is the app's whatever it was running: an OS
     * call with a bad argument from the app's own task is still the app. */
    if (f->apps_task) return T5_ISO_END_TASK;
    /* A system task: only with a recovery point to go back to, and only if
     * the app is on the call chain. */
    if (!f->has_point || !f->point_ok || !f->unwind_safe) return T5_ISO_NONE;
    if (t5_iso_in_text(f, f->pc) || t5_iso_in_text(f, f->ra)) return T5_ISO_UNWIND;
    /* Deeper in the OS (an LVGL call with a bad pointer, a wait that never
     * ends): a return address into the app between here and the point. */
    for (size_t i = 0; f->stack && i < f->words; i++) {
        if (t5_iso_in_text(f, f->stack[i])) return T5_ISO_UNWIND;
    }
    /* The app called into the OS as its last act (a tail call leaves no
     * frame), or handed it a bad pointer that blew up later in the same
     * iteration. A wild jump or bad instruction is not guessed at. */
    if (f->working_for_app && f->data_fault && f->guess_ok) return T5_ISO_UNWIND_GUESS;
    return T5_ISO_NONE;
}

bool t5_iso_heap_room(size_t n, size_t psram_free)
{
    return psram_free >= T5_ISO_PSRAM_RESERVE && psram_free - T5_ISO_PSRAM_RESERVE >= n;
}

size_t t5_iso_arena_next(size_t last)
{
    if (!last) return T5_ISO_ARENA_FIRST;
    return last >= T5_ISO_ARENA_MAX / 2 ? T5_ISO_ARENA_MAX : last * 2;
}

t5_iso_l2_action_t t5_iso_l2_check(t5_iso_l2_t *s, size_t l2_free, uint32_t now_ms)
{
    if (!s->released && l2_free < T5_ISO_L2_CRITICAL) {
        s->released = true;
        s->low = true;
        s->since_ms = now_ms;
        return T5_ISO_L2_RELEASE;
    }
    if (s->released && l2_free >= T5_ISO_L2_PLENTY) {
        s->released = false;
        s->low = false;
        return T5_ISO_L2_RETAKE;
    }
    /* What is free, not counting the reserve just handed out. */
    size_t effective = s->released && l2_free >= T5_ISO_L2_RESERVE ? l2_free - T5_ISO_L2_RESERVE : l2_free;
    if (effective >= T5_ISO_L2_CRITICAL) {
        s->low = false;
        return T5_ISO_L2_NONE;
    }
    if (!s->low) {
        s->low = true;
        s->since_ms = now_ms;
        return T5_ISO_L2_NONE;
    }
    if (now_ms - s->since_ms < T5_ISO_LOW_FOR_MS) return T5_ISO_L2_NONE;
    s->low = false;
    return T5_ISO_L2_CLOSE_APP;
}

bool t5_iso_mem_check(t5_iso_mem_t *s, size_t psram_free, uint32_t now_ms)
{
    if (psram_free >= T5_ISO_PSRAM_CRITICAL) {
        s->low = false;
        return false;
    }
    if (!s->low) {
        s->low = true;
        s->since_ms = now_ms;
        return false;
    }
    if (now_ms - s->since_ms < T5_ISO_LOW_FOR_MS) return false;
    s->low = false;
    return true;
}

bool t5_iso_hog(uint8_t *strikes, uint64_t ran_us, uint64_t wall_us)
{
    if (!wall_us || ran_us * 100u < wall_us * T5_ISO_HOG_PCT) {
        *strikes = 0;
        return false;
    }
    if (*strikes < 255) (*strikes)++;
    return *strikes >= T5_ISO_HOG_STRIKES;
}

bool t5_iso_net_enter(t5_iso_net_t *w, bool waits, bool exiting)
{
    if (waits && exiting) return false;
    (*w)++;
    return true;
}

bool t5_iso_net_leave(t5_iso_net_t *w)
{
    if (t5_iso_net_inside(*w)) (*w)--;
    return (*w & T5_ISO_NET_ORPHAN) && !t5_iso_net_inside(*w);
}

bool t5_iso_net_delete(t5_iso_net_t *w)
{
    if (!t5_iso_net_inside(*w)) return true;
    *w |= T5_ISO_NET_ORPHAN;
    return false;
}
