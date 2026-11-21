/*
 * par.c - fork-join helper: a data-parallel loop split between the calling
 * (game) thread and one worker thread.
 *
 * The game is single-threaded; this is only for loops whose iterations are
 * independent and write disjoint guest memory (vertex lighting, vertex
 * skinning). The range is cut into chunks taken from one atomic counter by
 * both threads, so a worker that is slow to wake costs nothing: the caller
 * simply takes every chunk itself. The caller returns only when every chunk
 * is done, so to the game the call is as synchronous as before.
 *
 * Job protocol: g_seq is odd while a job is open. The worker counts itself
 * in g_active, then checks the job is still the one it saw open; the caller
 * closes the job (seq even) and waits for g_active == 0 before it returns,
 * so the fields are never rewritten under a worker.
 *
 * On the Switch the worker runs on core 2 at low priority (sound threads
 * there pre-empt it). FAMILIA_PAR=0 turns it off.
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "par.h"

int fam_config_int(const char *key, int def);

static fam_par_fn g_fn;
static void *g_ctx;
static uint32_t g_n, g_chunk;
static _Atomic uint32_t g_next;
static _Atomic uint32_t g_seq;
static _Atomic int g_active;
static _Atomic int g_sleeping;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_on = -1;
uint64_t fam_par_jobs, fam_par_worker_chunks, fam_par_chunks;

static void run_chunks(void) {
    uint32_t b;
    while ((b = atomic_fetch_add_explicit(&g_next, g_chunk, memory_order_relaxed)) < g_n) {
        uint32_t e = b + g_chunk < g_n ? b + g_chunk : g_n;
        g_fn(g_ctx, b, e);
        fam_par_worker_chunks++;
    }
}

static void *worker(void *arg) {
    (void)arg;
#ifdef __SWITCH__
    void nx_set_self_priority(int prio);
    nx_set_self_priority(0x3B);          /* below the sound threads (0x2C): they pre-empt it */
#endif
    for (;;) {
        /* spin a while after each job: lighting/skinning jobs come in bursts
         * (one per object) during the render pass */
        int idle = 0;
        uint32_t s;
        while (!((s = atomic_load_explicit(&g_seq, memory_order_acquire)) & 1)) {
            if (++idle < 4000) { sched_yield(); continue; }
            pthread_mutex_lock(&g_mx);
            atomic_store_explicit(&g_sleeping, 1, memory_order_seq_cst);
            if (!(atomic_load_explicit(&g_seq, memory_order_seq_cst) & 1)) {
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 2000000;
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&g_cv, &g_mx, &ts);
            }
            atomic_store_explicit(&g_sleeping, 0, memory_order_relaxed);
            pthread_mutex_unlock(&g_mx);
            idle = 0;
        }
        atomic_fetch_add_explicit(&g_active, 1, memory_order_seq_cst);
        if (atomic_load_explicit(&g_seq, memory_order_seq_cst) == s) run_chunks();
        atomic_fetch_sub_explicit(&g_active, 1, memory_order_release);
        /* wait for this job to close before looking for the next one */
        while (atomic_load_explicit(&g_seq, memory_order_acquire) == s) sched_yield();
    }
    return NULL;
}

static void start(void) {
    g_on = fam_config_int("par", 1);
    if (!g_on) return;
    pthread_t t;
#ifdef __SWITCH__
    void nx_next_thread_core(int core);
    nx_next_thread_core(2);
#endif
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64 << 10);
    if (pthread_create(&t, &at, worker, NULL)) { g_on = 0; return; }
    fprintf(stderr, "[par] worker thread for data-parallel loops\n");
}

void fam_par_for(fam_par_fn fn, void *ctx, uint32_t n, uint32_t chunk) {
    if (g_on < 0) start();
    if (!g_on || n < 2 * chunk) { fn(ctx, 0, n); return; }
    g_fn = fn; g_ctx = ctx; g_n = n; g_chunk = chunk;
    atomic_store_explicit(&g_next, 0, memory_order_relaxed);
    uint32_t s = atomic_load_explicit(&g_seq, memory_order_relaxed);
    atomic_store_explicit(&g_seq, s + 1, memory_order_seq_cst);          /* open */
    if (atomic_load_explicit(&g_sleeping, memory_order_seq_cst)) {
        pthread_mutex_lock(&g_mx); pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mx);
    }
    uint32_t b;
    while ((b = atomic_fetch_add_explicit(&g_next, chunk, memory_order_relaxed)) < n) {
        fn(ctx, b, b + chunk < n ? b + chunk : n);
        fam_par_chunks++;
    }
    atomic_store_explicit(&g_seq, s + 2, memory_order_seq_cst);          /* close */
    while (atomic_load_explicit(&g_active, memory_order_acquire)) { /* a chunk in flight */ }
    atomic_thread_fence(memory_order_acquire);                           /* its writes are visible */
    fam_par_jobs++;
}
