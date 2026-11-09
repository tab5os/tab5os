/* SPDX-License-Identifier: Apache-2.0
 * Module profiles: a small JSON reader (no cJSON: IDF 6.1 dropped it from the
 * tree and the schema is tiny), the profile registry, scan and suggestions.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t5_core/log.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
#include "t5_ports/profiles.h"
#include "t5_storage/paths.h"
#include "ports_priv.h"

static const char *TAG = "t5.profiles";

#define FILE_MAX   8192
#define DEPTH_MAX  16

/* ---- built-ins (best-effort: vendor docs, not bench-tested) --------------- */
#define GROVE { "grove", "ext" }, 2
#define E5V T5_PROFILE_POWER_EXT5V

static const t5_profile_t s_builtin[] = {
    { "ENV III", "sht3x+qmp6988", "SHT30 humidity 0x44, QMP6988 pressure 0x70", GROVE, T5_MODE_I2C, { 0x44, 0x70 }, 2, E5V, true },
    { "ENV IV", "sht4x+bmp280", "SHT40 humidity 0x44, BMP280 pressure 0x76", GROVE, T5_MODE_I2C, { 0x44, 0x76 }, 2, E5V, true },
    { "ENV II", "sht3x+bmp280", "SHT30 0x44, BMP280 0x76", GROVE, T5_MODE_I2C, { 0x44, 0x76 }, 2, E5V, true },
    { "ToF", "vl53l0x", "VL53L0X distance, 0x29", GROVE, T5_MODE_I2C, { 0x29 }, 1, E5V, true },
    { "Color", "tcs34725", "TCS3472 colour sensor, 0x29", GROVE, T5_MODE_I2C, { 0x29 }, 1, E5V, true },
    { "ADC", "ads1100", "ADS1100 16-bit ADC, 0x48", GROVE, T5_MODE_I2C, { 0x48 }, 1, E5V, true },
    { "RTC", "bm8563", "BM8563 RTC, 0x51", GROVE, T5_MODE_I2C, { 0x51 }, 1, E5V, true },
    { "DLight", "bh1750", "BH1750 ambient light, 0x23", GROVE, T5_MODE_I2C, { 0x23 }, 1, E5V, true },
    { "OLED", "sh1107", "SH1107 1.3in OLED, 0x3C", GROVE, T5_MODE_I2C, { 0x3C }, 1, E5V, true },
    { "PaHUB", "pca9548", "PCA9548 I2C hub, 0x70", GROVE, T5_MODE_I2C, { 0x70 }, 1, E5V, true },
    { "NCIR", "mlx90614", "MLX90614 IR thermometer, 0x5A", GROVE, T5_MODE_I2C, { 0x5A }, 1, E5V, true },
    { "Accel", "adxl345", "ADXL345 accelerometer, 0x53", GROVE, T5_MODE_I2C, { 0x53 }, 1, E5V, true },
    { "Ultrasonic I2C", "rcwl9620", "RCWL-9620 distance, 0x57", GROVE, T5_MODE_I2C, { 0x57 }, 1, E5V, true },
    { "EXT.IO", "pca9554", "PCA9554 8-bit expander, 0x27", GROVE, T5_MODE_I2C, { 0x27 }, 1, E5V, true },
    { "Joystick", "m5joystick", "STM32 joystick, 0x52", GROVE, T5_MODE_I2C, { 0x52 }, 1, E5V, true },
    { "Angle", "adc", "Potentiometer on the signal pin", { "grove" }, 1, T5_MODE_ADC, { 0 }, 0, E5V, true },
    { "GPS", "nmea", "NMEA at 9600 baud", { "grove" }, 1, T5_MODE_UART, { 0 }, 0, E5V, true },
};
#define N_BUILTIN ((int)(sizeof s_builtin / sizeof s_builtin[0]))
#define N_LOADED_MAX (T5_PROFILE_MAX - N_BUILTIN)

/* PSRAM: read when a port is plugged in, not on any hot path. */
EXT_RAM_BSS_ATTR static t5_profile_t s_loaded[N_LOADED_MAX];
static int s_nloaded;

/* ---- JSON reader ---------------------------------------------------------- */
typedef struct {
    const char *s;
    size_t n, i;
} js_t;

static void ws(js_t *j)
{
    while (j->i < j->n && (j->s[j->i] == ' ' || j->s[j->i] == '\t' || j->s[j->i] == '\n' || j->s[j->i] == '\r')) {
        j->i++;
    }
}

static int peek(js_t *j)
{
    ws(j);
    return j->i < j->n ? (unsigned char)j->s[j->i] : -1;
}

static bool eat(js_t *j, char c)
{
    if (peek(j) == c) {
        j->i++;
        return true;
    }
    return false;
}

/* String into out (truncated to cap-1). out may be NULL to skip. */
static bool str(js_t *j, char *out, size_t cap)
{
    if (!eat(j, '"')) {
        return false;
    }
    size_t o = 0;
    while (j->i < j->n) {
        char c = j->s[j->i++];
        if (c == '"') {
            if (out && cap) out[o < cap ? o : cap - 1] = '\0';
            return true;
        }
        if ((unsigned char)c < 0x20) {
            return false;
        }
        if (c == '\\') {
            if (j->i >= j->n) return false;
            char e = j->s[j->i++];
            switch (e) {
            case '"': case '\\': case '/': c = e; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
                if (j->i + 4 > j->n) return false;
                for (int k = 0; k < 4; k++) {
                    char h = j->s[j->i + k];
                    bool hex = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F');
                    if (!hex) return false;
                }
                j->i += 4;
                c = '?';    /* non-ASCII isn't needed for names */
                break;
            default: return false;
            }
        }
        if (out && o + 1 < cap) {
            out[o] = c;
        }
        o++;
    }
    return false;
}

static bool num(js_t *j, long *out)
{
    ws(j);
    size_t st = j->i;
    bool neg = false;
    long v = 0;
    if (j->i < j->n && j->s[j->i] == '-') {
        neg = true;
        j->i++;
    }
    size_t d0 = j->i;
    while (j->i < j->n && j->s[j->i] >= '0' && j->s[j->i] <= '9') {
        if (v < 100000000) v = v * 10 + (j->s[j->i] - '0');
        j->i++;
    }
    if (j->i == d0) {
        j->i = st;
        return false;
    }
    if (j->i < j->n && j->s[j->i] == '.') {
        j->i++;
        while (j->i < j->n && j->s[j->i] >= '0' && j->s[j->i] <= '9') j->i++;
    }
    if (j->i < j->n && (j->s[j->i] == 'e' || j->s[j->i] == 'E')) {
        j->i++;
        if (j->i < j->n && (j->s[j->i] == '+' || j->s[j->i] == '-')) j->i++;
        while (j->i < j->n && j->s[j->i] >= '0' && j->s[j->i] <= '9') j->i++;
    }
    if (out) *out = neg ? -v : v;
    return true;
}

static bool lit(js_t *j, const char *w)
{
    size_t l = strlen(w);
    ws(j);
    if (j->i + l <= j->n && !memcmp(j->s + j->i, w, l)) {
        j->i += l;
        return true;
    }
    return false;
}

static bool skip(js_t *j, int depth)
{
    if (depth > DEPTH_MAX) {
        return false;
    }
    int c = peek(j);
    if (c == '"') {
        return str(j, NULL, 0);
    }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->i++;
        if (eat(j, close)) return true;
        do {
            if (c == '{' && (!str(j, NULL, 0) || !eat(j, ':'))) return false;
            if (!skip(j, depth + 1)) return false;
        } while (eat(j, ','));
        return eat(j, close);
    }
    return num(j, NULL) || lit(j, "true") || lit(j, "false") || lit(j, "null");
}

/* A string, or an array of strings: calls fn for each. */
typedef bool (*str_fn_t)(t5_profile_t *p, const char *s);

static bool str_list(js_t *j, t5_profile_t *p, str_fn_t fn)
{
    char buf[32];
    if (peek(j) == '"') {
        return str(j, buf, sizeof buf) && fn(p, buf);
    }
    if (!eat(j, '[')) return false;
    if (eat(j, ']')) return true;
    do {
        if (!str(j, buf, sizeof buf) || !fn(p, buf)) return false;
    } while (eat(j, ','));
    return eat(j, ']');
}

static bool add_port(t5_profile_t *p, const char *s)
{
    if (p->nports < T5_PROFILE_MAX_PORTS) {
        snprintf(p->ports[p->nports++], sizeof p->ports[0], "%s", s);
    }
    return true;
}

static bool add_power(t5_profile_t *p, const char *s)
{
    if (!strcmp(s, "ext5v")) p->power |= T5_PROFILE_POWER_EXT5V;
    else if (!strcmp(s, "usb5v")) p->power |= T5_PROFILE_POWER_USB5V;
    else if (!strcmp(s, "hvin")) p->power |= T5_PROFILE_POWER_HVIN;
    return true;    /* unknown rails are ignored */
}

static bool add_addr(t5_profile_t *p, long v)
{
    if (v < 0 || v > 0x7F) return false;
    if (p->naddrs < T5_PROFILE_MAX_ADDRS) p->addrs[p->naddrs++] = (uint8_t)v;
    return true;
}

static bool addr_list(js_t *j, t5_profile_t *p)
{
    if (!eat(j, '[')) return false;
    if (eat(j, ']')) return true;
    do {
        long v;
        if (peek(j) == '"') {
            char buf[12], *end;
            if (!str(j, buf, sizeof buf)) return false;
            v = strtol(buf, &end, 0);
            if (end == buf || *end) return false;
        } else if (!num(j, &v)) {
            return false;
        }
        if (!add_addr(p, v)) return false;
    } while (eat(j, ','));
    return eat(j, ']');
}

static bool object(js_t *j, t5_profile_t *p)
{
    memset(p, 0, sizeof *p);
    p->bus = T5_MODE_I2C;
    if (!eat(j, '{')) return false;
    if (eat(j, '}')) return false;   /* no name */
    do {
        char key[16];
        if (!str(j, key, sizeof key) || !eat(j, ':')) return false;
        bool ok;
        if (!strcmp(key, "name")) ok = str(j, p->name, sizeof p->name);
        else if (!strcmp(key, "driver")) ok = str(j, p->driver, sizeof p->driver);
        else if (!strcmp(key, "notes")) ok = str(j, p->notes, sizeof p->notes);
        else if (!strcmp(key, "ports") || !strcmp(key, "port")) ok = str_list(j, p, add_port);
        else if (!strcmp(key, "power")) ok = str_list(j, p, add_power);
        else if (!strcmp(key, "i2c")) ok = addr_list(j, p);
        else if (!strcmp(key, "bus")) {
            char b[8];
            ok = str(j, b, sizeof b);
            p->bus = t5_port_mode_parse(b);
            ok = ok && p->bus != T5_MODE_COUNT;
        } else ok = skip(j, 1);
        if (!ok) return false;
    } while (eat(j, ','));
    return eat(j, '}') && p->name[0];
}

int t5_profile_parse(const char *json, size_t len, t5_profile_t *out, int max)
{
    if (!json || !out || max <= 0) {
        return T5_ERR_INVALID_ARG;
    }
    js_t j = { json, len, 0 };
    int n = 0;
    t5_profile_t tmp;
    if (eat(&j, '[')) {
        if (!eat(&j, ']')) {
            do {
                if (!object(&j, &tmp)) return T5_ERR_INVALID_ARG;
                if (n < max) out[n++] = tmp;
            } while (eat(&j, ','));
            if (!eat(&j, ']')) return T5_ERR_INVALID_ARG;
        }
    } else {
        if (!object(&j, &tmp)) return T5_ERR_INVALID_ARG;
        out[n++] = tmp;
    }
    return peek(&j) == -1 ? n : T5_ERR_INVALID_ARG;
}

/* ---- registry ------------------------------------------------------------- */
void profiles_reset(void)
{
    s_nloaded = 0;
}

int t5_profiles_count(void)
{
    return N_BUILTIN + s_nloaded;
}

const t5_profile_t *t5_profiles_get(int i)
{
    if (i < 0) return NULL;
    if (i < N_BUILTIN) return &s_builtin[i];
    i -= N_BUILTIN;
    return i < s_nloaded ? &s_loaded[i] : NULL;
}

static int load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return T5_ERR_NOT_FOUND;
    }
    char *buf = malloc(FILE_MAX);
    if (!buf) {
        fclose(f);
        return T5_ERR_NO_MEM;
    }
    size_t len = fread(buf, 1, FILE_MAX, f);
    bool too_big = len == FILE_MAX && fgetc(f) != EOF;
    fclose(f);
    int n = T5_ERR_NO_MEM;
    if (!too_big) {
        n = t5_profile_parse(buf, len, s_loaded + s_nloaded, N_LOADED_MAX - s_nloaded);
    }
    free(buf);
    if (n < 0) {
        T5_LOGW(TAG, "%s: %s", path, too_big ? "too big" : "bad profile JSON");
        return n;
    }
    for (int i = 0; i < n; i++) {
        s_loaded[s_nloaded + i].builtin = false;
    }
    s_nloaded += n;
    return n;
}

int t5_profiles_load_dir(const char *dir)
{
    DIR *d = dir ? opendir(dir) : NULL;
    if (!d) {
        return 0;
    }
    int added = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_nloaded < N_LOADED_MAX) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json") != 0) {
            continue;
        }
        char path[160];
        if (snprintf(path, sizeof path, "%.100s/%.58s", dir, e->d_name) >= (int)sizeof path) continue;
        int n = load_file(path);
        if (n > 0) {
            added += n;
        }
    }
    closedir(d);
    return added;
}

int t5_profiles_reload(void)
{
    profiles_reset();
    return t5_profiles_load_dir(T5_PROFILES_DATA_DIR) + t5_profiles_load_dir(T5_PROFILES_SD_DIR);
}

/* ---- scan and suggestions ------------------------------------------------- */
int t5_profiles_scan(const char *port, uint8_t *found, int max)
{
    t5_port_t *p;
    t5_err_t err = t5_port_open(port, T5_MODE_I2C, NULL, &p);
    if (err != T5_OK) {
        return err;
    }
    int n = t5_port_i2c_scan(p, found, max > 0 ? (size_t)max : 0);
    T5_IGNORE(t5_port_close(p));   /* we opened it just above */
    return n;
}

static bool lists_port(const t5_profile_t *p, const char *port)
{
    for (int i = 0; port && i < p->nports; i++) {
        if (!strcmp(p->ports[i], port)) return true;
    }
    return false;
}

/* Negative if a ranks before b. */
static int rank(const t5_profile_match_t *a, const t5_profile_match_t *b)
{
    if (a->complete != b->complete) return a->complete ? -1 : 1;
    if (a->matched != b->matched) return b->matched - a->matched;
    if (a->port_ok != b->port_ok) return a->port_ok ? -1 : 1;
    int ma = a->profile->naddrs - a->matched, mb = b->profile->naddrs - b->matched;
    if (ma != mb) return ma - mb;
    if (a->profile->builtin != b->profile->builtin) return a->profile->builtin ? 1 : -1;
    return strcmp(a->profile->name, b->profile->name);
}

int t5_profiles_suggest(const char *port, const uint8_t *found, int nfound, t5_profile_match_t *out, int max)
{
    t5_profile_match_t all[T5_PROFILE_MAX];
    int n = 0;
    if (!out || max <= 0 || (!found && nfound > 0)) {
        return 0;
    }
    for (int i = 0; i < t5_profiles_count() && n < T5_PROFILE_MAX; i++) {
        const t5_profile_t *p = t5_profiles_get(i);
        if (!p || p->bus != T5_MODE_I2C || !p->naddrs) continue;
        int m = 0;
        for (int a = 0; a < p->naddrs; a++) {
            for (int f = 0; f < nfound; f++) {
                if (found[f] == p->addrs[a]) {
                    m++;
                    break;
                }
            }
        }
        if (!m) continue;
        t5_profile_match_t cand = { p, (uint8_t)m, m == p->naddrs, lists_port(p, port) };
        /* insertion sort: lists are tiny */
        int k = n++;
        while (k > 0 && rank(&cand, &all[k - 1]) < 0) {
            all[k] = all[k - 1];
            k--;
        }
        all[k] = cand;
    }
    int w = n < max ? n : max;
    memcpy(out, all, (size_t)w * sizeof *out);
    return w;
}
