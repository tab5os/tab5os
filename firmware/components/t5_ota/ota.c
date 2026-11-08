/* SPDX-License-Identifier: Apache-2.0
 * OS updates, boot health and safe mode (plan 9.1, 9.3): see ota.h.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/efuse_hal.h"
#include "psa/crypto.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_ota/bootctl.h"
#include "t5_ota/ota.h"
#include "ota_keys.h"

static const char *TAG = "t5.ota";

#define TRIAL_MS (60 * 1000)
#define CHUNK 4096

struct t5_ota {
    t5_ota_hdr_t hdr;
    const esp_partition_t *part;
    esp_ota_handle_t h;
    psa_hash_operation_t sha;
    uint32_t done;
    uint8_t head[128];           /* the image's first bytes, checked before writing */
    size_t nhead;
    bool checked;
    int key;                     /* index into t5_ota_keys, -1 unsigned or unknown */
    bool allow_unsigned;
};

static t5_ota_status_t s_st;
static bool s_trial, s_safe;
static int s_failed;
static t5_mutex_t s_lock;

static void lock(void)
{
    if (!s_lock) s_lock = t5_mutex_create();
    t5_mutex_lock(s_lock);
}

static void unlock(void)
{
    t5_mutex_unlock(s_lock);
}

static void fail(t5_ota_t *o, char *err, size_t n, const char *why)
{
    (void)o;
    snprintf(err, n, "%s", why);
    lock();
    s_st.state = T5_OTA_FAILED;
    snprintf(s_st.error, sizeof s_st.error, "%s", err);
    unlock();
    T5_LOGW(TAG, "update refused: %s", err);
}

/* ---- streaming install --------------------------------------------------------------- */
static int find_key(const uint8_t id[32])
{
    for (int i = 0; i < t5_ota_nkeys; i++) {
        if (!memcmp(t5_ota_keys[i].id, id, 32)) return i;
    }
    return -1;
}

t5_err_t t5_ota_begin(const t5_ota_hdr_t *hdr, bool allow_unsigned, t5_ota_t **out, char *err, size_t errlen)
{
    *out = NULL;
    if (memcmp(hdr->magic, T5_OTA_MAGIC, 4) || hdr->version != T5_OTA_HDR_VERSION) {
        fail(NULL, err, errlen, "not a Tab5 OS update (.t5os)");
        return T5_ERR_INVALID_ARG;
    }
    if (s_trial) {
        fail(NULL, err, errlen, "the running OS is still on trial: wait a minute after an update");
        return T5_ERR_STATE;
    }
    int key = hdr->sig_len == 64 ? find_key(hdr->key_id) : -1;
    if (key < 0 && !allow_unsigned) {
        fail(NULL, err, errlen,
             hdr->sig_len ? "signed by a key this OS doesn't trust" : "unsigned: install it in developer mode");
        return T5_ERR_DENIED;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || hdr->image_len < 1024 || hdr->image_len > part->size) {
        fail(NULL, err, errlen, part ? "the image doesn't fit the update slot" : "no update slot");
        return T5_ERR_INVALID_ARG;
    }
    /* An earlier install may have made this slot the next boot: point the
     * next boot back at the running system before overwriting it, so a
     * failed attempt can't leave a half-written image to boot. */
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot && boot->address == part->address) esp_ota_set_boot_partition(esp_ota_get_running_partition());
    t5_ota_t *o = calloc(1, sizeof *o);
    if (!o) return T5_ERR_NO_MEM;
    o->hdr = *hdr;
    o->part = part;
    o->key = key;
    o->allow_unsigned = allow_unsigned;
    psa_crypto_init();
    o->sha = psa_hash_operation_init();
    if (psa_hash_setup(&o->sha, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        esp_ota_begin(part, hdr->image_len, &o->h) != ESP_OK) {
        psa_hash_abort(&o->sha);
        free(o);
        fail(NULL, err, errlen, "can't open the update slot");
        return T5_ERR_IO;
    }
    lock();
    memset(&s_st, 0, sizeof s_st);
    s_st.state = T5_OTA_RECEIVING;
    s_st.total = hdr->image_len;
    snprintf(s_st.version, sizeof s_st.version, "%.15s", hdr->os_version);
    s_st.signed_ok = key >= 0;
    if (key >= 0) {
        for (int i = 0; i < 8; i++) snprintf(s_st.signer + 2 * i, 3, "%02x", hdr->key_id[i]);
    }
    unlock();
    T5_LOGI(TAG, "receiving Tab5 OS %.15s (%u bytes, %s) into %s", hdr->os_version, (unsigned)hdr->image_len,
            key >= 0 ? t5_ota_keys[key].name : "unsigned, developer mode", part->label);
    *out = o;
    return T5_OK;
}

/* The first 128 bytes: image header and app description (after the first
 * segment header). */
static t5_err_t check_head(t5_ota_t *o)
{
    const esp_image_header_t *ih = (const esp_image_header_t *)o->head;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(o->head + sizeof(esp_image_header_t) +
                                                       sizeof(esp_image_segment_header_t));
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC || ih->chip_id != ESP_CHIP_ID_ESP32P4) return T5_ERR_INVALID_ARG;
    /* rev1 and rev3 builds differ in exactly this range. */
    uint32_t rev = efuse_hal_chip_revision();
    if (rev < ih->min_chip_rev_full || rev > ih->max_chip_rev_full) {
        T5_LOGW(TAG, "image is for chip revisions v%u.%u-v%u.%u, this is v%u.%u", ih->min_chip_rev_full / 100,
                ih->min_chip_rev_full % 100, ih->max_chip_rev_full / 100, ih->max_chip_rev_full % 100,
                (unsigned)rev / 100, (unsigned)rev % 100);
        return T5_ERR_NOT_SUPPORTED;
    }
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD || strncmp(d->project_name, "tab5os", sizeof d->project_name)) {
        return T5_ERR_INVALID_ARG;      /* some other ESP-IDF app */
    }
    return T5_OK;
}

t5_err_t t5_ota_write(t5_ota_t *o, const void *data, size_t len)
{
    if (!o) return T5_ERR_INVALID_ARG;
    if (o->done + len > o->hdr.image_len) return T5_ERR_INVALID_ARG;
    const uint8_t *p = data;
    if (!o->checked) {
        size_t take = len < sizeof o->head - o->nhead ? len : sizeof o->head - o->nhead;
        memcpy(o->head + o->nhead, p, take);
        o->nhead += take;
        if (o->nhead == sizeof o->head) {
            t5_err_t e = check_head(o);
            if (e != T5_OK) return e;
            o->checked = true;
        }
    }
    psa_hash_update(&o->sha, p, len);
    if (esp_ota_write(o->h, p, len) != ESP_OK) return T5_ERR_IO;
    o->done += (uint32_t)len;
    lock();
    s_st.done = o->done;
    unlock();
    return T5_OK;
}

static bool verify_sig(const t5_ota_t *o, const uint8_t hash[32])
{
    const t5_ota_key_t *k = &t5_ota_keys[o->key];
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_key_id_t id = 0;
    bool ok = false;
    if (psa_import_key(&a, k->point, sizeof k->point, &id) == PSA_SUCCESS) {
        ok = psa_verify_hash(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, 32, o->hdr.sig, 64) == PSA_SUCCESS;
        psa_destroy_key(id);
    }
    psa_reset_key_attributes(&a);
    return ok;
}

t5_err_t t5_ota_finish(t5_ota_t *o, char *err, size_t errlen)
{
    if (!o) return T5_ERR_INVALID_ARG;
    lock();
    s_st.state = T5_OTA_VERIFYING;
    unlock();
    uint8_t hash[32];
    size_t hl = 0;
    t5_err_t e = T5_OK;
    psa_hash_finish(&o->sha, hash, sizeof hash, &hl);
    if (o->done != o->hdr.image_len || !o->checked) {
        fail(o, err, errlen, "the image is incomplete");
        e = T5_ERR_INVALID_ARG;
    } else if (memcmp(hash, o->hdr.image_sha256, 32)) {
        fail(o, err, errlen, "the image doesn't match its hash (damaged in transfer?)");
        e = T5_ERR_INVALID_ARG;
    } else if (o->key >= 0 && !verify_sig(o, hash)) {
        fail(o, err, errlen, "the signature doesn't verify");
        e = T5_ERR_DENIED;
    }
    if (e != T5_OK) {
        esp_ota_abort(o->h);
        free(o);
        return e;
    }
    /* esp_ota_end checks the image structure (segments, its own SHA-256). */
    if (esp_ota_end(o->h) != ESP_OK) {
        fail(o, err, errlen, "the image failed ESP-IDF's checks");
        free(o);
        return T5_ERR_INVALID_ARG;
    }
    if (esp_ota_set_boot_partition(o->part) != ESP_OK) {
        fail(o, err, errlen, "can't switch the boot slot");
        free(o);
        return T5_ERR_IO;
    }
    T5_LOGI(TAG, "Tab5 OS %.15s installed in %s; it runs on trial after a restart", o->hdr.os_version,
            o->part->label);
    lock();
    s_st.state = T5_OTA_READY;
    unlock();
    free(o);
    return T5_OK;
}

void t5_ota_abort(t5_ota_t *o)
{
    if (!o) return;
    psa_hash_abort(&o->sha);
    esp_ota_abort(o->h);
    free(o);
    lock();
    if (s_st.state == T5_OTA_RECEIVING) s_st.state = T5_OTA_FAILED;
    unlock();
}

void t5_ota_status(t5_ota_status_t *out)
{
    lock();
    *out = s_st;
    unlock();
}

/* ---- sources ------------------------------------------------------------------------- */
t5_err_t t5_ota_install_file(const char *path, bool allow_unsigned, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "can't open %s", path);
        return T5_ERR_NOT_FOUND;
    }
    t5_ota_hdr_t hdr;
    t5_ota_t *o = NULL;
    t5_err_t e = fread(&hdr, 1, sizeof hdr, f) == sizeof hdr ? T5_OK : T5_ERR_INVALID_ARG;
    if (e != T5_OK) snprintf(err, errlen, "not a .t5os file");
    if (e == T5_OK) e = t5_ota_begin(&hdr, allow_unsigned, &o, err, errlen);
    uint8_t *buf = e == T5_OK ? malloc(CHUNK) : NULL;
    if (e == T5_OK && !buf) e = T5_ERR_NO_MEM;
    while (e == T5_OK) {
        size_t n = fread(buf, 1, CHUNK, f);
        if (!n) break;
        e = t5_ota_write(o, buf, n);
        if (e != T5_OK) {
            snprintf(err, errlen, "%s", e == T5_ERR_NOT_SUPPORTED ? "built for a different chip revision"
                                        : e == T5_ERR_INVALID_ARG ? "not a Tab5 OS image" : "write failed");
        }
    }
    free(buf);
    fclose(f);
    if (e != T5_OK) {
        t5_ota_abort(o);
        return e;
    }
    return t5_ota_finish(o, err, errlen);
}

t5_err_t t5_ota_install_url(const char *url, bool allow_unsigned, char *err, size_t errlen)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 15000, .buffer_size = CHUNK,
                                     .crt_bundle_attach = esp_crt_bundle_attach };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return T5_ERR_NO_MEM;
    t5_err_t e = esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0 &&
                         esp_http_client_get_status_code(c) == 200
                     ? T5_OK : T5_ERR_IO;
    if (e != T5_OK) snprintf(err, errlen, "download failed (HTTP %d)", esp_http_client_get_status_code(c));
    t5_ota_hdr_t hdr;
    t5_ota_t *o = NULL;
    if (e == T5_OK && esp_http_client_read_response(c, (char *)&hdr, sizeof hdr) != (int)sizeof hdr) {
        snprintf(err, errlen, "not a .t5os file");
        e = T5_ERR_INVALID_ARG;
    }
    if (e == T5_OK) e = t5_ota_begin(&hdr, allow_unsigned, &o, err, errlen);
    uint8_t *buf = e == T5_OK ? malloc(CHUNK) : NULL;
    if (e == T5_OK && !buf) e = T5_ERR_NO_MEM;
    while (e == T5_OK) {
        int n = esp_http_client_read(c, (char *)buf, CHUNK);
        if (n < 0) {
            snprintf(err, errlen, "download interrupted");
            e = T5_ERR_IO;
        }
        if (n <= 0) break;
        e = t5_ota_write(o, buf, (size_t)n);
    }
    free(buf);
    esp_http_client_cleanup(c);
    if (e != T5_OK) {
        t5_ota_abort(o);
        return e;
    }
    return t5_ota_finish(o, err, errlen);
}

int t5_ota_find_on_sd(char (*paths)[128], int max)
{
    static const char *const dirs[] = { "/sd", "/sd/update" };
    int n = 0;
    for (size_t d = 0; d < 2; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir)) && n < max) {
            size_t l = strlen(e->d_name);
            if (l > 5 && l < 100 && !strcmp(e->d_name + l - 5, ".t5os")) snprintf(paths[n++], 128, "%s/%.100s", dirs[d], e->d_name);
        }
        closedir(dir);
    }
    return n;
}

/* ---- boot health -------------------------------------------------------------------------- */
static const esp_partition_t *ctl_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, "recovery");
}

static t5_err_t ctl_request(const char *req)
{
    const esp_partition_t *p = ctl_part();
    if (!p) return T5_ERR_NOT_FOUND;
    size_t off = p->size - T5_BOOTCTL_SECTOR_FROM_END;
    if (esp_partition_erase_range(p, off, T5_BOOTCTL_SECTOR_FROM_END) != ESP_OK) return T5_ERR_IO;
    if (req && esp_partition_write(p, off + T5_BOOTCTL_REQ_OFFSET, req, T5_BOOTCTL_REQ_LEN) != ESP_OK) {
        return T5_ERR_IO;
    }
    return T5_OK;
}

void t5_ota_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_trial = true;
        T5_LOGW(TAG, "this OS was just installed: on trial until it has run %d s", TRIAL_MS / 1000);
    }
    const esp_partition_t *p = ctl_part();
    uint8_t rec[T5_BOOTCTL_ATTEMPT_BYTES + T5_BOOTCTL_REQ_LEN];
    if (p && esp_partition_read(p, p->size - T5_BOOTCTL_SECTOR_FROM_END, rec, sizeof rec) == ESP_OK) {
        /* The bootloader counted this boot too. */
        int n = t5_bootctl_count(rec);
        s_failed = n > 0 ? n - 1 : 0;
        bool asked = !memcmp(rec + T5_BOOTCTL_REQ_OFFSET, T5_BOOTCTL_REQ_SAFEMODE, T5_BOOTCTL_REQ_LEN);
        s_safe = asked || s_failed >= T5_BOOTCTL_SAFEMODE_AFTER;
        if (s_failed) T5_LOGW(TAG, "%d unconfirmed boot%s before this one", s_failed, s_failed > 1 ? "s" : "");
        if (s_safe) T5_LOGW(TAG, "safe mode: %s", asked ? "requested" : "after failed boots");
        if (asked) ctl_request(NULL);       /* once */
    }
}

static esp_timer_handle_t s_trial_timer;

static void trial_passed(void *arg)
{
    (void)arg;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_trial = false;
        T5_LOGI(TAG, "update confirmed: this OS is now the one that boots");
    }
}

void t5_ota_boot_ok(void)
{
    static bool done;
    if (done) return;
    done = true;
    ctl_request(NULL);        /* healthy: the counter starts again */
    if (!s_trial) {
        /* Images flashed over USB have no trial; this is a no-op for them. */
        esp_ota_mark_app_valid_cancel_rollback();
        return;
    }
    const esp_timer_create_args_t a = { .callback = trial_passed, .name = "ota_trial" };
    if (esp_timer_create(&a, &s_trial_timer) == ESP_OK) esp_timer_start_once(s_trial_timer, (uint64_t)TRIAL_MS * 1000);
}

static t5_err_t reboot_with(const char *req);

t5_err_t t5_ota_check_sd_flag(void)
{
    static const char *const flags[] = { "/sd/RECOVERY", "/sd/recovery.txt", "/sd/tab5/recovery" };
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        struct stat st;
        if (stat(flags[i], &st) != 0) continue;
        /* Once: the flag goes before the restart, so recovery's "restart"
         * comes back here. */
        char done[64];
        snprintf(done, sizeof done, "%s.done", flags[i]);
        remove(done);
        rename(flags[i], done);
        T5_LOGW(TAG, "%s on the SD card: restarting into recovery", flags[i]);
        return reboot_with(T5_BOOTCTL_REQ_RECOVERY);
    }
    return T5_OK;
}

void t5_ota_bootctl_clear(void)
{
    ctl_request(NULL);
}

bool t5_ota_on_trial(void)
{
    return s_trial;
}

int t5_ota_failed_boots(void)
{
    return s_failed;
}

bool t5_safe_mode(void)
{
    return s_safe;
}

static void (*s_before_restart)(void);

void t5_ota_set_restart_hook(void (*fn)(void))
{
    s_before_restart = fn;
}

static t5_err_t reboot_with(const char *req)
{
    T5_RETURN_ON_ERR(ctl_request(req));
    if (s_before_restart) s_before_restart();
    T5_LOGI(TAG, "restarting: %s", req);
    t5_delay_ms(200);
    esp_restart();
    return T5_OK;
}

t5_err_t t5_ota_reboot_to_recovery(void)
{
    return reboot_with(T5_BOOTCTL_REQ_RECOVERY);
}

t5_err_t t5_ota_reboot_safe_mode(void)
{
    return reboot_with(T5_BOOTCTL_REQ_SAFEMODE);
}

/* ---- self test ------------------------------------------------------------------------------ */
t5_err_t t5_ota_selftest(const uint8_t *sig64, const uint8_t *key_id32, char *err, size_t errlen)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_partition_pos_t pos = { .offset = run->address, .size = run->size };
    esp_image_metadata_t meta = {0};
    if (esp_image_get_metadata(&pos, &meta) != ESP_OK) {
        snprintf(err, errlen, "can't read the running image");
        return T5_ERR_IO;
    }
    t5_ota_hdr_t hdr = {0};
    memcpy(hdr.magic, T5_OTA_MAGIC, 4);
    hdr.version = T5_OTA_HDR_VERSION;
    hdr.image_len = meta.image_len;
    snprintf(hdr.os_version, sizeof hdr.os_version, "%.15s", esp_app_get_description()->version);
    uint8_t *buf = malloc(CHUNK);
    if (!buf) return T5_ERR_NO_MEM;
    /* The header carries the image's hash: compute it first. */
    psa_crypto_init();
    psa_hash_operation_t h = psa_hash_operation_init();
    psa_hash_setup(&h, PSA_ALG_SHA_256);
    for (uint32_t off = 0; off < meta.image_len; off += CHUNK) {
        uint32_t n = meta.image_len - off < CHUNK ? meta.image_len - off : CHUNK;
        esp_partition_read(run, off, buf, n);
        psa_hash_update(&h, buf, n);
    }
    size_t hl;
    psa_hash_finish(&h, hdr.image_sha256, 32, &hl);
    if (sig64 && key_id32) {
        hdr.sig_len = 64;
        memcpy(hdr.sig, sig64, 64);
        memcpy(hdr.key_id, key_id32, 32);
    }
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", hdr.image_sha256[i]);
    T5_LOGI(TAG, "self test: running image %u bytes, sha256 %s...", (unsigned)meta.image_len, hex);
    t5_ota_t *o = NULL;
    t5_err_t e = t5_ota_begin(&hdr, !sig64, &o, err, errlen);
    for (uint32_t off = 0; e == T5_OK && off < meta.image_len; off += CHUNK) {
        uint32_t n = meta.image_len - off < CHUNK ? meta.image_len - off : CHUNK;
        esp_partition_read(run, off, buf, n);
        e = t5_ota_write(o, buf, n);
    }
    free(buf);
    if (e != T5_OK) {
        if (!err[0]) snprintf(err, errlen, "%s", t5_err_str(e));
        t5_ota_abort(o);
        return e;
    }
    return t5_ota_finish(o, err, errlen);
}
