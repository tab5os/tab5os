/* SPDX-License-Identifier: Apache-2.0
 * Protocol helpers for the network stack (sock.c, http.c, ws.c): URL
 * parsing, the HTTP response head, chunked bodies, WebSocket frames, and the
 * SHA-1/base64 behind the WebSocket handshake. Pure C with no ESP-IDF
 * dependency, so the host tests cover all of it (test/host/test_net.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- URLs ---------------------------------------------------------------------------- */
#define T5NP_HOST_MAX 254

typedef enum { T5NP_HTTP, T5NP_HTTPS, T5NP_WS, T5NP_WSS } t5np_scheme_t;

typedef struct {
    t5np_scheme_t scheme;
    bool tls;                       /* https or wss */
    char host[T5NP_HOST_MAX + 1];   /* without [] for IPv6 literals */
    uint16_t port;                  /* explicit, or the scheme's default */
    bool port_given;
    const char *path;               /* into the parsed string: "/..." or "?..." ; "" means "/" */
    const char *userinfo;           /* into the parsed string, or NULL */
    size_t userinfo_len;
} t5np_url_t;

/* 0, or -1 for anything that isn't scheme://host[:port][/path]. Fragments
 * ("#...") are left in path; http.c cuts them. */
int t5np_url_parse(const char *url, t5np_url_t *out);

/* Resolve a Location header against the URL it came from. Writes the
 * absolute URL into out. 0, or -1 if it doesn't fit or can't be parsed. */
int t5np_url_join(const char *base, const char *ref, char *out, size_t n);

/* ---- HTTP response head ---------------------------------------------------------------- */
#define T5NP_MAX_HEADERS 48

typedef struct {
    int status;                     /* 100..999 (responses) */
    const char *method;             /* requests: "GET" */
    const char *target;             /* requests: "/path?query" */
    int minor;                      /* HTTP/1.minor */
    int n;                          /* headers */
    const char *name[T5NP_MAX_HEADERS];
    const char *value[T5NP_MAX_HEADERS];   /* trimmed */
} t5np_head_t;

/* Offset just past "\r\n\r\n" (or "\n\n") in buf, or 0 if it isn't there yet. */
size_t t5np_head_end(const char *buf, size_t n);
/* Parse the head in place (buf[0..len), len from t5np_head_end): splits it
 * into NUL-terminated names and values. 0, or -1 if malformed. Headers past
 * T5NP_MAX_HEADERS are dropped. */
int t5np_head_parse(char *buf, size_t len, t5np_head_t *out);
/* A request head ("GET /x HTTP/1.1"), the same way: method and target set. */
int t5np_req_parse(char *buf, size_t len, t5np_head_t *out);
/* Case-insensitive lookup: the first value, or NULL. */
const char *t5np_head_get(const t5np_head_t *h, const char *name);
/* Does a comma-separated header value contain token (case-insensitive)? */
bool t5np_has_token(const char *value, const char *token);

/* ---- chunked transfer coding ----------------------------------------------------------- */
typedef struct {
    int state;
    uint64_t left;                  /* bytes left in the current chunk */
    int digits;
    bool done;                      /* the last chunk and trailers were read */
    bool bad;
} t5np_chunk_t;

void t5np_chunk_init(t5np_chunk_t *c);
/* Decode n bytes of chunked data in place. Returns the number of payload
 * bytes now at the front of buf. *used is how many input bytes were
 * consumed (less than n only once done). Sets c->bad on malformed input. */
size_t t5np_chunk_decode(t5np_chunk_t *c, uint8_t *buf, size_t n, size_t *used);

/* ---- WebSocket frames (RFC 6455) ------------------------------------------------------- */
enum { T5NP_WS_CONT = 0, T5NP_WS_TEXT = 1, T5NP_WS_BINARY = 2, T5NP_WS_CLOSE = 8, T5NP_WS_PING = 9, T5NP_WS_PONG = 10 };

typedef struct {
    bool fin;
    uint8_t rsv;                    /* RSV1-3 bits, must be 0 without extensions */
    uint8_t opcode;
    bool masked;
    uint8_t mask[4];
    uint64_t len;
    size_t hdr_len;
} t5np_ws_frame_t;

/* Write a frame header (2-14 bytes) into out. mask NULL: unmasked. */
size_t t5np_ws_header(uint8_t out[14], uint8_t opcode, bool fin, uint64_t len, const uint8_t *mask);
/* XOR data with mask, starting at offset off into the payload. */
void t5np_ws_mask(uint8_t *data, size_t n, const uint8_t mask[4], uint64_t off);
/* Parse a frame header from buf: 1 if complete (f filled), 0 if more bytes
 * are needed, -1 if invalid (bad length encoding, control frame > 125 bytes
 * or fragmented). */
int t5np_ws_parse(const uint8_t *buf, size_t n, t5np_ws_frame_t *f);

/* ---- SHA-1, base64, the handshake key ---------------------------------------------------- */
void t5np_sha1(const void *data, size_t n, uint8_t out[20]);
/* Encode n bytes; out needs 4 * ((n + 2) / 3) + 1. Returns the length written. */
size_t t5np_base64(const uint8_t *in, size_t n, char *out);
/* Sec-WebSocket-Accept for a Sec-WebSocket-Key: 28 chars + NUL. */
void t5np_ws_accept(const char *key, char out[29]);

#ifdef __cplusplus
}
#endif
