/* SPDX-License-Identifier: Apache-2.0
 * TOML subset parser, enough for app manifests:
 *   - comments, blank lines, LF or CRLF line ends
 *   - key = value, with bare keys and dotted bare keys (a.b = 1)
 *   - basic strings with \" \\ \b \t \n \f \r \uXXXX \UXXXXXXXX escapes,
 *     literal 'strings'
 *   - integers (decimal, optional sign, '_' between digits), booleans
 *   - arrays of strings or integers (one type per array, may span lines,
 *     trailing comma allowed)
 *   - [table] and [dotted.table] headers
 * Not supported (reported as errors, never misparsed): floats, dates,
 * multi-line strings, quoted keys, inline tables, arrays of tables, nested
 * arrays.
 *
 * The result is a flat list of fully qualified keys ("permissions.net").
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_TOML_STRING,
    T5_TOML_INT,
    T5_TOML_BOOL,
    T5_TOML_ARRAY,
} t5_toml_type_t;

typedef struct t5_toml_value {
    t5_toml_type_t type;
    char *s;                          /* STRING (UTF-8, NUL-terminated) */
    int64_t i;                        /* INT */
    bool b;                           /* BOOL */
    struct t5_toml_value *items;      /* ARRAY elements (STRING or INT) */
    int nitems;
} t5_toml_value_t;

typedef struct {
    char *key;                        /* full dotted path */
    t5_toml_value_t val;
    int line;
} t5_toml_kv_t;

typedef struct {
    t5_toml_kv_t *kv;
    int n, cap;
    char **tables;                    /* every [table] header, and implicit parents */
    int ntables, tcap;
} t5_toml_doc_t;

/* Parse text (need not be NUL-terminated). On failure err holds
 * "line N: reason" and doc is left empty. Always call t5_toml_free(). */
T5_MUST_CHECK t5_err_t t5_toml_parse(const char *text, size_t len, t5_toml_doc_t *doc, char *err, size_t errlen);
void t5_toml_free(t5_toml_doc_t *doc);

const t5_toml_value_t *t5_toml_get(const t5_toml_doc_t *doc, const char *path);
/* True if [name] was declared (or is the parent of a declared table/key). */
bool t5_toml_has_table(const t5_toml_doc_t *doc, const char *name);

#ifdef __cplusplus
}
#endif
