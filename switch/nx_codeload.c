/*
 * nx_codeload.c - map the translated game code (romfs:/gentext.bin) back
 * where it was linked.
 *
 * The console refuses to start a ~200 MB main module ("could not start the
 * software"), so switch/familia.ld links the recompiled game code into a
 * separate .gentext segment a gap after the module, and switch/Makefile cuts
 * it out of the NSO into gentext.bin. Here, before any of that code runs,
 * it is read into heap memory, turned into code memory and mapped read +
 * execute at __gentext_start: the exact address it was linked for (relative
 * to the module), so every PC-relative call and reference between the two
 * parts is valid as linked. Needs svcCreateCodeMemory / svcControlCodeMemory
 * (main.npdm.json).
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char __gentext_start[], __gentext_end[];

static Handle g_code;

int nx_load_gentext(char *err, size_t errsz) {
    uintptr_t dst = (uintptr_t)__gentext_start;
    size_t size = (size_t)((uintptr_t)__gentext_end - dst);
    if (!size) return 0;                         /* a build without the split */
    if ((dst | size) & 0xFFF) { snprintf(err, errsz, "gentext not page aligned (%lx, %zx)", (unsigned long)dst, size); return -1; }

    /* the range must be free (the kernel randomises where the other regions go) */
    MemoryInfo mi; u32 pi;
    Result rc = svcQueryMemory(&mi, &pi, dst);
    if (R_FAILED(rc) || mi.type != MemType_Unmapped || mi.addr + mi.size < dst + size) {
        snprintf(err, errsz, "the address range for the game code is in use (type %u, rc %x).\nStart the game again.", (unsigned)mi.type, rc);
        return -1;
    }

    FILE *f = fopen("romfs:/gentext.bin", "rb");
    if (!f) { snprintf(err, errsz, "romfs:/gentext.bin missing: the NSP was not packed with the game code"); return -1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz != (long)size) {
        fclose(f);
        snprintf(err, errsz, "gentext.bin is %ld bytes, this program needs %zu:\nthe NSP mixes files from two builds", fsz, size);
        return -1;
    }
    /* The kernel fills new code memory with 0xFF (KCodeMemory::Initialize),
     * so the code is read in only after creating it, through the writable
     * (owner) view; then the read + execute view goes at __gentext_start. */
    void *buf = aligned_alloc(0x1000, size);
    if (!buf) { fclose(f); snprintf(err, errsz, "out of memory for the game code (%zu MB)", size >> 20); return -1; }
    u64 t0 = armGetSystemTick();
    rc = svcCreateCodeMemory(&g_code, buf, size);
    if (R_FAILED(rc)) { fclose(f); free(buf); snprintf(err, errsz, "svcCreateCodeMemory failed (rc %x)", rc); return -1; }
    virtmemLock();
    void *rw = virtmemFindCodeMemory(size, 0x1000);
    rc = rw ? svcControlCodeMemory(g_code, CodeMapOperation_MapOwner, rw, size, Perm_Rw) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    virtmemUnlock();
    if (R_FAILED(rc)) { fclose(f); snprintf(err, errsz, "mapping the game code (writable view) failed (rc %x)", rc); return -1; }
    size_t got = 0;
    while (got < size) {
        size_t n = fread((char *)rw + got, 1, size - got > (8u << 20) ? (8u << 20) : size - got, f);
        if (!n) break;
        got += n;
    }
    fclose(f);
    if (got != size) { snprintf(err, errsz, "reading gentext.bin stopped at %zu of %zu bytes", got, size); return -1; }
    armDCacheFlush(rw, size);
    rc = svcControlCodeMemory(g_code, CodeMapOperation_MapSlave, (void *)dst, size, Perm_Rx);
    if (R_FAILED(rc)) { snprintf(err, errsz, "mapping the game code at %lx failed (rc %x)", (unsigned long)dst, rc); return -1; }
    /* both views stay mapped, as libnx's jit does */
    armICacheInvalidate((void *)dst, size);
    fprintf(stderr, "[nx] game code: %zu MB mapped at %lx in %llu ms\n", size >> 20, (unsigned long)dst,
            (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000));
    return 0;
}
