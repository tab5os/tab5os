/* SPDX-License-Identifier: Apache-2.0
 * HTTP/1.1 client (http:// and https://) for apps with the net permission.
 *
 * Any method, request headers, a request body, response status and headers,
 * and the response body streamed (chunked and Content-Length bodies are
 * decoded) or read whole. Redirects are followed (up to 5) unless
 * T5_HTTP_NO_REDIRECT. Each request uses its own connection.
 *
 *   t5_http_header_t hdr[] = { { "Authorization", "Bearer xyz" } };
 *   t5_http_req_t req = { .method = "PUT", .url = "https://api.example.com/v1/item",
 *                         .headers = hdr, .n_headers = 1,
 *                         .body = json, .body_len = strlen(json) };
 *   t5_http_t *h;
 *   if (t5_http_open(&req, &h) == T5_OK) {
 *       int status = t5_http_status(h);
 *       char buf[512];
 *       int n;
 *       while ((n = t5_http_read(h, buf, sizeof buf, 10000)) > 0) consume(buf, n);
 *       t5_http_close(h);
 *   }
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct t5_http t5_http_t;

typedef struct {
    const char *name;
    const char *value;
} t5_http_header_t;

#define T5_HTTP_NO_REDIRECT (1u << 0)
#define T5_HTTP_TLS_NO_VERIFY (1u << 1)   /* don't check the server certificate (testing only) */

typedef struct {
    const char *method;             /* "GET" if NULL */
    const char *url;
    const t5_http_header_t *headers;
    size_t n_headers;
    const void *body;               /* sent with Content-Length; NULL for none */
    size_t body_len;
    uint32_t timeout_ms;            /* connect, and each wait for data; 0 = 10 s */
    uint32_t flags;                 /* T5_HTTP_* */
    const char *ca_pem;             /* trust this PEM CA instead of the bundle */
    const char *cert_pem;           /* client certificate for mutual TLS, or NULL */
    const char *key_pem;            /* its private key */
} t5_http_req_t;

/* Connect, send the request, read the status line and headers. */
T5_MUST_CHECK t5_err_t t5_http_open(const t5_http_req_t *req, t5_http_t **out);
int t5_http_status(const t5_http_t *h);
/* Response header (case-insensitive), or NULL. Valid until close. */
const char *t5_http_header(const t5_http_t *h, const char *name);
/* The i-th response header; T5_ERR_NOT_FOUND past the end. */
T5_MUST_CHECK t5_err_t t5_http_header_at(const t5_http_t *h, int i, const char **name, const char **value);
/* Content-Length, or -1 if the server didn't say (chunked or until close). */
int64_t t5_http_content_length(const t5_http_t *h);
/* The URL that answered, after redirects. */
const char *t5_http_url(const t5_http_t *h);
/* Up to n body bytes; 0 at the end of the body. */
int t5_http_read(t5_http_t *h, void *buf, size_t n, uint32_t timeout_ms);
T5_MUST_CHECK t5_err_t t5_http_close(t5_http_t *h);

/* The whole exchange: the body goes into buf (at most cap bytes, else
 * T5_ERR_NO_MEM). Returns the body length, or a negative t5_err_t;
 * *status gets the HTTP status. */
int t5_http_fetch(const t5_http_req_t *req, void *buf, size_t cap, int *status);

#ifdef __cplusplus
}
#endif
