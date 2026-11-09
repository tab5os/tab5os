/* SPDX-License-Identifier: Apache-2.0
 * Audio service (plan 7.1-7.3). Two tasks at the top priority:
 *
 *   output  runs while there is something to play: app streams or system
 *           sounds. Every 10 ms it pulls each stream (resampled to
 *           T5_AUDIO_RATE stereo), mixes them with the active sounds (MEDIA
 *           streams ducked while a sound or an ALERT/SPEECH stream plays,
 *           with a ramp so there is no click), keeps a mono copy for the
 *           echo canceller, and writes the block to I2S (which paces the
 *           loop). After 2 s with nothing to play the DAC powers down.
 *           Idle, it wakes once a second to watch the headphone jack.
 *   input   runs while an input stream is open: reads 10 ms from the mics,
 *           optionally cancels the device's own output, converts to the
 *           stream's format and queues it for the reader.
 *
 * Only the output task touches the codecs: opening and closing them,
 * volume, mute and mic gain, all between its writes. esp_codec_dev
 * reconfigures the shared I2S pair on every open and close, which must never
 * overlap a read or a write. The input task only reads. Others ask through
 * flags and requests and wake the output task.
 *
 * Holds (Sleep, USB-C storage borrowing the I2S pads) close both codecs and
 * keep them closed: streams stay open but go silent until the hold ends.
 *
 * Underruns: a stream that has started playing and then has less than a
 * block queued counts one underrun per gap (the plan's endurance metric).
 * Wake lock: an open, unpaused output stream holds T5_LOCK_AUDIO, so the
 * screen can go off while playback continues.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "t5_audio/audio.h"
#include "t5_audio/hold.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_power/autosave.h"
#include "t5_power/power.h"
#include "t5_storage/paths.h"
#include "t5_storage/t5_storage.h"
#include "audio_hw.h"

static const char *TAG = "t5.audio";

#define BLOCK_MS 10
#define BLOCK_FRAMES (T5_AUDIO_RATE * BLOCK_MS / 1000)
#define BLOCK_SAMPLES (BLOCK_FRAMES * T5_AUDIO_CH)
#define IDLE_CLOSE_MS 2000
#define HP_POLL_MS 500
#define MAX_VOICES 4
#define AUDIO_PRIO (configMAX_PRIORITIES - 2)
#define SETTINGS_FILE "audio.bin"
#define SETTINGS_VERSION 1
#define REF_RING (T5_AUDIO_RATE / 5)      /* 200 ms of mono reference */
#define AEC_RATE 16000                    /* T5_AUDIO_RATE / 3 */
#define AEC_TAPS 512                      /* 32 ms */
#define AEC_DELAY (AEC_RATE * AUDIO_HW_OUT_LATENCY_MS / 1000)
#define MIC_OPEN_MS 1000
#define MIC_CLOSE_MS 500
#define RELEASE_WAIT_MS 1000          /* for a task to let go of a closing stream */
#define HOLD_WAIT_MS 100
#define HOLD_MIC_WAIT_MS 50           /* the input task's block in flight */
#define HP_IDLE_POLL_MS 1000          /* the jack, while nothing plays */
#define UNITY_Q15 32768
#define DUCK_Q15 8192                 /* -12 dB */
#define DUCK_DOWN_STEP ((UNITY_Q15 - DUCK_Q15) / 10)    /* 100 ms */
#define DUCK_UP_STEP ((UNITY_Q15 - DUCK_Q15) / 30)      /* 300 ms */

typedef struct {
    int volume;
    bool muted;
    bool sounds;
    float in_gain;
} settings_t;

struct t5_audio_out {
    uint32_t magic;
    t5_audio_out_cfg_t cfg;
    StreamBufferHandle_t sb;
    t5_resampler_t rs;
    int16_t pend[BLOCK_FRAMES * 2 * 2];   /* input frames not yet consumed by the resampler */
    size_t npend;                         /* frames */
    size_t frame_bytes;
    volatile bool paused, closing;
    bool primed;                          /* has played; gaps after this are underruns */
    bool starved;                         /* ran dry; counted as an underrun if data comes again */
    t5_audio_usage_t usage;
    volatile int32_t level_q15;           /* t5_audio_out_set_volume */
    int32_t duck_q15;                     /* the ducking gain now (output task) */
};

struct t5_audio_in {
    uint32_t magic;
    t5_audio_in_cfg_t cfg;
    StreamBufferHandle_t sb;
    t5_resampler_t rs;
    bool aec;
    t5_aec_t a;
    volatile bool closing;
};

#define OUT_MAGIC 0x41554f54u
#define IN_MAGIC 0x4155494eu

static t5_mutex_t s_mx;
static bool s_ready, s_dac, s_adc;
static settings_t s_set = { .volume = 60, .muted = false, .sounds = true, .in_gain = 30.0f }, s_saved;
static QueueHandle_t s_cmd;                /* t5_sound_t, or -1 to wake */
static TaskHandle_t s_out_task, s_in_task;
static t5_audio_out_t *s_streams[T5_AUDIO_OUT_MAX];
static int16_t *s_pull;                   /* T5_AUDIO_OUT_MAX blocks, PSRAM */
static t5_audio_in_t *s_input;
static t5_wakelock_t *s_lock;
static t5_audio_stats_t s_st;
#define HOLD_SLEEP 0x1u
#define HOLD_PINS 0x2u
static uint32_t s_hold;                 /* HOLD_*, atomic */
static volatile bool s_mic_parked;      /* mic closed by a hold, reopened after it */
static volatile bool s_apply;           /* volume, mute or gain changed */
static volatile bool s_want_clock;      /* an input is opening: run the output side first */
/* Set while a task holds no stream (cleared under s_mx as it takes one), so
 * a close can wait for it to let go before freeing. EV_HELD: the output task
 * has closed the codecs for a hold. */
#define EV_IN_FREE BIT0
#define EV_OUT_FREE BIT1
#define EV_HELD BIT2
#define EV_DRAINED BIT3          /* the output task found the stream empty */
static EventGroupHandle_t s_ev;
/* The output task claims a mic request (atomically, so a caller that timed
 * out can take it back if it hasn't started), runs it and gives s_req_done. */
typedef enum { IN_REQ_NONE, IN_REQ_OPEN, IN_REQ_CLOSE } in_req_t;
static in_req_t s_in_req;
static t5_err_t s_in_req_err;
static SemaphoreHandle_t s_req_done;

/* Mono reference for the echo canceller, written by output, read by input. */
static int16_t *s_ref;
static volatile uint32_t s_ref_w, s_ref_r;

static t5_err_t save_settings(void *arg)
{
    (void)arg;
    if (!memcmp(&s_set, &s_saved, sizeof s_set)) return T5_OK;
    settings_t copy = s_set;
    char path[96];
    T5_RETURN_ON_ERR(t5_sys_path(path, sizeof path, SETTINGS_FILE));
    t5_err_t e = t5_storage_blob_write(path, SETTINGS_VERSION, &copy, sizeof copy);
    if (e == T5_OK) s_saved = copy;
    return e;
}

static void load_settings(void)
{
    settings_t s;
    size_t len = 0;
    char path[96];
    if (t5_sys_path(path, sizeof path, SETTINGS_FILE) == T5_OK &&
        t5_storage_blob_read(path, SETTINGS_VERSION, &s, sizeof s, &len) == T5_OK && len == sizeof s) {
        if (s.volume >= 0 && s.volume <= 100) s_set = s;
    }
    s_saved = s_set;
}

static void wake_out(void)
{
    int v = -1;
    if (s_cmd) xQueueSend(s_cmd, &v, 0);
}

static bool playing(const t5_audio_out_t *o)
{
    return o && !o->paused && !o->closing;
}

static void lock_update(void)
{
    /* The wake lock follows "a stream that wants to play". */
    bool want = false;
    for (int i = 0; i < T5_AUDIO_OUT_MAX; i++) want |= playing(s_streams[i]);
    /* The service's lock, not the calling app's: the stream's own release
     * path drops it. */
    if (want && !s_lock) s_lock = t5_wakelock_acquire_system(T5_LOCK_AUDIO, "audio");
    if (!want && s_lock) {
        t5_wakelock_release(s_lock);
        s_lock = NULL;
    }
}

static bool held(void)
{
    return __atomic_load_n(&s_hold, __ATOMIC_ACQUIRE) != 0;
}

/* Opening the mic needs the I2S clock (the output side running); closing it
 * doesn't. */
static void serve_mic_request(bool clocked)
{
    in_req_t req = __atomic_load_n(&s_in_req, __ATOMIC_ACQUIRE);
    if (req == IN_REQ_NONE || (req == IN_REQ_OPEN && !clocked)) return;
    if (!__atomic_compare_exchange_n(&s_in_req, &req, IN_REQ_NONE, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return;                          /* taken back */
    }
    if (req == IN_REQ_OPEN) {
        s_in_req_err = audio_hw_in_open(s_set.in_gain);
    } else {
        audio_hw_in_close();
        s_mic_parked = false;
        s_in_req_err = T5_OK;
    }
    xSemaphoreGive(s_req_done);
}

/* A hold: the mic first, while the output side still clocks the input
 * task's last read, then the output. */
static void close_for_hold(bool mic, bool running)
{
    if (mic && !s_mic_parked) {
        if (!(xEventGroupWaitBits(s_ev, EV_IN_FREE, pdFALSE, pdTRUE, pdMS_TO_TICKS(HOLD_MIC_WAIT_MS)) & EV_IN_FREE)) {
            T5_LOGW(TAG, "input task still reading: closing the mic anyway");
        }
        audio_hw_in_close();
        s_mic_parked = true;
    }
    if (running) audio_hw_out_close();
    xEventGroupSetBits(s_ev, EV_HELD);
}

static void reopen_after_hold(bool mic)
{
    if (!s_mic_parked) return;
    s_mic_parked = false;
    if (!mic) return;
    if (audio_hw_in_open(s_set.in_gain) != T5_OK) s_st.codec_errors++;
    xTaskNotifyGive(s_in_task);
}

static void apply_settings(void)
{
    if (!__atomic_exchange_n(&s_apply, false, __ATOMIC_ACQ_REL)) return;
    audio_hw_set_volume(s_set.volume);
    audio_hw_set_mute(s_set.muted);
    audio_hw_set_in_gain(s_set.in_gain);
}

/* Ask the output task to open the mic codec and wait up to timeout_ms. A
 * request that timed out before the task claimed it is taken back. */
static t5_err_t mic_open_request(uint32_t timeout_ms)
{
    xSemaphoreTake(s_req_done, 0);       /* a late answer to a request taken back */
    __atomic_store_n(&s_in_req, IN_REQ_OPEN, __ATOMIC_RELEASE);
    wake_out();
    if (xSemaphoreTake(s_req_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) return s_in_req_err;
    in_req_t expect = IN_REQ_OPEN;
    if (__atomic_compare_exchange_n(&s_in_req, &expect, IN_REQ_NONE, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return T5_ERR_TIMEOUT;
    }
    xSemaphoreTake(s_req_done, portMAX_DELAY);   /* claimed: it is running now */
    return s_in_req_err;
}

/* Pull up to BLOCK_FRAMES of T5_AUDIO_RATE stereo from the stream. */
static size_t stream_pull(t5_audio_out_t *st, int16_t *out)
{
    size_t made = 0;
    for (int guard = 0; guard < 4 && made < BLOCK_FRAMES; guard++) {
        size_t room = sizeof st->pend / st->frame_bytes - st->npend;
        if (room) {
            size_t got = xStreamBufferReceive(st->sb, (uint8_t *)st->pend + st->npend * st->frame_bytes,
                                              room * st->frame_bytes, 0);
            st->npend += got / st->frame_bytes;
        }
        if (!st->npend) break;
        size_t used = 0;
        made += t5_resample(&st->rs, st->pend, st->npend, &used, out + made * 2, BLOCK_FRAMES - made, 2);
        if (used) {
            memmove(st->pend, (uint8_t *)st->pend + used * st->frame_bytes, (st->npend - used) * st->frame_bytes);
            st->npend -= used;
        }
        if (!used && made < BLOCK_FRAMES) break;
    }
    return made;
}

static void ref_push(const int16_t *stereo, size_t frames)
{
    if (!s_ref) return;
    for (size_t i = 0; i < frames; i++) {
        s_ref[s_ref_w % REF_RING] = (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) / 2);
        s_ref_w++;
    }
}

static void post_audio(t5_event_id_t id)
{
    t5_ev_audio_t ev = { .volume = (uint8_t)s_set.volume, .muted = s_set.muted, .headphones = s_st.headphones };
    t5_event_post(id, &ev, sizeof ev);
}

static void headphones_seen(bool hp)
{
    if (hp == s_st.headphones) return;
    s_st.headphones = hp;
    post_audio(T5_EV_AUDIO_ROUTE);
}

/* The streams that want to play, under s_mx; the output task keeps them
 * until it sets EV_OUT_FREE again. */
static int take_streams(t5_audio_out_t **sts)
{
    int n = 0;
    for (int i = 0; i < T5_AUDIO_OUT_MAX; i++) {
        if (playing(s_streams[i])) sts[n++] = s_streams[i];
    }
    if (n) xEventGroupClearBits(s_ev, EV_OUT_FREE);
    return n;
}

/* One stream's block into buf (zeros past what it had), with the underrun
 * bookkeeping. Returns the frames it gave. */
static size_t pull_block(t5_audio_out_t *st, int16_t *buf)
{
    size_t got = stream_pull(st, buf);
    if (got) {
        /* Data after running dry: that gap was an underrun (running dry
         * at the end of a stream is not). */
        if (st->starved) s_st.underruns++;
        st->primed = true;
        st->starved = false;
    }
    if (got < BLOCK_FRAMES) {
        memset(buf + got * 2, 0, (BLOCK_FRAMES - got) * 2 * sizeof buf[0]);
        if (st->primed) st->starved = true;
    }
    if (!st->npend && xStreamBufferIsEmpty(st->sb)) xEventGroupSetBits(s_ev, EV_DRAINED);
    return got;
}

/* Every stream into acc: MEDIA ducked while duck is set, each ramped from
 * its last gain to the new one over the block. */
static void mix_streams(int32_t *acc, t5_audio_out_t **sts, int n, bool duck)
{
    for (int i = 0; i < n; i++) {
        t5_audio_out_t *st = sts[i];
        int32_t want = st->usage == T5_AUDIO_USAGE_MEDIA && duck ? DUCK_Q15 : UNITY_Q15;
        int32_t from = st->duck_q15;
        st->duck_q15 = t5_gain_step_q15(from, want, want < from ? DUCK_DOWN_STEP : DUCK_UP_STEP);
        int32_t lv = st->level_q15;
        t5_mix_add_ramp(acc, s_pull + (size_t)i * BLOCK_SAMPLES, BLOCK_FRAMES, 2, (int32_t)((int64_t)from * lv >> 15),
                        (int32_t)((int64_t)st->duck_q15 * lv >> 15));
    }
}

static void out_task(void *arg)
{
    (void)arg;
    static int32_t acc[BLOCK_SAMPLES];
    static int16_t blk[BLOCK_SAMPLES];
    t5_voice_t voices[MAX_VOICES] = { 0 };
    t5_audio_out_t *sts[T5_AUDIO_OUT_MAX];
    bool running = false, hp = false, speaker = false;
    uint32_t idle_ms = 0, hp_poll = 0;
    for (;;) {
        xEventGroupSetBits(s_ev, EV_OUT_FREE);      /* the last block is done with its streams */
        int cmd;
        TickType_t wait = running ? 0 : pdMS_TO_TICKS(HP_IDLE_POLL_MS);
        bool woke = false;
        while (xQueueReceive(s_cmd, &cmd, wait) == pdTRUE) {
            wait = 0;
            woke = true;
            if (cmd >= 0 && cmd < T5_SOUND_COUNT && s_set.sounds && !s_set.muted) {
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (!voices[i].active) {
                        t5_voice_start(&voices[i], (t5_sound_t)cmd);
                        break;
                    }
                }
            }
        }
        bool hold = held();
        if (!running && !woke && !hold) headphones_seen(audio_hw_headphones());
        t5_mutex_lock(s_mx);
        int nst = hold ? 0 : take_streams(sts);
        bool have_stream = nst > 0;
        bool mic = s_input != NULL;
        /* Full-duplex I2S: the receive side runs on the transmit side's
         * clock, so output keeps running (silently) while a mic is open. */
        bool have_input = mic || s_want_clock;
        t5_mutex_unlock(s_mx);
        bool have_voice = false;
        for (int i = 0; i < MAX_VOICES; i++) have_voice |= voices[i].active;

        if (hold || (!have_stream && !have_voice && !have_input && running && idle_ms >= IDLE_CLOSE_MS)) {
            serve_mic_request(false);
            if (hold) close_for_hold(mic, running);
            else if (running) audio_hw_out_close();
            if (running) {
                speaker = false;
                running = false;
                s_st.out_running = false;
                s_st.speaker_on = false;
            }
            continue;
        }
        if (!running) {
            serve_mic_request(false);
            if (!have_stream && !have_voice && !have_input) continue;
            if (audio_hw_out_open(s_set.volume, s_set.muted) != T5_OK) {
                s_st.codec_errors++;
                for (int i = 0; i < MAX_VOICES; i++) voices[i].active = false;
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            running = true;
            s_st.out_running = true;
            s_st.opens++;
            idle_ms = 0;
            hp = audio_hw_headphones();
            headphones_seen(hp);
            hp_poll = 0;
            speaker = false;
            reopen_after_hold(mic);
        }
        apply_settings();
        serve_mic_request(true);
        if (hp_poll++ % (HP_POLL_MS / BLOCK_MS) == 0) {
            hp = audio_hw_headphones();
            headphones_seen(hp);
        }
        bool want_spk = !hp && (have_stream || have_voice);   /* not for mic-only clocking */
        if (want_spk != speaker) {
            if (audio_hw_speaker(want_spk) == T5_OK) speaker = want_spk;
            s_st.speaker_on = speaker;
        }

        int64_t t0 = esp_timer_get_time();
        memset(acc, 0, sizeof acc);
        bool duck = have_voice;
        for (int i = 0; i < nst; i++) {
            size_t got = pull_block(sts[i], s_pull + (size_t)i * BLOCK_SAMPLES);
            if (got && sts[i]->usage != T5_AUDIO_USAGE_MEDIA) duck = true;
        }
        mix_streams(acc, sts, nst, duck);
        if (have_stream) idle_ms = 0;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (voices[i].active) {
                t5_voice_render(&voices[i], acc, BLOCK_FRAMES);
                idle_ms = 0;
            }
        }
        if (!have_stream && !have_voice) idle_ms += BLOCK_MS;
        t5_mix_to_s16(acc, blk, BLOCK_SAMPLES);
        ref_push(blk, BLOCK_FRAMES);
        s_st.out_peak_dbfs = t5_pcm_peak_dbfs(blk, BLOCK_SAMPLES);
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > s_st.loop_max_us) s_st.loop_max_us = us;
        if (audio_hw_write(blk, sizeof blk) != T5_OK) {
            s_st.codec_errors++;
            vTaskDelay(pdMS_TO_TICKS(BLOCK_MS));
        }
        s_st.out_frames += BLOCK_FRAMES;
    }
}

static void in_task(void *arg)
{
    (void)arg;
    static int16_t raw[BLOCK_SAMPLES], mono[BLOCK_FRAMES], ref48[BLOCK_FRAMES];
    static int16_t mic16[BLOCK_FRAMES / 3], ref16[BLOCK_FRAMES / 3], conv[BLOCK_SAMPLES];
    for (;;) {
        xEventGroupSetBits(s_ev, EV_IN_FREE);
        t5_mutex_lock(s_mx);
        t5_audio_in_t *in = s_input;
        if (in && (in->closing || held() || s_mic_parked)) in = NULL;
        if (in) xEventGroupClearBits(s_ev, EV_IN_FREE);
        t5_mutex_unlock(s_mx);
        if (!in) {
            /* Opening an input and the end of a hold notify. */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (audio_hw_read(raw, sizeof raw) != T5_OK) {
            s_st.codec_errors++;
            xEventGroupSetBits(s_ev, EV_IN_FREE);
            vTaskDelay(pdMS_TO_TICKS(BLOCK_MS));
            continue;
        }
        s_st.in_frames += BLOCK_FRAMES;
        s_st.in_peak_dbfs = t5_pcm_peak_dbfs(raw, BLOCK_SAMPLES);
        const int16_t *src = raw;
        size_t frames = BLOCK_FRAMES;
        if (in->aec) {
            /* Mono, to 16 kHz (average of three: a crude low-pass that is
             * enough ahead of an echo canceller), and the reference alike. */
            t5_pcm_downmix(raw, mono, BLOCK_FRAMES);
            uint32_t avail = s_ref_w - s_ref_r;
            if (avail > REF_RING) s_ref_r = s_ref_w - REF_RING, avail = REF_RING;
            for (int i = 0; i < BLOCK_FRAMES; i++) {
                ref48[i] = avail > (uint32_t)i ? s_ref[(s_ref_r + i) % REF_RING] : 0;
            }
            s_ref_r += avail < BLOCK_FRAMES ? avail : BLOCK_FRAMES;
            for (int i = 0; i < BLOCK_FRAMES / 3; i++) {
                mic16[i] = (int16_t)(((int32_t)mono[3 * i] + mono[3 * i + 1] + mono[3 * i + 2]) / 3);
                ref16[i] = (int16_t)(((int32_t)ref48[3 * i] + ref48[3 * i + 1] + ref48[3 * i + 2]) / 3);
            }
            t5_aec_process(&in->a, mic16, ref16, mic16, BLOCK_FRAMES / 3);
            s_st.aec_erle_db = t5_aec_erle_db(&in->a);
            src = mic16;
            frames = BLOCK_FRAMES / 3;
        }
        size_t used = 0;
        size_t n = t5_resample(&in->rs, src, frames, &used, conv, BLOCK_FRAMES, in->cfg.channels);
        size_t bytes = n * in->cfg.channels * sizeof(int16_t);
        if (xStreamBufferSpacesAvailable(in->sb) < bytes) {
            s_st.overruns++;
        } else {
            xStreamBufferSend(in->sb, conv, bytes, 0);
        }
    }
}

/* The output task applies it between writes; a closed codec gets the
 * settings when it opens. */
static void apply_later(void)
{
    __atomic_store_n(&s_apply, true, __ATOMIC_RELEASE);
    wake_out();
}

int t5_audio_volume(void)
{
    return s_set.volume;
}

t5_err_t t5_audio_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    bool changed = s_set.volume != pct;
    s_set.volume = pct;
    apply_later();
    if (changed) post_audio(T5_EV_AUDIO_VOLUME);
    return T5_OK;
}

bool t5_audio_muted(void)
{
    return s_set.muted;
}

t5_err_t t5_audio_set_muted(bool on)
{
    bool changed = s_set.muted != on;
    s_set.muted = on;
    apply_later();
    if (changed) post_audio(T5_EV_AUDIO_VOLUME);
    return T5_OK;
}

bool t5_audio_headphones(void)
{
    return s_st.headphones;
}

void t5_audio_play_sound(t5_sound_t s)
{
    if (!s_ready || !s_dac || s >= T5_SOUND_COUNT || !s_set.sounds || s_set.muted) return;
    int v = (int)s;
    xQueueSend(s_cmd, &v, 0);
}

bool t5_audio_system_sounds(void)
{
    return s_set.sounds;
}

void t5_audio_set_system_sounds(bool on)
{
    s_set.sounds = on;
}

static void out_release(void *h)
{
    t5_audio_out_close(h);
}

t5_err_t t5_audio_out_open(const t5_audio_out_cfg_t *cfg, t5_audio_out_t **out)
{
    return t5_audio_out_open_usage(cfg, T5_AUDIO_USAGE_MEDIA, out);
}

t5_err_t t5_audio_out_open_usage(const t5_audio_out_cfg_t *cfg, t5_audio_usage_t usage, t5_audio_out_t **out)
{
    if ((unsigned)usage > T5_AUDIO_USAGE_SPEECH) return T5_ERR_INVALID_ARG;
    if (!s_ready || !s_dac) return T5_ERR_NOT_FOUND;
    if (!cfg || !out || cfg->rate < 8000 || cfg->rate > T5_AUDIO_RATE || (cfg->channels != 1 && cfg->channels != 2))
        return T5_ERR_INVALID_ARG;
    t5_audio_out_t *o = heap_caps_calloc(1, sizeof *o, MALLOC_CAP_SPIRAM);
    if (!o) return T5_ERR_NO_MEM;
    o->magic = OUT_MAGIC;
    o->cfg = *cfg;
    o->usage = usage;
    o->level_q15 = UNITY_Q15;
    o->duck_q15 = UNITY_Q15;
    if (!o->cfg.buffer_ms) o->cfg.buffer_ms = 200;
    o->frame_bytes = cfg->channels * sizeof(int16_t);
    size_t sz = (size_t)cfg->rate * o->frame_bytes * o->cfg.buffer_ms / 1000;
    o->sb = xStreamBufferCreateWithCaps(sz, 1, MALLOC_CAP_SPIRAM);
    if (!o->sb) {
        free(o);
        return T5_ERR_NO_MEM;
    }
    t5_resampler_init(&o->rs, cfg->rate, cfg->channels, T5_AUDIO_RATE);
    t5_mutex_lock(s_mx);
    int slot = 0;
    while (slot < T5_AUDIO_OUT_MAX && s_streams[slot]) slot++;
    if (slot == T5_AUDIO_OUT_MAX) {
        t5_mutex_unlock(s_mx);
        vStreamBufferDeleteWithCaps(o->sb);
        free(o);
        return T5_ERR_BUSY;
    }
    s_streams[slot] = o;
    lock_update();
    t5_res_add(T5_RES_OTHER, o, out_release);   /* under the lock: an exiting app can't be stopped between */
    t5_mutex_unlock(s_mx);
    wake_out();
    *out = o;
    return T5_OK;
}

int t5_audio_out_write(t5_audio_out_t *o, const void *pcm, size_t bytes, uint32_t timeout_ms)
{
    if (!o || o->magic != OUT_MAGIC || o->closing) return T5_ERR_INVALID_ARG;
    bytes -= bytes % o->frame_bytes;
    size_t done = 0;
    uint32_t t0 = t5_time_ms();
    while (done < bytes) {
        uint32_t el = t5_time_ms() - t0;
        if (el > timeout_ms && done) break;
        TickType_t w = el >= timeout_ms ? 0 : pdMS_TO_TICKS(timeout_ms - el);
        size_t n = xStreamBufferSend(o->sb, (const uint8_t *)pcm + done, bytes - done, w ? w : 0);
        done += n;
        if (!n && !w) break;
    }
    return (int)(done - done % o->frame_bytes);
}

t5_err_t t5_audio_out_set_volume(t5_audio_out_t *o, int pct)
{
    if (!o || o->magic != OUT_MAGIC) return T5_ERR_INVALID_ARG;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    o->level_q15 = pct * UNITY_Q15 / 100;
    return T5_OK;
}

bool t5_audio_out_active(void)
{
    bool any = false;
    for (int i = 0; i < T5_AUDIO_OUT_MAX; i++) any |= playing(s_streams[i]);
    return any;
}

t5_err_t t5_audio_out_pause(t5_audio_out_t *o, bool paused)
{
    if (!o || o->magic != OUT_MAGIC) return T5_ERR_INVALID_ARG;
    t5_mutex_lock(s_mx);
    o->paused = paused;
    o->primed = false;           /* a pause isn't an underrun */
    lock_update();
    t5_mutex_unlock(s_mx);
    wake_out();
    return T5_OK;
}

uint32_t t5_audio_out_queued_ms(const t5_audio_out_t *o)
{
    if (!o || o->magic != OUT_MAGIC) return 0;
    size_t b = xStreamBufferBytesAvailable(o->sb) + o->npend * o->frame_bytes;
    return (uint32_t)(b * 1000 / (o->cfg.rate * o->frame_bytes));
}

t5_err_t t5_audio_out_drain(t5_audio_out_t *o, uint32_t timeout_ms)
{
    if (!o || o->magic != OUT_MAGIC) return T5_ERR_INVALID_ARG;
    uint32_t t0 = t5_time_ms();
    for (;;) {
        xEventGroupClearBits(s_ev, EV_DRAINED);
        if (!t5_audio_out_queued_ms(o)) break;
        uint32_t spent = t5_time_ms() - t0;
        if (spent >= timeout_ms) return T5_ERR_TIMEOUT;
        xEventGroupWaitBits(s_ev, EV_DRAINED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms - spent));
    }
    t5_delay_ms(AUDIO_HW_OUT_LATENCY_MS + BLOCK_MS);   /* the block in flight, then the DMA */
    return T5_OK;
}

t5_err_t t5_audio_out_close(t5_audio_out_t *o)
{
    if (!o || o->magic != OUT_MAGIC) return T5_ERR_INVALID_ARG;
    t5_res_remove(o);
    t5_mutex_lock(s_mx);
    o->closing = true;
    for (int i = 0; i < T5_AUDIO_OUT_MAX; i++) {
        if (s_streams[i] == o) s_streams[i] = NULL;
    }
    lock_update();
    t5_mutex_unlock(s_mx);
    /* The output task may be using it (opening the codec first can take a
     * while): wait for it to let go. */
    if (!(xEventGroupWaitBits(s_ev, EV_OUT_FREE, pdFALSE, pdTRUE, pdMS_TO_TICKS(RELEASE_WAIT_MS)) & EV_OUT_FREE)) {
        T5_LOGE(TAG, "output task still using the stream after 1 s: leaking it");
        return T5_ERR_TIMEOUT;
    }
    o->magic = 0;
    vStreamBufferDeleteWithCaps(o->sb);
    free(o);
    wake_out();
    return T5_OK;
}

t5_err_t t5_audio_tone(uint32_t hz, uint32_t ms)
{
    t5_audio_out_cfg_t c = { .rate = T5_AUDIO_RATE, .channels = 1 };
    t5_audio_out_t *o;
    T5_RETURN_ON_ERR(t5_audio_out_open(&c, &o));
    int16_t buf[BLOCK_FRAMES];
    uint32_t phase = 0, inc = (uint32_t)(((uint64_t)hz << 32) / T5_AUDIO_RATE);
    for (uint32_t done = 0; done < ms; done += BLOCK_MS) {
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            phase += inc;
            /* triangle from the phase: no libm on this path */
            int32_t p = (int32_t)(phase >> 16) - 32768;
            int32_t tri = (p < 0 ? -p : p) * 2 - 32768;
            buf[i] = (int16_t)(tri / 4);
        }
        t5_audio_out_write(o, buf, sizeof buf, 100);
    }
    t5_audio_out_drain(o, ms + 500);
    return t5_audio_out_close(o);
}

static void in_release(void *h)
{
    t5_audio_in_close(h);
}

t5_err_t t5_audio_in_open(const t5_audio_in_cfg_t *cfg, t5_audio_in_t **out)
{
    if (!s_ready || !s_adc) return T5_ERR_NOT_FOUND;
    if (!cfg || !out || cfg->rate < 8000 || cfg->rate > T5_AUDIO_RATE || (cfg->channels != 1 && cfg->channels != 2))
        return T5_ERR_INVALID_ARG;
    int pe = t5_core_perm_check(T5_CORE_PERM_MIC);
    if (pe != T5_OK) return (t5_err_t)pe;
    t5_audio_in_t *in = heap_caps_calloc(1, sizeof *in, MALLOC_CAP_SPIRAM);
    if (!in) return T5_ERR_NO_MEM;
    in->magic = IN_MAGIC;
    in->cfg = *cfg;
    if (!in->cfg.buffer_ms) in->cfg.buffer_ms = 200;
    in->aec = cfg->aec && cfg->rate <= AEC_RATE && cfg->channels == 1;
    if (in->aec && !t5_aec_init(&in->a, AEC_TAPS, AEC_DELAY)) {
        free(in);
        return T5_ERR_NO_MEM;
    }
    size_t sz = (size_t)cfg->rate * cfg->channels * 2 * in->cfg.buffer_ms / 1000;
    in->sb = xStreamBufferCreateWithCaps(sz, 1, MALLOC_CAP_SPIRAM);
    if (!in->sb) {
        if (in->aec) t5_aec_free(&in->a);
        free(in);
        return T5_ERR_NO_MEM;
    }
    t5_resampler_init(&in->rs, in->aec ? AEC_RATE : T5_AUDIO_RATE, in->aec ? 1 : 2, cfg->rate);
    t5_mutex_lock(s_mx);
    if (s_input) {
        t5_mutex_unlock(s_mx);
        vStreamBufferDeleteWithCaps(in->sb);
        if (in->aec) t5_aec_free(&in->a);
        free(in);
        return T5_ERR_BUSY;
    }
    /* The output side must be up before the input opens, and must not be
     * reconfigured while the input task reads: both share one I2S port and
     * esp_codec_dev reconfigures the pair on open. */
    s_want_clock = true;
    t5_mutex_unlock(s_mx);
    t5_err_t e = mic_open_request(MIC_OPEN_MS);
    t5_mutex_lock(s_mx);
    if (e != T5_OK) {
        s_want_clock = false;
        t5_mutex_unlock(s_mx);
        vStreamBufferDeleteWithCaps(in->sb);
        if (in->aec) t5_aec_free(&in->a);
        free(in);
        s_st.codec_errors++;
        return e;
    }
    s_ref_r = s_ref_w;
    s_input = in;
    s_want_clock = false;
    s_st.in_running = true;
    t5_res_add(T5_RES_OTHER, in, in_release);   /* under the lock, as for output */
    t5_mutex_unlock(s_mx);
    xTaskNotifyGive(s_in_task);
    t5_ev_media_t ev = { .on = true };
    t5_event_post(T5_EV_MIC_STATE, &ev, sizeof ev);
    *out = in;
    return T5_OK;
}

int t5_audio_in_read(t5_audio_in_t *in, void *pcm, size_t bytes, uint32_t timeout_ms)
{
    if (!in || in->magic != IN_MAGIC || in->closing) return T5_ERR_INVALID_ARG;
    size_t fb = in->cfg.channels * sizeof(int16_t);
    bytes -= bytes % fb;
    size_t n = xStreamBufferReceive(in->sb, pcm, bytes, pdMS_TO_TICKS(timeout_ms));
    return (int)(n - n % fb);
}

t5_err_t t5_audio_in_close(t5_audio_in_t *in)
{
    if (!in || in->magic != IN_MAGIC) return T5_ERR_INVALID_ARG;
    t5_res_remove(in);
    t5_mutex_lock(s_mx);
    in->closing = true;
    if (s_input == in) s_input = NULL;
    t5_mutex_unlock(s_mx);
    /* The input task may be inside a block for this input (a read after a
     * resume can take a while): it must let go before the input is freed.
     * Sleeping 25 ms instead was a use-after-free (2026-11-07). */
    if (!(xEventGroupWaitBits(s_ev, EV_IN_FREE, pdFALSE, pdTRUE, pdMS_TO_TICKS(RELEASE_WAIT_MS)) & EV_IN_FREE)) {
        T5_LOGE(TAG, "input task still using the input after 1 s: leaking it");
        return T5_ERR_TIMEOUT;
    }
    if (!s_input) {
        /* Not taken back on a timeout: the output task closes it whenever
         * it next runs, clocked or not. */
        in_req_t expect = IN_REQ_NONE;
        xSemaphoreTake(s_req_done, 0);
        if (__atomic_compare_exchange_n(&s_in_req, &expect, IN_REQ_CLOSE, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            wake_out();
            if (xSemaphoreTake(s_req_done, pdMS_TO_TICKS(MIC_CLOSE_MS)) != pdTRUE) {
                T5_LOGW(TAG, "mic codec close queued: the output task is busy");
            }
        }
        s_st.in_running = false;
    }
    in->magic = 0;
    vStreamBufferDeleteWithCaps(in->sb);
    if (in->aec) t5_aec_free(&in->a);
    free(in);
    t5_ev_media_t ev = { .on = false };
    t5_event_post(T5_EV_MIC_STATE, &ev, sizeof ev);
    return T5_OK;
}

bool t5_audio_in_active(void)
{
    return s_input != NULL;
}

float t5_audio_in_gain_db(void)
{
    return s_set.in_gain;
}

t5_err_t t5_audio_in_set_gain_db(float db)
{
    if (db < 0 || db > 37.5f) return T5_ERR_INVALID_ARG;
    s_set.in_gain = db;
    apply_later();
    return T5_OK;
}

void t5_audio_stats(t5_audio_stats_t *out)
{
    *out = s_st;
    out->ready = s_ready;
    out->muted = s_set.muted;
    out->volume = s_set.volume;
}

static t5_err_t hold(uint32_t why, bool on)
{
    if (!on) {
        __atomic_and_fetch(&s_hold, ~why, __ATOMIC_ACQ_REL);
        wake_out();
        if (s_in_task) xTaskNotifyGive(s_in_task);   /* it may have seen the hold before the output task did */
        return T5_OK;
    }
    if (s_ev) xEventGroupClearBits(s_ev, EV_HELD);
    __atomic_or_fetch(&s_hold, why, __ATOMIC_ACQ_REL);
    if (!s_ev) return T5_OK;
    wake_out();
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_HELD, pdFALSE, pdTRUE, pdMS_TO_TICKS(HOLD_WAIT_MS));
    return (b & EV_HELD) ? T5_OK : T5_ERR_TIMEOUT;
}

t5_err_t t5_audio_hold_pins(bool on)
{
    return hold(HOLD_PINS, on);
}

/* Sleep only happens without an audio lock, so nothing should be playing:
 * this makes sure both codecs are powered down. */
static t5_err_t audio_suspend(void *arg)
{
    (void)arg;
    T5_LOG_ON_ERR(TAG, hold(HOLD_SLEEP, true));
    return T5_OK;
}

static t5_err_t audio_resume(void *arg)
{
    (void)arg;
    return hold(HOLD_SLEEP, false);
}

t5_err_t t5_audio_init(void)
{
    if (s_ready) return T5_OK;
    s_mx = t5_mutex_create();
    if (!s_mx) return T5_ERR_NO_MEM;
    load_settings();
    T5_RETURN_ON_ERR(audio_hw_init(&s_dac, &s_adc));
    s_ref = heap_caps_calloc(REF_RING, sizeof *s_ref, MALLOC_CAP_SPIRAM);
    s_pull = heap_caps_calloc((size_t)T5_AUDIO_OUT_MAX * BLOCK_SAMPLES, sizeof *s_pull, MALLOC_CAP_SPIRAM);
    s_cmd = xQueueCreate(8, sizeof(int));
    s_req_done = xSemaphoreCreateBinary();
    s_ev = xEventGroupCreate();
    if (!s_ref || !s_pull || !s_cmd || !s_req_done || !s_ev) return T5_ERR_NO_MEM;
    xEventGroupSetBits(s_ev, EV_IN_FREE | EV_OUT_FREE);
    /* Stacks in PSRAM: internal RAM is what the C6 link and the H.264
     * encoder need, and these tasks never touch flash. */
    if (xTaskCreatePinnedToCoreWithCaps(out_task, "audio.out", 6144, NULL, AUDIO_PRIO, &s_out_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
        xTaskCreatePinnedToCoreWithCaps(in_task, "audio.in", 8192, NULL, AUDIO_PRIO, &s_in_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return T5_ERR_NO_MEM;
    }
    s_st.headphones = audio_hw_headphones();
    const t5_suspend_hook_t hook = {
        .name = "audio", .prio = T5_SUSPEND_PRIO_MEDIA, .budget_ms = 150,
        .suspend = audio_suspend, .resume = audio_resume,
    };
    T5_LOG_ON_ERR(TAG, t5_power_register_hook(&hook));
    T5_LOG_ON_ERR(TAG, t5_autosave_register("audio", save_settings, NULL));
    s_ready = true;
    T5_LOGI(TAG, "audio up: volume %d%s, speaker/headphones %s", s_set.volume, s_set.muted ? " (muted)" : "",
            s_st.headphones ? "headphones" : "speaker");
    return T5_OK;
}
