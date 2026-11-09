/* SPDX-License-Identifier: Apache-2.0
 * Phase 5 app runtime logic: resource tracker, crash guard, ELF imports,
 * ABI resolution and permission stubs; app isolation decisions.
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
#include "t5_app/isolate.h"
#include "t5_app/tracker.h"
#include "t5_app/fspolicy.h"
#include "t5_pkg/manifest.h"
#include "t5_storage/paths.h"
#include "app_session.h"

/* A small ABI table standing in for the generated one (sorted by name). */
static int fake_open(void) { return 0; }
static void *fake_screen(void) { return (void *)0x1234; }
static const t5_abi_sym_t s_table[] = {
    { "calloc", (const void *)calloc, 0, T5_ABI_RET_PTR, 1 },
    { "free", (const void *)free, 0, T5_ABI_RET_VOID, 1 },
    { "t5_app_screen", (const void *)fake_screen, 0, T5_ABI_RET_PTR, 1 },
    { "t5_port_open", (const void *)fake_open, T5_PERM_PORTS, T5_ABI_RET_ERR, 1 },
};

static char s_dir[64];

void setUp(void)
{
    mock_reset_all();
    t5_abi_set_table(s_table, sizeof s_table / sizeof s_table[0]);
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

static char s_order[64];
static int s_norder;

static void rel_tag(void *h)
{
    s_order[s_norder++] = *(char *)h;
}

static void test_tracker_release_order(void)
{
    static t5_tracker_t t;
    t5_tracker_init(&t);
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
    t5_tracker_init(&t);
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
    t5_tracker_init(&t);
    static char h[T5_TRACK_MAX + 5];
    for (int i = 0; i < T5_TRACK_MAX; i++) TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_OTHER, &h[i], NULL));
    TEST_ASSERT_FALSE(t5_tracker_add(&t, T5_RES_OTHER, &h[T5_TRACK_MAX], NULL));
    TEST_ASSERT_EQUAL(1, t.dropped);
    TEST_ASSERT_TRUE(t5_tracker_remove(&t, &h[10]));
    TEST_ASSERT_FALSE(t5_tracker_remove(&t, &h[10]));
    TEST_ASSERT_TRUE(t5_tracker_add(&t, T5_RES_OTHER, &h[T5_TRACK_MAX], NULL));
}

/* Freed blocks are kept until the test ends, so it can check what a free
 * leaves behind without reading released memory. */
static int s_allocs, s_frees;
static void *s_freed[64];
static void *count_malloc(t5_tracker_t *t, size_t n) { (void)t; s_allocs++; return malloc(n); }
static void count_free(t5_tracker_t *t, void *p)
{
    (void)t;
    if (s_frees < (int)(sizeof s_freed / sizeof s_freed[0])) s_freed[s_frees] = p;
    else free(p);
    s_frees++;
}
static void release_freed(void)
{
    int n = s_frees < (int)(sizeof s_freed / sizeof s_freed[0]) ? s_frees : (int)(sizeof s_freed / sizeof s_freed[0]);
    for (int i = 0; i < n; i++) free(s_freed[i]);
}

static void test_tracker_heap(void)
{
    t5_tracker_alloc_t a = { count_malloc, count_free, NULL };
    t5_tracker_set_allocator(&a);
    s_allocs = s_frees = 0;
    static t5_tracker_t t, other;
    t5_tracker_init(&t);
    t5_tracker_init(&other);
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
    release_freed();
}

static bool even(void *h)
{
    return (*(char *)h & 1) == 0;
}

static void test_tracker_list_and_prune(void)
{
    static t5_tracker_t t;
    t5_tracker_init(&t);
    static char h[6] = { 0, 1, 2, 3, 4, 5 };
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_TRUE(t5_tracker_add(&t, i < 4 ? T5_RES_ANIM : T5_RES_TASK, &h[i], rel_tag));
    }
    void *out[2];
    TEST_ASSERT_EQUAL(4, t5_tracker_list(&t, T5_RES_ANIM, out, 2));     /* how many there are... */
    TEST_ASSERT_EQUAL_PTR(&h[0], out[0]);                                /* ...oldest first */
    TEST_ASSERT_EQUAL_PTR(&h[1], out[1]);
    TEST_ASSERT_TRUE(t5_tracker_holds(&t, T5_RES_TASK, &h[4]));
    TEST_ASSERT_FALSE(t5_tracker_holds(&t, T5_RES_ANIM, &h[4]));
    /* Pruning forgets the dead ones without releasing them. */
    s_norder = 0;
    t5_tracker_prune(&t, T5_RES_ANIM, even);
    TEST_ASSERT_EQUAL(0, s_norder);
    TEST_ASSERT_EQUAL(2, t5_tracker_count(&t, T5_RES_ANIM));
    TEST_ASSERT_FALSE(t5_tracker_has(&t, &h[1]));
    TEST_ASSERT_TRUE(t5_tracker_has(&t, &h[5]));                        /* other kinds untouched */
}

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
    t5_sys_dir_set(s_dir);
    TEST_ASSERT_EQUAL(T5_OK, t5_sys_path(path, sizeof path, T5_CRASH_DB_NAME));
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
    t5_abi_set_table(NULL, 0);
    TEST_ASSERT_NULL(t5_abi_find("free"));
}

#define TXT_LO 0x48100000u
#define TXT_HI 0x48104000u

static t5_iso_fault_t sys_fault(uint32_t pc, uint32_t ra)
{
    return (t5_iso_fault_t){ .text_lo = TXT_LO, .text_hi = TXT_HI, .has_point = true, .point_ok = true,
                             .unwind_safe = true, .pc = pc, .ra = ra };
}

static void test_iso_blame_pc_and_ra(void)
{
    t5_iso_fault_t f = sys_fault(TXT_LO + 0x10, 0x40000000u);
    TEST_ASSERT_EQUAL(T5_ISO_UNWIND, t5_iso_blame(&f));             /* store through NULL in app code */
    f = sys_fault(0x4fc00000u, TXT_LO + 0x200);
    TEST_ASSERT_EQUAL(T5_ISO_UNWIND, t5_iso_blame(&f));             /* strlen(NULL) called by the app */
    f = sys_fault(TXT_HI, 0x40000000u);
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));               /* one past the end isn't the app */
    f = sys_fault(0x40001000u, 0x40002000u);
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));               /* OS fault, app not on the stack */
}

static void test_iso_blame_stack_scan(void)
{
    uint32_t stack[6] = { 0, 0x3ff00000u, 0x40001234u, 0, 0, 0 };
    t5_iso_fault_t f = sys_fault(0x40001000u, 0x40002000u);
    f.stack = stack;
    f.words = 6;
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));
    stack[4] = TXT_LO + 0x88;                                       /* app frame further up */
    TEST_ASSERT_EQUAL(T5_ISO_UNWIND, t5_iso_blame(&f));
    f.words = 4;                                                    /* above the recovery point: not ours */
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));
}

static void test_iso_blame_needs_point_and_safety(void)
{
    t5_iso_fault_t f = sys_fault(TXT_LO + 4, 0);
    f.has_point = false;
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));               /* system task, nowhere to go back to */
    f = sys_fault(TXT_LO + 4, 0);
    f.point_ok = false;
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));               /* point belongs to another app */
    f = sys_fault(TXT_LO + 4, 0);
    f.unwind_safe = false;
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));               /* UI task mid-render */
    f = sys_fault(TXT_LO + 4, 0);
    f.text_lo = f.text_hi = 0;
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));               /* built-in app: OS code */
}

static void test_iso_blame_guess(void)
{
    t5_iso_fault_t f = sys_fault(0x40001000u, 0x40002000u);
    f.working_for_app = f.data_fault = f.guess_ok = true;
    TEST_ASSERT_EQUAL(T5_ISO_UNWIND_GUESS, t5_iso_blame(&f));       /* tail call into LVGL with a bad object */
    f.guess_ok = false;
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));               /* a second one soon: OS bug, panic */
    f.guess_ok = true;
    f.data_fault = false;
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));               /* wild jump: not guessed */
    f.data_fault = true;
    f.working_for_app = false;
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));               /* system UI between apps */
    f.working_for_app = true;
    f.unwind_safe = false;
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));               /* mid-render */
}

static void test_iso_blame_apps_task(void)
{
    t5_iso_fault_t f = { .text_lo = TXT_LO, .text_hi = TXT_HI, .apps_task = true, .pc = 0x40001000u };
    TEST_ASSERT_EQUAL(T5_ISO_END_TASK, t5_iso_blame(&f));          /* anywhere, even in the OS */
    f.text_lo = f.text_hi = 0;
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(&f));
    TEST_ASSERT_EQUAL(T5_ISO_NONE, t5_iso_blame(NULL));
}

static t5_fs_verdict_t fs(const char *p, bool w, uint32_t *need)
{
    static t5_fs_pick_t picks[1] = { { "/sd/Other/song.mp3", false } };
    t5_fs_scope_t s = { "/data/appdata/com.a", "/sd/apps/com.a", picks, 1 };
    return t5_fs_check(&s, p, w, need);
}

static void test_fs_scope(void)
{
    uint32_t n = 0;
    TEST_ASSERT_EQUAL(T5_FS_ALLOW, fs("/data/appdata/com.a/x", true, &n));
    TEST_ASSERT_EQUAL(T5_FS_ALLOW, fs("/DATA/AppData/com.a/x", true, &n));         /* FAT ignores case */
    TEST_ASSERT_EQUAL(T5_FS_ALLOW, fs("/sd/apps/com.a/app.elf", false, &n));
    TEST_ASSERT_EQUAL(T5_FS_DENY, fs("/sd/apps/com.a/app.elf", true, &n));         /* install dir is read only */
    TEST_ASSERT_EQUAL(T5_FS_DENY, fs("/data/appdata/com.ab/x", false, &n));        /* a neighbour, same prefix */
    TEST_ASSERT_EQUAL(T5_FS_DENY, fs("/data/appdata/com.b/x", false, &n));
    TEST_ASSERT_EQUAL(T5_FS_DENY, fs(T5_SYS_DIR "/" T5_CRASH_DB_NAME, false, &n));
    TEST_ASSERT_EQUAL(T5_FS_DENY, fs("/SD/APPS/com.b/x", false, &n));
    TEST_ASSERT_EQUAL(T5_FS_DENY, fs("/data/other", false, &n));
    TEST_ASSERT_EQUAL(T5_FS_ALLOW, fs("/sd/Other/song.mp3", false, &n));           /* picked */
    TEST_ASSERT_EQUAL(T5_FS_NEEDS_PERM, fs("/sd/Other/song.mp3", true, &n));       /* picked read only */
    TEST_ASSERT_EQUAL(T5_PERM_ALL_FILES, n);
    TEST_ASSERT_EQUAL(T5_FS_NEEDS_PERM, fs("/sd/Music/a.mp3", false, &n));
    TEST_ASSERT_EQUAL(T5_PERM_SHARED_READ | T5_PERM_SHARED_WRITE | T5_PERM_ALL_FILES, n);
    TEST_ASSERT_EQUAL(T5_FS_NEEDS_PERM, fs("/sd/Music/a.mp3", true, &n));
    TEST_ASSERT_EQUAL(T5_PERM_SHARED_WRITE | T5_PERM_ALL_FILES, n);
    TEST_ASSERT_EQUAL(T5_FS_NEEDS_PERM, fs("/sd/random", false, &n));
    TEST_ASSERT_EQUAL(T5_PERM_ALL_FILES, n);
}

static void test_fs_normalise(void)
{
    char o[64];
    TEST_ASSERT_TRUE(t5_fs_normalise("//sd//./a/b/", o, sizeof o));
    TEST_ASSERT_EQUAL_STRING("/sd/a/b", o);
    TEST_ASSERT_FALSE(t5_fs_normalise("sd/a", o, sizeof o));                       /* relative */
    TEST_ASSERT_FALSE(t5_fs_normalise("/sd/a/../../data/sys", o, sizeof o));
    TEST_ASSERT_FALSE(t5_fs_normalise("/sd/apps./x", o, sizeof o));                /* FAT drops the dot */
    TEST_ASSERT_FALSE(t5_fs_normalise("/sd/apps /x", o, sizeof o));
    TEST_ASSERT_FALSE(t5_fs_normalise("/sd/APPDAT~1/x", o, sizeof o));             /* 8.3 alias */
    char lng[100];
    memset(lng, 'a', sizeof lng - 1);
    lng[0] = '/';
    lng[99] = 0;
    TEST_ASSERT_FALSE(t5_fs_normalise(lng, o, sizeof o));
    TEST_ASSERT_TRUE(t5_fs_quota_ok(T5_FS_QUOTA_BYTES - 10, 10));
    TEST_ASSERT_FALSE(t5_fs_quota_ok(T5_FS_QUOTA_BYTES - 10, 11));
    TEST_ASSERT_TRUE(t5_fs_ext_matches("a.JPG", "png,jpg"));
    TEST_ASSERT_FALSE(t5_fs_ext_matches("a.jpeg", "png,jpg"));
    TEST_ASSERT_FALSE(t5_fs_ext_matches("noext", "png"));
    TEST_ASSERT_TRUE(t5_fs_ext_matches("noext", NULL));
}

static void test_iso_heap_room(void)
{
    const size_t MB = 1024 * 1024;
    TEST_ASSERT_TRUE(t5_iso_heap_room(MB, T5_ISO_PSRAM_RESERVE + MB));
    TEST_ASSERT_FALSE(t5_iso_heap_room(MB + 1, T5_ISO_PSRAM_RESERVE + MB));
    TEST_ASSERT_FALSE(t5_iso_heap_room(1024, MB));                  /* under the reserve */
    TEST_ASSERT_FALSE(t5_iso_heap_room((size_t)-1, 20 * MB));       /* no wrap */
    TEST_ASSERT_EQUAL(T5_ISO_ARENA_FIRST, t5_iso_arena_next(0));
    TEST_ASSERT_EQUAL(T5_ISO_ARENA_FIRST * 2, t5_iso_arena_next(T5_ISO_ARENA_FIRST));
    TEST_ASSERT_EQUAL(T5_ISO_ARENA_MAX, t5_iso_arena_next(T5_ISO_ARENA_MAX));
}

static void test_iso_l2_reserve(void)
{
    t5_iso_l2_t s = {0};
    TEST_ASSERT_EQUAL(T5_ISO_L2_NONE, t5_iso_l2_check(&s, 80 * 1024, 0));
    TEST_ASSERT_EQUAL(T5_ISO_L2_RELEASE, t5_iso_l2_check(&s, 10 * 1024, 100));          /* reserve goes first */
    /* The reserve is back in the heap: free memory counts without it. */
    TEST_ASSERT_EQUAL(T5_ISO_L2_NONE, t5_iso_l2_check(&s, 10 * 1024 + T5_ISO_L2_RESERVE, 600));
    TEST_ASSERT_EQUAL(T5_ISO_L2_CLOSE_APP, t5_iso_l2_check(&s, 10 * 1024 + T5_ISO_L2_RESERVE, 100 + T5_ISO_LOW_FOR_MS));
    TEST_ASSERT_EQUAL(T5_ISO_L2_RETAKE, t5_iso_l2_check(&s, T5_ISO_L2_PLENTY, 5000));
    TEST_ASSERT_EQUAL(T5_ISO_L2_NONE, t5_iso_l2_check(&s, T5_ISO_L2_PLENTY, 5100));
    /* Recovered in time: no close. */
    TEST_ASSERT_EQUAL(T5_ISO_L2_RELEASE, t5_iso_l2_check(&s, 10 * 1024, 6000));
    TEST_ASSERT_EQUAL(T5_ISO_L2_NONE, t5_iso_l2_check(&s, 40 * 1024, 6500));
    TEST_ASSERT_EQUAL(T5_ISO_L2_NONE, t5_iso_l2_check(&s, 40 * 1024, 9000));
}

static bool s_dropped;
static void *s_only_block;
static bool drop_everything(t5_tracker_t *t) { (void)t; s_dropped = true; return true; }
static void *keep_malloc(t5_tracker_t *t, size_t n) { (void)t; return s_only_block = malloc(n); }

/* The blocks are gone without their headers being read: the app may have
 * overwritten them. */
static void test_tracker_drop_all(void)
{
    static t5_tracker_t t;
    t5_tracker_init(&t);
    t5_tracker_alloc_t a = { keep_malloc, count_free, drop_everything };
    t5_tracker_set_allocator(&a);
    s_dropped = false;
    s_frees = 0;
    uint8_t *p = t5_tracker_malloc(&t, 64);
    TEST_ASSERT_NOT_NULL(p);
    t5_tracker_free_all(&t);
    TEST_ASSERT_TRUE(s_dropped);
    TEST_ASSERT_EQUAL(0, s_frees);
    TEST_ASSERT_EQUAL(0, t.heap_bytes);
    TEST_ASSERT_NULL(t.blocks);
    t5_tracker_set_allocator(NULL);
    free(s_only_block);
}

static void test_iso_mem_check(void)
{
    t5_iso_mem_t m = { 0 };
    const size_t low = T5_ISO_PSRAM_CRITICAL - 1;
    TEST_ASSERT_FALSE(t5_iso_mem_check(&m, 8u << 20, 0));
    TEST_ASSERT_FALSE(t5_iso_mem_check(&m, low, 1000));
    TEST_ASSERT_FALSE(t5_iso_mem_check(&m, low, 1000 + T5_ISO_LOW_FOR_MS - 1));
    TEST_ASSERT_TRUE(t5_iso_mem_check(&m, low, 1000 + T5_ISO_LOW_FOR_MS));
    TEST_ASSERT_FALSE(t5_iso_mem_check(&m, low, 1000 + T5_ISO_LOW_FOR_MS + 10));    /* starts over */
    TEST_ASSERT_FALSE(t5_iso_mem_check(&m, 8u << 20, 5000));                         /* a blip resets */
    TEST_ASSERT_FALSE(t5_iso_mem_check(&m, low, 5100));
    TEST_ASSERT_FALSE(t5_iso_mem_check(&m, low, 5100 + T5_ISO_LOW_FOR_MS - 1));
}

static void test_iso_hog(void)
{
    uint8_t st = 0;
    TEST_ASSERT_FALSE(t5_iso_hog(&st, 950000, 1000000));
    TEST_ASSERT_FALSE(t5_iso_hog(&st, 950000, 1000000));
    TEST_ASSERT_FALSE(t5_iso_hog(&st, 500000, 1000000));            /* breathed: start over */
    TEST_ASSERT_FALSE(t5_iso_hog(&st, 1000000, 1000000));
    TEST_ASSERT_FALSE(t5_iso_hog(&st, 1000000, 1000000));
    TEST_ASSERT_TRUE(t5_iso_hog(&st, 1000000, 1000000));
    TEST_ASSERT_FALSE(t5_iso_hog(&st, 0, 0));
}

/* The app in front comes back after a power-off; a torn file brings back nothing. */
static void test_session_survives_power_off(void)
{
    t5_sys_dir_set(s_dir);
    app_session_load();
    TEST_ASSERT_EQUAL_STRING("", app_session_restored());
    app_session_set("com.example.notes");
    TEST_ASSERT_EQUAL(T5_OK, app_session_save(NULL));
    app_session_load();
    TEST_ASSERT_EQUAL_STRING("com.example.notes", app_session_restored());

    app_session_set(NULL);            /* back on the launcher */
    TEST_ASSERT_EQUAL(T5_OK, app_session_save(NULL));
    app_session_load();
    TEST_ASSERT_EQUAL_STRING("", app_session_restored());

    app_session_set("com.example.notes");
    TEST_ASSERT_EQUAL(T5_OK, app_session_save(NULL));
    char path[96];
    snprintf(path, sizeof path, "%s/session.bin", s_dir);
    FILE *f = fopen(path, "r+b");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, -3, SEEK_END);
    fputc('X', f);
    fclose(f);
    app_session_load();
    TEST_ASSERT_EQUAL_STRING("", app_session_restored());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tracker_release_order);
    RUN_TEST(test_tracker_reentrant_release);
    RUN_TEST(test_tracker_table_full);
    RUN_TEST(test_tracker_heap);
    RUN_TEST(test_tracker_list_and_prune);
    RUN_TEST(test_crash_classify);
    RUN_TEST(test_crash_block_after_two);
    RUN_TEST(test_crash_db_persist_and_evict);
    RUN_TEST(test_elfsym_real_app);
    RUN_TEST(test_elfsym_rejects_garbage);
    RUN_TEST(test_abi_resolve_and_stubs);
    RUN_TEST(test_iso_blame_pc_and_ra);
    RUN_TEST(test_iso_blame_stack_scan);
    RUN_TEST(test_iso_blame_needs_point_and_safety);
    RUN_TEST(test_iso_blame_guess);
    RUN_TEST(test_iso_blame_apps_task);
    RUN_TEST(test_iso_heap_room);
    RUN_TEST(test_fs_scope);
    RUN_TEST(test_fs_normalise);
    RUN_TEST(test_iso_l2_reserve);
    RUN_TEST(test_tracker_drop_all);
    RUN_TEST(test_iso_mem_check);
    RUN_TEST(test_iso_hog);
    RUN_TEST(test_session_survives_power_off);
    return UNITY_END();
}
