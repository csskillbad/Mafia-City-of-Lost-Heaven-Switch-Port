/*
 * nx_textinput.c - the Switch's on-screen keyboard for the game's text fields
 * (the profile name on the first start, save names).
 *
 * The game's text field (GM_TextEdit, Game.exe vtable 0x626B80) only grows
 * from typed characters, and a Switch has no keyboard. Its Draw (vtable slot
 * 5, 0x5DC950) is pointed at nx_textedit_draw: when a text field is drawn as
 * the menu's active component, the system keyboard opens once, pre-filled
 * with the field's text, and the result is written into the field. Then the
 * menu's next item (OK) becomes the active one, so A confirms. A on the field
 * opens the keyboard again; B cancels the dialog (Esc).
 *
 * GM_TextEdit layout (mafia-re g_m_comps.h): +0x08 flags (0x80000000 = the
 * menu's active component), +0x40 char buffer[0x40], +0x80 max length.
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"
#include "input.h"

#define TEXTEDIT_VTABLE   0x626B80u
#define TEXTEDIT_DRAW     0x5DC950u
#define DRAW_SLOT         5
#define COMPF_ACTIVE      0x80000000u
#define TE_FLAGS          0x08u
#define TE_BUFFER         0x40u
#define TE_MAXLEN         0x80u
#define TE_SIZE           0x40u
#define TE_ID             0x04u
/* GM_Menu (mafia-re g_menu.h): +0xC the active component, +0x14/+0x18 the
 * components' vector (VC6: allocator, first, last, end) */
#define MENU_ACTIVE_WINDOW 0x6BD890u  /* GM_Menu::activeMenuWindow */
#define MENU_MOUSE_IN_USE  0x6BD894u  /* GM_Menu::mouseInUse (bool) */
#define MENU_ACTIVE       0x0Cu
#define MENU_COMPS_FIRST  0x14u
#define MENU_COMPS_LAST   0x18u
#define COMPF_CAN_ACTIVATE (0x10u | 0x20u | 0x8000u)   /* visible, enabled, activatable */

static uint32_t g_shown_for;     /* the field (and below, the dialog) the keyboard opened for by itself */
static recomp_func_t g_orig_draw;

/* After the keyboard: the active menu's next item by id (the dialog's OK)
 * becomes the active one, as the down arrow would make it. */
static void activate_next(uint32_t te) {
    uint32_t menu = MEM32(MENU_ACTIVE_WINDOW);
    if (!menu) { fprintf(stderr, "[nx] keyboard: no active menu\n"); return; }
    uint32_t first = MEM32(menu + MENU_COMPS_FIRST), last = MEM32(menu + MENU_COMPS_LAST);
    if (!first || last < first || last - first > 4 * 4096) {
        fprintf(stderr, "[nx] keyboard: menu %08X components %08X..%08X?\n", menu, first, last); return;
    }
    uint32_t id = MEM32(te + TE_ID), best = 0, best_id = 0xFFFFFFFFu;
    fprintf(stderr, "[nx] keyboard: menu %08X active %08X, field %08X id %X:", menu, MEM32(menu + MENU_ACTIVE), te, id);
    for (uint32_t it = first; it < last; it += 4) {
        uint32_t c = MEM32(it);
        if (!c) continue;
        uint32_t cid = MEM32(c + TE_ID), f = MEM32(c + TE_FLAGS);
        fprintf(stderr, " %X:%08X", cid, f);
        if (c == te) continue;
        if ((f & COMPF_CAN_ACTIVATE) == COMPF_CAN_ACTIVATE && cid > id && cid < best_id) best = c, best_id = cid;
    }
    fprintf(stderr, "\n");
    if (MEM32(menu + MENU_ACTIVE) != te || !best) return;
    MEM32(te + TE_FLAGS) &= ~COMPF_ACTIVE;
    MEM32(best + TE_FLAGS) |= COMPF_ACTIVE;
    MEM32(menu + MENU_ACTIVE) = best;
    MEM8(MENU_MOUSE_IN_USE) = 0;              /* the cursor must not take the activation back */
    fprintf(stderr, "[nx] keyboard: item %X is active now\n", best_id);
}

static int open_keyboard(uint32_t te) {
    char cur[TE_SIZE + 1], out[TE_SIZE * 4 + 4];
    memcpy(cur, hle_ptr(te + TE_BUFFER), TE_SIZE);
    cur[TE_SIZE] = 0;
    uint32_t maxlen = MEM32(te + TE_MAXLEN);
    if (!maxlen || maxlen > TE_SIZE - 1) maxlen = TE_SIZE - 1;

    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) { fprintf(stderr, "[nx] keyboard: swkbdCreate failed (rc %x)\n", rc); return 0; }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_All);
    swkbdConfigSetOkButtonText(&kbd, "OK");
    swkbdConfigSetGuideText(&kbd, "Mafia - enter a name");
    swkbdConfigSetInitialText(&kbd, cur);
    swkbdConfigSetStringLenMax(&kbd, maxlen);
    out[0] = 0;
    rc = swkbdShow(&kbd, out, sizeof out);
    swkbdClose(&kbd);
    fam_input_swallow();                      /* the button that closed the keyboard, if still held */
    if (R_FAILED(rc)) { fprintf(stderr, "[nx] keyboard: closed without text (rc %x)\n", rc); return 0; }

    /* the game's font covers ASCII: keep printable ASCII only */
    char clean[TE_SIZE];
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)out; *p && n < maxlen; p++)
        if (*p >= 0x20 && *p < 0x7F) clean[n++] = (char)*p;
    clean[n] = 0;
    memcpy(hle_ptr(te + TE_BUFFER), clean, n + 1);
    fprintf(stderr, "[nx] keyboard: text field set (%u characters)\n", n);
    return 1;
}

static void nx_textedit_draw(void) {
    uint32_t te = g_ecx, menu = MEM32(MENU_ACTIVE_WINDOW);
    static uint32_t prev_buttons, auto_menu;
    uint32_t b = fam_pad_buttons(), pressed = b & ~prev_buttons;
    prev_buttons = b;
    if (MEM32(te + TE_FLAGS) & COMPF_ACTIVE) {
        /* by itself the first time the dialog shows the field active, then A on it */
        if (g_shown_for != te || auto_menu != menu || (pressed & (1u << FAM_PB_A))) {
            g_shown_for = te; auto_menu = menu;
            if (open_keyboard(te)) activate_next(te);
            prev_buttons = fam_pad_buttons();
            g_ecx = te;               /* the keyboard ran C code only, but keep 'this' for Draw */
        }
    }
    g_orig_draw();                    /* the game's Draw: pops its return address and argument */
}

/* Called once the game's modules are loaded (fam_input_pump's first call). */
void nx_textinput_install(void) {
    static int done;
    if (done) return;
    done = 1;
    { const char *e = getenv("FAMILIA_SYSTEM_KEYBOARD");      /* SYSTEM_KEYBOARD=0: off (emulator tests type by script) */
      if (e && *e == '0') { fprintf(stderr, "[nx] keyboard: on-screen keyboard off (config)\n"); return; } }
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
