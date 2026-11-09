/* SPDX-License-Identifier: Apache-2.0
 * HTTP(S) downloads: see download.h.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "t5_net/download.h"
#include "t5_net/http.h"

#define BUF_SIZE 4096
#define TIMEOUT_MS 15000

t5_err_t t5_download_open(const char *url, t5_download_t **out, int64_t *len, char *err, size_t errlen)
{
    *out = NULL;
    const t5_http_req_t req = { .url = url, .timeout_ms = TIMEOUT_MS };
    t5_http_t *h;
    t5_err_t e = t5_http_open(&req, &h);
    if (e != T5_OK) {
        snprintf(err, errlen, "download failed (%s)", t5_err_str(e));
        return T5_ERR_IO;
    }
    int st = t5_http_status(h);
    if (st != 200) {
        snprintf(err, errlen, "download failed (HTTP %d)", st);
        T5_IGNORE(t5_http_close(h));
        return T5_ERR_IO;
    }
    if (len) *len = t5_http_content_length(h);
    *out = (t5_download_t *)h;
    return T5_OK;
}

int t5_download_read(void *dl, void *buf, size_t n)
{
    int r = t5_http_read((t5_http_t *)dl, buf, n, TIMEOUT_MS);
    return r >= 0 ? r : T5_ERR_IO;
}

void t5_download_close(t5_download_t *dl)
{
    if (dl) T5_IGNORE(t5_http_close((t5_http_t *)dl));
}

t5_err_t t5_download_file(const char *url, const char *path, int64_t *bytes, char *err, size_t errlen)
{
    t5_download_t *dl;
    int64_t len = 0;
    T5_RETURN_ON_ERR(t5_download_open(url, &dl, &len, err, errlen));
    char *buf = malloc(BUF_SIZE);
    FILE *f = buf ? fopen(path, "wb") : NULL;
    if (!f) {
        if (buf) snprintf(err, errlen, "can't create %s", path);
        else snprintf(err, errlen, "out of memory");
        t5_err_t e = buf ? T5_ERR_IO : T5_ERR_NO_MEM;
        free(buf);
        t5_download_close(dl);
        return e;
    }
    int64_t got = 0;
    int r;
    bool wrote = true;
    while ((r = t5_download_read(dl, buf, BUF_SIZE)) > 0) {
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
            wrote = false;
            break;
        }
        got += r;
    }
    wrote &= fclose(f) == 0;
    free(buf);
    t5_download_close(dl);
    if (!wrote) {
        snprintf(err, errlen, "can't write %s", path);
        return T5_ERR_IO;
    }
    if (r < 0 || (len > 0 && got != len)) {
        snprintf(err, errlen, "the download broke off after %lld bytes", (long long)got);
        return T5_ERR_IO;
    }
    if (bytes) *bytes = got;
    return T5_OK;
}
