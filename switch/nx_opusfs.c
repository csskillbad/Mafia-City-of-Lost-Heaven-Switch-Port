/*
 * nx_opusfs.c - the game's sounds/x.wav served from sounds/x.opus.
 *
 * The all-in-one NSP carries the loose sounds (~1 GB of 16-bit PCM) as Opus
 * (tools/encode_sounds.py), about 4x smaller. When the game names a WAV that
 * is not there, hle_path.c asks the hook below; if x.opus exists, the path
 * becomes "mem:/<path of x.opus>", a device (newlib devoptab) that decodes
 * the Opus file on open into the exact WAV the game shipped with (sample
 * rate, channels, 16 bits and length from the file's FAMILIA_WAV tag) and
 * serves it from memory. CreateFileA / ReadFile / mmioOpen / fopen all keep
 * working on plain descriptors, unchanged.
 */
#include <switch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <opusfile.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include "win32hle.h"
extern int (*hle_missing_file_hook)(char *path, size_t n);   /* hle_path.c */

#define PREFIX "mem:/"

typedef struct { uint8_t *buf; size_t size, pos; } memfile;
typedef struct { unsigned rate, ch, frames; } wavfmt;

/* the WAV sizes already worked out (stat before open), by path hash */
#define META_N 16384
static struct { uint64_t key; uint32_t size; } g_meta[META_N];
static Mutex g_meta_lock;
static uint64_t g_decode_ns; static unsigned g_decoded;

static uint64_t hash(const char *s) { uint64_t h = 1469598103934665603ull; while (*s) h = (h ^ (uint8_t)*s++) * 1099511628211ull; return h | 1; }

static int read_fmt(OggOpusFile *of, wavfmt *f) {
    const char *v = opus_tags_query(op_tags(of, -1), "FAMILIA_WAV", 0);
    unsigned bits = 16;
    if (v && sscanf(v, "%u,%u,%u,%u", &f->rate, &f->ch, &bits, &f->frames) == 4 && bits == 16 && f->rate && (f->ch == 1 || f->ch == 2))
        return 1;
    f->rate = 48000; f->ch = (unsigned)op_channel_count(of, -1); f->frames = (unsigned)op_pcm_total(of, -1);
    return f->ch == 1 || f->ch == 2;
}

static void wav_header(uint8_t *h, const wavfmt *f) {
    uint32_t data = f->frames * f->ch * 2, v;
    memcpy(h, "RIFF", 4); v = 36 + data; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t w = 1; memcpy(h + 20, &w, 2); w = (uint16_t)f->ch; memcpy(h + 22, &w, 2);
    v = f->rate; memcpy(h + 24, &v, 4); v = f->rate * f->ch * 2; memcpy(h + 28, &v, 4);
    w = (uint16_t)(f->ch * 2); memcpy(h + 32, &w, 2); w = 16; memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
}

static uint8_t *decode(const char *opus, size_t *size) {
    u64 t0 = armGetSystemTick();
    int err = 0;
    OggOpusFile *of = op_open_file(opus, &err);
    if (!of) { fprintf(stderr, "[opusfs] %s: cannot open (%d)\n", opus, err); return NULL; }
    wavfmt f;
    if (!read_fmt(of, &f)) { op_free(of); return NULL; }
    /* all of it at 48 kHz */
    long total = (long)op_pcm_total(of, -1);
    if (total < 0) total = 0;
    size_t cap = (size_t)total + 5760;
    int16_t *pcm = malloc(cap * f.ch * 2);
    long got = 0;
    while (pcm && (size_t)got < cap) {
        int r = op_read(of, pcm + (size_t)got * f.ch, (int)((cap - (size_t)got) * f.ch), NULL);
        if (r <= 0) break;
        got += r;
    }
    op_free(of);
    if (!pcm) return NULL;
    size_t data = (size_t)f.frames * f.ch * 2;
    uint8_t *wav = calloc(1, 44 + data);
    if (!wav) { free(pcm); return NULL; }
    wav_header(wav, &f);
    int16_t *out = (int16_t *)(wav + 44);
    if (f.rate == 48000) {
        memcpy(out, pcm, (size_t)(got < (long)f.frames ? got : (long)f.frames) * f.ch * 2);
    } else {
        SwrContext *swr = NULL;
        AVChannelLayout lay; av_channel_layout_default(&lay, (int)f.ch);
        if (swr_alloc_set_opts2(&swr, &lay, AV_SAMPLE_FMT_S16, (int)f.rate, &lay, AV_SAMPLE_FMT_S16, 48000, 0, NULL) >= 0 && swr_init(swr) >= 0) {
            /* room for the resampler's whole output; the WAV keeps the original length */
            int room = (int)((int64_t)got * f.rate / 48000 + 256);
            int16_t *tmp = malloc((size_t)room * f.ch * 2);
            if (tmp) {
                const uint8_t *in[1] = { (const uint8_t *)pcm };
                uint8_t *o[1] = { (uint8_t *)tmp };
                int n = swr_convert(swr, o, room, in, (int)got);
                if (n < 0) n = 0;
                o[0] = (uint8_t *)(tmp + (size_t)n * f.ch);
                int m = swr_convert(swr, o, room - n, NULL, 0);
                if (m > 0) n += m;
                memcpy(out, tmp, (size_t)(n < (int)f.frames ? n : (int)f.frames) * f.ch * 2);
                free(tmp);
            }
        }
        swr_free(&swr);
    }
    free(pcm);
    *size = 44 + data;
    g_decode_ns += armTicksToNs(armGetSystemTick() - t0);
    if (++g_decoded % 200 == 0)
        fprintf(stderr, "[opusfs] %u sounds decoded, %llu ms in all\n", g_decoded, (unsigned long long)(g_decode_ns / 1000000));
    return wav;
}

/* the size of the WAV x.opus stands for, without decoding it */
static int wav_size(const char *opus, uint32_t *size) {
    uint64_t k = hash(opus);
    unsigned i = (unsigned)(k % META_N);
    mutexLock(&g_meta_lock);
    for (unsigned n = 0; n < 8; n++, i = (i + 1) % META_N)
        if (g_meta[i].key == k) { *size = g_meta[i].size; mutexUnlock(&g_meta_lock); return 1; }
    mutexUnlock(&g_meta_lock);
    int err = 0;
    OggOpusFile *of = op_test_file(opus, &err);
    if (!of) return 0;
    wavfmt f;
    int ok = read_fmt(of, &f);
    op_free(of);
    if (!ok) return 0;
    *size = 44 + f.frames * f.ch * 2;
    i = (unsigned)(k % META_N);
    mutexLock(&g_meta_lock);
    for (unsigned n = 0; n < 8; n++, i = (i + 1) % META_N)
        if (!g_meta[i].key) { g_meta[i].key = k; g_meta[i].size = *size; break; }
    mutexUnlock(&g_meta_lock);
    return 1;
}

static const char *opus_of(const char *path) {
    if (strncmp(path, PREFIX, sizeof PREFIX - 1)) return NULL;
    return path + sizeof PREFIX - 1;
}

/* development (config OPUSFS_CHECK=1): compare with the original WAV in
 * sounds.orig/ next to sounds/ - header bytes and the signal-to-noise ratio */
int fam_config_int(const char *key, int def);
static void check(const char *opus, const uint8_t *wav, size_t size) {
    static int on = -1, n;
    if (on < 0) on = fam_config_int("opusfs_check", 0);
    if (!on || n >= 400) return;
    char orig[1100]; const char *base = strrchr(opus, '/');
    if (!base) return;
    snprintf(orig, sizeof orig, "%.*s.orig/%.*s.wav", (int)(base - opus), opus, (int)(strlen(base + 1) - 5), base + 1);
    FILE *f = fopen(orig, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *o = malloc((size_t)len);
    if (!o || fread(o, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(o); return; }
    fclose(f);
    size_t m = (size_t)len < size ? (size_t)len : size;
    long ns = (long)(m - 44) / 2;
    const int16_t *X = (const int16_t *)(o + 44), *Y = (const int16_t *)(wav + 44);
    double best = -99, snr0 = 0; int best_lag = 0;
    for (int lag = -64; lag <= 64; lag++) {          /* in samples (interleaved) */
        double sig = 0, err = 0;
        for (long i = 64; i < ns - 64; i++) { double x = X[i], y = Y[i + lag]; sig += x * x; err += (x - y) * (x - y); }
        double snr = err > 0 ? 10.0 * __builtin_log10(sig / err) : 99.0;
        if (lag == 0) snr0 = snr;
        if (snr > best) best = snr, best_lag = lag;
    }
    n++;
    fprintf(stderr, "[opusfs] check %s: size %s, header %s, SNR %.1f dB, best %.1f dB at lag %d\n", base + 1,
            (size_t)len == size ? "same" : "DIFFERENT", memcmp(o, wav, 44) ? "DIFFERENT" : "same", snr0, best, best_lag);
    free(o);
}

static int m_open(struct _reent *r, void *fs, const char *path, int flags, int mode) {
    (void)mode;
    memfile *m = fs;
    const char *opus = opus_of(path);
    if (!opus) { r->_errno = ENOENT; return -1; }
    if ((flags & O_ACCMODE) != O_RDONLY) { r->_errno = EROFS; return -1; }
    m->buf = decode(opus, &m->size);
    m->pos = 0;
    if (!m->buf) { r->_errno = EIO; return -1; }
    check(opus, m->buf, m->size);
    return 0;
}
static int m_close(struct _reent *r, void *fd) { (void)r; memfile *m = fd; free(m->buf); m->buf = NULL; return 0; }
static ssize_t m_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    (void)r; memfile *m = fd;
    size_t n = m->pos < m->size ? m->size - m->pos : 0;
    if (n > len) n = len;
    memcpy(ptr, m->buf + m->pos, n);
    m->pos += n;
    return (ssize_t)n;
}
static off_t m_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    memfile *m = fd;
    off_t to = pos + (dir == SEEK_CUR ? (off_t)m->pos : dir == SEEK_END ? (off_t)m->size : 0);
    if (to < 0) { r->_errno = EINVAL; return -1; }
    m->pos = (size_t)to;
    return to;
}
static void fill_stat(struct stat *st, size_t size) {
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFREG | 0444; st->st_size = (off_t)size; st->st_nlink = 1; st->st_blksize = 4096;
}
static int m_fstat(struct _reent *r, void *fd, struct stat *st) { (void)r; fill_stat(st, ((memfile *)fd)->size); return 0; }
static int m_stat(struct _reent *r, const char *file, struct stat *st) {
    const char *opus = opus_of(file);
    uint32_t size;
    if (!opus || !wav_size(opus, &size)) { r->_errno = ENOENT; return -1; }
    fill_stat(st, size);
    return 0;
}

static const devoptab_t g_dev = {
    .name = "mem", .structSize = sizeof(memfile),
    .open_r = m_open, .close_r = m_close, .read_r = m_read, .seek_r = m_seek,
    .fstat_r = m_fstat, .stat_r = m_stat, .lstat_r = m_stat,
};

/* sounds/x.wav missing: sounds/x.opus (lower case) stands in */
static int missing_file(char *path, size_t n) {
    size_t len = strlen(path);
    if (len < 5 || strcasecmp(path + len - 4, ".wav")) return 0;
    char opus[1024];
    const char *base = strrchr(path, '/');
    size_t dir = base ? (size_t)(base - path) + 1 : 0;
    if (dir + (len - dir) + 2 >= sizeof opus) return 0;
    memcpy(opus, path, dir);
    size_t o = dir;
    for (size_t i = dir; i < len - 4; i++) opus[o++] = (char)((path[i] >= 'A' && path[i] <= 'Z') ? path[i] + 32 : path[i]);
    strcpy(opus + o, ".opus");
    struct stat st;
    if (stat(opus, &st) != 0) return 0;
    if (strlen(PREFIX) + strlen(opus) + 1 > n) return 0;
    snprintf(path, n, PREFIX "%s", opus);
    return 1;
}

void nx_opusfs_install(const char *game_dir) {
    char probe[1024];
    struct stat st;
    snprintf(probe, sizeof probe, "%s/sounds", game_dir);
    if (stat(probe, &st) != 0) return;
    mutexInit(&g_meta_lock);
    if (AddDevice(&g_dev) < 0) { fprintf(stderr, "[opusfs] no device slot\n"); return; }
    hle_missing_file_hook = missing_file;
    fprintf(stderr, "[opusfs] sounds/*.wav also served from sounds/*.opus\n");
}
