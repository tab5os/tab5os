/* SPDX-License-Identifier: Apache-2.0
 * OS update install (plan 9.1): see ota.h. Boot health is in boot.c, URL
 * downloads in t5_ota_net.
 */
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "hal/efuse_hal.h"
#include "psa/crypto.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_bootfmt/espimg.h"
#include "t5_ota/ota.h"
#include "t5_storage/paths.h"
#include "ota_keys.h"
#include "ota_priv.h"
#include "ota_rule.h"
#include "ota_slot.h"

static const char *TAG = "t5.ota";

_Static_assert(sizeof(esp_image_header_t) == T5_IMG_HDR_LEN, "espimg.h");
_Static_assert(sizeof(esp_image_segment_header_t) == T5_IMG_SEG_HDR_LEN, "espimg.h");
_Static_assert(offsetof(esp_image_header_t, chip_id) == T5_IMG_CHIP_ID_OFF, "espimg.h");
_Static_assert(offsetof(esp_image_header_t, min_chip_rev_full) == T5_IMG_MIN_REV_OFF, "espimg.h");
_Static_assert(offsetof(esp_image_header_t, max_chip_rev_full) == T5_IMG_MAX_REV_OFF, "espimg.h");
_Static_assert(offsetof(esp_image_header_t, hash_appended) == T5_IMG_HASH_APPENDED_OFF, "espimg.h");
_Static_assert(offsetof(esp_app_desc_t, version) == T5_IMG_DESC_VERSION_OFF, "espimg.h");
_Static_assert(offsetof(esp_app_desc_t, project_name) == T5_IMG_DESC_PROJECT_OFF, "espimg.h");
_Static_assert(offsetof(esp_app_desc_t, magic_word) == 0, "espimg.h");
_Static_assert(sizeof((esp_app_desc_t *)0)->project_name == T5_IMG_DESC_PROJECT_LEN, "espimg.h");
_Static_assert(ESP_APP_DESC_MAGIC_WORD == T5_IMG_DESC_MAGIC, "espimg.h");
_Static_assert(ESP_CHIP_ID_ESP32P4 == T5_IMG_CHIP_ESP32P4, "espimg.h");
_Static_assert(sizeof T5_OTA_PROJECT <= T5_IMG_DESC_PROJECT_LEN, "project name");

#define CHUNK 4096
#define SD_SCAN_MAX 16
#define HEAD_LEN 128                 /* image header, segment header, app description */
_Static_assert(HEAD_LEN >= T5_IMG_DESC_OFF + T5_IMG_DESC_VERSION_OFF + T5_IMG_DESC_VERSION_LEN, "head");
_Static_assert(HEAD_LEN >= T5_IMG_DESC_OFF + T5_IMG_DESC_PROJECT_OFF + T5_IMG_DESC_PROJECT_LEN, "head");

typedef struct {
    t5_ota_hdr_t hdr;
    const esp_partition_t *part;
    esp_ota_handle_t h;
    psa_hash_operation_t sha;
    uint32_t done;
    uint8_t head[HEAD_LEN];      /* checked before anything is written */
    size_t nhead;
    bool checked;
    int key;                     /* index into t5_ota_keys, -1 unsigned or unknown */
    t5_ota_source_t from;
    bool consent;
} ota_t;

static t5_ota_status_t s_st;
static t5_mutex_t s_lock;

static void lock(void)
{
    t5_mutex_lock(t5_mutex_once(&s_lock));
}

static void unlock(void)
{
    t5_mutex_unlock(s_lock);
}

const char *t5_ota_state_name(t5_ota_state_t s)
{
    static const char *const names[] = { "idle", "receiving", "verifying", "ready (restart)", "failed" };
    return (unsigned)s < sizeof names / sizeof names[0] ? names[s] : "?";
}

static t5_err_t fail(t5_err_t e, char *err, size_t n, const char *why)
{
    if (err && n) snprintf(err, n, "%s", why);
    lock();
    s_st.state = T5_OTA_FAILED;
    snprintf(s_st.error, sizeof s_st.error, "%s", why);
    unlock();
    T5_LOGW(TAG, "update refused: %s", why);
    return e;
}

static int find_key(const uint8_t id[32])
{
    for (int i = 0; i < t5_ota_nkeys; i++) {
        if (!memcmp(t5_ota_keys[i].id, id, 32)) return i;
    }
    return -1;
}

static int ota_slot_of(const esp_partition_t *p)
{
    if (!p || p->type != ESP_PARTITION_TYPE_APP || p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        return -1;
    }
    return p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
}

bool ota_running_from_slot(void)
{
    return ota_slot_of(esp_ota_get_running_partition()) >= 0;
}

static const esp_partition_t *target_partition(void)
{
    int slot = t5_ota_target_slot(ota_slot_of(esp_ota_get_running_partition()),
                                  ota_slot_of(esp_ota_get_boot_partition()), esp_ota_get_app_partition_count());
    if (slot < 0) return NULL;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + slot), NULL);
}

/* T5_OK, or an error and why. key: the built-in key that signed it, or -1. */
static t5_err_t check_hdr(const t5_ota_hdr_t *hdr, t5_ota_source_t from, bool consent, int *key, const char **why)
{
    *key = -1;
    if (memcmp(hdr->magic, T5_OTA_MAGIC, 4) || hdr->version != T5_OTA_HDR_VERSION) {
        *why = "not a Tab5 OS update (.t5os)";
        return T5_ERR_INVALID_ARG;
    }
    *key = hdr->sig_len == T5_OTA_SIG_LEN ? find_key(hdr->key_id) : -1;
    *why = t5_ota_refusal(hdr, *key >= 0, from, consent);
    return *why ? T5_ERR_DENIED : T5_OK;
}

static t5_err_t ota_begin(const t5_ota_hdr_t *hdr, t5_ota_source_t from, bool consent, ota_t **out, char *err,
                          size_t errlen)
{
    *out = NULL;
    int key;
    const char *why;
    t5_err_t ce = check_hdr(hdr, from, consent, &key, &why);
    if (ce != T5_OK) return fail(ce, err, errlen, why);
    if (t5_ota_on_trial()) {
        return fail(T5_ERR_STATE, err, errlen, "the running OS is still on trial: wait a minute after an update");
    }
    const esp_partition_t *part = target_partition();
    if (!part) return fail(T5_ERR_INVALID_ARG, err, errlen, "no update slot");
    if (hdr->image_len < 1024 || hdr->image_len > part->size) {
        return fail(T5_ERR_INVALID_ARG, err, errlen, "the image doesn't fit the update slot");
    }
    /* An earlier install may have made this slot the next boot: point the
     * next boot back at the running system before overwriting it, so a
     * failed attempt can't leave a half-written image to boot. From
     * recovery the target is the boot slot by design, and recovery itself
     * is never a boot slot. */
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (ota_running_from_slot() && boot && boot->address == part->address &&
        esp_ota_set_boot_partition(esp_ota_get_running_partition()) != ESP_OK) {
        T5_LOGW(TAG, "can't point the next boot back at the running system");
    }
    ota_t *o = calloc(1, sizeof *o);
    if (!o) return fail(T5_ERR_NO_MEM, err, errlen, "out of memory");
    o->hdr = *hdr;
    o->part = part;
    o->key = key;
    o->from = from;
    o->consent = consent;
    psa_crypto_init();
    o->sha = psa_hash_operation_init();
    if (psa_hash_setup(&o->sha, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        esp_ota_begin(part, hdr->image_len, &o->h) != ESP_OK) {
        psa_hash_abort(&o->sha);
        free(o);
        return fail(T5_ERR_IO, err, errlen, "can't open the update slot");
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
            key >= 0 ? t5_ota_keys[key].name : "not signed by a built-in key", part->label);
    *out = o;
    return T5_OK;
}

static t5_err_t check_head(const ota_t *o, char *err, size_t errlen)
{
    const esp_image_header_t *ih = (const esp_image_header_t *)o->head;
    /* Only the start of esp_app_desc_t is in head. */
    uint32_t desc_magic;
    memcpy(&desc_magic, o->head + T5_IMG_DESC_OFF, sizeof desc_magic);
    const char *project = (const char *)o->head + T5_IMG_DESC_OFF + T5_IMG_DESC_PROJECT_OFF;
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC || ih->chip_id != ESP_CHIP_ID_ESP32P4) {
        return fail(T5_ERR_INVALID_ARG, err, errlen, "not a Tab5 OS image");
    }
    uint32_t rev = efuse_hal_chip_revision();
    if (rev < ih->min_chip_rev_full || rev > ih->max_chip_rev_full) {
        T5_LOGW(TAG, "image is for chip revisions v%u.%u-v%u.%u, this is v%u.%u", ih->min_chip_rev_full / 100,
                ih->min_chip_rev_full % 100, ih->max_chip_rev_full / 100, ih->max_chip_rev_full % 100,
                (unsigned)rev / 100, (unsigned)rev % 100);
        return fail(T5_ERR_NOT_SUPPORTED, err, errlen, "built for a different chip revision");
    }
    if (desc_magic != ESP_APP_DESC_MAGIC_WORD || strncmp(project, T5_OTA_PROJECT, T5_IMG_DESC_PROJECT_LEN)) {
        return fail(T5_ERR_INVALID_ARG, err, errlen, "not a Tab5 OS image");
    }
    char version[T5_IMG_DESC_VERSION_LEN + 1];
    snprintf(version, sizeof version, "%.*s", T5_IMG_DESC_VERSION_LEN,
             (const char *)o->head + T5_IMG_DESC_OFF + T5_IMG_DESC_VERSION_OFF);
    const char *why = t5_ota_downgrade_refusal(version, esp_app_get_description()->version, o->from, o->consent);
    if (why) return fail(T5_ERR_DENIED, err, errlen, why);
    return T5_OK;
}

static t5_err_t ota_write(ota_t *o, const void *data, size_t len, char *err, size_t errlen)
{
    if (o->done + len > o->hdr.image_len) return fail(T5_ERR_INVALID_ARG, err, errlen, "more data than the header says");
    const uint8_t *p = data;
    if (!o->checked) {
        size_t take = len < sizeof o->head - o->nhead ? len : sizeof o->head - o->nhead;
        memcpy(o->head + o->nhead, p, take);
        o->nhead += take;
        if (o->nhead == sizeof o->head) {
            T5_RETURN_ON_ERR(check_head(o, err, errlen));
            o->checked = true;
        }
    }
    psa_hash_update(&o->sha, p, len);
    if (esp_ota_write(o->h, p, len) != ESP_OK) return fail(T5_ERR_IO, err, errlen, "writing the update slot failed");
    o->done += (uint32_t)len;
    lock();
    s_st.done = o->done;
    unlock();
    return T5_OK;
}

static bool verify_sig(const ota_t *o, const uint8_t hash[32])
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
        ok = psa_verify_hash(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, 32, o->hdr.sig, T5_OTA_SIG_LEN) == PSA_SUCCESS;
        psa_destroy_key(id);
    }
    psa_reset_key_attributes(&a);
    return ok;
}

/* Verify and switch the boot slot. Frees o either way. */
static t5_err_t ota_finish(ota_t *o, char *err, size_t errlen)
{
    lock();
    s_st.state = T5_OTA_VERIFYING;
    unlock();
    uint8_t hash[32];
    size_t hl = 0;
    t5_err_t e = T5_OK;
    psa_hash_finish(&o->sha, hash, sizeof hash, &hl);
    if (o->done != o->hdr.image_len || !o->checked) {
        e = fail(T5_ERR_INVALID_ARG, err, errlen, "the image is incomplete");
    } else if (memcmp(hash, o->hdr.image_sha256, 32)) {
        e = fail(T5_ERR_INVALID_ARG, err, errlen, "the image doesn't match its hash (damaged in transfer?)");
    } else if (o->key >= 0 && !verify_sig(o, hash)) {
        e = fail(T5_ERR_DENIED, err, errlen, "the signature doesn't verify");
    }
    if (e != T5_OK) {
        esp_ota_abort(o->h);
        free(o);
        return e;
    }
    /* esp_ota_end checks the image structure (segments, its own SHA-256). */
    if (esp_ota_end(o->h) != ESP_OK) e = fail(T5_ERR_INVALID_ARG, err, errlen, "the image failed ESP-IDF's checks");
    else if (esp_ota_set_boot_partition(o->part) != ESP_OK) e = fail(T5_ERR_IO, err, errlen, "can't switch the boot slot");
    if (e == T5_OK) {
        T5_LOGI(TAG, "Tab5 OS %.15s installed in %s; it runs on trial after a restart", o->hdr.os_version,
                o->part->label);
        lock();
        s_st.state = T5_OTA_READY;
        unlock();
    }
    free(o);
    return e;
}

static void ota_abort(ota_t *o)
{
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

/* Exactly n bytes, unless the source ends or fails first. */
static int read_full(t5_ota_read_fn rd, void *ctx, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = rd(ctx, (uint8_t *)buf + got, n - got);
        if (r < 0) return r;
        if (r == 0) break;
        got += (size_t)r;
    }
    return (int)got;
}

t5_err_t t5_ota_install_stream(t5_ota_read_fn rd, void *ctx, t5_ota_source_t from, bool consent,
                               t5_ota_hdr_t *hdr_out, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    t5_ota_hdr_t hdr;
    int r = read_full(rd, ctx, &hdr, sizeof hdr);
    if (r < 0) return fail((t5_err_t)r, err, errlen, "the transfer was interrupted");
    if (r != (int)sizeof hdr) return fail(T5_ERR_INVALID_ARG, err, errlen, "not a .t5os file");
    if (hdr_out) *hdr_out = hdr;
    ota_t *o = NULL;
    T5_RETURN_ON_ERR(ota_begin(&hdr, from, consent, &o, err, errlen));
    uint8_t *buf = malloc(CHUNK);
    t5_err_t e = buf ? T5_OK : fail(T5_ERR_NO_MEM, err, errlen, "out of memory");
    while (e == T5_OK && o->done < hdr.image_len) {
        uint32_t left = hdr.image_len - o->done;
        r = rd(ctx, buf, left < CHUNK ? left : CHUNK);
        if (r < 0) e = fail((t5_err_t)r, err, errlen, "the transfer was interrupted");
        if (r <= 0) break;
        e = ota_write(o, buf, (size_t)r, err, errlen);
    }
    free(buf);
    if (e != T5_OK) {
        ota_abort(o);
        return e;
    }
    return ota_finish(o, err, errlen);
}

static int file_read(void *ctx, void *buf, size_t n)
{
    FILE *f = ctx;
    size_t r = fread(buf, 1, n, f);
    return r || !ferror(f) ? (int)r : T5_ERR_IO;
}

t5_err_t t5_ota_install_file(const char *path, t5_ota_source_t from, bool consent, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "can't open %s", path);
        return T5_ERR_NOT_FOUND;
    }
    t5_err_t e = t5_ota_install_stream(file_read, f, from, consent, NULL, err, errlen);
    fclose(f);
    return e;
}

t5_err_t t5_ota_peek(const char *path, t5_ota_source_t from, bool consent, t5_ota_hdr_t *hdr, bool *trusted,
                     char *err, size_t errlen)
{
    *trusted = false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "can't open %s", path);
        return T5_ERR_NOT_FOUND;
    }
    bool whole = fread(hdr, 1, sizeof *hdr, f) == sizeof *hdr;
    fclose(f);
    if (!whole) {
        snprintf(err, errlen, "not a Tab5 OS update (.t5os)");
        return T5_ERR_INVALID_ARG;
    }
    int key;
    const char *why;
    t5_err_t e = check_hdr(hdr, from, consent, &key, &why);
    *trusted = key >= 0;
    if (e != T5_OK) snprintf(err, errlen, "%s", why);
    return e;
}

int t5_ota_find_on_sd(char (*paths)[128], int max)
{
    static const char *const dirs[] = { T5_SD_PATH, T5_SD_PATH "/update" };
    time_t when[SD_SCAN_MAX];
    if (max > SD_SCAN_MAX) max = SD_SCAN_MAX;
    int n = 0;
    char path[128];
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir))) {
            size_t l = strlen(e->d_name);
            if (l <= 5 || l >= 100 || strcmp(e->d_name + l - 5, ".t5os")) continue;
            snprintf(path, sizeof path, "%s/%.100s", dirs[d], e->d_name);
            struct stat st;
            time_t t = stat(path, &st) == 0 ? st.st_mtime : 0;
            /* Insert by time, newest first; past max the oldest drops. */
            int i = n < max ? n++ : max;
            while (i > 0 && when[i - 1] < t) {
                if (i < max) {
                    when[i] = when[i - 1];
                    memcpy(paths[i], paths[i - 1], sizeof paths[i]);
                }
                i--;
            }
            if (i < max) {
                when[i] = t;
                memcpy(paths[i], path, sizeof path);
            }
        }
        closedir(dir);
    }
    return n;
}

/* The running image behind a synthesized header, as a stream. */
typedef struct {
    const t5_ota_hdr_t *hdr;
    const esp_partition_t *part;
    uint32_t off;                /* into header + image */
} self_src_t;

static int self_read(void *ctx, void *buf, size_t n)
{
    self_src_t *s = ctx;
    if (s->off < sizeof *s->hdr) {
        size_t k = sizeof *s->hdr - s->off < n ? sizeof *s->hdr - s->off : n;
        memcpy(buf, (const uint8_t *)s->hdr + s->off, k);
        s->off += (uint32_t)k;
        return (int)k;
    }
    uint32_t at = s->off - (uint32_t)sizeof *s->hdr;
    if (at >= s->hdr->image_len) return 0;
    size_t k = s->hdr->image_len - at < n ? s->hdr->image_len - at : n;
    if (esp_partition_read(s->part, at, buf, k) != ESP_OK) return T5_ERR_IO;
    s->off += (uint32_t)k;
    return (int)k;
}

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
    free(buf);
    size_t hl;
    psa_hash_finish(&h, hdr.image_sha256, 32, &hl);
    if (sig64 && key_id32) {
        hdr.sig_len = T5_OTA_SIG_LEN;
        memcpy(hdr.sig, sig64, T5_OTA_SIG_LEN);
        memcpy(hdr.key_id, key_id32, 32);
    }
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", hdr.image_sha256[i]);
    T5_LOGI(TAG, "self test: running image %u bytes, sha256 %s...", (unsigned)meta.image_len, hex);
    self_src_t src = { .hdr = &hdr, .part = run };
    return t5_ota_install_stream(self_read, &src, T5_OTA_FROM_CONSOLE, !sig64, NULL, err, errlen);
}
