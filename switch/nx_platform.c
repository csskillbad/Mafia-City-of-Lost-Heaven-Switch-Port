/*
 * nx_platform.c - Nintendo Switch (libnx) entry point and the few Linux
 * facilities The Familia's runtime expects. The game itself is unchanged:
 * familia_main() is runtime/main.c's main(), renamed at compile time.
 *
 * Layout on the SD card:
 *   sdmc:/switch/mafia/data/      the user's Mafia 1.2 files (Game.exe, *.dta, ...)
 *   sdmc:/switch/mafia/userdata/  saves, registry, config (created on first run)
 *   sdmc:/switch/mafia/log.txt    stderr of the last run
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <switch.h>

#define NX_ROOT "/switch/mafia"

/* Host heap: libnx would otherwise take the whole application pool and
 * leave nothing for svcMapPhysicalMemory, which backs the guest's memory. */
u64 __nx_heap_size = 0x60000000ull;          /* 1.5 GiB host heap */

int familia_main(int argc, char **argv);

long nx_gettid(void) {
    u64 id = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return (long)id;
}

/* Host-side mmap users left after the guest memory backend: anonymous
 * memory only, from the heap. File mappings are not supported here. */
#include <sys/mman.h>
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)addr; (void)prot; (void)off;
    if (!(flags & MAP_ANONYMOUS) || fd >= 0) { errno = ENODEV; return MAP_FAILED; }
    void *p = aligned_alloc(0x1000, (len + 0xFFF) & ~(size_t)0xFFF);
    if (!p) { errno = ENOMEM; return MAP_FAILED; }
    memset(p, 0, len);
    return p;
}
int munmap(void *addr, size_t len) { (void)len; free(addr); return 0; }
int mprotect(void *addr, size_t len, int prot) { (void)addr; (void)len; (void)prot; return 0; }
int madvise(void *addr, size_t len, int advice) { (void)addr; (void)len; (void)advice; return 0; }
int msync(void *addr, size_t len, int flags) { (void)addr; (void)len; (void)flags; return 0; }

/* ---- thread placement -------------------------------------------------
 * libnx creates every thread on the process's default core (core 0) unless
 * told otherwise: the game thread, the GL replay thread, the guest's sound /
 * streaming threads and SDL's audio thread would all share one core. Every
 * thread (pthreads and SDL's go through threadCreate) is placed here:
 *   core 0  the game (main thread)
 *   core 1  the GL replay thread (driver work), requested by gl_thread.c
 *   core 2  everything else: sound mixing, streaming, SDL audio
 * FAMILIA_PIN=0 keeps libnx's default placement. */
#include <stdatomic.h>
static atomic_int g_next_core = -1;
static int g_pin = -1;
void nx_next_thread_core(int core) { atomic_store(&g_next_core, core); }

Result __real_threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz, int prio, int cpuid);
Result __wrap_threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz, int prio, int cpuid) {
    if (g_pin < 0) { const char *e = getenv("FAMILIA_PIN"); g_pin = e && *e ? atoi(e) : 1; }
    if (g_pin && cpuid == -2) {
        int want = atomic_exchange(&g_next_core, -1);
        cpuid = want >= 0 ? want : 2;
    }
    Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
    if (R_SUCCEEDED(rc) && getenv("FAMILIA_PROF")) {
        void nx_prof_track(Handle, const char *);
        char nm[16]; snprintf(nm, sizeof nm, "core%d", cpuid);
        nx_prof_track(t->handle, nm);
    }
    return rc;
}

void nx_set_self_priority(int prio) { svcSetThreadPriority(CUR_THREAD_HANDLE, prio); }

/* Linux-only diagnostics (signal crash handler, SIGPROF profiler). */
void fam_crash_install(void) {}
void fam_prof_start(void) {}

/* ---- CPU clock (CPU_CLOCK=MHz) ----
 * The game thread is what limits the frame rate (switch/nx_prof.c profile:
 * the GL thread idles ~80%), so its clock is the frame rate. Set through
 * clkrst (8.0.0+) or pcv (older), the services sys-clk uses. The system puts
 * its own profile back on dock/undock, sleep and focus changes, so a small
 * thread re-applies the rate every 2 s if it changed. */
static volatile int g_clk_run;
static u32 g_clk_hz;
static Thread g_clk_thr;
static int clk_open(ClkrstSession *s) {
    return hosversionAtLeast(8, 0, 0) ? R_SUCCEEDED(clkrstOpenSession(s, PcvModuleId_CpuBus, 3)) : 1;
}
static u32 clk_get(ClkrstSession *s) {
    u32 hz = 0;
    if (hosversionAtLeast(8, 0, 0)) clkrstGetClockRate(s, &hz); else pcvGetClockRate(PcvModule_CpuBus, &hz);
    return hz;
}
static Result clk_set(ClkrstSession *s, u32 hz) {
    return hosversionAtLeast(8, 0, 0) ? clkrstSetClockRate(s, hz) : pcvSetClockRate(PcvModule_CpuBus, hz);
}
static void clk_thread(void *arg) {
    (void)arg;
    ClkrstSession s;
    if (!clk_open(&s)) return;
    while (g_clk_run) {
        if (clk_get(&s) != g_clk_hz) clk_set(&s, g_clk_hz);
        for (int i = 0; i < 20 && g_clk_run; i++) svcSleepThread(100000000ull);
    }
    clk_set(&s, 1020000000u);        /* the normal application clock */
    if (hosversionAtLeast(8, 0, 0)) clkrstCloseSession(&s);
}
static void nx_clock_start(int mhz) {
    if (mhz <= 0) return;
    if (mhz > 1785) mhz = 1785;      /* the highest official clock; beyond is overclocking */
    Result rc = hosversionAtLeast(8, 0, 0) ? clkrstInitialize() : pcvInitialize();
    if (R_FAILED(rc)) { fprintf(stderr, "[nx] CPU clock: service unavailable (%x)\n", rc); return; }
    g_clk_hz = (u32)mhz * 1000000u;
    ClkrstSession s;
    if (clk_open(&s)) {
        rc = clk_set(&s, g_clk_hz);
        fprintf(stderr, "[nx] CPU clock %d MHz: %s (now %u MHz)\n", mhz, R_SUCCEEDED(rc) ? "set" : "refused", clk_get(&s) / 1000000u);
        if (hosversionAtLeast(8, 0, 0)) clkrstCloseSession(&s);
    }
    g_clk_run = 1;
    if (R_SUCCEEDED(__real_threadCreate(&g_clk_thr, clk_thread, NULL, NULL, 0x4000, 0x3B, 2))) threadStart(&g_clk_thr);
}
static void nx_clock_stop(void) {
    if (!g_clk_run) return;
    g_clk_run = 0;
    threadWaitForExit(&g_clk_thr); threadClose(&g_clk_thr);
    if (hosversionAtLeast(8, 0, 0)) clkrstExit(); else pcvExit();
}

static void nx_setenv_default(const char *k, const char *v) {
    if (!getenv(k)) setenv(k, v, 1);
}

/* sd:/switch/mafia/config.ini: KEY=VALUE lines become FAMILIA_<KEY> (what
 * the PortMaster launcher exports from familia.cfg). Written with the Switch
 * defaults on first run. */
static const char k_default_config[] =
    "# Mafia (The Familia) for Nintendo Switch - settings, read at start.\n"
    "# Lines are KEY=VALUE; each becomes FAMILIA_<KEY>. Delete this file to reset.\n"
    "\n"
    "# 60 = cap at 60 fps, 30 = cap at 30 fps (steadier), 0 = no cap\n"
    "LIMIT=60\n"
    "# 0 = present as soon as a frame is ready (the Switch compositor does not\n"
    "#     tear; LIMIT paces it), 1 = wait for the refresh (drops to 30 under 60)\n"
    "VSYNC=0\n"
    "# 1 = 16:9 with a wider field of view (Hor+), 0 = original 4:3\n"
    "WIDESCREEN=1\n"
    "# 1 = spread threads over the CPU cores (game / GL driver / sound)\n"
    "PIN=1\n"
    "# 1 = stream vertex/index data through a persistent GPU ring (faster, and\n"
    "#     needed for the game's text: Mesa drops its client-side indexed draws)\n"
    "STREAM=1\n"
    "# 1 = FPS counter in the top-left corner, 0 = off\n"
    "FPS=1\n"
    "# right-stick look speed (TheFamilia default 30)\n"
    "LOOK_SENS=30\n"
    "# 32 or 16 bit textures (16 saves memory, slightly lower quality)\n"
    "TEXTURES=32\n"
    "# CPU clock in MHz while playing. The game's main thread is the limit for\n"
    "# 60 FPS; 1785 is the Switch's own boost clock (used by official games\n"
    "# while loading). 1020 = the normal game clock. 0 = leave as the system sets it.\n"
    "# Higher clock = more battery use and heat in handheld mode.\n"
    "CPU_CLOCK=1785\n"
    "# (old option) 1 = system boost mode: faster CPU but the GPU drops to\n"
    "# 76 MHz - leave at 0 and use CPU_CLOCK instead\n"
    "CPU_BOOST=0\n"
    "# 1 = write performance lines to log.txt every 5 s\n"
    "STATS=0\n"
    "# 0 = skip the intro videos\n"
    "MOVIES=1\n";

static void nx_load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        FILE *w = fopen(path, "w");
        if (w) { fputs(k_default_config, w); fclose(w); }
        f = fopen(path, "r");
        if (!f) return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || !*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1, *end = v + strlen(v);
        while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        char *kend = eq;
        while (kend > p && kend[-1] == ' ') *--kend = 0;
        /* HLE_* keys (win32hle diagnostics such as HLE_TRACE) are set as-is */
        char env[96] = "FAMILIA_";
        if (!strncmp(p, "HLE_", 4)) env[0] = 0;
        size_t i = strlen(env);
        for (char *k = p; *k && i < sizeof env - 1; k++) env[i++] = (char)((*k >= 'a' && *k <= 'z') ? *k - 32 : *k);
        env[i] = 0;
        if (!getenv(env)) setenv(env, v, 1);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    romfsInit();                  /* fonts for the game's GDI text (Liberation, SIL OFL) */
    mkdir(NX_ROOT, 0755);
    mkdir(NX_ROOT "/userdata", 0755);
    /* stderr is the port's log, as log.txt is on PortMaster */
    freopen(NX_ROOT "/log.txt", "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);

    /* the translated game code is not in the NSO (too big to start): map it
     * from romfs:/gentext.bin before anything can call into it */
    {
        int nx_load_gentext(char *err, size_t errsz);
        char err[256] = "";
        if (nx_load_gentext(err, sizeof err)) {
            fprintf(stderr, "[nx] game code: %s\n", err);
            fflush(stderr);
            consoleInit(NULL);
            printf("Mafia could not load its game code:\n\n%s\n\nPlease report this with sd:%s/log.txt\n\nPress + to exit.\n", err, NX_ROOT);
            PadState pad;
            padConfigureInput(1, HidNpadStyleSet_NpadStandard);
            padInitializeDefault(&pad);
            while (appletMainLoop()) {
                padUpdate(&pad);
                if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
                consoleUpdate(NULL);
            }
            consoleExit(NULL);
            romfsExit();
            return 1;
        }
    }

    /* Game files: sd:/switch/mafia/data, or inside the NSP (romfs:/data) for a
     * personal all-in-one build (switch/make_nsp.sh FULL=1). An all-in-one NSP uses its own copy. */
    struct stat st;
    const char *game_dir = NULL;
    if (stat("romfs:/data/Game.exe", &st) == 0 || stat("romfs:/data/game.exe", &st) == 0) game_dir = "romfs:/data";
    else if (stat(NX_ROOT "/data/Game.exe", &st) == 0 || stat(NX_ROOT "/data/game.exe", &st) == 0) game_dir = NX_ROOT "/data";
    if (!game_dir) {
        fprintf(stderr, "[nx] %s/data/Game.exe not found: copy your Mafia 1.2 files there\n", NX_ROOT);
        consoleInit(NULL);
        printf("Mafia\n\nCopy your own Mafia 1.2 game files to\n  sd:%s/data/\n(Game.exe, LS3DF.dll, rw_data.dll, IJoy.dll, *.dta, ...)\n\nPress + to exit.\n", NX_ROOT);
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
        return 1;
    }

    nx_load_config(NX_ROOT "/config.ini");
    nx_setenv_default("FAMILIA_GAME_DIR", game_dir);
    fprintf(stderr, "[nx] game files: %s\n", game_dir);
    nx_setenv_default("FAMILIA_USER_DIR", NX_ROOT "/userdata");
    /* No OS page cache on Horizon: the Linux read-ahead would only cost I/O */
    nx_setenv_default("FAMILIA_READAHEAD", "0");
    /* GPU streaming ring: on the Switch's Mesa (nouveau) it has none of the
     * Mali driver's leak, and the game's indexed client-memory 2D draws (all
     * text) only render through it */
    nx_setenv_default("FAMILIA_STREAM", "1");
    fprintf(stderr, "[nx] The Familia (Switch) starting\n");
    AppletOperationMode mode = appletGetOperationMode();
    fprintf(stderr, "[nx] %s mode\n", mode == AppletOperationMode_Console ? "docked (1080p)" : "handheld (720p)");
    nx_setenv_default("FAMILIA_CPU_CLOCK", "1785");
    nx_clock_start(atoi(getenv("FAMILIA_CPU_CLOCK")));
    if (atoi(getenv("FAMILIA_CPU_BOOST") ? getenv("FAMILIA_CPU_BOOST") : "0"))
        appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
    { void nx_prof_init(void); nx_prof_init(); }
#ifdef FAMILIA_PGO_GEN
    mkdir("/switch/mafia/pgo", 0777);   /* libgcov writes the counters here */
    fprintf(stderr, "[nx] PGO training build: counters go to /switch/mafia/pgo\n");
#endif
    int rc = familia_main(argc, argv);
    nx_clock_stop();
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    fprintf(stderr, "[nx] exit %d\n", rc);
    romfsExit();
    return rc;
}

/* glibc extras the runtime uses (see compat/nx_compat.h). */
time_t timegm(struct tm *tm) {
    /* days from civil (Howard Hinnant), UTC */
    int y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const long long days = (long long)era * 146097 + (long long)doe - 719468;
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}
int setpriority(int which, id_t who, int prio) { (void)which; (void)who; (void)prio; return 0; }

/* ---- POSIX calls newlib/libnx do not provide ------------------------- */
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
static pthread_mutex_t g_pio_lock = PTHREAD_MUTEX_INITIALIZER;
/* pread/pwrite as seek + read/write + seek back, serialised */
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    pthread_mutex_lock(&g_pio_lock);
    off_t cur = lseek(fd, 0, SEEK_CUR);
    ssize_t r = -1;
    if (cur >= 0 && lseek(fd, off, SEEK_SET) == off) {
        r = read(fd, buf, n);
        lseek(fd, cur, SEEK_SET);
    }
    pthread_mutex_unlock(&g_pio_lock);
    return r;
}
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    pthread_mutex_lock(&g_pio_lock);
    off_t cur = lseek(fd, 0, SEEK_CUR);
    ssize_t r = -1;
    if (cur >= 0 && lseek(fd, off, SEEK_SET) == off) {
        r = write(fd, buf, n);
        lseek(fd, cur, SEEK_SET);
    }
    pthread_mutex_unlock(&g_pio_lock);
    return r;
}
/* SetFileTime: file times are not settable on the SD card here */
struct timespec;
int futimens(int fd, const struct timespec times[2]) { (void)fd; (void)times; return 0; }
/* gdidc.c asks fc-match for a font file: no processes on Horizon */
FILE *popen(const char *cmd, const char *mode) { (void)cmd; (void)mode; errno = ENOSYS; return NULL; }
int pclose(FILE *f) { (void)f; return -1; }
long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE: return 0x1000;
    case _SC_NPROCESSORS_CONF: case _SC_NPROCESSORS_ONLN: return 3;   /* application cores */
    case _SC_CLK_TCK: return 100;
    default: errno = EINVAL; return -1;
    }
}
