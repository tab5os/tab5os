/* SPDX-License-Identifier: Apache-2.0
 * Phase 6 and 7 console commands.
 *
 *   shot [div]                     the screen as it is scanned out, 1/div size, as
 *                                  base64 RGB565 between SHOT and END lines
 *   shell home|qs|toast <text>|launch <id> [arg]
 *   audio                          state and counters
 *   audio vol <0-100> | mute on|off | sounds on|off | sound <click|notify|error|shutter|start>
 *   audio tone <hz> <ms>
 *   audio play <s> [hz]            a tone stream for s seconds, report underruns (7.3)
 *   audio rec <s> [aec]            record 16 kHz mono, report levels (7.2)
 *   cam open [photo|rgb]|close|stats|photo <path>|rec <path> <s>|snap [w h]  (rec: .avi is MJPEG, else H.264)
 *   usb                            devices and counters
 *   usb host on|off | eject | midi <s> | serial <baud> <text>
 *   usbc console | storage [timeout s]
 */
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t5_audio/audio.h"
#include "t5_audio/decode.h"
#include "t5_audio/media_sys.h"
#include "t5_camera/camera.h"
#include "t5_core/os.h"
#include "t5_display/shot.h"
#include "t5_dev/control.h"
#include "t5_power/power.h"
#include "t5_shell/shell.h"
#include "t5_shell/standby.h"
#include "t5_app/apps.h"
#include "t5_ui/t5_ui.h"
#include "t5_usb/usb.h"
#include "t5_usb/usb_diag.h"
#include "diag_priv.h"

static int cmd_shot(int argc, char **argv)
{
    uint32_t div = 4;
    if (argc >= 2 && !diag_parse_u32(argv[1], 1, 16, &div)) return 1;
    t5_dev_shot_t shot;
    t5_err_t e = t5_dev_shot((int)div, &shot);
    if (e != T5_OK) {
        printf("%s\n", e == T5_ERR_STATE ? "no framebuffer" : t5_err_str(e));
        return 1;
    }
    t5_display_print_shot(shot.px, shot.w, shot.h, shot.w, 1, shot.rotation);
    t5_dev_shot_free(&shot);
    return 0;
}

/* ---- shell ------------------------------------------------------------------------- */
static int cmd_shell(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "home")) return t5_shell_home() != T5_OK;
    if (argc >= 2 && !strcmp(argv[1], "qs")) {
        t5_power_user_activity();
        return t5_shell_quick_settings(!(argc >= 3 && !strcmp(argv[2], "off"))) != T5_OK;
    }
    if (argc >= 3 && !strcmp(argv[1], "toast")) return t5_shell_toast(argv[2]) != T5_OK;
    if (argc >= 3 && !strcmp(argv[1], "launch")) {
        t5_err_t e = t5_apps_launch_arg(argv[2], argc >= 4 ? argv[3] : NULL);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (argc >= 3 && !strcmp(argv[1], "standby")) {
        const char *c = argv[2];
        if (!strcmp(c, "enable") || !strcmp(c, "disable")) {
            t5_shell_set_standby_enabled(!strcmp(c, "enable"));
            return 0;
        }
        t5_power_user_activity();
        t5_err_t e = t5_shell_standby(!strcmp(c, "on"));
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (argc == 2 && !strcmp(argv[1], "standby")) {
        printf("standby %s, %s\n", t5_shell_standby_enabled() ? "enabled" : "disabled",
               t5_shell_standby_shown() ? "shown" : "not shown");
        return 0;
    }
    printf("usage: shell home | qs [off] | toast <text> | launch <id> [arg] | standby [on|off|enable|disable]\n");
    return 1;
}

/* ---- media sessions ------------------------------------------------------------------ */
static int cmd_media(int argc, char **argv)
{
    if (argc == 1) {
        t5_media_now_t m;
        if (!t5_media_now(&m)) {
            printf("no media session\n");
            return 0;
        }
        static const char *st[] = { "stopped", "playing", "paused" };
        printf("%s: %s, \"%s\" by \"%s\", %u / %u ms\n", m.app_id, st[m.state], m.info.title, m.info.artist,
               (unsigned)m.position_ms, (unsigned)m.info.duration_ms);
        return 0;
    }
    static const struct { const char *name; t5_media_cmd_type_t type; } cmds[] = {
        { "play", T5_MEDIA_CMD_PLAY }, { "pause", T5_MEDIA_CMD_PAUSE }, { "toggle", T5_MEDIA_CMD_TOGGLE },
        { "next", T5_MEDIA_CMD_NEXT }, { "prev", T5_MEDIA_CMD_PREVIOUS }, { "stop", T5_MEDIA_CMD_STOP },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        if (!strcmp(argv[1], cmds[i].name)) {
            t5_err_t e = t5_media_command(&(t5_media_cmd_t){ .type = cmds[i].type });
            printf("%s\n", t5_err_str(e));
            return e != T5_OK;
        }
    }
    printf("usage: media [play | pause | toggle | next | prev | stop]\n");
    return 1;
}

/* ---- audio --------------------------------------------------------------------------- */
static void audio_print(void)
{
    t5_audio_stats_t s;
    t5_audio_stats(&s);
    printf("audio %s: out %s, in %s, %s, volume %d%s, sounds %s\n", s.ready ? "ready" : "not ready",
           s.out_running ? "running" : "off", s.in_running ? "running" : "off",
           s.headphones ? "headphones" : (s.speaker_on ? "speaker on" : "speaker off"), s.volume,
           s.muted ? " (muted)" : "", t5_audio_system_sounds() ? "on" : "off");
    printf("  frames out %u in %u, underruns %u, overruns %u, codec errors %u, opens %u, loop max %u us\n",
           (unsigned)s.out_frames, (unsigned)s.in_frames, (unsigned)s.underruns, (unsigned)s.overruns,
           (unsigned)s.codec_errors, (unsigned)s.opens, (unsigned)s.loop_max_us);
    printf("  peak out %d dBFS, in %d dBFS, ERLE %.1f dB, mic gain %.1f dB\n", s.out_peak_dbfs, s.in_peak_dbfs,
           (double)s.aec_erle_db, (double)t5_audio_in_gain_db());
}

static int audio_play(uint32_t seconds, uint32_t hz)
{
    t5_audio_out_cfg_t c = { .rate = 44100, .channels = 2, .buffer_ms = 300 };
    t5_audio_out_t *o;
    t5_err_t e = t5_audio_out_open(&c, &o);
    if (e != T5_OK) {
        printf("open: %s\n", t5_err_str(e));
        return 1;
    }
    t5_audio_stats_t s0, s1;
    t5_audio_stats(&s0);
    EXT_RAM_BSS_ATTR static int16_t buf[441 * 2];
    uint32_t phase = 0, inc = (uint32_t)(((uint64_t)hz << 32) / 44100);
    int64_t t0 = t5_time_us();
    for (uint32_t blk = 0; blk < seconds * 100; blk++) {
        for (int i = 0; i < 441; i++) {
            phase += inc;
            int32_t p = (int32_t)(phase >> 16) - 32768;
            int16_t v = (int16_t)(((p < 0 ? -p : p) * 2 - 32768) / 8);
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        if (t5_audio_out_write(o, buf, sizeof buf, 1000) != (int)sizeof buf) {
            printf("write stalled at block %u\n", (unsigned)blk);
            break;
        }
        if (blk % 1000 == 999) {
            t5_audio_stats(&s1);
            printf("  %u s: underruns %u, power %s\n", (unsigned)((blk + 1) / 100),
                   (unsigned)(s1.underruns - s0.underruns), t5_power_state_name(t5_power_state()));
        }
    }
    t5_audio_out_drain(o, 2000);
    t5_audio_stats(&s1);
    t5_audio_out_close(o);
    printf("played %u s in %u ms: underruns %u, loop max %u us\n", (unsigned)seconds,
           (unsigned)((t5_time_us() - t0) / 1000), (unsigned)(s1.underruns - s0.underruns), (unsigned)s1.loop_max_us);
    return s1.underruns != s0.underruns;
}

static int audio_rec(uint32_t seconds, bool aec)
{
    t5_audio_in_cfg_t c = { .rate = 16000, .channels = 1, .aec = aec, .buffer_ms = 500 };
    t5_audio_in_t *in;
    t5_err_t e = t5_audio_in_open(&c, &in);
    if (e != T5_OK) {
        printf("open: %s\n", t5_err_str(e));
        return 1;
    }
    EXT_RAM_BSS_ATTR static int16_t buf[1600];
    uint32_t total = 0, next = 1;
    int peak = -96, sec_peak = -96;
    double sec_sq = 0;
    uint32_t sec_n = 0;
    int64_t t0 = t5_time_us(), end = t0 + (int64_t)seconds * 1000000;
    while (t5_time_us() < end) {
        int n = t5_audio_in_read(in, buf, sizeof buf, 300);
        if (n <= 0) continue;
        total += (uint32_t)n;
        int p = t5_pcm_peak_dbfs(buf, (size_t)n / 2);
        if (p > peak) peak = p;
        if (p > sec_peak) sec_peak = p;
        for (int i = 0; i < n / 2; i++) sec_sq += (double)buf[i] * buf[i];
        sec_n += (uint32_t)n / 2;
        if (t5_time_us() - t0 >= (int64_t)next * 1000000) {
            t5_audio_stats_t st;
            t5_audio_stats(&st);
            double rms = sec_n ? sqrt(sec_sq / sec_n) : 0;
            printf("  %u s: peak %d dBFS, rms %.1f dBFS, frames in %u out %u\n", (unsigned)next, sec_peak,
                   rms > 0 ? 20 * log10(rms / 32768) : -96.0, (unsigned)st.in_frames, (unsigned)st.out_frames);
            sec_peak = -96;
            sec_sq = 0;
            sec_n = 0;
            next++;
        }
    }
    t5_audio_stats_t s;
    t5_audio_stats(&s);
    t5_audio_in_close(in);
    printf("recorded %u bytes (%u expected), peak %d dBFS, overruns %u%s\n", (unsigned)total,
           (unsigned)(seconds * 32000), peak, (unsigned)s.overruns, aec ? "" : "");
    if (aec) printf("  ERLE %.1f dB\n", (double)s.aec_erle_db);
    return 0;
}

/* Echo test: noise from the speaker while recording, with and without the
 * echo canceller. */
static volatile uint32_t s_echo_secs;

static void echo_player(void *arg)
{
    (void)arg;
    t5_audio_out_cfg_t c = { .rate = 16000, .channels = 1, .buffer_ms = 300 };
    t5_audio_out_t *o;
    if (t5_audio_out_open(&c, &o) != T5_OK) return;
    static int16_t buf[160];
    uint32_t seed = 7;
    for (uint32_t blk = 0; blk < s_echo_secs * 100 + 50; blk++) {
        for (int i = 0; i < 160; i++) {
            seed = seed * 1664525u + 1013904223u;
            buf[i] = (int16_t)((int32_t)(seed >> 16) - 32768) / 6;
        }
        t5_audio_out_write(o, buf, sizeof buf, 1000);
    }
    t5_audio_out_drain(o, 2000);
    t5_audio_out_close(o);
}

static int audio_echo(uint32_t seconds)
{
    s_echo_secs = seconds;
    for (int pass = 0; pass < 2; pass++) {
        printf("%s echo canceller:\n", pass ? "with" : "without");
        t5_task_create("echo.play", echo_player, NULL, 4096, 5, 0, NULL);
        t5_delay_ms(300);
        audio_rec(seconds, pass == 1);
        t5_delay_ms(1500);
    }
    return 0;
}

/* A sine WAV for the file tests: 16-bit, mono or stereo. */
static int audio_wav_write(const char *path, uint32_t seconds, uint32_t hz, uint32_t rate, uint16_t ch)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("can't create %s\n", path);
        return 1;
    }
    uint32_t frames = seconds * rate, data = frames * ch * 2;
    uint8_t h[44] = "RIFF....WAVEfmt ";
    const uint32_t v32[] = { 36 + data, 16, rate, rate * ch * 2, data };
    memcpy(h + 4, &v32[0], 4);
    memcpy(h + 16, &v32[1], 4);
    const uint16_t fmt[] = { 1, ch };
    memcpy(h + 20, fmt, 4);
    memcpy(h + 24, &v32[2], 4);
    memcpy(h + 28, &v32[3], 4);
    const uint16_t blk[] = { (uint16_t)(ch * 2), 16 };
    memcpy(h + 32, blk, 4);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &v32[4], 4);
    fwrite(h, 1, sizeof h, f);
    EXT_RAM_BSS_ATTR static int16_t buf[480 * 2];
    for (uint32_t done = 0; done < frames;) {
        uint32_t n = frames - done < 480 ? frames - done : 480;
        for (uint32_t i = 0; i < n; i++) {
            int16_t v = (int16_t)(sinf(2.0f * (float)M_PI * (float)hz * (float)(done + i) / (float)rate) * 8000.0f);
            for (uint16_t c = 0; c < ch; c++) buf[i * ch + c] = v;
        }
        fwrite(buf, (size_t)n * ch * 2, 1, f);
        done += n;
    }
    fclose(f);
    printf("%s: %u s of %u Hz, %u Hz, %u ch\n", path, (unsigned)seconds, (unsigned)hz, (unsigned)rate, (unsigned)ch);
    return 0;
}

static int audio_file(const char *path)
{
    t5_audio_dec_t *d;
    t5_err_t e = t5_audio_dec_open(path, &d);
    if (e != T5_OK) {
        printf("%s: %s\n", path, t5_err_str(e));
        return 1;
    }
    t5_audio_info_t in;
    T5_IGNORE(t5_audio_dec_info(d, &in));    /* a decoder just opened */
    printf("%s: WAV %u Hz, %u ch (%u in the file), %u-bit, %u ms\n", path, (unsigned)in.rate,
           (unsigned)in.channels, (unsigned)in.file_channels, (unsigned)in.bits, (unsigned)in.duration_ms);
    T5_IGNORE(t5_audio_dec_close(d));
    t5_audio_player_t *p;
    e = t5_audio_play_file(path, T5_AUDIO_USAGE_MEDIA, &p);
    if (e != T5_OK) {
        printf("play: %s\n", t5_err_str(e));
        return 1;
    }
    int64_t t0 = t5_time_us();
    e = t5_audio_player_wait(p, in.duration_ms + 3000);
    uint32_t pos = 0;
    t5_audio_player_state_t st = t5_audio_player_state(p, &pos);
    printf("%s after %u ms: %s at %u ms\n", t5_err_str(e), (unsigned)((t5_time_us() - t0) / 1000),
           st == T5_AUDIO_PLAYER_ENDED ? "ended" : st == T5_AUDIO_PLAYER_ERROR ? "error" : "playing", (unsigned)pos);
    T5_IGNORE(t5_audio_player_close(p));
    return e != T5_OK || st != T5_AUDIO_PLAYER_ENDED;
}

/* Ducking: the output's peak with a MEDIA tone alone, then with an ALERT
 * stream (silent) open beside it: should drop about 12 dB. */
static int audio_duck(void)
{
    t5_audio_out_cfg_t c = { .rate = 48000, .channels = 1, .buffer_ms = 200 };
    t5_audio_out_t *m = NULL, *a = NULL;
    t5_err_t e = t5_audio_out_open(&c, &m);
    if (e == T5_OK) e = t5_audio_out_open_usage(&c, T5_AUDIO_USAGE_ALERT, &a);
    if (e != T5_OK) {
        printf("open: %s\n", t5_err_str(e));
        if (m) T5_IGNORE(t5_audio_out_close(m));
        return 1;
    }
    EXT_RAM_BSS_ATTR static int16_t tone[480], quiet[480];
    for (int i = 0; i < 480; i++) tone[i] = (int16_t)(sinf(2.0f * (float)M_PI * 1000.0f * (float)i / 48000.0f) * 16000.0f);
    int peak[2] = { 0 };
    for (int phase = 0; phase < 2; phase++) {
        int lo = 0;
        for (int blk = 0; blk < 60; blk++) {        /* 600 ms; read the last 200 */
            t5_audio_out_write(m, tone, sizeof tone, 100);
            if (phase) t5_audio_out_write(a, quiet, sizeof quiet, 100);
            if (blk >= 40) {
                t5_audio_stats_t s;
                t5_audio_stats(&s);
                lo = blk == 40 ? s.out_peak_dbfs : (s.out_peak_dbfs < lo ? s.out_peak_dbfs : lo);
            }
        }
        peak[phase] = lo;
    }
    T5_IGNORE(t5_audio_out_close(a));
    T5_IGNORE(t5_audio_out_close(m));
    int drop = peak[0] - peak[1];
    printf("media alone %d dBFS, under an alert %d dBFS: ducked %d dB (want 9..15)\n", peak[0], peak[1], drop);
    return drop < 9 || drop > 15;
}

static int cmd_audio(int argc, char **argv)
{
    if (argc == 1) {
        audio_print();
        return 0;
    }
    const char *c = argv[1];
    uint32_t a, b;
    if (!strcmp(c, "vol") && argc == 3 && diag_parse_u32(argv[2], 0, 100, &a)) return t5_audio_set_volume((int)a) != T5_OK;
    if (!strcmp(c, "mute") && argc == 3) return t5_audio_set_muted(!strcmp(argv[2], "on")) != T5_OK;
    if (!strcmp(c, "sounds") && argc == 3) {
        t5_audio_set_system_sounds(!strcmp(argv[2], "on"));
        return 0;
    }
    if (!strcmp(c, "sound") && argc == 3) {
        static const char *n[] = { "click", "notify", "error", "shutter", "start" };
        for (int i = 0; i < T5_SOUND_COUNT; i++) {
            if (!strcmp(argv[2], n[i])) {
                t5_audio_play_sound((t5_sound_t)i);
                return 0;
            }
        }
    }
    if (!strcmp(c, "tone") && argc == 4 && diag_parse_u32(argv[2], 20, 20000, &a) && diag_parse_u32(argv[3], 10, 10000, &b)) {
        t5_err_t e = t5_audio_tone(a, b);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "play") && argc >= 3 && diag_parse_u32(argv[2], 1, 7200, &a)) {
        b = 440;
        if (argc >= 4 && !diag_parse_u32(argv[3], 20, 20000, &b)) return 1;
        return audio_play(a, b);
    }
    if (!strcmp(c, "echo") && argc == 3 && diag_parse_u32(argv[2], 1, 60, &a)) return audio_echo(a);
    if (!strcmp(c, "file") && argc == 3) return audio_file(argv[2]);
    if (!strcmp(c, "duck") && argc == 2) return audio_duck();
    if (!strcmp(c, "wav") && argc >= 5 && diag_parse_u32(argv[3], 1, 60, &a) && diag_parse_u32(argv[4], 20, 20000, &b)) {
        uint32_t rate = 44100, ch = 2;
        if (argc >= 6 && !diag_parse_u32(argv[5], 8000, 48000, &rate)) return 1;
        if (argc >= 7 && !diag_parse_u32(argv[6], 1, 2, &ch)) return 1;
        return audio_wav_write(argv[2], a, b, rate, (uint16_t)ch);
    }
    if (!strcmp(c, "rec") && argc >= 3 && diag_parse_u32(argv[2], 1, 600, &a))
        return audio_rec(a, argc >= 4 && !strcmp(argv[3], "aec"));
    printf("usage: audio [vol <n> | mute on|off | sounds on|off | sound <name> | tone <hz> <ms> |\n"
           "              play <s> [hz] | rec <s> [aec] | echo <s> | duck |\n"
           "              wav <path> <s> <hz> [rate] [ch] | file <path>]\n");
    return 1;
}

/* ---- camera ------------------------------------------------------------------------------ */
static t5_camera_t *s_cam;
static lv_obj_t *s_uirec_img;
typedef struct { bool start; t5_err_t err; } uirec_t;
static void uirec_preview(void *p)
{
    if (p) {
        if (s_uirec_img) lv_obj_delete(s_uirec_img);
        s_uirec_img = NULL;
        return;
    }
    if (!s_uirec_img && s_cam) {
        /* A frame about the Camera app's size, not the whole screen. */
        s_uirec_img = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(s_uirec_img);
        lv_obj_set_size(s_uirec_img, 1000, 562);
        lv_obj_center(s_uirec_img);
        t5_camera_preview_create(s_cam, s_uirec_img);
    }
}
static void uirec_call(void *p)
{
    uirec_t *r = p;
    r->err = r->start ? t5_camera_record_start(s_cam, "/sd/uirec.h264", 6000) : t5_camera_record_stop(s_cam);
}

static void cam_print(void)
{
    t5_camera_stats_t s;
    t5_camera_stats(&s);
    printf("camera %s%s: %u frames (%u dropped), %u fps, preview %u fps (%u frames), photos %u\n",
           s.open ? "open" : "closed", s.recording ? ", recording" : "", (unsigned)s.frames, (unsigned)s.dropped,
           (unsigned)s.fps, (unsigned)s.preview_fps, (unsigned)s.preview_frames, (unsigned)s.photos);
    printf("  recording: %u frames, %u dropped, %llu bytes, %u ms, encode max %u us, buffer peak %u KB\n",
           (unsigned)s.rec_frames, (unsigned)s.rec_dropped, (unsigned long long)s.rec_bytes, (unsigned)s.rec_ms,
           (unsigned)s.encode_us_max, (unsigned)(s.write_buffer_peak / 1024));
}

static int cmd_cam(int argc, char **argv)
{
    if (argc == 1 || !strcmp(argv[1], "stats")) {
        cam_print();
        return 0;
    }
    const char *c = argv[1];
    /* Sleep closes the camera behind our back (its suspend hook). */
    if (s_cam && !t5_camera_active()) s_cam = NULL;
    if (!strcmp(c, "open")) {
        if (s_cam) return 0;
        bool photo = argc == 3 && !strcmp(argv[2], "photo"), rgb = argc == 3 && !strcmp(argv[2], "rgb");
        t5_err_t e = photo ? t5_camera_open_photo(&s_cam) : rgb ? t5_camera_open_rgb(&s_cam) : t5_camera_open(&s_cam);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "uirec") && argc == 3 && s_cam) {
        /* As the Camera app does it: a live preview, and recording started
         * and stopped from the UI task. */
        uint32_t secs;
        if (!diag_parse_u32(argv[2], 1, 600, &secs)) return 1;
        t5_ui_sync(uirec_preview, NULL, 2000);
        vTaskDelay(pdMS_TO_TICKS(1500));
        uirec_t r = { .start = true };
        int64_t t0 = esp_timer_get_time();
        t5_ui_sync(uirec_call, &r, 8000);
        printf("start from the UI task: %s in %lld ms (expect STATE)\n", t5_err_str(r.err), (long long)(esp_timer_get_time() - t0) / 1000);
        /* Then as the Camera app does it: from a worker, with the preview live. */
        t0 = esp_timer_get_time();
        r.err = t5_camera_record_start(s_cam, "/sd/uirec.h264", 6000);
        printf("start from a worker: %s in %lld ms\n", t5_err_str(r.err), (long long)(esp_timer_get_time() - t0) / 1000);
        vTaskDelay(pdMS_TO_TICKS(secs * 1000));
        t0 = esp_timer_get_time();
        r.err = t5_camera_record_stop(s_cam);
        printf("stop from a worker: %s in %lld ms\n", t5_err_str(r.err), (long long)(esp_timer_get_time() - t0) / 1000);
        cam_print();
        t5_ui_sync(uirec_preview, (void *)1, 2000);
        return r.err != T5_OK;
    }
    if (!strcmp(c, "close")) {
        if (s_cam) t5_camera_close(s_cam);
        s_cam = NULL;
        return 0;
    }
    if (!strcmp(c, "photo") && argc == 3 && s_cam) {
        t5_err_t e = t5_camera_photo(s_cam, argv[2], 90);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "rec") && argc == 4 && s_cam) {
        uint32_t secs;
        if (!diag_parse_u32(argv[3], 1, 3600, &secs)) return 1;
        t5_err_t e = t5_camera_record_start(s_cam, argv[2], 6000);
        if (e != T5_OK) {
            printf("start: %s\n", t5_err_str(e));
            return 1;
        }
        for (uint32_t t = 0; t < secs; t++) {
            t5_delay_ms(1000);
            if (t % 30 == 29) cam_print();
        }
        t5_camera_record_stop(s_cam);
        cam_print();
        return 0;
    }
    if (!strcmp(c, "snap")) {
        uint32_t w = 160, h = 90;
        if (argc == 4 && (!diag_parse_u32(argv[2], 16, 640, &w) || !diag_parse_u32(argv[3], 16, 640, &h))) return 1;
        uint16_t *px = heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM);
        if (!px) return 1;
        t5_err_t e = t5_camera_snapshot(px, (int)w, (int)h);
        if (e == T5_OK) t5_display_print_shot(px, (int)w, (int)h, (int)w, 1, -1);
        else printf("snap: %s (needs an open camera with a preview)\n", t5_err_str(e));
        heap_caps_free(px);
        return e != T5_OK;
    }
    printf("usage: cam [open [photo|rgb] | close | stats | photo <path> | rec <path> <s> | uirec <s> | snap [w h]]\n");
    return 1;
}

/* ---- USB ---------------------------------------------------------------------------------- */
static void midi_cb(const uint8_t *m, size_t n, void *arg)
{
    (void)arg;
    printf("  midi");
    for (size_t i = 0; i < n; i++) printf(" %02x", m[i]);
    printf("\n");
}

static int cmd_usb(int argc, char **argv)
{
    if (argc == 1) {
        t5_usb_stats_t s;
        t5_usb_stats(&s);
        printf("USB-A host %s (installed %s): %u devices, %u attached, %u detached, keyboard %s, mouse %s, /usb %s\n",
               s.host_on ? "on" : "off", s.installed ? "yes" : "no", s.devices, (unsigned)s.attached,
               (unsigned)s.detached, t5_usb_keyboard_present() ? "yes" : "no", t5_usb_mouse_present() ? "yes" : "no",
               t5_usb_storage_mounted() ? "mounted" : "no");
        printf("  keys %u, mouse reports %u, midi in %u, uvc frames %u (errors %u), errors %u; USB-C %s\n",
               (unsigned)s.keys, (unsigned)s.mouse_reports, (unsigned)s.midi_in, (unsigned)s.uvc_frames,
               (unsigned)s.uvc_errors, (unsigned)s.errors, t5_usbc_mode() == T5_USBC_STORAGE ? "storage" : "console");
        t5_usb_dev_t d[T5_USB_MAX_DEVICES];
        int n = t5_usb_devices(d, T5_USB_MAX_DEVICES);
        for (int i = 0; i < n; i++)
            printf("  addr %u  %04x:%04x  %-8s %s speed  %s%s\n", d[i].addr, d[i].vid, d[i].pid,
                   t5_usb_class_name(d[i].cls), d[i].speed == 2 ? "high" : d[i].speed == 1 ? "full" : "low",
                   d[i].product, d[i].driver ? "" : " (no driver)");
        return 0;
    }
    const char *c = argv[1];
    if (!strcmp(c, "host") && argc == 3) {
        t5_err_t e = t5_usb_host_enable(!strcmp(argv[2], "on"));
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "eject")) return t5_usb_storage_eject() != T5_OK;
    if (!strcmp(c, "midi") && argc == 3) {
        uint32_t s;
        if (!diag_parse_u32(argv[2], 1, 600, &s)) return 1;
        if (!t5_usb_midi_present()) printf("no MIDI device (listening anyway)\n");
        t5_usb_midi_listen(midi_cb, NULL);
        t5_delay_ms(s * 1000);
        t5_usb_midi_listen(NULL, NULL);
        return 0;
    }
    if (!strcmp(c, "serial") && argc == 4) {
        uint32_t baud;
        if (!diag_parse_u32(argv[2], 300, 4000000, &baud)) return 1;
        t5_usb_serial_t *s;
        t5_err_t e = t5_usb_serial_open(baud, &s);
        if (e != T5_OK) {
            printf("open: %s\n", t5_err_str(e));
            return 1;
        }
        t5_usb_serial_write(s, argv[3], strlen(argv[3]), 500);
        t5_usb_serial_write(s, "\r\n", 2, 500);
        char buf[129];
        int n = t5_usb_serial_read(s, buf, 128, 1000);
        if (n > 0) {
            buf[n] = 0;
            printf("reply: %s\n", buf);
        }
        t5_usb_serial_close(s);
        return 0;
    }
    printf("usage: usb [host on|off | eject | midi <s> | serial <baud> <text>]\n");
    return 1;
}

static int cmd_usbc(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "console")) return t5_usbc_set_mode(T5_USBC_CONSOLE, 0) != T5_OK;
    if (argc >= 2 && !strcmp(argv[1], "storage")) {
        uint32_t t = 0;
        if (argc >= 3 && !diag_parse_u32(argv[2], 5, 3600, &t)) return 1;
        if (t) printf("USB-C mass storage for %u s (or until ejected); the console comes back afterwards\n", (unsigned)t);
        else printf("USB-C mass storage until the computer ejects the card; then the console comes back\n");
        fflush(stdout);
        t5_err_t e = t5_usbc_set_mode(T5_USBC_STORAGE, t);
        if (e != T5_OK) printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (argc >= 2 && !strcmp(argv[1], "trace")) {
        printf("hand-back trace 0x%08x\n", (unsigned)t5_usbc_trace());
        return 0;
    }
    printf("usage: usbc console | storage [timeout s]   (no timeout: until the computer ejects it)\n");
    return 1;
}

void diag_register_media(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "shot", .help = "Screen as base64 RGB565", .hint = "[div]", .func = cmd_shot },
        { .command = "shell", .help = "Shell: home, quick settings, toast, launch", .hint = "[home|qs|toast|launch]",
          .func = cmd_shell },
        { .command = "media", .help = "The active media session: state, commands", .hint = "[play|pause|next|...]",
          .func = cmd_media },
        { .command = "audio", .help = "Audio: state, volume, tones, play/record tests",
          .hint = "[vol|mute|sounds|sound|tone|play|rec]", .func = cmd_audio },
        { .command = "cam", .help = "Camera: open, stats, photo, record, snapshot",
          .hint = "[open|close|stats|photo|rec|snap]", .func = cmd_cam },
        { .command = "usb", .help = "USB-A devices and classes", .hint = "[host|eject|midi|serial]", .func = cmd_usb },
        { .command = "usbc", .help = "USB-C role (console or mass storage)", .hint = "console|storage [s]",
          .func = cmd_usbc },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
