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

/* Linux-only diagnostics (signal crash handler, SIGPROF profiler). */
void fam_crash_install(void) {}
void fam_prof_start(void) {}

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
    "# 1 = wait for the display refresh (no tearing), 0 = off\n"
    "VSYNC=1\n"
    "# 1 = show an FPS counter\n"
    "FPS=0\n"
    "# right-stick look speed (TheFamilia default 30)\n"
    "LOOK_SENS=30\n"
    "# 32 or 16 bit textures (16 saves memory, slightly lower quality)\n"
    "TEXTURES=32\n"
    "# 1 = CPU boost while playing (faster CPU, lower GPU clock)\n"
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
        char env[96] = "FAMILIA_";
        size_t i = strlen(env);
        for (char *k = p; *k && i < sizeof env - 1; k++) env[i++] = (char)((*k >= 'a' && *k <= 'z') ? *k - 32 : *k);
        env[i] = 0;
        if (!getenv(env)) setenv(env, v, 1);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    mkdir(NX_ROOT, 0755);
    mkdir(NX_ROOT "/userdata", 0755);
    /* stderr is the port's log, as log.txt is on PortMaster */
    freopen(NX_ROOT "/log.txt", "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);

    struct stat st;
    if (stat(NX_ROOT "/data/Game.exe", &st) != 0 && stat(NX_ROOT "/data/game.exe", &st) != 0) {
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
    nx_setenv_default("FAMILIA_GAME_DIR", NX_ROOT "/data");
    nx_setenv_default("FAMILIA_USER_DIR", NX_ROOT "/userdata");
    fprintf(stderr, "[nx] The Familia (Switch) starting\n");
    AppletOperationMode mode = appletGetOperationMode();
    fprintf(stderr, "[nx] %s mode\n", mode == AppletOperationMode_Console ? "docked (1080p)" : "handheld (720p)");
    if (atoi(getenv("FAMILIA_CPU_BOOST") ? getenv("FAMILIA_CPU_BOOST") : "0"))
        appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
    int rc = familia_main(argc, argv);
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    fprintf(stderr, "[nx] exit %d\n", rc);
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
