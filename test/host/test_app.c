/* SPDX-License-Identifier: Apache-2.0
 * Phase 5 app runtime logic: resource tracker, crash guard, ELF imports,
 * ABI resolution and permission stubs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"
#include "t5_mocks.h"
#include "fixtures/app_elf_fixture.h"
#include "t5_app/abi.h"
#include "t5_app/crashguard.h"
#include "t5_app/elfsym.h"
#include "t5_app/tracker.h"
#include "t5_pkg/manifest.h"

/* A small ABI table standing in for the generated one (sorted by name). */
static int fake_open(void) { return 0; }
static void *fake_screen(void) { return (void *)0x1234; }
const t5_abi_sym_t t5_abi_table[] = {
    { "calloc", (const void *)calloc, 0, T5_ABI_RET_PTR, 1 },
    { "free", (const void *)free, 0, T5_ABI_RET_VOID, 1 },
    { "t5_app_screen", (const void *)fake_screen, 0, T5_ABI_RET_PTR, 1 },
    { "t5_port_open", (const void *)fake_open, T5_PERM_PORTS, T5_ABI_RET_ERR, 1 },
};
const size_t t5_abi_count = sizeof t5_abi_table / sizeof t5_abi_table[0];

static char s_dir[64];

void setUp(void)
{
    mock_reset_all();
    strcpy(s_dir, "/tmp/t5app.XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(s_dir));
}

void tearDown(void)
{
    char cmd[96];
    snprintf(cmd, sizeof cmd, "rm -rf %s", s_dir);
    (void)!system(cmd);
    t5_tracker_set_allocator(NULL);
}

/* ---- tracker ---------------------------------------------------------------- */
static char s_order[64];
static int s_norder;

static void rel_tag(void *h)
{
    s_order[s_norder++] = *(char *)h;
}

static void test_tracker_release_order(void)
{
    static t5_tracker_t t;
    t5_tracker_init(&t, 0);
    static char task = 'T', timer = 'M', ui = 'U', port = 'P', lock = 'L', rail = 'R', other = 'O', task2 = 't';
    s_norder = 0;
    /* Added out of order; released by kind, newest first within a kind. */
    TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_LOCK, &lock, rel_tag));
    TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_PORT, &port, rel_tag));
    TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_TASK, &task, rel_tag));
    TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_UI, &ui, rel_tag));
    TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_OTHER, &other, rel_tag));
    TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_RAIL, &rail, rel_tag));
    TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_TIMER, &timer, rel_tag));
    TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_TASK, &task2, rel_tag));
    TEST_ASSERT_EQUAL(2, t5_tracker_count(&t, T5_RES_TASK));
    /* Released normally: forgotten. Re-adding updates, never duplicates. */
    TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_OTHER, &other, rel_tag));
    TEST_ASSERT_EQUAL(1, t5_tracker_count(&t, T5_RES_OTHER));
    TEST_ASSERT_EQUAL(8, t5_tracker_release_all(&t));
    s_order[s_norder] = 0;
    TEST_ASSERT_EQUAL_STRING("tTMUPRLO", s_order);
    TEST_ASSERT_EQUAL(0, t.n);
}

static t5_tracker_t *s_reent;
static char s_child = 'c';

/* Releasing a parent releases its child, which reports its own removal. */
static void rel_parent(void *h)
{
    (void)h;
    t5_tracker_remove(s_reent, &s_child);
}

static void test_tracker_reentrant_release(void)
{
    static t5_tracker_t t;
    t5_tracker_init(&t, 0);
    s_reent = &t;
    static char parent = 'p';
    s_norder = 0;
    t5_tracker_add(&t, T5_RES_UI, &s_child, rel_tag);
    t5_tracker_add(&t, T5_RES_UI, &parent, rel_parent);
    TEST_ASSERT_EQUAL(1, t5_tracker_release_all(&t));   /* child was freed with its parent */
    TEST_ASSERT_EQUAL(0, s_norder);
}

static void test_tracker_table_full(void)
{
    static t5_tracker_t t;
    t5_tracker_init(&t, 0);
    static char h[T5_TRACK_MAX + 5];
    for (int i = 0; i < T5_TRACK_MAX; i++) TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_OTHER, &h[i], NULL));
    TEST_ASSERT_FALSE(t5_tracker_add(&t, T5_RES_OTHER, &h[T5_TRACK_MAX], NULL));
    TEST_ASSERT_EQUAL(1, t.dropped);
    TEST_ASSERT_TRUE(t5_tracker_remove(&t, &h[10]));
    TEST_ASSERT_FALSE(t5_tracker_remove(&t, &h[10]));
    TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_OTHER, &h[T5_TRACK_MAX], NULL));
}

static int s_allocs, s_frees;
static void *count_malloc(size_t n) { s_allocs++; return malloc(n); }
static void count_free(void *p) { s_frees++; free(p); }

static void test_tracker_heap(void)
{
    t5_tracker_alloc_t a = { count_malloc, count_free };
    t5_tracker_set_allocator(&a);
    s_allocs = s_frees = 0;
    static t5_tracker_t t, other;
    t5_tracker_init(&t, 0);
    t5_tracker_init(&other, 0);
    void *p[20];
    for (int i = 0; i < 20; i++) {
        p[i] = t5_tracker_malloc(&t, 100 + (size_t)i);
        TEST_ASSERT_NOT_NULL(p[i]);
        TEST_ASSERT_EQUAL(0, (uintptr_t)p[i] % 8);
        memset(p[i], 0xAB, 100 + (size_t)i);
    }
    TEST_ASSERT_EQUAL(20, t.heap_blocks);
    TEST_ASSERT_TRUE(t5_tracker_owns(p[3]));
    /* Memory the tracker didn't hand out is left alone. */
    char *plain = malloc(32);
    memset(plain, 0, 32);
    TEST_ASSERT_FALSE(t5_tracker_owns(plain));
    TEST_ASSERT_FALSE(t5_tracker_free(&t, plain));
    free(plain);
    TEST_ASSERT_TRUE(t5_tracker_free(&t, p[0]));
    TEST_ASSERT_FALSE(t5_tracker_owns(p[0]));      /* header wiped: no double free */
    /* realloc keeps contents. */
    char *q = t5_tracker_realloc(&t, p[1], 4000);
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_HEX8(0xAB, (uint8_t)q[100]);
    TEST_ASSERT_EQUAL(4000, t5_tracker_block_size(q));
    /* A block of another app is freed from its own list. */
    void *o = t5_tracker_malloc(&other, 10);
    TEST_ASSERT_TRUE(t5_tracker_free(&t, o));
    TEST_ASSERT_EQUAL(0, other.heap_blocks);
    void *z = t5_tracker_calloc(&t, 10, 10);
    for (int i = 0; i < 100; i++) TEST_ASSERT_EQUAL(0, ((uint8_t *)z)[i]);
    TEST_ASSERT_NULL(t5_tracker_calloc(&t, SIZE_MAX / 2, 4));
    /* Exit frees everything that's left. */
    t5_tracker_release_all(&t);
    TEST_ASSERT_EQUAL(0, t.heap_blocks);
    TEST_ASSERT_EQUAL(0, t.heap_bytes);
    TEST_ASSERT_EQUAL(s_allocs, s_frees);
    TEST_ASSERT_TRUE(t.heap_peak >= 4000);
}

static void test_tracker_heap_limit(void)
{
    static t5_tracker_t t;
    t5_tracker_init(&t, 1);    /* 1 KB */
    void *a = t5_tracker_malloc(&t, 800);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NULL(t5_tracker_malloc(&t, 800));
    TEST_ASSERT_EQUAL(1, t.heap_refused);
    t5_tracker_free_all(&t);
}

/* ---- crash guard ------------------------------------------------------------ */
static void test_crash_classify(void)
{
    t5_crash_boot_t b = { .valid = true };
    strcpy(b.running, "com.x.app");
    TEST_ASSERT_EQUAL(T5_CRASH_PANIC, t5_crash_classify(&b, true));
    TEST_ASSERT_EQUAL(T5_CRASH_NONE, t5_crash_classify(&b, false));      /* software restart etc. */
    b.pending_kind = T5_CRASH_ANR;
    TEST_ASSERT_EQUAL(T5_CRASH_ANR, t5_crash_classify(&b, false));
    t5_crash_boot_t none = { .valid = true };
    TEST_ASSERT_EQUAL(T5_CRASH_NONE, t5_crash_classify(&none, true));    /* OS crash, no app */
    t5_crash_boot_t junk = { .valid = false };
    strcpy(junk.running, "x");
    TEST_ASSERT_EQUAL(T5_CRASH_NONE, t5_crash_classify(&junk, true));
}

static void test_crash_block_after_two(void)
{
    static t5_crash_db_t db;
    memset(&db, 0, sizeof db);
    TEST_ASSERT_TRUE(t5_crash_autolaunch_allowed(&db, "a.b"));
    t5_crash_record(&db, "a.b", T5_CRASH_PANIC, 100);
    TEST_ASSERT_TRUE(t5_crash_autolaunch_allowed(&db, "a.b"));
    t5_crash_record(&db, "a.b", T5_CRASH_PANIC, 200);
    TEST_ASSERT_FALSE(t5_crash_autolaunch_allowed(&db, "a.b"));
    TEST_ASSERT_TRUE(t5_crash_autolaunch_allowed(&db, "c.d"));
    /* A clean exit clears the streak but keeps the total. */
    t5_crash_clean_exit(&db, "a.b");
    TEST_ASSERT_TRUE(t5_crash_autolaunch_allowed(&db, "a.b"));
    TEST_ASSERT_EQUAL(2, t5_crash_find(&db, "a.b")->total);
    t5_crash_forget(&db, "a.b");
    TEST_ASSERT_NULL(t5_crash_find(&db, "a.b"));
}

static void test_crash_db_persist_and_evict(void)
{
    static t5_crash_db_t db, back;
    memset(&db, 0, sizeof db);
    char id[32];
    for (int i = 0; i < T5_CRASH_MAX_APPS; i++) {
        snprintf(id, sizeof id, "app.n%d", i);
        t5_crash_record(&db, id, T5_CRASH_PANIC, 1000 + i);
    }
    t5_crash_record(&db, "app.new", T5_CRASH_ANR, 5000);     /* evicts app.n0, the oldest */
    TEST_ASSERT_EQUAL(T5_CRASH_MAX_APPS, db.n);
    TEST_ASSERT_NULL(t5_crash_find(&db, "app.n0"));
    TEST_ASSERT_NOT_NULL(t5_crash_find(&db, "app.new"));
    char path[96];
    snprintf(path, sizeof path, "%s/crashes.bin", s_dir);
    TEST_ASSERT_EQUAL(T5_OK, t5_crash_db_save(&db, path));
    TEST_ASSERT_EQUAL(T5_OK, t5_crash_db_load(&back, path));
    TEST_ASSERT_EQUAL(db.n, back.n);
    TEST_ASSERT_EQUAL(T5_CRASH_ANR, t5_crash_find(&back, "app.new")->last_kind);
    /* A corrupt file loads as empty, never as garbage. */
    FILE *f = fopen(path, "r+b");
    fseek(f, 20, SEEK_SET);
    fputc(0x55, f);
    fclose(f);
    TEST_ASSERT_NOT_EQUAL(T5_OK, t5_crash_db_load(&back, path));
    TEST_ASSERT_EQUAL(0, back.n);
}

/* ---- ELF imports ------------------------------------------------------------------- */
static int s_nundef;
static char s_undef[32][48];

static bool collect(const char *name, bool weak, void *arg)
{
    (void)weak; (void)arg;
    if (s_nundef < 32) snprintf(s_undef[s_nundef++], 48, "%s", name);
    return true;
}

static bool has_undef(const char *name)
{
    for (int i = 0; i < s_nundef; i++) if (!strcmp(s_undef[i], name)) return true;
    return false;
}

static void test_elfsym_real_app(void)
{
    s_nundef = 0;
    TEST_ASSERT_EQUAL(T5_OK, t5_elfsym_undefined(app_elf_fixture, sizeof app_elf_fixture, collect, NULL));
    TEST_ASSERT_EQUAL(16, s_nundef);              /* nm -D --undefined-only hello.elf */
    TEST_ASSERT_TRUE(has_undef("t5_app_screen"));
    TEST_ASSERT_TRUE(has_undef("calloc"));
    TEST_ASSERT_TRUE(has_undef("md_button_create"));
    TEST_ASSERT_FALSE(has_undef("t5_app_entry"));
    TEST_ASSERT_TRUE(t5_elfsym_defines(app_elf_fixture, sizeof app_elf_fixture, "t5_app_entry"));
    TEST_ASSERT_FALSE(t5_elfsym_defines(app_elf_fixture, sizeof app_elf_fixture, "app_main"));
}

static void test_elfsym_rejects_garbage(void)
{
    uint8_t junk[64] = { 0x7f, 'E', 'L', 'F' };
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_elfsym_undefined(junk, sizeof junk, collect, NULL));
    /* Truncations and random corruption never crash the parser. */
    uint8_t *copy = malloc(sizeof app_elf_fixture);
    for (size_t cut = 0; cut < sizeof app_elf_fixture; cut += 97) {
        s_nundef = 0;
        (void)t5_elfsym_undefined(app_elf_fixture, cut, collect, NULL);
    }
    srand(7);
    for (int round = 0; round < 300; round++) {
        memcpy(copy, app_elf_fixture, sizeof app_elf_fixture);
        for (int k = 0; k < 8; k++) copy[rand() % sizeof app_elf_fixture] = (uint8_t)rand();
        s_nundef = 0;
        (void)t5_elfsym_undefined(copy, sizeof app_elf_fixture, collect, NULL);
        (void)t5_elfsym_defines(copy, sizeof app_elf_fixture, "t5_app_entry");
    }
    free(copy);
}

/* ---- ABI resolution ----------------------------------------------------------------- */
static void test_abi_resolve_and_stubs(void)
{
    TEST_ASSERT_NOT_NULL(t5_abi_find("free"));
    TEST_ASSERT_NULL(t5_abi_find("esp_restart"));         /* not exported */
    TEST_ASSERT_NULL(t5_abi_find("zzz"));
    TEST_ASSERT_NULL(t5_abi_find("a"));
    bool denied;
    TEST_ASSERT_EQUAL_PTR(fake_screen, (void *)t5_abi_resolve("t5_app_screen", 0, &denied));
    TEST_ASSERT_FALSE(denied);
    /* Declared: the real function. Not declared: a stub returning DENIED. */
    TEST_ASSERT_EQUAL_PTR(fake_open, (void *)t5_abi_resolve("t5_port_open", T5_PERM_PORTS, &denied));
    TEST_ASSERT_FALSE(denied);
    uintptr_t stub = t5_abi_resolve("t5_port_open", T5_PERM_NET, &denied);
    TEST_ASSERT_TRUE(denied);
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, ((int (*)(void))stub)());
    TEST_ASSERT_NULL(((void *(*)(void))t5_abi_stub(T5_ABI_RET_PTR))());
    TEST_ASSERT_FALSE(((bool (*)(void))t5_abi_stub(T5_ABI_RET_BOOL))());
    TEST_ASSERT_EQUAL(0, t5_abi_resolve("esp_restart", ~0u, &denied));
    TEST_ASSERT_TRUE(t5_abi_is_runtime_helper("__divdi3"));
    TEST_ASSERT_FALSE(t5_abi_is_runtime_helper("_start"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tracker_release_order);
    RUN_TEST(test_tracker_reentrant_release);
    RUN_TEST(test_tracker_table_full);
    RUN_TEST(test_tracker_heap);
    RUN_TEST(test_tracker_heap_limit);
    RUN_TEST(test_crash_classify);
    RUN_TEST(test_crash_block_after_two);
    RUN_TEST(test_crash_db_persist_and_evict);
    RUN_TEST(test_elfsym_real_app);
    RUN_TEST(test_elfsym_rejects_garbage);
    RUN_TEST(test_abi_resolve_and_stubs);
    return UNITY_END();
}
