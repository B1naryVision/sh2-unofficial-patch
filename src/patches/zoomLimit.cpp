#include "zoomLimit.h"
#include "../core/config.h"
#include "../core/hook.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Raises how far the camera may zoom out, to a limit measured from the game
// rather than configured: the point past which the view stops being usable.
// That point depends on the camera's pitch, so the limit is interpolated
// between two calibration captures and scaled to the map being played.
// Opt-in via [camera] ZoomOutLimit in sh2-unofficial-patch.ini. Patch details,
// offsets, and the reverse-engineering trail live in
// docs/features/zoom-limit.md.

// GlowWorm::Camera update, called once per frame with the camera in ecx.
static const uintptr_t UPDATE_SITE_RVA = 0x362480;

static const uint8_t UPDATE_STOCK[6] = {0x55, // push ebp
                                        0x8b, 0xec, // mov  ebp,esp
                                        0x83, 0xec, 0x08}; // sub  esp,8

// The active game camera; the update hook ignores every other camera.
static const uintptr_t CAMERA_RVA = 0x6e5a70;

static const uintptr_t PITCH_OFFSET = 0x18;
static const uintptr_t DISTANCE_OFFSET = 0x20;
static const uintptr_t MAX_DISTANCE_OFFSET = 0x4c;

// Camera position bounds, which are the map's extent: Camera::pan clamps the
// camera's x against +0x5c/+0x60 and its z against +0x64/+0x68.
static const uintptr_t MAP_MIN_X_OFFSET = 0x5c;
static const uintptr_t MAP_MAX_X_OFFSET = 0x60;
static const uintptr_t MAP_MIN_Z_OFFSET = 0x64;
static const uintptr_t MAP_MAX_Z_OFFSET = 0x68;

// Renderer viewport size in pixels, as ints.
static const uintptr_t VIEWPORT_WIDTH_RVA = 0x6c7818;
static const uintptr_t VIEWPORT_HEIGHT_RVA = 0x6c781c;

// Calibration, read out of two process dumps taken with the camera zoomed to
// the furthest usable point — once looking straight down, once at the shallow
// default pitch. Both on the same 255-tile map, whose limiting extent was
// 260096 world units. See docs/features/zoom-limit.md for the capture.
static const float CAL_MAP_EXTENT = 260096.0f;
static const float CAL_PITCH_ANGLED = 36.0f;
static const float CAL_DISTANCE_ANGLED = 144000.0f;
static const float CAL_PITCH_TOPDOWN = 90.0f;
static const float CAL_DISTANCE_TOPDOWN = 234392.90625f;

static uintptr_t s_camera = 0;
static int32_t *s_viewportWidth = 0;
static int32_t *s_viewportHeight = 0;

// The limit we last wrote and the pitch we computed it for, so the hook can
// tell its own value from a fresh one the engine has installed, and knows to
// recompute when the player tilts.
static uint32_t s_writtenBits = 0;
static uint32_t s_pitchBits = 0;

// The engine's own limit, captured whenever it installs one. The computed
// limit never goes below it, so a small map cannot end up zooming out less
// than vanilla.
static float s_stockMax = 0.0f;

// Read by the hook below, so it may not be static (GAS cannot resolve local
// statics) and it must outlive the game — the patch DLL is never unloaded.
uintptr_t g_zoomUpdateReturn = 0;

// Runs at a function prologue, where the x87 stack is empty by calling
// convention, so the float arithmetic here is legal (see CLAUDE.md).
//
// The limiting extent is min(mapWidth, mapDepth * aspect) because the ground
// width visible at distance d is exactly d — the projection is built with
// D3DXMatrixPerspectiveLH(w = 1024, zNear = 1024), so tan(halfFOV) is 0.5
// horizontally — and the visible depth is that divided by the viewport aspect.
// Whichever of the two runs out first is what brings the map's edge into view.
extern "C" void zoomLimitApply(uintptr_t camera) {
    if (camera != s_camera) {
        return;
    }

    uint32_t *maxDistance = (uint32_t *)(camera + MAX_DISTANCE_OFFSET);
    uint32_t pitchBits = *(uint32_t *)(camera + PITCH_OFFSET);

    if (*maxDistance != s_writtenBits) {
        // Not ours: the engine has just installed its own limit.
        s_stockMax = *(float *)maxDistance;
    } else if (pitchBits == s_pitchBits) {
        return;
    }

    int32_t viewportWidth = *s_viewportWidth;
    int32_t viewportHeight = *s_viewportHeight;

    if (viewportWidth <= 0 || viewportHeight <= 0) {
        return;
    }

    float mapWidth = *(float *)(camera + MAP_MAX_X_OFFSET) - *(float *)(camera + MAP_MIN_X_OFFSET);
    float mapDepth = *(float *)(camera + MAP_MAX_Z_OFFSET) - *(float *)(camera + MAP_MIN_Z_OFFSET);
    float aspect = (float)viewportWidth / (float)viewportHeight;

    float extent = mapWidth;

    if (mapDepth * aspect < extent) {
        extent = mapDepth * aspect;
    }

    if (!(extent > 0.0f)) {
        return;
    }

    float pitch = *(float *)(camera + PITCH_OFFSET);

    if (pitch < CAL_PITCH_ANGLED) {
        pitch = CAL_PITCH_ANGLED;
    }

    if (pitch > CAL_PITCH_TOPDOWN) {
        pitch = CAL_PITCH_TOPDOWN;
    }

    float t = (pitch - CAL_PITCH_ANGLED) / (CAL_PITCH_TOPDOWN - CAL_PITCH_ANGLED);
    float calibrated = CAL_DISTANCE_ANGLED + t * (CAL_DISTANCE_TOPDOWN - CAL_DISTANCE_ANGLED);
    float limit = calibrated * (extent / CAL_MAP_EXTENT);

    if (limit < s_stockMax) {
        limit = s_stockMax;
    }

    memcpy(&s_writtenBits, &limit, sizeof(s_writtenBits));
    *maxDistance = s_writtenBits;
    s_pitchBits = pitchBits;

    // Tilting towards the horizon lowers the limit, and the engine only clamps
    // the distance when the player zooms — so pull the camera in here too,
    // otherwise tilting at full zoom would leave it parked past the limit.
    float *distance = (float *)(camera + DISTANCE_OFFSET);

    if (*distance > limit) {
        *distance = limit;
    }
}

// Hook site RVA 0x362480 (6 bytes: 55 8b ec 83 ec 08), the prologue of the
// camera update. ecx holds the camera; the callback is bracketed by
// pushal/pushfl so the re-emitted prologue and the `mov esi,ecx` that follows
// it see untouched registers and flags.
__declspec(naked) static void cameraUpdateHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "pushl %ecx\n\t"
                     "call _zoomLimitApply\n\t"
                     "addl $4, %esp\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "subl $8, %esp\n\t"
                     "jmp *_g_zoomUpdateReturn\n\t");
}

void installZoomLimit() {
    char mode[32] = {0};

    if (!configString("camera", "ZoomOutLimit", mode, sizeof(mode))) {
        return;
    }

    if (_stricmp(mode, "Auto") != 0) {
        return;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint8_t *site = (uint8_t *)(base + UPDATE_SITE_RVA);

    // Only patch if the stock bytes are present, so a future game update that
    // shifts this code cannot be silently corrupted.
    if (memcmp(site, UPDATE_STOCK, sizeof(UPDATE_STOCK)) != 0) {
        return;
    }

    s_camera = base + CAMERA_RVA;
    s_viewportWidth = (int32_t *)(base + VIEWPORT_WIDTH_RVA);
    s_viewportHeight = (int32_t *)(base + VIEWPORT_HEIGHT_RVA);
    g_zoomUpdateReturn = (uintptr_t)site + sizeof(UPDATE_STOCK);

    installHook(site, reinterpret_cast<void *>(cameraUpdateHook), sizeof(UPDATE_STOCK));
}
