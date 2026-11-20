/*
 * nx_prof.c - sampling profiler for Horizon (real Switch and emulators).
 *
 * Linux's SIGPROF profiler (runtime/prof.c) has no equivalent on Horizon, but
 * a process may pause its own threads and read their registers:
 * svcSetThreadActivity(Paused) + svcGetThreadContext3 (both listed in
 * main.npdm.json). A sampler thread on core 2 stops each tracked thread for a
 * moment every PROF_RATE microseconds and counts its PC.
 *
 * config.ini:  PROF=<seconds after the first frame to start>  (0 = off)
 *              PROF_LEN=<seconds to sample>                    (default 20)
 * Writes sd:/switch/mafia/prof.txt; tools/nx_prof_report.py turns it into a
 * function list with build/switch/familia.elf.
 */
#include <switch.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int familia_main(int argc, char **argv);
Result __real_threadCreate(Thread *, ThreadFunc, void *, void *, size_t, int, int);

#define MAX_THREADS 16
#define HBITS 16
typedef struct { u64 pc; u32 n; } slot;
typedef struct { Handle h; char name[16]; slot *tab; u32 total, lost; } tracked;

static tracked g_thr[MAX_THREADS];
static _Atomic int g_nthr;
static Thread g_sampler;
static volatile int g_prof_go;          /* set by the first frame */

void nx_prof_track(Handle h, const char *name) {
    int i = atomic_fetch_add(&g_nthr, 1);
    if (i >= MAX_THREADS) return;
    g_thr[i].h = h;
    snprintf(g_thr[i].name, sizeof g_thr[i].name, "%s", name);
}

static void count(tracked *t, u64 pc) {
    if (!t->tab) return;
    u32 i = (u32)((pc >> 2) * 0x9E3779B1u) >> (32 - HBITS);
    for (u32 k = 0; k < (1u << HBITS); k++, i = (i + 1) & ((1u << HBITS) - 1)) {
        if (t->tab[i].pc == pc) { t->tab[i].n++; t->total++; return; }
        if (!t->tab[i].pc) { t->tab[i].pc = pc; t->tab[i].n = 1; t->total++; return; }
    }
    t->lost++;
}

static void sampler(void *arg) {
    (void)arg;
    const char *e = getenv("FAMILIA_PROF");
    double start = e ? atof(e) : 0, len = getenv("FAMILIA_PROF_LEN") ? atof(getenv("FAMILIA_PROF_LEN")) : 20;
    u64 rate_us = getenv("FAMILIA_PROF_RATE") ? strtoull(getenv("FAMILIA_PROF_RATE"), NULL, 10) : 1000;
    while (!g_prof_go) svcSleepThread(100000000ull);
    svcSleepThread((u64)(start * 1e9));
    int n = atomic_load(&g_nthr); if (n > MAX_THREADS) n = MAX_THREADS;
    for (int i = 0; i < n; i++) g_thr[i].tab = calloc(1u << HBITS, sizeof(slot));
    fprintf(stderr, "[prof] sampling %d threads for %.0f s every %llu us\n", n, len, (unsigned long long)rate_us);
    u64 t_end = armGetSystemTick() + armNsToTicks((u64)(len * 1e9));
    u32 fails = 0;
    while (armGetSystemTick() < t_end) {
        for (int i = 0; i < n; i++) {
            tracked *t = &g_thr[i];
            if (R_FAILED(svcSetThreadActivity(t->h, ThreadActivity_Paused))) { fails++; continue; }
            ThreadContext ctx;
            Result rc = svcGetThreadContext3(&ctx, t->h);
            svcSetThreadActivity(t->h, ThreadActivity_Runnable);
            if (R_SUCCEEDED(rc)) count(t, ctx.pc.x); else fails++;
        }
        svcSleepThread(rate_us * 1000ull);
    }
    FILE *f = fopen("/switch/mafia/prof.txt", "w");
    if (f) {
        /* the anchor gives the load address: offline, nm's value of
         * familia_main minus this is the slide */
        fprintf(f, "anchor familia_main %llx\n", (unsigned long long)(uintptr_t)familia_main);
        for (int i = 0; i < n; i++) {
            tracked *t = &g_thr[i];
            fprintf(f, "thread %d %s total %u lost %u\n", i, t->name, t->total, t->lost);
            if (t->tab) for (u32 k = 0; k < (1u << HBITS); k++)
                if (t->tab[k].pc) fprintf(f, "s %d %llx %u\n", i, (unsigned long long)t->tab[k].pc, t->tab[k].n);
        }
        fclose(f);
    }
    fprintf(stderr, "[prof] done (%u failed samples): /switch/mafia/prof.txt\n", fails);
}

void nx_prof_frame(void) { g_prof_go = 1; }

void nx_prof_init(void) {
    const char *e = getenv("FAMILIA_PROF");
    if (!e || atof(e) <= 0) return;
    /* the main thread runs the game; its handle is a real one, not the pseudo-handle */
    nx_prof_track(envGetMainThreadHandle(), "game");
    if (R_SUCCEEDED(__real_threadCreate(&g_sampler, sampler, NULL, NULL, 0x10000, 0x20, 2)))
        threadStart(&g_sampler);
}
