/*
 * nx_textinput.c - the Switch's on-screen keyboard for the game's text fields
 * (the profile name on the first start, save names).
 *
 * The game's text field (GM_TextEdit, Game.exe vtable 0x626B80) only grows
 * from typed characters, and a Switch has no keyboard. Its Draw (vtable slot
 * 5, 0x5DC950) is pointed at nx_textedit_draw: when a text field is drawn as
 * the menu's active component, the system keyboard opens once, pre-filled
 * with the field's text, and the result is written into the field. Then the
 * player selects OK as usual. Activating the field again (A on it) opens the
 * keyboard again.
 *
 * GM_TextEdit layout (mafia-re g_m_comps.h): +0x08 flags (0x80000000 = the
 * menu's active component), +0x40 char buffer[0x40], +0x80 max length.
 */
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

#define TEXTEDIT_VTABLE   0x626B80u
#define TEXTEDIT_DRAW     0x5DC950u
#define DRAW_SLOT         5
#define COMPF_ACTIVE      0x80000000u
#define TE_FLAGS          0x08u
#define TE_BUFFER         0x40u
#define TE_MAXLEN         0x80u
#define TE_SIZE           0x40u

static uint32_t g_shown_for;     /* the field the keyboard was shown for during this activation */
static recomp_func_t g_orig_draw;

static void open_keyboard(uint32_t te) {
    char cur[TE_SIZE + 1], out[TE_SIZE * 4 + 4];
    memcpy(cur, hle_ptr(te + TE_BUFFER), TE_SIZE);
    cur[TE_SIZE] = 0;
    uint32_t maxlen = MEM32(te + TE_MAXLEN);
    if (!maxlen || maxlen > TE_SIZE - 1) maxlen = TE_SIZE - 1;

    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) { fprintf(stderr, "[nx] keyboard: swkbdCreate failed (rc %x)\n", rc); return; }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_All);
    swkbdConfigSetOkButtonText(&kbd, "OK");
    swkbdConfigSetGuideText(&kbd, "Mafia - enter a name");
    swkbdConfigSetInitialText(&kbd, cur);
    swkbdConfigSetStringLenMax(&kbd, maxlen);
    out[0] = 0;
    fprintf(stderr, "[nx] keyboard: opening for the text field at %08X\n", te);
    rc = swkbdShow(&kbd, out, sizeof out);
    fprintf(stderr, "[nx] keyboard: returned (rc %x)\n", rc);
    swkbdClose(&kbd);
    if (R_FAILED(rc)) { fprintf(stderr, "[nx] keyboard: closed without text (rc %x)\n", rc); return; }

    /* the game's font covers ASCII: keep printable ASCII only */
    char clean[TE_SIZE];
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)out; *p && n < maxlen; p++)
        if (*p >= 0x20 && *p < 0x7F) clean[n++] = (char)*p;
    clean[n] = 0;
    memcpy(hle_ptr(te + TE_BUFFER), clean, n + 1);
    fprintf(stderr, "[nx] keyboard: text field set (%u characters)\n", n);
}

static void nx_textedit_draw(void) {
    uint32_t te = g_ecx;
    if (MEM32(te + TE_FLAGS) & COMPF_ACTIVE) {
        if (g_shown_for != te) {
            g_shown_for = te;
            open_keyboard(te);
            g_ecx = te;               /* the keyboard ran C code only, but keep 'this' for Draw */
        }
    } else if (g_shown_for == te) {
        g_shown_for = 0;              /* deactivated: the next activation opens it again */
    }
    g_orig_draw();                    /* the game's Draw: pops its return address and argument */
}

/* Called once the game's modules are loaded (fam_input_pump's first call). */
void nx_textinput_install(void) {
    static int done;
    if (done) return;
    done = 1;
    g_orig_draw = recomp_lookup(TEXTEDIT_DRAW);
    if (!g_orig_draw || MEM32(TEXTEDIT_VTABLE + 4 * DRAW_SLOT) != TEXTEDIT_DRAW) {
        fprintf(stderr, "[nx] keyboard: text field not found (vtable %08X), on-screen keyboard off\n",
                MEM32(TEXTEDIT_VTABLE + 4 * DRAW_SLOT));
        return;
    }
    static const win32hle_shim shims[] = { { "familia::GM_TextEdit::Draw", nx_textedit_draw }, { NULL, NULL } };
    win32hle_register(shims);
    uint32_t va = hle_resolve("familia::GM_TextEdit::Draw");
    if (!va) { fprintf(stderr, "[nx] keyboard: no shim address, on-screen keyboard off\n"); return; }
    MEM32(TEXTEDIT_VTABLE + 4 * DRAW_SLOT) = va;
    fprintf(stderr, "[nx] keyboard: on-screen keyboard for text fields on\n");
}
