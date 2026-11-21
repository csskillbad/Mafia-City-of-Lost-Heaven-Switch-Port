/*
 * nx_pad.c - the Switch's controllers, read straight from HID (libnx).
 *
 * Every controller counts at once: the Joy-Cons attached in handheld mode,
 * a pair of Joy-Cons in the grip, a Pro Controller, any player slot. SDL's
 * game-controller layer is not used on the Switch: it only knew the first
 * player slot and mapped the buttons by position (Nintendo B = Xbox A).
 * Here they go by their labels: A confirms, B goes back.
 */
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include "input.h"

static PadState g_pad;
static int g_init, g_was_connected = -1;
static uint32_t g_last_styles;

static void init(void) {
    g_init = 1;
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&g_pad);         /* players 1-8 and handheld, merged */
}

/* the buttons held (FAM_PB_* bits) and the sticks (-32767..32767, y down,
 * as SDL); returns whether any controller is connected */
int nx_pad_poll(uint32_t *buttons, int16_t axes[4], int *lt, int *rt) {
    if (!g_init) init();
    padUpdate(&g_pad);
    u64 h = padGetButtons(&g_pad);
    static const struct { u64 nx; int bit; } map[] = {
        { HidNpadButton_A, FAM_PB_A }, { HidNpadButton_B, FAM_PB_B },
        { HidNpadButton_X, FAM_PB_X }, { HidNpadButton_Y, FAM_PB_Y },
        { HidNpadButton_L, FAM_PB_LB }, { HidNpadButton_R, FAM_PB_RB },
        { HidNpadButton_ZL, FAM_PB_LT }, { HidNpadButton_ZR, FAM_PB_RT },
        { HidNpadButton_Minus, FAM_PB_BACK }, { HidNpadButton_Plus, FAM_PB_START },
        { HidNpadButton_StickL, FAM_PB_L3 }, { HidNpadButton_StickR, FAM_PB_R3 },
        { HidNpadButton_Up, FAM_PB_UP }, { HidNpadButton_Down, FAM_PB_DOWN },
        { HidNpadButton_Left, FAM_PB_LEFT }, { HidNpadButton_Right, FAM_PB_RIGHT },
        /* a single Joy-Con held sideways: its small SL / SR as the shoulders */
        { HidNpadButton_LeftSL | HidNpadButton_RightSL, FAM_PB_LB },
        { HidNpadButton_LeftSR | HidNpadButton_RightSR, FAM_PB_RB },
    };
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (h & map[i].nx) b |= 1u << map[i].bit;
    *buttons = b;
    HidAnalogStickState l = padGetStickPos(&g_pad, 0), r = padGetStickPos(&g_pad, 1);
    axes[0] = (int16_t)l.x; axes[1] = (int16_t)(l.y == -32768 ? 32767 : -l.y);
    axes[2] = (int16_t)r.x; axes[3] = (int16_t)(r.y == -32768 ? 32767 : -r.y);
    *lt = (h & HidNpadButton_ZL) ? 32767 : 0;     /* ZL / ZR are digital */
    *rt = (h & HidNpadButton_ZR) ? 32767 : 0;

    int connected = padIsConnected(&g_pad);
    uint32_t styles = (uint32_t)padGetStyleSet(&g_pad);
    if (connected != g_was_connected || styles != g_last_styles) {
        fprintf(stderr, "[input] switch controllers: %s (styles %X%s%s%s)\n", connected ? "connected" : "none",
                styles, (styles & HidNpadStyleTag_NpadHandheld) ? " handheld" : "",
                (styles & HidNpadStyleTag_NpadFullKey) ? " pro" : "", (styles & HidNpadStyleTag_NpadJoyDual) ? " joy-con pair" : "");
        g_was_connected = connected; g_last_styles = styles;
    }
    return connected;
}
