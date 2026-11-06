/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Dalvik heap's tracing collector.
 *
 * The interpreter's objects carry no native reference the moment they are born (refs == 0: Java
 * reachability owns them), so a reference count cannot say when one dies. Freeing on the count
 * was what crashed TikTok and Telegram (SIGSEGV 0x400: Java still reached them, commit 8b6e803),
 * and freeing nothing was the other extreme: an app's churn is reclaimed by nobody until the
 * phone's jetsam ends the whole process -- which is how TikTok and exteraGram died. This collector
 * answers reachability directly:
 *
 *   - every allocation is born on the candidate list: it is the registry of the whole Java heap;
 *   - the world stops at the gate: every interpreter instruction and every DVM mutation checks
 *     tl_gc_gate_open and parks while a collection runs, so no allocation or field resize happens
 *     under the mark phase;
 *   - roots are every thread's registers and stack (conservatively: any word that looks like an
 *     object is one), the host binary's writable data segments, the interpreter's precise roots
 *     (class statics, mirrors, dex string caches, interned strings, loaders), each registered
 *     thread's pending exception, every candidate whose native refs are still positive, and each
 *     live object's native state, scanned through malloc_size;
 *   - reachability flows through instance fields and object arrays;
 *   - sweep frees an unreachable candidate with refs == 0 only when it was unreachable on the
 *     previous round too (doom-release): a pointer held only in a register or a transient frame
 *     the scan missed has a whole round to resurface.
 *
 * Objects born during a collection carry that round's epoch and are never swept by it. The
 * heartbeat calls tl_gc_maybe() when the phone reports little memory left; HUSK_GC=0 turns that
 * trigger off.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <malloc/malloc.h>

#include "husk-tl-jni.h"
#include "husk-tl-jni-internal.h"
#include "husk-tl-bionic.h"

static const size_t k_trigger_bytes = 1150ull << 20;    /* collect while there is still room to jetsam */

/* ------------------------------------------------------------ the gate */

_Atomic int tl_gc_gate_open = 1;

static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_parked;

void tl_gc_park_slow(void)
{
    pthread_mutex_lock(&g_gate_mu);
    atomic_fetch_add(&g_parked, 1);
    while (!atomic_load_explicit(&tl_gc_gate_open, memory_order_relaxed)) pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    atomic_fetch_sub(&g_parked, 1);
    pthread_mutex_unlock(&g_gate_mu);
}

/* --------------------------------------------------------------- epochs */

static _Atomic uint32_t g_epoch;
uint32_t tl_gc_epoch(void) { return atomic_load_explicit(&g_epoch, memory_order_relaxed); }

static _Atomic int g_collecting;

/* ------------------------------------------------- registered threads ---
 * One node per thread that ever ran bytecode: the collector roots its pending exception (which can
 * exist nowhere else while the thread sits parked, having just thrown). Nodes are not freed: a
 * thread's slot must stay readable until the snapshot has copied it, and their number is small.
 */
typedef struct gc_thread {
    struct gc_thread *next;
    jobj **pending;                 /* the thread's own pending slot, read on that thread or under g_threads_mu */
} gc_thread;

static pthread_mutex_t g_threads_mu = PTHREAD_MUTEX_INITIALIZER;
static gc_thread *g_threads;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t g_key;

static void thread_gone(void *v)
{
    gc_thread *t = v;
    pthread_mutex_lock(&g_threads_mu);
    for (gc_thread **pp = &g_threads; *pp; pp = &(*pp)->next) if (*pp == t) { *pp = t->next; break; }
    pthread_mutex_unlock(&g_threads_mu);            /* t itself stays: its slot is dead, the node is nothing */
}

static void key_make(void) { pthread_key_create(&g_key, thread_gone); }

void tl_gc_register_thread(void)
{
    static _Thread_local bool done;
    if (done) return;
    done = true;
    gc_thread *t = calloc(1, sizeof(*t));
    if (!t) return;
    t->pending = tl_jni_pending_slot();
    pthread_once(&g_key_once, key_make);
    pthread_setspecific(g_key, t);
    pthread_mutex_lock(&g_threads_mu);
    t->next = g_threads;
    g_threads = t;
    pthread_mutex_unlock(&g_threads_mu);
}

/* ------------------------------------------------------ candidate list ---
 * Every allocation is born here and leaves only when swept (or, without the interpreter, when the
 * legacy free path unregisters it). Doubly linked, so one node can leave in constant time.
 */
static pthread_mutex_t g_cand_mu = PTHREAD_MUTEX_INITIALIZER;
static jobj *g_candidates;
static _Atomic uint64_t g_candidate_adds;

void tl_gc_queue_candidate(jobj *o)
{
    if (__atomic_load_n(&o->gc_queued, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&g_cand_mu);
    if (!__atomic_load_n(&o->gc_queued, __ATOMIC_RELAXED)) {
        o->gc_prev = NULL;
        o->gc_next = g_candidates;
        if (g_candidates) g_candidates->gc_prev = o;
        g_candidates = o;
        __atomic_store_n(&o->gc_queued, 1, __ATOMIC_RELAXED);
        atomic_fetch_add(&g_candidate_adds, 1);
    }
    pthread_mutex_unlock(&g_cand_mu);
}

void tl_gc_unregister_candidate(jobj *o)
{
    pthread_mutex_lock(&g_cand_mu);
    if (__atomic_load_n(&o->gc_queued, __ATOMIC_RELAXED)) {
        if (o->gc_prev) o->gc_prev->gc_next = o->gc_next; else g_candidates = o->gc_next;
        if (o->gc_next) o->gc_next->gc_prev = o->gc_prev;
        o->gc_next = o->gc_prev = NULL;
        __atomic_store_n(&o->gc_queued, 0, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_cand_mu);
}

/* ------------------------------------------------------------- marking */

typedef struct { jobj **v, **registered; size_t n, cap, slots; bool oom; } wl;

static void wl_push(wl *w, jobj *o)
{
    if (w->n == w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 4096;
        jobj **nv = realloc(w->v, nc * sizeof(*nv));
        if (!nv) { w->oom = true; return; }
        w->v = nv;
        w->cap = nc;
    }
    w->v[w->n++] = o;
}

static void mark_obj(wl *w, jobj *o)
{
    if (!o || o->gc_mark) return;
    o->gc_mark = 1;
    wl_push(w, o);
}

/* Conservative roots include native pointers and arbitrary integers. Only an exact
 * address from the guest allocation registry may be dereferenced or marked. */
static size_t object_slot(const wl *w, uintptr_t address)
{
    uint64_t hash = (address >> 3) * UINT64_C(11400714819323198485);
    return (size_t)(hash ^ (hash >> 32)) & (w->slots - 1);
}

static void snapshot_objects(wl *w)
{
    pthread_mutex_lock(&g_cand_mu);
    size_t count = 0;
    for (jobj *o = g_candidates; o; o = o->gc_next) count++;
    w->slots = 16;
    while (w->slots / 2 < count) w->slots *= 2;
    w->registered = calloc(w->slots, sizeof(*w->registered));
    if (!w->registered) w->oom = true;
    else for (jobj *o = g_candidates; o; o = o->gc_next) {
        size_t i = object_slot(w, (uintptr_t)o);
        while (w->registered[i]) i = (i + 1) & (w->slots - 1);
        w->registered[i] = o;
    }
    pthread_mutex_unlock(&g_cand_mu);
}

static void mark_word(wl *w, uintptr_t word)
{
    if (!word || (word & 7) || w->oom) return;
    size_t i = object_slot(w, word);
    while (w->registered[i]) {
        if ((uintptr_t)w->registered[i] == word) {
            mark_obj(w, w->registered[i]);
            return;
        }
        i = (i + 1) & (w->slots - 1);
    }
}

/* A live object's native state, if it has one: structures like an HTTP header map or an ArrayList
 * mirror hold object pointers of their own, two hops down at most (the block, then its malloc'd
 * arrays). malloc_size bounds every read. */
static void scan_native_block(wl *w, void *block, int depth)
{
    size_t n = malloc_size(block);
    if (n < 8) return;
    if (n > (4u << 20)) n = 4u << 20;
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uintptr_t p = ((uintptr_t *)block)[i / 8];
        if (depth > 0 && (p & 7) == 0 && p > 0x100000ull && malloc_zone_from_ptr((void *)p) && malloc_size((void *)p) >= 8)
            scan_native_block(w, (void *)p, depth - 1);
        mark_word(w, p);
    }
}

/* When scanning a registered object: a marked object's own header is
 * trusted (it was validated to get marked), but its arrays are dereferenced only after a zone test,
 * and bounded by what the allocation actually holds -- a garbage length would otherwise walk past
 * the array into whatever follows. */
static void mark_from_roots(wl *w)
{
    while (w->n && !w->oom) {
        jobj *o = w->v[--w->n];
        if (o->kind == TL_K_OBJ_ARRAY) {
            jobj **v = o->oarr.v;
            uint32_t len = o->oarr.len;
            if (v && malloc_zone_from_ptr(v)) {
                size_t cap = malloc_size(v) / sizeof(jobj *);
                if (len > cap) len = (uint32_t)cap;
                for (uint32_t i = 0; i < len; i++) mark_word(w, (uintptr_t)v[i]);
            }
        } else if (o->fields && malloc_zone_from_ptr(o->fields)) {
            jvalue *f = o->fields;
            uint32_t n = o->nfields;
            size_t cap = malloc_size(f) / sizeof(jvalue);
            if (n > cap) n = (uint32_t)cap;
            for (uint32_t i = 0; i < n; i++) mark_word(w, (uintptr_t)f[i].l);
        }
        if (o->native) scan_native_block(w, o->native, 1);
    }
}

/* -------------------------------------------------------------- roots */

static _Atomic uint64_t g_words_scanned;

static void scan_range(wl *w, uintptr_t lo, uintptr_t hi)
{
    if (hi <= lo) return;
    for (uintptr_t p = (lo + 7) & ~(uintptr_t)7; p + 8 <= hi; p += 8) {
        atomic_fetch_add(&g_words_scanned, 1);
        mark_word(w, *(uintptr_t *)p);
    }
}

/* Every thread the process owns: its registers (a running thread keeps live pointers there) and the
 * live part of its stack. Parked threads sit deep inside pthread_cond_wait, so sp and up covers
 * every interpreter frame above them; stale words below sp only cost time. */
static void scan_mach_threads(wl *w)
{
    thread_array_t threads = NULL;
    unsigned n = 0;
    if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS) return;
    for (unsigned i = 0; i < n; i++) {
        pthread_t p = pthread_from_mach_thread_np(threads[i]);
        uintptr_t lo = 0, hi = 0;
        uintptr_t sp = 0;
        if (p) {
            /* Darwin names the stack range differently from Linux: the stack address is the high end, and it grows down */
            hi = (uintptr_t)pthread_get_stackaddr_np(p);
            lo = hi - pthread_get_stacksize_np(p);
#if defined(__aarch64__)
            arm_thread_state64_t st;
            mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
                for (int r = 0; r < 29; r++) mark_word(w, (uintptr_t)st.__x[r]);
                sp = (uintptr_t)arm_thread_state64_get_sp(st);      /* the accessor works whether the SDK's thread state is opaque or not */
            }
#endif
        }
        if (hi > lo) {
            if (sp >= lo && sp < hi) lo = sp & ~(uintptr_t)7;
            scan_range(w, lo, hi);
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, (vm_size_t)n * sizeof(*threads));
}

/* The host binary's writable data: every static cache the C side keeps (handle tables, interned
 * strings, loader globals, pooled rings) is rooted by one sweep over __DATA. */
static void scan_host_data(wl *w)
{
    const struct mach_header_64 *hdr = (const struct mach_header_64 *)_dyld_get_image_header(0);
    if (!hdr) return;
    uintptr_t slide = (uintptr_t)_dyld_get_image_vmaddr_slide(0);
    struct load_command *lc = (struct load_command *)((uintptr_t)hdr + sizeof(*hdr));
    for (uint32_t i = 0; i < hdr->ncmds; i++, lc = (struct load_command *)((uintptr_t)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        struct segment_command_64 *seg = (struct segment_command_64 *)lc;
        if (strncmp(seg->segname, "__DATA", 6)) continue;
        scan_range(w, (uintptr_t)seg->vmaddr + slide, (uintptr_t)seg->vmaddr + slide + seg->vmsize);
    }
}

static void (*g_dvm_roots)(void (*visit)(jobj *));
static wl *g_mark;                                  /* the worklist the dvm roots walker feeds through visit_root */

void tl_gc_set_dvm_roots(void (*walk)(void (*visit)(jobj *))) { g_dvm_roots = walk; }

static void visit_root(jobj *o)                     /* the walker hands raw slot values: validated before marking */
{
    if (g_mark) mark_word(g_mark, (uintptr_t)o);
}

/* ------------------------------------------------------------ the sweep */

static void free_object_internals(jobj *o)
{
    switch (o->kind) {
    case TL_K_STRING: free(o->str.utf8); free(o->str.u16); break;
    case TL_K_PRIM_ARRAY: free(o->arr.data); break;
    case TL_K_OBJ_ARRAY: free(o->oarr.v); break;
    default: break;
    }
    free(o->fields);        /* the monitor stays: a dying object cannot have waiters, and destroying a mutex someone is
                               about to lock is worse than a small leak */
}

/* The registry never loses live objects: sweep unlinks only what it frees. */
static size_t sweep(uint32_t epoch, size_t *kept)
{
    pthread_mutex_lock(&g_cand_mu);
    size_t freed = 0;
    *kept = 0;
    jobj *o = g_candidates;
    while (o) {
        jobj *next = o->gc_next;
        if (o->gc_mark) {                               /* reachable: alive, and the bit is reset for the next round */
            o->gc_mark = 0;
            o->gc_doom = 0;
            (*kept)++;
        } else if (o->refs) {                           /* native-held, unreachable from Java: alive while its refs say so */
            (*kept)++;
        } else if (o->gc_epoch >= epoch) {              /* born this round: too young to judge */
            (*kept)++;
        } else if (!o->gc_doom) {                       /* first miss: doomed, released only if still missing next round */
            o->gc_doom = 1;
            o->gc_epoch = epoch;
            (*kept)++;
        } else {                                        /* missed twice in a row: nobody can reach it any more */
            if (o->gc_prev) o->gc_prev->gc_next = next; else g_candidates = next;
            if (next) next->gc_prev = o->gc_prev;
            o->gc_next = o->gc_prev = NULL;
            __atomic_store_n(&o->gc_queued, 0, __ATOMIC_RELAXED);
            free_object_internals(o);
            free(o);
            freed++;
        }
        o = next;
    }
    pthread_mutex_unlock(&g_cand_mu);
    return freed;
}

/* ------------------------------------------------------------ collection */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void tl_gc_collect_now(const char *why)
{
    int expected = 0;
    if (!tl_dvm || !atomic_compare_exchange_strong(&g_collecting, &expected, 1)) return;

    uint64_t t0 = now_ms();
    atomic_store(&g_words_scanned, 0);
    uint32_t epoch = atomic_fetch_add(&g_epoch, 1) + 1;

    /* close the gate; runnable interpreter threads park within one instruction, blocked ones stay
     * blocked (their stacks are scanned), and every allocation path parks too, so the registry and
     * the object graph are stable for the whole mark phase */
    atomic_store_explicit(&tl_gc_gate_open, 0, memory_order_seq_cst);
    usleep(150 * 1000);

    wl w = {0};
    snapshot_objects(&w);
    g_mark = &w;

    /* each registered thread's pending exception -- read under the lock, so a thread that exits
     * mid-collection cannot invalidate its slot before the snapshot has copied it */
    pthread_mutex_lock(&g_threads_mu);
    jobj *pendings[512];
    size_t npend = 0;
    for (gc_thread *t = g_threads; t && npend < sizeof(pendings) / sizeof(*pendings); t = t->next)
        pendings[npend++] = *t->pending;
    pthread_mutex_unlock(&g_threads_mu);
    for (size_t i = 0; i < npend; i++) mark_word(&w, (uintptr_t)pendings[i]);

    /* native-held objects are roots: an unmarked one is not garbage while its refs say otherwise,
     * and its fields and native state must keep their referents alive */
    for (size_t i = 0; i < w.slots && !w.oom; i++) {
        jobj *o = w.registered[i];
        if (o && __atomic_load_n(&o->refs, __ATOMIC_RELAXED)) mark_obj(&w, o);
    }

    if (!w.oom) scan_mach_threads(&w);
    if (!w.oom) scan_host_data(&w);
    if (!w.oom && g_dvm_roots) g_dvm_roots(visit_root);

    if (!w.oom) mark_from_roots(&w);

    size_t freed = 0, kept = 0;
    if (w.oom) {
        /* An incomplete traversal must leave no marks for the next pass. */
        if (w.registered) for (size_t i = 0; i < w.slots; i++)
            if (w.registered[i]) w.registered[i]->gc_mark = 0;
        tl_log_line("gc: %s: incomplete mark pass, skipping sweep", why);
    }
    else freed = sweep(epoch, &kept);
    free(w.registered);
    free(w.v);
    g_mark = NULL;

    pthread_mutex_lock(&g_gate_mu);
    atomic_store_explicit(&tl_gc_gate_open, 1, memory_order_seq_cst);
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);

    atomic_store(&g_candidate_adds, 0);
    atomic_store(&g_collecting, 0);
    tl_log_line("gc: %s: freed %zu objects, %zu alive, %llu words scanned, %llu ms",
                why, freed, kept, (unsigned long long)atomic_load(&g_words_scanned), (unsigned long long)(now_ms() - t0));
}

/* the heartbeat's trigger: collect when the phone reports little memory left and churn keeps coming */
void tl_gc_maybe(size_t avail_bytes)
{
    static _Atomic bool g_checked;
    static _Atomic bool g_disabled;
    static _Atomic uint64_t g_last_gc_ms;
    if (!atomic_exchange(&g_checked, true)) {
        if (getenv("HUSK_GC") && !strcmp(getenv("HUSK_GC"), "0")) {
            atomic_store(&g_disabled, true);
            tl_log_line("gc: disabled (HUSK_GC=0)");
        }
    }
    if (atomic_load(&g_disabled) || !tl_dvm) return;
    if (!avail_bytes || avail_bytes > k_trigger_bytes) return;
    uint64_t now = now_ms();
    uint64_t last = atomic_load(&g_last_gc_ms);
    if (now - last < 25 * 1000) return;
    if (atomic_load(&g_candidate_adds) < 20000 && now - last < 90 * 1000) return;
    atomic_store(&g_last_gc_ms, now);
    tl_gc_collect_now("memory pressure");
}

void tl_gc_init(void) { }
