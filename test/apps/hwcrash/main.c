/* Crash cleanup for the newer handles: opens an audio stream and a WAV
 * player, a BLE scanner, advertiser and GATT server, and a status-bar
 * activity, prints what it got, then crashes (a NULL write) from its own
 * task. After the crash the console must show no audio streams, no BLE
 * advertiser or server (a new `ble adv` succeeds), no activity and no wake
 * locks of its own. */
#include <stdio.h>
#include <string.h>
#include <t5/app.h>
#include <t5/audio.h>
#include <t5/ble.h>
#include <t5/shell.h>
#include <t5/ui.h>

/* 0.5 s of silence as a WAV, built in memory for the player. */
static uint8_t s_wav[44 + 8000];

static void make_wav(void)
{
    const uint32_t data = sizeof s_wav - 44, rate = 8000, rbytes = 16000, riff = 36 + data, fmtlen = 16;
    const uint16_t fmt = 1, ch = 1, block = 2, bits = 16;
    memcpy(s_wav, "RIFF", 4);
    memcpy(s_wav + 4, &riff, 4);
    memcpy(s_wav + 8, "WAVEfmt ", 8);
    memcpy(s_wav + 16, &fmtlen, 4);
    memcpy(s_wav + 20, &fmt, 2);
    memcpy(s_wav + 22, &ch, 2);
    memcpy(s_wav + 24, &rate, 4);
    memcpy(s_wav + 28, &rbytes, 4);
    memcpy(s_wav + 32, &block, 2);
    memcpy(s_wav + 34, &bits, 2);
    memcpy(s_wav + 36, "data", 4);
    memcpy(s_wav + 40, &data, 4);
}

static void crash(void *arg)
{
    (void)arg;
    t5_delay_ms(1500);
    printf("hwcrash: crashing now\n");
    *(volatile int *)0 = 1;
}

static void on_create(t5_app_t *app)
{
    (void)app;
    make_wav();
    t5_audio_out_t *out = NULL;
    t5_audio_player_t *pl = NULL;
    t5_ble_scanner_t *sc = NULL;
    t5_ble_advertiser_t *adv = NULL;
    t5_ble_server_t *srv = NULL;
    t5_shell_activity_t *act = NULL;
    static const t5_ble_server_chr_t chr[] = { { "0x2a19", T5_BLE_PROP_READ } };
    t5_err_t e1 = t5_audio_out_open(&(t5_audio_out_cfg_t){ .rate = 16000, .channels = 1 }, &out);
    t5_err_t e2 = t5_audio_play_mem(s_wav, sizeof s_wav, T5_AUDIO_USAGE_MEDIA, &pl);
    t5_err_t e3 = t5_ble_scan_start(&(t5_ble_scan_cfg_t){ .duration_ms = 0 }, &sc);
    t5_err_t e4 = t5_ble_adv_start(&(t5_ble_adv_cfg_t){ .name = "hwcrash" }, &adv);
    t5_err_t e5 = t5_ble_server_open("0x180f", chr, 1, &srv);
    t5_err_t e6 = t5_shell_activity_start(MD_ICON_BOLT, "About to crash", &act);
    printf("hwcrash: opened out %d player %d scan %d adv %d server %d activity %d\n", e1, e2, e3, e4, e5, e6);
    T5_IGNORE(t5_task_create("hwcrash", crash, NULL, 3072, 2, T5_CORE_ANY, NULL));   /* no task: no crash, the check fails */
}

T5_APP(.on_create = on_create);
