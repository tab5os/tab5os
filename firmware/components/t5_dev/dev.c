/* SPDX-License-Identifier: Apache-2.0
 * Developer mode (plan 8.5): see dev.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "md/md.h"
#include "t5_app/apps.h"
#include "t5_app/registry.h"
#include "t5_core/log.h"
#include "t5_core/logring.h"
#include "t5_core/os.h"
#include "t5_dev/control.h"
#include "t5_dev/dev.h"
#include "t5_pkg/manifest.h"
#include "t5_pkg/pkg.h"
#include "t5_power/power.h"
#include "t5_ota/ota.h"
#include "t5_py/py.h"
#include "t5_shell/shell.h"
#include "t5_storage/paths.h"
#include "t5_storage/t5_storage.h"
#include "t5_ui/t5_ui.h"

static const char *TAG = "t5.dev";

#define ENABLE_FILE "devmode"
#define CLIENTS_FILE "devclients.bin"
#define CLIENTS_VERSION 1
#define PUSH_PATH T5_TMP_DIR "/devpush.t5app"
#define PAIR_TIMEOUT_MS (120 * 1000)
#define PAIR_TRIES 3

typedef struct {
    char name[T5_DEV_NAME_LEN];
    uint8_t hash[32];
    int64_t time;
} client_t;

typedef struct {
    uint32_t n;
    client_t c[T5_DEV_MAX_CLIENTS];
} clients_db_t;

static t5_mutex_t s_lock;
static clients_db_t s_db;
static httpd_handle_t s_httpd;
static bool s_enabled;
static uint32_t s_requests, s_denied;

static struct {
    bool active;
    char id[17];
    char code[7];
    char client[T5_DEV_NAME_LEN];
    uint32_t t0;
    int tries;
    lv_obj_t *dialog;
} s_pair;

static void sha256(const void *data, size_t n, uint8_t out[32])
{
    size_t len = 0;
    psa_hash_compute(PSA_ALG_SHA_256, data, n, out, 32, &len);
}

static void db_load(void)
{
    size_t len = 0;
    char path[96];
    memset(&s_db, 0, sizeof s_db);
    if (t5_sys_path(path, sizeof path, CLIENTS_FILE) != T5_OK ||
        t5_storage_blob_read(path, CLIENTS_VERSION, &s_db, sizeof s_db, &len) != T5_OK || len != sizeof s_db ||
        s_db.n > T5_DEV_MAX_CLIENTS) {
        memset(&s_db, 0, sizeof s_db);
    }
}

static void db_save(void)
{
    char path[96];
    if (t5_sys_path(path, sizeof path, CLIENTS_FILE) != T5_OK ||
        t5_storage_blob_write(path, CLIENTS_VERSION, &s_db, sizeof s_db) != T5_OK) {
        T5_LOGW(TAG, "can't save paired clients");
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Constant-time over the stored hashes. */
static bool token_ok(const char *hex)
{
    uint8_t tok[32];
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        tok[i] = (uint8_t)(a << 4 | b);
    }
    uint8_t h[32];
    sha256(tok, sizeof tok, h);
    bool ok = false;
    t5_mutex_lock(s_lock);
    for (uint32_t i = 0; i < s_db.n; i++) {
        uint8_t d = 0;
        for (int k = 0; k < 32; k++) d |= (uint8_t)(h[k] ^ s_db.c[i].hash[k]);
        ok |= d == 0;
    }
    t5_mutex_unlock(s_lock);
    return ok;
}

static void add_client(const char *name, const uint8_t token[32])
{
    t5_mutex_lock(s_lock);
    int slot = -1;
    for (uint32_t i = 0; i < s_db.n; i++) {
        if (!strcmp(s_db.c[i].name, name)) slot = (int)i;
    }
    if (slot < 0) {
        if (s_db.n == T5_DEV_MAX_CLIENTS) {          /* drop the oldest */
            memmove(&s_db.c[0], &s_db.c[1], sizeof(client_t) * (T5_DEV_MAX_CLIENTS - 1));
            s_db.n--;
        }
        slot = (int)s_db.n++;
    }
    client_t *c = &s_db.c[slot];
    snprintf(c->name, sizeof c->name, "%s", name);
    sha256(token, 32, c->hash);
    c->time = (int64_t)t5_time_us() / 1000000;
    db_save();
    t5_mutex_unlock(s_lock);
}

int t5_dev_clients(char (*names)[T5_DEV_NAME_LEN], int max)
{
    if (!s_lock) return 0;
    t5_mutex_lock(s_lock);
    int n = 0;
    for (uint32_t i = 0; i < s_db.n && n < max; i++) snprintf(names[n++], T5_DEV_NAME_LEN, "%s", s_db.c[i].name);
    t5_mutex_unlock(s_lock);
    return n;
}

t5_err_t t5_dev_forget_all(void)
{
    if (!s_lock) return T5_ERR_STATE;
    t5_mutex_lock(s_lock);
    memset(&s_db, 0, sizeof s_db);
    db_save();
    t5_mutex_unlock(s_lock);
    T5_LOGI(TAG, "forgot every paired computer");
    return T5_OK;
}

/* The pairing dialog (UI task). */
static void pair_answer(int button, void *ud)
{
    (void)ud;
    (void)button;          /* the only button is Cancel; the scrim dismisses too */
    t5_mutex_lock(s_lock);
    s_pair.dialog = NULL;
    s_pair.active = false;
    t5_mutex_unlock(s_lock);
}

static void pair_show(void *arg)
{
    (void)arg;
    void *saved = t5_sys_enter();
    char text[200];
    t5_mutex_lock(s_lock);
    snprintf(text, sizeof text,
             "Type this code on %s:\n\n%.3s %.3s\n\nOnly pair with a computer you trust: it will be able to "
             "install and run apps.",
             s_pair.client, s_pair.code, s_pair.code + 3);
    t5_mutex_unlock(s_lock);
    lv_obj_t *d = md_dialog_show("Pair with a computer?", text, NULL, "Cancel", pair_answer, NULL);
    t5_mutex_lock(s_lock);
    s_pair.dialog = d;
    t5_mutex_unlock(s_lock);
    t5_sys_leave(saved);
}

static void pair_close(void *arg)
{
    (void)arg;
    t5_mutex_lock(s_lock);
    lv_obj_t *d = s_pair.dialog;
    s_pair.dialog = NULL;
    t5_mutex_unlock(s_lock);
    if (d) md_dialog_close(d);
}

typedef struct {
    int (*fn)(httpd_req_t *req);
    bool auth;
} route_t;

/* httpd_req_recv, but a stalled client gets RECV_TIMEOUTS_MAX receive
 * timeouts (10 s each, cfg.recv_wait_timeout), not forever, so it can't
 * hold the server (docs/coding-standard.md, rule 2). */
#define RECV_TIMEOUTS_MAX 3
static int recv_some(httpd_req_t *req, char *buf, size_t n)
{
    for (int i = 0; i < RECV_TIMEOUTS_MAX; i++) {
        int r = httpd_req_recv(req, buf, n);
        if (r != HTTPD_SOCK_ERR_TIMEOUT) return r;
    }
    return HTTPD_SOCK_ERR_TIMEOUT;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    char buf[200];
    snprintf(buf, sizeof buf, "{\"error\":\"%s\"}", msg);
    return send_json(req, status, buf);
}

/* A query parameter, URL-decoded (%XX and +). */
static bool query(httpd_req_t *req, const char *key, char *out, size_t n)
{
    char q[256];
    out[0] = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, out, n) != ESP_OK) return false;
    char *w = out;
    for (const char *r = out; *r; r++) {
        if (*r == '%' && hexval(r[1]) >= 0 && hexval(r[2]) >= 0) {
            *w++ = (char)(hexval(r[1]) << 4 | hexval(r[2]));
            r += 2;
        } else {
            *w++ = *r == '+' ? ' ' : *r;
        }
    }
    *w = 0;
    return true;
}

/* Every route goes through here: count it, check the token. */
static esp_err_t dispatch(httpd_req_t *req)
{
    const route_t *r = req->user_ctx;
    s_requests++;
    if (r->auth) {
        char h[96];
        if (httpd_req_get_hdr_value_str(req, "Authorization", h, sizeof h) != ESP_OK || strncmp(h, "Bearer ", 7) ||
            !token_ok(h + 7)) {
            s_denied++;
            T5_LOGW(TAG, "refused %s: no valid token", req->uri);
            return send_err(req, "401 Unauthorized", "pair first: t5 pair <address>");
        }
    }
    /* Keep the radio and CPU up while serving (an OS update takes a while,
     * and Sleep would suspend Wi-Fi under it). */
    t5_wakelock_t *net = t5_wakelock_acquire(T5_LOCK_NET, "devmode");
    t5_wakelock_t *cpu = t5_wakelock_acquire(T5_LOCK_CPU, "devmode");
    int rc = r->fn(req);
    t5_wakelock_release(cpu);
    t5_wakelock_release(net);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static int h_info(httpd_req_t *req)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char buf[256];
    char cur[64];
    t5_apps_current(cur, sizeof cur);
    snprintf(buf, sizeof buf,
             "{\"model\":\"M5Stack Tab5\",\"os\":\"Tab5 OS\",\"version\":\"%s\",\"api\":%d,\"idf\":\"%s\","
             "\"foreground\":\"%s\",\"python\":%s}",
             d->version, T5_API_LEVEL, d->idf_ver, cur, t5_py_busy() ? "true" : "false");
    return send_json(req, NULL, buf) == ESP_OK ? 0 : -1;
}

static int h_pair(httpd_req_t *req)
{
    char client[T5_DEV_NAME_LEN];
    if (!query(req, "client", client, sizeof client) || !client[0]) snprintf(client, sizeof client, "a computer");
    for (char *c = client; *c; c++) {
        if ((unsigned char)*c < 0x20 || *c == '"' || *c == '\\') *c = '_';
    }
    t5_mutex_lock(s_lock);
    if (s_pair.active && t5_time_ms() - s_pair.t0 < PAIR_TIMEOUT_MS) {
        t5_mutex_unlock(s_lock);
        return send_err(req, "409 Conflict", "a pairing is already waiting on the Tab5") == ESP_OK ? 0 : -1;
    }
    uint8_t rnd[8];
    esp_fill_random(rnd, sizeof rnd);
    for (int i = 0; i < 8; i++) snprintf(s_pair.id + 2 * i, 3, "%02x", rnd[i]);
    snprintf(s_pair.code, sizeof s_pair.code, "%06u", (unsigned)(esp_random() % 1000000u));
    snprintf(s_pair.client, sizeof s_pair.client, "%s", client);
    s_pair.t0 = t5_time_ms();
    s_pair.tries = 0;
    s_pair.active = true;
    char buf[96];
    snprintf(buf, sizeof buf, "{\"request\":\"%s\",\"expires_s\":%d}", s_pair.id, PAIR_TIMEOUT_MS / 1000);
    t5_mutex_unlock(s_lock);
    t5_power_user_activity();          /* light the screen: the code is on it */
    t5_ui_async(pair_show, NULL);
    T5_LOGI(TAG, "pairing requested by %s", client);
    return send_json(req, NULL, buf) == ESP_OK ? 0 : -1;
}

static int h_pair_confirm(httpd_req_t *req)
{
    char id[20], code[12];
    query(req, "request", id, sizeof id);
    query(req, "code", code, sizeof code);
    /* Codes may come as "123 456". */
    char digits[8] = "";
    size_t k = 0;
    for (const char *c = code; *c && k < 6; c++) {
        if (*c >= '0' && *c <= '9') digits[k++] = *c;
    }
    digits[k] = 0;
    t5_mutex_lock(s_lock);
    bool live = s_pair.active && t5_time_ms() - s_pair.t0 < PAIR_TIMEOUT_MS && !strcmp(id, s_pair.id);
    if (!live) {
        t5_mutex_unlock(s_lock);
        return send_err(req, "410 Gone", "no pairing waiting (it times out after 2 minutes): start again") == ESP_OK
                   ? 0 : -1;
    }
    uint8_t d = 0;
    for (int i = 0; i < 6; i++) d |= (uint8_t)(digits[i] ^ s_pair.code[i]);
    if (k != 6 || d) {
        bool last = ++s_pair.tries >= PAIR_TRIES;
        if (last) s_pair.active = false;
        t5_mutex_unlock(s_lock);
        s_denied++;
        if (last) t5_ui_async(pair_close, NULL);
        return send_err(req, "403 Forbidden", last ? "wrong code: pairing cancelled" : "wrong code") == ESP_OK ? 0 : -1;
    }
    char client[T5_DEV_NAME_LEN];
    snprintf(client, sizeof client, "%s", s_pair.client);
    s_pair.active = false;
    t5_mutex_unlock(s_lock);
    uint8_t tok[32];
    esp_fill_random(tok, sizeof tok);
    add_client(client, tok);
    t5_ui_async(pair_close, NULL);
    char msg[64];
    snprintf(msg, sizeof msg, "Paired with %s", client);
    t5_shell_toast(msg);
    T5_LOGI(TAG, "paired with %s", client);
    char buf[128] = "{\"token\":\"";
    size_t n = strlen(buf);
    for (int i = 0; i < 32; i++) n += (size_t)snprintf(buf + n, sizeof buf - n, "%02x", tok[i]);
    snprintf(buf + n, sizeof buf - n, "\"}");
    return send_json(req, NULL, buf) == ESP_OK ? 0 : -1;
}

static int h_apps(httpd_req_t *req)
{
    t5_app_entry_t *list = calloc(T5_APPS_MAX, sizeof *list);
    if (!list) return send_err(req, "500 Internal Server Error", "no memory") == ESP_OK ? 0 : -1;
    int n = t5_apps_list(list, T5_APPS_MAX);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    char buf[256];
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, "%s{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"builtin\":%s,\"running\":%s,"
                 "\"crashes\":%u}",
                 i ? "," : "", list[i].id, list[i].name, list[i].version, list[i].builtin ? "true" : "false",
                 list[i].running ? "true" : "false", list[i].crashes);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    free(list);
    return 0;
}

static t5_err_t recv_to_file(httpd_req_t *req, const char *path, size_t max)
{
    if (req->content_len == 0 || req->content_len > max) return T5_ERR_INVALID_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return T5_ERR_IO;
    char *buf = malloc(8192);
    t5_err_t e = buf ? T5_OK : T5_ERR_NO_MEM;
    size_t left = req->content_len;
    while (e == T5_OK && left > 0) {
        int r = recv_some(req, buf, left < 8192 ? left : 8192);
        if (r <= 0) {
            e = T5_ERR_IO;
            break;
        }
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) e = T5_ERR_IO;
        left -= (size_t)r;
    }
    free(buf);
    if (fclose(f) != 0 && e == T5_OK) e = T5_ERR_IO;
    if (e != T5_OK) remove(path);
    return e;
}

static int h_install(httpd_req_t *req)
{
    char dest[8] = "auto";
    query(req, "dest", dest, sizeof dest);
    t5_err_t e = recv_to_file(req, PUSH_PATH, T5_DEV_PUSH_MAX);
    if (e != T5_OK) {
        return send_err(req, "400 Bad Request", e == T5_ERR_INVALID_ARG ? "empty or too big" : "upload failed") ==
                       ESP_OK ? 0 : -1;
    }
    t5_manifest_t *man = calloc(1, sizeof *man);
    char err[200] = "";
    e = man ? t5_apps_install(PUSH_PATH, t5_apps_dest_parse(dest), man, err, sizeof err) : T5_ERR_NO_MEM;
    remove(PUSH_PATH);
    char buf[320];
    if (e != T5_OK) {
        for (char *c = err; *c; c++) {
            if (*c == '"') *c = '\'';
        }
        snprintf(buf, sizeof buf, "{\"error\":\"%s: %s\"}", t5_err_str(e), err);
        free(man);
        return send_json(req, "422 Unprocessable Entity", buf) == ESP_OK ? 0 : -1;
    }
    snprintf(buf, sizeof buf, "{\"id\":\"%s\",\"version\":\"%s\"}", man->id, man->version);
    free(man);
    return send_json(req, NULL, buf) == ESP_OK ? 0 : -1;
}

static int h_launch(httpd_req_t *req)
{
    char id[64], arg[128];
    if (!query(req, "id", id, sizeof id) || !t5_manifest_id_ok(id)) {
        return send_err(req, "400 Bad Request", "id?") == ESP_OK ? 0 : -1;
    }
    bool has_arg = query(req, "arg", arg, sizeof arg);
    t5_err_t e = t5_apps_launch_arg(id, has_arg ? arg : NULL);
    if (e != T5_OK) return send_err(req, "409 Conflict", t5_err_str(e)) == ESP_OK ? 0 : -1;
    return send_json(req, NULL, "{\"ok\":true}") == ESP_OK ? 0 : -1;
}

static int h_exit(httpd_req_t *req)
{
    t5_err_t e = t5_apps_exit_wait(3000);
    if (e != T5_OK) return send_err(req, "409 Conflict", t5_err_str(e)) == ESP_OK ? 0 : -1;
    return send_json(req, NULL, "{\"ok\":true}") == ESP_OK ? 0 : -1;
}

static int h_remove(httpd_req_t *req)
{
    char id[64], keep[4] = "";
    if (!query(req, "id", id, sizeof id)) return send_err(req, "400 Bad Request", "id?") == ESP_OK ? 0 : -1;
    query(req, "keep", keep, sizeof keep);
    t5_err_t e = t5_apps_remove(id, keep[0] == '1');
    if (e != T5_OK) return send_err(req, "409 Conflict", t5_err_str(e)) == ESP_OK ? 0 : -1;
    return send_json(req, NULL, "{\"ok\":true}") == ESP_OK ? 0 : -1;
}

static int h_logs(httpd_req_t *req)
{
    char v[24];
    uint64_t pos = query(req, "pos", v, sizeof v) ? strtoull(v, NULL, 10) : t5_log_head();
    uint32_t wait = query(req, "wait", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 10) : 0;
    if (wait > T5_DEV_LOG_WAIT_MAX_MS) wait = T5_DEV_LOG_WAIT_MAX_MS;
    uint32_t t0 = t5_time_ms();
    while (t5_log_head() <= pos && t5_time_ms() - t0 < wait) t5_delay_ms(50);
    char *buf = malloc(T5_LOG_RING_BYTES);
    if (!buf) return send_err(req, "500 Internal Server Error", "no memory") == ESP_OK ? 0 : -1;
    size_t n = t5_log_read(&pos, buf, T5_LOG_RING_BYTES);
    char hdr[24];
    snprintf(hdr, sizeof hdr, "%llu", (unsigned long long)pos);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "X-Log-Pos", hdr);
    esp_err_t r = httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return r == ESP_OK ? 0 : -1;
}

static int h_screenshot(httpd_req_t *req)
{
    char v[8];
    int div = query(req, "div", v, sizeof v) ? atoi(v) : 2;
    if (div < 1 || div > 8) div = 2;
    t5_dev_shot_t shot;
    t5_err_t e = t5_dev_shot(div, &shot);
    if (e == T5_ERR_STATE) return send_err(req, "503 Service Unavailable", "no framebuffer") == ESP_OK ? 0 : -1;
    if (e != T5_OK) return send_err(req, "500 Internal Server Error", "no memory") == ESP_OK ? 0 : -1;
    char hw[8], hh[8], hr[8];
    snprintf(hw, sizeof hw, "%d", shot.w);
    snprintf(hh, sizeof hh, "%d", shot.h);
    snprintf(hr, sizeof hr, "%d", shot.rotation);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Width", hw);
    httpd_resp_set_hdr(req, "X-Height", hh);
    httpd_resp_set_hdr(req, "X-Rotation", hr);
    esp_err_t r = httpd_resp_send(req, (const char *)shot.px, (ssize_t)shot.w * shot.h * 2);
    t5_dev_shot_free(&shot);
    return r == ESP_OK ? 0 : -1;
}

#define PY_SRC_MAX (32 * 1024)
#define PY_OUT_MAX (32 * 1024)

static int h_py(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > PY_SRC_MAX) {
        return send_err(req, "400 Bad Request", "empty or too big") == ESP_OK ? 0 : -1;
    }
    char *src = malloc(req->content_len + 1);
    char *out = heap_caps_malloc(PY_OUT_MAX, MALLOC_CAP_SPIRAM);
    if (!src || !out) {
        free(src);
        heap_caps_free(out);
        return send_err(req, "500 Internal Server Error", "no memory") == ESP_OK ? 0 : -1;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = recv_some(req, src + got, req->content_len - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got < req->content_len) {
        /* Never run half a script. */
        free(src);
        heap_caps_free(out);
        return send_err(req, "400 Bad Request", "upload interrupted") == ESP_OK ? 0 : -1;
    }
    src[got] = 0;
    t5_py_state_t st = t5_py_exec_capture(src, out, PY_OUT_MAX, 30000);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "X-Py-State", t5_py_state_name(st));
    esp_err_t r = httpd_resp_sendstr(req, out);
    free(src);
    heap_caps_free(out);
    return r == ESP_OK ? 0 : -1;
}

/* POST /api/ota, body: a .t5os (plan 9.1). */
typedef struct {
    httpd_req_t *req;
    size_t left;
} body_src_t;

static int body_read(void *ctx, void *buf, size_t n)
{
    body_src_t *b = ctx;
    if (!b->left) return 0;
    int r = recv_some(b->req, buf, n < b->left ? n : b->left);
    if (r <= 0) return T5_ERR_IO;
    b->left -= (size_t)r;
    return r;
}

static int h_ota(httpd_req_t *req)
{
    char err[128] = "";
    char q[32] = "", v[4] = "";
    bool reboot = httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
                  httpd_query_key_value(q, "reboot", v, sizeof v) == ESP_OK && v[0] == '1';
    t5_ota_hdr_t hdr = { 0 };
    body_src_t src = { .req = req, .left = req->content_len };
    t5_err_t e = t5_ota_install_stream(body_read, &src, T5_OTA_FROM_DEV, true, &hdr, err, sizeof err);
    char resp[200];
    if (e != T5_OK) {
        if (!err[0]) snprintf(err, sizeof err, "%s", t5_err_str(e));
        snprintf(resp, sizeof resp, "{\"error\":\"%s\"}", err);
        httpd_resp_set_status(req, "422 Unprocessable Entity");
    } else {
        snprintf(resp, sizeof resp, "{\"ok\":true,\"version\":\"%.15s\",\"restarting\":%s}", hdr.os_version,
                 reboot ? "true" : "false");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    if (e == T5_OK && reboot) t5_power_restart();
    return e == T5_OK ? 0 : -1;
}

typedef struct {
    const char *uri;
    httpd_method_t method;
    route_t route;
} def_t;

static const def_t s_routes[] = {
    { "/api/info", HTTP_GET, { h_info, false } },
    { "/api/pair", HTTP_POST, { h_pair, false } },
    { "/api/pair/confirm", HTTP_POST, { h_pair_confirm, false } },
    { "/api/apps", HTTP_GET, { h_apps, true } },
    { "/api/install", HTTP_POST, { h_install, true } },
    { "/api/launch", HTTP_POST, { h_launch, true } },
    { "/api/exit", HTTP_POST, { h_exit, true } },
    { "/api/remove", HTTP_POST, { h_remove, true } },
    { "/api/logs", HTTP_GET, { h_logs, true } },
    { "/api/screenshot", HTTP_GET, { h_screenshot, true } },
    { "/api/py", HTTP_POST, { h_py, true } },
    { "/api/ota", HTTP_POST, { h_ota, true } },
};

static t5_err_t server_start(void)
{
    if (s_httpd) return T5_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = T5_DEV_PORT;
    cfg.ctrl_port = 32770;
    cfg.max_uri_handlers = (uint16_t)(sizeof s_routes / sizeof s_routes[0]);
    cfg.stack_size = 12288;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 4;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        s_httpd = NULL;
        T5_LOGE(TAG, "can't start the server");
        return T5_ERR_FAIL;
    }
    for (size_t i = 0; i < sizeof s_routes / sizeof s_routes[0]; i++) {
        httpd_uri_t u = { .uri = s_routes[i].uri, .method = s_routes[i].method, .handler = dispatch,
                          .user_ctx = (void *)&s_routes[i].route };
        httpd_register_uri_handler(s_httpd, &u);
    }
    T5_LOGI(TAG, "developer mode: listening on port %d, %u paired", T5_DEV_PORT, (unsigned)s_db.n);
    return T5_OK;
}

static void server_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    T5_LOGI(TAG, "developer mode: stopped listening");
}

t5_err_t t5_dev_init(void)
{
    if (!s_lock) s_lock = t5_mutex_create();
    if (!s_lock) return T5_ERR_NO_MEM;
    psa_crypto_init();
    db_load();
    char path[96];
    struct stat st;
    s_enabled = t5_sys_path(path, sizeof path, ENABLE_FILE) == T5_OK && stat(path, &st) == 0;
    return s_enabled ? server_start() : T5_OK;
}

bool t5_dev_enabled(void)
{
    return s_enabled;
}

t5_err_t t5_dev_set_enabled(bool on)
{
    if (!s_lock) return T5_ERR_STATE;
    char path[96];
    t5_err_t e = t5_sys_path(path, sizeof path, ENABLE_FILE);
    if (e == T5_OK) e = on ? t5_storage_write_atomic(path, "1", 1) : (remove(path), T5_OK);
    s_enabled = on;
    if (on) return server_start() != T5_OK ? T5_ERR_FAIL : e;
    server_stop();
    return e;
}

void t5_dev_status(t5_dev_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->enabled = s_enabled;
    out->listening = s_httpd != NULL;
    out->port = T5_DEV_PORT;
    out->clients = (int)s_db.n;
    out->pairing = s_pair.active && t5_time_ms() - s_pair.t0 < PAIR_TIMEOUT_MS;
    out->requests = s_requests;
    out->denied = s_denied;
}
