/*
 * native_widescreen.c - Hor+ widescreen for LS3D's camera.
 *
 * LS3D builds a "Vert-" projection: fov_a is the horizontal field of view and
 * the vertical scale is cot(fov/2) * aspect_ratio (I3D_camera::UpdateProjMatrix,
 * LS3DF 0x1000B400). On a 16:9 screen that keeps the 4:3 width and crops the
 * top and bottom: the picture looks zoomed in.
 *
 * The hook widens the horizontal FOV for the duration of UpdateProjMatrix so
 * the vertical FOV equals what the game designed at 4:3:
 *     fov' = 2 * atan(tan(fov / 2) * aspect / (4/3))
 * That call also produces the near-plane extents (ncp_x/ncp_y) the camera's
 * frustum is built from, so culling widens with the picture (no pop-in at the
 * new edges). fov_a itself is restored afterwards: game code that reads the
 * FOV and sets it again cannot compound the widening, and the renderer's
 * distance/LOD correction (pi/2 / fov_a) keeps the original draw distance.
 *
 * Only cameras with a widescreen aspect (>= 1.5) are touched; the HUD's
 * special cameras (aspect * 0.5, * 1/3) and orthogonal cameras are not.
 * FAMILIA_WIDESCREEN=0 turns it off.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "recomp_types.h"

void recomp_ls3df_1000B400_orig(void);
int fam_config_int(const char *key, int def);

#define CAM_FOV        0x140u   /* float fov_a */
#define CAM_NEAR_FOV   0x14Cu   /* float near_fov_a */
#define CAM_ASPECT     0x158u   /* float aspect_ratio (width / height) */
#define CAM_ORTHOGONAL 0x15Cu   /* bool */

static int g_ws = -1;

static float horplus(float fov, float aspect) {
    float wide = 2.0f * atanf(tanf(fov * 0.5f) * aspect * 0.75f);
    return wide < 3.1f ? wide : 3.1f;   /* SetFOV's own limit is < pi */
}

void fam_native_cam_update_proj(void) {   /* 0x1000B400, thiscall, no arguments */
    if (g_ws < 0) {
        g_ws = fam_config_int("widescreen", 1);
        fprintf(stderr, "[widescreen] Hor+ camera %s\n", g_ws ? "on" : "off");
    }
    uint32_t cam = g_ecx;
    float aspect = MEMF(cam + CAM_ASPECT);
    if (!g_ws || MEM8(cam + CAM_ORTHOGONAL) || !(aspect >= 1.5f && aspect < 4.0f)) {
        recomp_ls3df_1000B400_orig();
        return;
    }
    const uint32_t fov_bits = MEM32(cam + CAM_FOV), near_bits = MEM32(cam + CAM_NEAR_FOV);
    MEMF(cam + CAM_FOV) = horplus(MEMF(cam + CAM_FOV), aspect);
    MEMF(cam + CAM_NEAR_FOV) = horplus(MEMF(cam + CAM_NEAR_FOV), aspect);
    recomp_ls3df_1000B400_orig();          /* consumes the return address as the original */
    MEM32(cam + CAM_FOV) = fov_bits;
    MEM32(cam + CAM_NEAR_FOV) = near_bits;
}
