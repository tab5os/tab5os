/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_app/elfsym.h"

#define EM_RISCV 243
#define SHT_SYMTAB 2
#define SHT_DYNSYM 11
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_FUNC 2
#define SHN_UNDEF 0

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct {
    const uint8_t *elf;
    size_t len;
    uint32_t shoff;
    uint16_t shnum, shentsize;
} elf_t;

static bool in(const elf_t *e, uint32_t off, uint32_t n)
{
    return off <= e->len && n <= e->len - off;
}

static t5_err_t open_elf(elf_t *e, const uint8_t *elf, size_t len)
{
    if (!elf || len < 52 || memcmp(elf, "\x7f" "ELF", 4) || elf[4] != 1 || elf[5] != 1) return T5_ERR_INVALID_ARG;
    if (rd16(elf + 18) != EM_RISCV) return T5_ERR_INVALID_ARG;
    e->elf = elf;
    e->len = len;
    e->shoff = rd32(elf + 32);
    e->shentsize = rd16(elf + 46);
    e->shnum = rd16(elf + 48);
    if (e->shentsize < 40 || !in(e, e->shoff, (uint32_t)e->shnum * e->shentsize)) return T5_ERR_INVALID_ARG;
    return T5_OK;
}

typedef struct {
    uint32_t type, offset, size, link, entsize;
} sec_t;

static bool section(const elf_t *e, uint32_t i, sec_t *s)
{
    if (i >= e->shnum) return false;
    const uint8_t *h = e->elf + e->shoff + i * e->shentsize;
    s->type = rd32(h + 4);
    s->offset = rd32(h + 16);
    s->size = rd32(h + 20);
    s->link = rd32(h + 24);
    s->entsize = rd32(h + 36);
    return in(e, s->offset, s->size);
}

/* Name at off in a string table, or NULL if it runs off the end. */
static const char *str(const elf_t *e, const sec_t *strtab, uint32_t off)
{
    if (off >= strtab->size) return NULL;
    const char *p = (const char *)e->elf + strtab->offset + off;
    if (!memchr(p, 0, strtab->size - off)) return NULL;
    return p;
}

typedef bool (*sym_fn_t)(const elf_t *e, const char *name, uint8_t info, uint16_t shndx, void *arg);

static t5_err_t walk(const elf_t *e, uint32_t type, sym_fn_t fn, void *arg)
{
    for (uint32_t i = 0; i < e->shnum; i++) {
        sec_t s, strs;
        if (!section(e, i, &s) || s.type != type) continue;
        if (s.entsize != 16 || !section(e, s.link, &strs)) return T5_ERR_INVALID_ARG;
        for (uint32_t k = 1; k < s.size / 16; k++) {
            const uint8_t *sym = e->elf + s.offset + k * 16;
            const char *name = str(e, &strs, rd32(sym));
            if (!name) return T5_ERR_INVALID_ARG;
            if (!name[0]) continue;
            if (!fn(e, name, sym[12], rd16(sym + 14), arg)) return T5_OK;
        }
    }
    return T5_OK;
}

typedef struct {
    t5_elfsym_fn_t fn;
    void *arg;
    bool stop;
    bool second_pass;
} undef_ctx_t;

static bool in_dynsym_undef(const elf_t *e, const char *name);

static bool undef_cb(const elf_t *e, const char *name, uint8_t info, uint16_t shndx, void *arg)
{
    undef_ctx_t *c = arg;
    uint8_t bind = info >> 4;
    if (shndx != SHN_UNDEF || bind == STB_LOCAL) return true;
    /* .symtab repeats what .dynsym already reported. */
    if (c->second_pass && in_dynsym_undef(e, name)) return true;
    if (!c->fn(name, bind == STB_WEAK, c->arg)) {
        c->stop = true;
        return false;
    }
    return true;
}

typedef struct {
    const char *name;
    bool found;
} find_ctx_t;

static bool find_undef_cb(const elf_t *e, const char *name, uint8_t info, uint16_t shndx, void *arg)
{
    (void)e;
    find_ctx_t *f = arg;
    if (shndx == SHN_UNDEF && (info >> 4) != STB_LOCAL && !strcmp(name, f->name)) {
        f->found = true;
        return false;
    }
    return true;
}

static bool in_dynsym_undef(const elf_t *e, const char *name)
{
    find_ctx_t f = { name, false };
    walk(e, SHT_DYNSYM, find_undef_cb, &f);
    return f.found;
}

t5_err_t t5_elfsym_undefined(const uint8_t *elf, size_t len, t5_elfsym_fn_t fn, void *arg)
{
    elf_t e;
    T5_RETURN_ON_ERR(open_elf(&e, elf, len));
    if (!fn) return T5_ERR_INVALID_ARG;
    undef_ctx_t c = { fn, arg, false, false };
    T5_RETURN_ON_ERR(walk(&e, SHT_DYNSYM, undef_cb, &c));
    if (c.stop) return T5_OK;
    c.second_pass = true;
    return walk(&e, SHT_SYMTAB, undef_cb, &c);
}

static bool def_cb(const elf_t *e, const char *name, uint8_t info, uint16_t shndx, void *arg)
{
    (void)e;
    find_ctx_t *f = arg;
    uint8_t bind = info >> 4;
    if (shndx != SHN_UNDEF && (bind == STB_GLOBAL || bind == STB_WEAK) && (info & 0xF) == STT_FUNC &&
        !strcmp(name, f->name)) {
        f->found = true;
        return false;
    }
    return true;
}

bool t5_elfsym_defines(const uint8_t *elf, size_t len, const char *name)
{
    elf_t e;
    if (open_elf(&e, elf, len) != T5_OK || !name) return false;
    find_ctx_t f = { name, false };
    walk(&e, SHT_DYNSYM, def_cb, &f);
    if (!f.found) walk(&e, SHT_SYMTAB, def_cb, &f);
    return f.found;
}
