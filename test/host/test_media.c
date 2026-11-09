/* SPDX-License-Identifier: Apache-2.0
 * Phase 6 and 7 logic: audio resampling, mixing, system sounds and echo
 * cancellation; per-app energy; USB-MIDI packets, HID key mapping and
 * keyboard and mouse reports.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "t5_app/energy.h"
#include "t5_audio/dsp.h"
#include "t5_audio/wav.h"
#include "t5_mocks.h"
#include "t5_usb/usb.h"
#include "usb_proto.h"

void setUp(void)
{
    mock_reset_all();
}

void tearDown(void)
{
    t5_usb_set_hid_handler(NULL);
}

/* ---- resampler ------------------------------------------------------------------ */
static void test_resample_identity(void)
{
    t5_resampler_t r;
    t5_resampler_init(&r, 48000, 2, 48000);
    int16_t in[2 * 100], out[2 * 100];
    for (int i = 0; i < 200; i++) in[i] = (int16_t)(i * 37);
    size_t used = 0;
    size_t n = t5_resample(&r, in, 100, &used, out, 100, 2);
    TEST_ASSERT_EQUAL(100, n);
    TEST_ASSERT_EQUAL(100, used);
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 200);
}

static void test_resample_rate_ratio(void)
{
    /* 16 kHz mono in, 48 kHz stereo out: three outputs per input, in blocks. */
    t5_resampler_t r;
    t5_resampler_init(&r, 16000, 1, 48000);
    int16_t in[160];
    for (int i = 0; i < 160; i++) in[i] = (int16_t)(1000 + i);
    int16_t out[2 * 480];
    size_t total = 0, used_total = 0;
    for (int blk = 0; blk < 10; blk++) {
        size_t used = 0;
        total += t5_resample(&r, in + used_total % 160, 16, &used, out, 480, 2);
        used_total += used;
    }
    TEST_ASSERT_EQUAL(160, used_total);
    TEST_ASSERT_INT_WITHIN(3, 480, (int)total);
}

static void test_resample_44k1_is_smooth(void)
{
    /* A ramp stays a ramp (monotonic) through a fractional ratio, across
     * block boundaries. */
    t5_resampler_t r;
    t5_resampler_init(&r, 44100, 1, 48000);
    int16_t in[441];
    for (int i = 0; i < 441; i++) in[i] = (int16_t)(i * 50);
    int16_t out[1000];
    size_t made = 0, pos = 0;
    while (pos < 441) {
        size_t chunk = 441 - pos < 37 ? 441 - pos : 37, used = 0;
        made += t5_resample(&r, in + pos, chunk, &used, out + made, 1000 - made, 1);
        pos += used;
        if (!used) break;
    }
    TEST_ASSERT_INT_WITHIN(3, 480, (int)made);
    for (size_t i = 1; i < made; i++) TEST_ASSERT_TRUE(out[i] >= out[i - 1]);
}

static void test_resample_downmix(void)
{
    t5_resampler_t r;
    t5_resampler_init(&r, 48000, 2, 48000);
    int16_t in[4] = { 100, 300, -200, -400 }, out[2];
    size_t used;
    TEST_ASSERT_EQUAL(2, t5_resample(&r, in, 2, &used, out, 2, 1));
    TEST_ASSERT_EQUAL(200, out[0]);
    TEST_ASSERT_EQUAL(-300, out[1]);
}

/* ---- mixer and sounds ----------------------------------------------------------- */
/* ---- WAV ------------------------------------------------------------------- */
static size_t put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    return 4;
}

static size_t put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    return 2;
}

/* A WAV header (optionally a LIST chunk first, optionally extensible) and
 * data_len bytes of samples copied from data. Returns the total length. */
static size_t make_wav(uint8_t *b, uint16_t fmt, uint16_t ch, uint32_t rate, uint16_t bits, bool ext, size_t list,
                       const void *data, uint32_t data_len)
{
    size_t at = 0;
    memcpy(b, "RIFF", 4); at = 8;
    memcpy(b + at, "WAVE", 4); at += 4;
    if (list) {
        memcpy(b + at, "LIST", 4); at += 4;
        at += put32(b + at, (uint32_t)list);
        memset(b + at, 'x', list); at += list + (list & 1);
    }
    memcpy(b + at, "fmt ", 4); at += 4;
    at += put32(b + at, ext ? 40 : 16);
    at += put16(b + at, ext ? 0xfffe : fmt);
    at += put16(b + at, ch);
    at += put32(b + at, rate);
    at += put32(b + at, rate * ch * bits / 8);
    at += put16(b + at, (uint16_t)(ch * bits / 8));
    at += put16(b + at, bits);
    if (ext) {
        at += put16(b + at, 22);
        at += put16(b + at, bits);
        at += put32(b + at, 3);
        at += put16(b + at, fmt);            /* the sub-format GUID starts with the format */
        memset(b + at, 0, 14); at += 14;
    }
    memcpy(b + at, "data", 4); at += 4;
    at += put32(b + at, data_len);
    memcpy(b + at, data, data_len); at += data_len;
    put32(b + 4, (uint32_t)(at - 8));
    return at;
}

static void test_wav_pcm16_stereo(void)
{
    static uint8_t b[512];
    const int16_t pcm[] = { 1000, -1000, 32767, -32768 };
    size_t n = make_wav(b, 1, 2, 44100, 16, false, 0, pcm, sizeof pcm);
    t5_wav_t w;
    TEST_ASSERT_EQUAL(T5_OK, t5_wav_parse(b, n, &w));
    TEST_ASSERT_EQUAL(44100, w.rate);
    TEST_ASSERT_EQUAL(2, t5_wav_out_channels(&w));
    TEST_ASSERT_EQUAL(8, w.data_len);
    int16_t out[4];
    t5_wav_convert(&w, b + w.data_off, 2, out);
    TEST_ASSERT_EQUAL_INT16_ARRAY(pcm, out, 4);
}

static void test_wav_formats(void)
{
    static uint8_t b[1024];
    t5_wav_t w;
    int16_t out[8];
    const uint8_t u8[] = { 128, 255, 0 };                       /* 8-bit unsigned mono */
    TEST_ASSERT_EQUAL(T5_OK, t5_wav_parse(b, make_wav(b, 1, 1, 8000, 8, false, 0, u8, 3), &w));
    t5_wav_convert(&w, b + w.data_off, 3, out);
    TEST_ASSERT_EQUAL(0, out[0]);
    TEST_ASSERT_EQUAL(127 << 8, out[1]);
    TEST_ASSERT_EQUAL(-32768, out[2]);
    const uint8_t s24[] = { 0x00, 0x34, 0x12 };                 /* 24-bit: the top 16 bits */
    TEST_ASSERT_EQUAL(T5_OK, t5_wav_parse(b, make_wav(b, 1, 1, 48000, 24, false, 0, s24, 3), &w));
    t5_wav_convert(&w, b + w.data_off, 1, out);
    TEST_ASSERT_EQUAL(0x1234, out[0]);
    const float f[] = { 0.5f, -1.5f };                           /* float, extensible, clipped */
    TEST_ASSERT_EQUAL(T5_OK, t5_wav_parse(b, make_wav(b, 3, 1, 16000, 32, true, 0, f, sizeof f), &w));
    TEST_ASSERT_EQUAL(T5_WAV_FLOAT, w.format);
    t5_wav_convert(&w, b + w.data_off, 2, out);
    TEST_ASSERT_INT_WITHIN(1, 16384, out[0]);
    TEST_ASSERT_EQUAL(-32768, out[1]);
    const int16_t six[] = { 1, 2, 3, 4, 5, 6 };                  /* 6 channels: the first two */
    TEST_ASSERT_EQUAL(T5_OK, t5_wav_parse(b, make_wav(b, 1, 6, 48000, 16, false, 0, six, sizeof six), &w));
    TEST_ASSERT_EQUAL(2, t5_wav_out_channels(&w));
    t5_wav_convert(&w, b + w.data_off, 1, out);
    TEST_ASSERT_EQUAL(1, out[0]);
    TEST_ASSERT_EQUAL(2, out[1]);
}

static void test_wav_bad_and_more(void)
{
    static uint8_t b[8192];
    t5_wav_t w;
    const int16_t pcm[2] = { 0 };
    size_t n = make_wav(b, 1, 1, 8000, 16, false, 5000, pcm, sizeof pcm);   /* a long LIST first */
    TEST_ASSERT_EQUAL(T5_ERR_STATE, t5_wav_parse(b, 4096, &w));            /* needs more of the file */
    TEST_ASSERT_EQUAL(T5_OK, t5_wav_parse(b, n, &w));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_wav_parse(b, make_wav(b, 2, 1, 8000, 4, false, 0, pcm, 4), &w)); /* ADPCM */
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_wav_parse(b, make_wav(b, 1, 1, 8000, 12, false, 0, pcm, 4), &w));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_wav_parse(b, make_wav(b, 1, 0, 8000, 16, false, 0, pcm, 4), &w));
    memcpy(b, "RIFX", 4);
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_wav_parse(b, 64, &w));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_wav_parse(b, 4, &w));
    /* An odd data size is cut to whole frames. */
    n = make_wav(b, 1, 2, 8000, 16, false, 0, pcm, 3);
    TEST_ASSERT_EQUAL(T5_OK, t5_wav_parse(b, n, &w));
    TEST_ASSERT_EQUAL(0, w.data_len);
}

/* Ducking: a ramp from full to a quarter ends exactly there, with no step. */
static void test_mix_ramp(void)
{
    int16_t pcm[2 * 100];
    for (int i = 0; i < 200; i++) pcm[i] = 16000;
    int32_t acc[200] = { 0 };
    t5_mix_add_ramp(acc, pcm, 100, 2, 32768, 8192);
    TEST_ASSERT_INT_WITHIN(2, 4000, acc[198]);          /* the last frame at the target */
    TEST_ASSERT_EQUAL(acc[198], acc[199]);              /* both channels alike */
    for (int f = 1; f < 100; f++) TEST_ASSERT_TRUE(acc[2 * f] <= acc[2 * (f - 1)] && acc[2 * (f - 1)] - acc[2 * f] < 200);
    TEST_ASSERT_EQUAL(8192, t5_gain_step_q15(32768, 8192, 40000));
    TEST_ASSERT_EQUAL(30000, t5_gain_step_q15(32768, 8192, 2768));
    TEST_ASSERT_EQUAL(10000, t5_gain_step_q15(8192, 32768, 1808));
}

static void test_mix_saturates(void)
{
    int32_t acc[4] = { 0 };
    int16_t a[4] = { 30000, -30000, 100, 0 };
    t5_mix_add(acc, a, 4, 32767);
    t5_mix_add(acc, a, 4, 32767);
    int16_t out[4];
    t5_mix_to_s16(acc, out, 4);
    TEST_ASSERT_EQUAL(32767, out[0]);
    TEST_ASSERT_EQUAL(-32768, out[1]);
    TEST_ASSERT_INT_WITHIN(1, 200, out[2]);
}

static void test_peak_dbfs(void)
{
    int16_t s[4] = { 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL(-96, t5_pcm_peak_dbfs(s, 4));
    s[2] = 16384;
    TEST_ASSERT_INT_WITHIN(1, -6, t5_pcm_peak_dbfs(s, 4));
}

static void test_sounds_end_and_stay_quiet(void)
{
    for (int k = 0; k < T5_SOUND_COUNT; k++) {
        t5_voice_t v;
        t5_voice_start(&v, (t5_sound_t)k);
        uint32_t frames = t5_sound_ms((t5_sound_t)k) * (T5_AUDIO_RATE / 1000);
        int32_t *acc = calloc(frames * 2 + 960, sizeof *acc);
        uint32_t done = 0;
        bool active = true;
        while (active && done < frames + 480) {
            active = t5_voice_render(&v, acc + done * 2, 480);
            done += 480;
        }
        TEST_ASSERT_FALSE(v.active);
        int32_t peak = 0;
        for (uint32_t i = 0; i < frames * 2; i++) peak = abs(acc[i]) > peak ? abs(acc[i]) : peak;
        TEST_ASSERT_TRUE(peak > 1000);           /* audible */
        TEST_ASSERT_TRUE(peak <= 8192 + 16);     /* -12 dBFS ceiling */
        free(acc);
    }
}

/* ---- echo canceller --------------------------------------------------------------- */
static void test_aec_cancels_echo(void)
{
    t5_aec_t a;
    TEST_ASSERT_TRUE(t5_aec_init(&a, 256, 0));
    /* Echo path: 40-sample delay, gain 0.3, a small reflection at 90. */
    enum { N = 16000 * 4 };
    int16_t *ref = malloc(N * 2), *mic = malloc(N * 2), *out = malloc(N * 2);
    uint32_t seed = 1;
    for (int i = 0; i < N; i++) {
        seed = seed * 1664525u + 1013904223u;
        ref[i] = (int16_t)((int32_t)(seed >> 16) - 32768) / 4;
    }
    for (int i = 0; i < N; i++) {
        float e = 0;
        if (i >= 40) e += 0.3f * ref[i - 40];
        if (i >= 90) e -= 0.1f * ref[i - 90];
        mic[i] = (int16_t)e;
    }
    for (int i = 0; i < N; i += 160) t5_aec_process(&a, mic + i, ref + i, out + i, 160);
    double ein = 0, eout = 0;
    for (int i = N - 16000; i < N; i++) {
        ein += (double)mic[i] * mic[i];
        eout += (double)out[i] * out[i];
    }
    double erle = 10 * log10(ein / (eout + 1));
    TEST_ASSERT_TRUE_MESSAGE(erle > 20, "echo reduced by more than 20 dB after convergence");
    TEST_ASSERT_TRUE(t5_aec_erle_db(&a) > 10);
    t5_aec_free(&a);
    free(ref);
    free(mic);
    free(out);
}

static void test_aec_keeps_near_end(void)
{
    /* No reference at all: the near-end talker passes unchanged. */
    t5_aec_t a;
    TEST_ASSERT_TRUE(t5_aec_init(&a, 128, 16));
    int16_t ref[160] = { 0 }, mic[160], out[160];
    for (int i = 0; i < 160; i++) mic[i] = (int16_t)(8000 * sinf(i * 0.2f));
    for (int k = 0; k < 20; k++) t5_aec_process(&a, mic, ref, out, 160);
    for (int i = 0; i < 160; i++) TEST_ASSERT_INT_WITHIN(2, mic[i], out[i]);
    t5_aec_free(&a);
}

/* ---- energy -------------------------------------------------------------------------- */
static void test_energy_average(void)
{
    t5_energy_db_t db;
    t5_energy_init(&db);
    for (int i = 0; i < 12; i++) t5_energy_add(&db, "com.a", 2000, 5000);   /* 60 s at 2 W */
    for (int i = 0; i < 12; i++) t5_energy_add(&db, "com.b", 1000, 5000);
    t5_energy_add(&db, "com.c", 500, 5000);                               /* too short */
    t5_energy_add(&db, "com.a", -500, 5000);                              /* charging: ignored */
    TEST_ASSERT_EQUAL(2000, t5_energy_avg_mw(t5_energy_find(&db, "com.a")));
    TEST_ASSERT_EQUAL(-1, t5_energy_avg_mw(t5_energy_find(&db, "com.c")));
    t5_energy_rec_t out[T5_ENERGY_MAX];
    int n = t5_energy_sorted(&db, out, T5_ENERGY_MAX);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_STRING("com.a", out[0].id);
    TEST_ASSERT_EQUAL_STRING("com.b", out[1].id);
    t5_energy_forget(&db, "com.a");
    TEST_ASSERT_NULL(t5_energy_find(&db, "com.a"));
}

static void test_energy_evicts_lru(void)
{
    t5_energy_db_t db;
    t5_energy_init(&db);
    char id[16];
    for (int i = 0; i < T5_ENERGY_MAX; i++) {
        snprintf(id, sizeof id, "app%d", i);
        t5_energy_add(&db, id, 100, 1000);
    }
    t5_energy_add(&db, "app0", 100, 1000);         /* app0 recent again; app1 is the oldest */
    t5_energy_add(&db, "new", 100, 1000);
    TEST_ASSERT_NOT_NULL(t5_energy_find(&db, "app0"));
    TEST_ASSERT_NULL(t5_energy_find(&db, "app1"));
    TEST_ASSERT_NOT_NULL(t5_energy_find(&db, "new"));
}

/* ---- USB ------------------------------------------------------------------------------------- */
static void test_midi_packets(void)
{
    uint8_t note_on[3] = { 0x90, 60, 100 }, pkt[4], msg[3];
    TEST_ASSERT_TRUE(usb_midi_pack(note_on, 3, pkt));
    TEST_ASSERT_EQUAL_HEX8(0x09, pkt[0]);
    TEST_ASSERT_EQUAL(3, usb_midi_parse(pkt, msg));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(note_on, msg, 3);
    uint8_t pc[2] = { 0xC3, 5 };
    TEST_ASSERT_TRUE(usb_midi_pack(pc, 2, pkt));
    TEST_ASSERT_EQUAL(2, usb_midi_parse(pkt, msg));
    uint8_t clock = 0xF8;
    TEST_ASSERT_TRUE(usb_midi_pack(&clock, 1, pkt));
    TEST_ASSERT_EQUAL(1, usb_midi_parse(pkt, msg));
    TEST_ASSERT_FALSE(usb_midi_pack(note_on, 2, pkt));   /* note on needs 3 bytes */
    uint8_t pad[4] = { 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL(0, usb_midi_parse(pad, msg));
}

/* HID usage IDs from the USB HID Usage Tables, keyboard page (0x07). */
static void test_hid_keys(void)
{
    TEST_ASSERT_EQUAL('a', usb_hid_key(0x04, false));
    TEST_ASSERT_EQUAL('Z', usb_hid_key(0x1D, true));
    TEST_ASSERT_EQUAL('1', usb_hid_key(0x1E, false));
    TEST_ASSERT_EQUAL('!', usb_hid_key(0x1E, true));
    TEST_ASSERT_EQUAL('0', usb_hid_key(0x27, false));
    TEST_ASSERT_EQUAL(T5_HID_KEY_ENTER, usb_hid_key(0x28, false));
    TEST_ASSERT_EQUAL(T5_HID_KEY_ENTER, usb_hid_key(0x58, false));   /* keypad enter */
    TEST_ASSERT_EQUAL(T5_HID_KEY_BACKSPACE, usb_hid_key(0x2A, false));
    TEST_ASSERT_EQUAL(T5_HID_KEY_BACKTAB, usb_hid_key(0x2B, true));
    TEST_ASSERT_EQUAL('?', usb_hid_key(0x38, true));
    TEST_ASSERT_EQUAL(T5_HID_KEY_LEFT, usb_hid_key(0x50, false));
    TEST_ASSERT_EQUAL(0, usb_hid_key(0x3A, false));   /* F1: nothing */
}

static uint32_t s_keys[16];
static int s_nkeys, s_dx, s_dy, s_attached;
static bool s_button;

static void rec_key(uint32_t key, void *arg) { (void)arg; if (s_nkeys < 16) s_keys[s_nkeys++] = key; }
static void rec_mouse(int dx, int dy, bool b, void *arg) { (void)arg; s_dx += dx; s_dy += dy; s_button = b; }
static void rec_attached(t5_hid_dev_t d, bool on, void *arg) { (void)arg; s_attached += (on ? 1 : -1) * (d == T5_HID_MOUSE ? 10 : 1); }

static void test_hid_keyboard_reports(void)
{
    s_nkeys = 0;
    const t5_usb_hid_handler_t h = { .key = rec_key };
    t5_usb_set_hid_handler(&h);
    usb_hid_attached(T5_HID_KEYBOARD, true);
    TEST_ASSERT_TRUE(t5_usb_keyboard_present());
    /* Boot report: modifiers, reserved, six keys down. */
    const uint8_t a[8] = { 0, 0, 0x04 };
    const uint8_t ab[8] = { 0, 0, 0x04, 0x05 };
    const uint8_t shift_b[8] = { 0x02, 0, 0x05 };
    const uint8_t none[8] = { 0 };
    usb_hid_keyboard_report(a, sizeof a);
    usb_hid_keyboard_report(a, sizeof a);          /* held: not again */
    usb_hid_keyboard_report(ab, sizeof ab);        /* b joins */
    usb_hid_keyboard_report(none, sizeof none);
    usb_hid_keyboard_report(shift_b, sizeof shift_b);
    usb_hid_keyboard_report(a, 4);                 /* short: ignored */
    TEST_ASSERT_EQUAL(3, s_nkeys);
    TEST_ASSERT_EQUAL('a', s_keys[0]);
    TEST_ASSERT_EQUAL('b', s_keys[1]);
    TEST_ASSERT_EQUAL('B', s_keys[2]);
    usb_hid_attached(T5_HID_KEYBOARD, false);
    TEST_ASSERT_FALSE(t5_usb_keyboard_present());
}

static void test_hid_mouse_and_late_handler(void)
{
    s_dx = s_dy = s_attached = 0;
    usb_hid_attached(T5_HID_MOUSE, true);           /* before anyone listens */
    const t5_usb_hid_handler_t h = { .attached = rec_attached, .mouse = rec_mouse };
    t5_usb_set_hid_handler(&h);
    TEST_ASSERT_EQUAL(10, s_attached);              /* told on install */
    const uint8_t m[3] = { 0x01, (uint8_t)-3, 5 };
    usb_hid_mouse_report(m, sizeof m);
    TEST_ASSERT_EQUAL(-3, s_dx);
    TEST_ASSERT_EQUAL(5, s_dy);
    TEST_ASSERT_TRUE(s_button);
    usb_hid_attached(T5_HID_MOUSE, false);
    TEST_ASSERT_EQUAL(0, s_attached);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_resample_identity);
    RUN_TEST(test_resample_rate_ratio);
    RUN_TEST(test_resample_44k1_is_smooth);
    RUN_TEST(test_resample_downmix);
    RUN_TEST(test_mix_saturates);
    RUN_TEST(test_mix_ramp);
    RUN_TEST(test_wav_pcm16_stereo);
    RUN_TEST(test_wav_formats);
    RUN_TEST(test_wav_bad_and_more);
    RUN_TEST(test_peak_dbfs);
    RUN_TEST(test_sounds_end_and_stay_quiet);
    RUN_TEST(test_aec_cancels_echo);
    RUN_TEST(test_aec_keeps_near_end);
    RUN_TEST(test_energy_average);
    RUN_TEST(test_energy_evicts_lru);
    RUN_TEST(test_midi_packets);
    RUN_TEST(test_hid_keys);
    RUN_TEST(test_hid_keyboard_reports);
    RUN_TEST(test_hid_mouse_and_late_handler);
    return UNITY_END();
}
