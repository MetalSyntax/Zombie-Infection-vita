/**
 * @file patch.c
 * @brief Game-specific hooks, memory patches, and custom symbol intercepts.
 */
#include <kubridge.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vitaGL.h>

#include "hle.h"
#include "imports.h"
#include "input.h"
#include "log.h"
#include "macho.h"
#include "patch.h"
#include "video.h"

// Game-specific import tables for the port being developed.
// Register any guest binary symbols you wish to intercept or replace here.
const import_entry patch_imports[] = {
    // Example:
    // F(my_custom_function, my_implementation),
};
const int patch_imports_count = sizeof(patch_imports) / sizeof(patch_imports[0]);

// Hook for PlayerTest::isCarryingVaccineTank: returns 0 if player is NULL instead of crashing.
static int hooked_isCarryingVaccineTank(void *player) {
    if (!player)
        return 0;
    return *(uint32_t *)((uint8_t *)player + 0x7e0) != 0;
}

void patch_init(void) {
    l_info("Initializing game-specific patches...");
    macho_hook("__ZN10PlayerTest21isCarryingVaccineTankEv", hooked_isCarryingVaccineTank);
}

void patch_frame(void) {
    static uint32_t last_buttons = 0;
    // Debug shortcut: L1 + R1 + TRIANGLE triggers cutscene video test
    if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_TRIANGLE)) ==
        (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_TRIANGLE)) {
        if (!(last_buttons & SCE_CTRL_TRIANGLE)) {
            l_info("Debug combo: playing cutscene cinematic_02_lab.m4v");
            video_play("ux0:data/zombieinfection/ZombieInfection.app/cinematic_02_lab.m4v");
        }
    }
    last_buttons = pad.buttons;
}

// ---------------------------------------------------------------- physical controls

// Physical controls press the game's own HUD controls: each button touches the center of the
// touch region of the matching HUD element, read at runtime. The HUD is player-rearrangeable
// (CHudManager edit mode), so fixed coordinates cannot work.
//
// CHudManager* = Application(+0xc8), Application = (*CSingletonManager::c_pTls)->+4->+0x18 (same
// chain as -[EAGLView drawView:]). Fields are HudElement*: byte +8 = visible, rect<float>
// x1,y1,x2,y2 at +0x24..+0x30 in landscape 480x320 coordinates. Candidates are tried in the order
// the game's CHudManager::get*ButtonID() uses (first visible one wins).
static const struct {
    uint32_t buttons;
    uint8_t offs[6];
} hud_map[] = {
    {SCE_CTRL_CROSS | SCE_CTRL_RTRIGGER, {0x74, 0x1c, 0x84, 0x80}}, // attack
    {SCE_CTRL_SQUARE, {0x20}},                                      // second attack
    {SCE_CTRL_CIRCLE, {0x24, 0x28, 0x30, 0x34}},                    // use / context action
    {SCE_CTRL_TRIANGLE, {0x2c, 0x3c, 0x64}},                        // hide guns (holster: Back / unDrawWeapon) or jump
    {SCE_CTRL_LTRIGGER, {0x98}},                                    // reload weapon (touch gun near map)
    {SCE_CTRL_UP, {0x78}},                                          // rotate
    {SCE_CTRL_START, {0x38}},                                       // pause menu
    {SCE_CTRL_DOWN, {0x2c, 0x3c}},                                  // holster (Back -> PlayerTest::unDrawWeapon)
};
#define HUD_STICK 0x18

typedef struct {
    float x1, y1, x2, y2;
} hud_rect;

static uint8_t *hud_manager(void) {
    static uint32_t *tls;
    if (!tls)
        tls = (uint32_t *)macho_sym("__ZN17CSingletonManager6c_pTlsE");
    uint32_t sm = tls ? *tls : 0, tbl = sm ? ((uint32_t *)sm)[1] : 0;
    uint32_t app = tbl ? *(uint32_t *)(tbl + 0x18) : 0;
    uint8_t *hud = app ? *(uint8_t **)(app + 0xc8) : NULL;
    return hud && hud[4] ? hud : NULL; // +4: HUD shown (checked by every get*ButtonID)
}

// Visible HUD element at offset off, with a sane touch region.
static int hud_element(uint8_t *hud, int off, hud_rect *r) {
    uint8_t *e = *(uint8_t **)(hud + off);
    if (!e || !e[8])
        return 0;
    memcpy(r, e + 0x24, sizeof(*r));
    return r->x2 > r->x1 && r->y2 > r->y1 && r->x1 > -480 && r->x2 < 960 && r->y1 > -320 && r->y2 < 640;
}

static void add_touch_raw(int id, int x, int y) {
    if (touch_count >= SCE_TOUCH_MAX_REPORT)
        return;
    touch_point *t = &touches[touch_count++];
    t->id = id;
    t->gx = x;
    t->gy = y;
    t->down = 1;
}

static void add_touch(int id, float x, float y) {
    add_touch_raw(id, x < 0 ? 0 : x > 479 ? 479 : (int)x, y < 0 ? 0 : y > 319 ? 319 : (int)y);
}

// Stick deflection with dead zone, -1..1.
static float axis(uint8_t v) {
    float f = (v - 128) / 127.0f;
    return f > 0.2f || f < -0.2f ? f : 0.0f;
}

// ---------------------------------------------------------------- right stick camera

// Same model as the Shadow Guardian port: one finger kept down on an empty spot of the screen and
// dragged continuously by velocity (no lift/re-press, which reset the engine's camera inertia and
// felt "mechanical"). Coordinates may leave the screen: the game only uses the drag delta.
// Dead zone + 0.35t + 0.65t^2 curve for fine aiming; float accumulator so slow tilts still move.
// Sensitivity 1..10 with SELECT + Up/Down, saved in camera_sens.txt, shown as an OSD bar.
#define LOOK_DZ 0.15f
#define LOOK_SPEED_X 9.0f // px/frame at level 10 (480x320 game space)
#define LOOK_SPEED_Y 6.0f
#define CAM_WRAP 3000 // re-anchor long before the game's vector2d<short> coordinates overflow
#define CAM_LEVEL_DEFAULT 5
#define CAM_LEVEL_MAX 10
#define CAM_CFG DATA_PATH "camera_sens.txt"
#define FID_CAM 3000

static int cam_level = CAM_LEVEL_DEFAULT, cam_active;
static float cam_x, cam_y, cam_ax = 330, cam_ay = 130;
static uint64_t cam_osd_until;

static void cam_load(void) {
    char b[8] = {0};
    SceUID fd = sceIoOpen(CAM_CFG, SCE_O_RDONLY, 0);
    if (fd < 0)
        return;
    sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    int v = atoi(b);
    if (v >= 1 && v <= CAM_LEVEL_MAX)
        cam_level = v;
}

static void cam_adjust(int d) {
    cam_level = cam_level + d < 1 ? 1 : cam_level + d > CAM_LEVEL_MAX ? CAM_LEVEL_MAX : cam_level + d;
    char b[8];
    int n = snprintf(b, sizeof(b), "%d\n", cam_level);
    SceUID fd = sceIoOpen(CAM_CFG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, b, n);
        sceIoClose(fd);
    }
    cam_osd_until = sceKernelGetProcessTimeWide() + 1500000;
    l_info("camera sensitivity %d/%d", cam_level, CAM_LEVEL_MAX);
}

static float look_curve(float v) {
    float a = v < 0 ? -v : v;
    if (a <= LOOK_DZ)
        return 0.0f;
    float t = (a - LOOK_DZ) / (1.0f - LOOK_DZ);
    t = t > 1.0f ? 1.0f : t;
    t = 0.35f * t + 0.65f * t * t;
    return v < 0 ? -t : t;
}

static int inside(const hud_rect *r, float x, float y) { return x >= r->x1 && x <= r->x2 && y >= r->y1 && y <= r->y2; }

// Anchor for the camera finger: the first candidate spot not covered by a visible HUD control.
static void cam_pick_anchor(uint8_t *hud) {
    static const float spots[][2] = {{330, 130}, {270, 100}, {380, 90}, {240, 160}, {300, 60}};
    for (unsigned s = 0; s < sizeof(spots) / sizeof(spots[0]); s++) {
        int free = 1;
        hud_rect r;
        for (unsigned i = 0; i < sizeof(hud_map) / sizeof(hud_map[0]) && free; i++)
            for (int k = 0; k < 6 && hud_map[i].offs[k] && free; k++)
                free = !(hud_element(hud, hud_map[i].offs[k], &r) && inside(&r, spots[s][0], spots[s][1]));
        if (free && hud_element(hud, HUD_STICK, &r) && inside(&r, spots[s][0], spots[s][1]))
            free = 0;
        if (free) {
            cam_ax = spots[s][0];
            cam_ay = spots[s][1];
            return;
        }
    }
}

static void cam_update(uint8_t *hud) {
    float k = (float)cam_level / CAM_LEVEL_MAX;
    float vx = hud ? look_curve((pad.rx - 128) / 127.0f) * LOOK_SPEED_X * k : 0;
    float vy = hud ? look_curve((pad.ry - 128) / 127.0f) * LOOK_SPEED_Y * k : 0;
    if (!vx && !vy) {
        cam_active = 0; // finger lifted: the touch simply stops being reported
        return;
    }
    if (!cam_active) {
        cam_pick_anchor(hud);
        cam_x = cam_ax;
        cam_y = cam_ay;
        cam_active = 1;
    } else {
        cam_x += vx;
        cam_y += vy;
        if (cam_x < cam_ax - CAM_WRAP || cam_x > cam_ax + CAM_WRAP || cam_y < cam_ay - CAM_WRAP ||
            cam_y > cam_ay + CAM_WRAP) {
            cam_active = 0; // one frame lifted, then a new drag from the anchor
            return;
        }
    }
    add_touch_raw(FID_CAM, (int)cam_x, (int)cam_y);
}

void patch_overlay(void) {
    if (!cam_osd_until || sceKernelGetProcessTimeWide() > cam_osd_until)
        return;
    GLint box[4];
    GLfloat cc[4];
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glEnable(GL_SCISSOR_TEST);
    const int bw = 22, bh = 14, gap = 4, w = CAM_LEVEL_MAX * (bw + gap) - gap;
    int x0 = (960 - w) / 2, y0 = 544 - 20 - bh; // GL origin bottom-left: 20 px from the top
    glScissor(x0 - 4, y0 - 4, w + 8, bh + 8);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    for (int i = 0; i < CAM_LEVEL_MAX; i++) {
        glScissor(x0 + i * (bw + gap), y0, bw, bh);
        i < cam_level ? glClearColor(0.2f, 0.8f, 1.0f, 1) : glClearColor(0.25f, 0.25f, 0.25f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glScissor(box[0], box[1], box[2], box[3]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glDisable(GL_SCISSOR_TEST); // gles_present restores the game's scissor enable
}

// ---------------------------------------------------------------- HUD opacity (L + R)

// The touch controls are drawn at ~0% opacity (physical buttons drive them); L + R toggles them
// fully visible. VirtualButton::draw and AnalogStick::draw pass a constant alpha of 255 to
// ASprite::PaintFrame (`add rX, rX, #255`); that immediate is rewritten in the guest code.
static const struct {
    uint32_t va, insn; // Mach-O address, original `add rX, rX, #255`
} alpha_sites[] = {
    {0x00053e60, 0xE28330FF}, // VirtualButton::draw
    {0x00052158, 0xE28CC0FF}, // AnalogStick::draw (base)
    {0x000521b0, 0xE28330FF}, // AnalogStick::draw (knob)
};
#define HUD_ALPHA_HIDDEN 0 // 0% opacity (clean screen, hidden)
static int hud_visible = -1, alpha_ok;

static void hud_set_visible(int on) {
    if (!alpha_ok || on == hud_visible)
        return;
    hud_visible = on;
    for (unsigned i = 0; i < sizeof(alpha_sites) / sizeof(alpha_sites[0]); i++) {
        uint32_t v = (alpha_sites[i].insn & ~0xFFu) | (on ? 0xFF : HUD_ALPHA_HIDDEN);
        void *a = (void *)(MACHO_SLIDE + alpha_sites[i].va);
        kuKernelCpuUnrestrictedMemcpy(a, &v, 4);
        kuKernelFlushCaches(a, 4);
    }
    l_info("touch controls %s", on ? "visible" : "hidden (1%)");
}

static void hud_alpha_init(void) {
    alpha_ok = 1;
    for (unsigned i = 0; i < sizeof(alpha_sites) / sizeof(alpha_sites[0]); i++) {
        uint32_t cur = *(uint32_t *)(MACHO_SLIDE + alpha_sites[i].va);
        if (cur != alpha_sites[i].insn) {
            l_error("HUD alpha patch: 0x%08X has 0x%08X, expected 0x%08X (disabled)",
                    (unsigned)alpha_sites[i].va, (unsigned)cur, (unsigned)alpha_sites[i].insn);
            alpha_ok = 0;
        }
    }
    hud_set_visible(0);
}

// ---------------------------------------------------------------- per frame

void patch_input(void) {
    static uint32_t prev;
    uint32_t now = pad.buttons, pressed = now & ~prev;
    prev = now;
    static int loaded;
    if (!loaded++) {
        cam_load();
        hud_alpha_init();
    }
    // L + R: show/hide the touch controls. While both are held they don't dodge/attack.
    const uint32_t LR = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    if ((now & LR) == LR) {
        if (pressed & LR)
            hud_set_visible(!hud_visible);
        pad.buttons &= ~LR;
    }
    // SELECT + Up/Down: camera sensitivity (those buttons do nothing else while SELECT is held).
    // Tapping SELECT alone toggles the pause menu (same as START).
    static int select_cam_used;
    if (pressed & SCE_CTRL_SELECT)
        select_cam_used = 0;
    if (now & SCE_CTRL_SELECT) {
        if (pressed & SCE_CTRL_UP) {
            cam_adjust(+1);
            select_cam_used = 1;
        }
        if (pressed & SCE_CTRL_DOWN) {
            cam_adjust(-1);
            select_cam_used = 1;
        }
        pad.buttons &= ~(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_SELECT);
    }
    if ((prev & SCE_CTRL_SELECT) && !(now & SCE_CTRL_SELECT)) {
        if (!select_cam_used)
            pad.buttons |= SCE_CTRL_START;
    }

    static void *(*c_level_get_instance)(void);
    static int (*c_level_is_paused)(void *level);
    static void (*c_level_set_resume)(void *level);
    static int c_level_funcs_loaded;
    if (!c_level_funcs_loaded) {
        c_level_get_instance = (void *(*)(void))macho_sym("__ZN6CLevel11GetInstanceEv");
        c_level_is_paused = (int (*)(void *))macho_sym("__ZN6CLevel8IsPausedEv");
        c_level_set_resume = (void (*)(void *))macho_sym("__ZN6CLevel9SetResumeEv");
        c_level_funcs_loaded = 1;
    }

    void *level = c_level_get_instance ? c_level_get_instance() : NULL;
    int is_paused = (level && c_level_is_paused) ? c_level_is_paused(level) : 0;

    int trigger_pause_toggle = (pressed & SCE_CTRL_START) ||
                               (((prev & SCE_CTRL_SELECT) && !(now & SCE_CTRL_SELECT)) && !select_cam_used);

    uint8_t *hud = hud_manager();
    if (trigger_pause_toggle && is_paused) {
        if (c_level_set_resume) {
            l_info("Resume game requested via START/SELECT");
            c_level_set_resume(level);
        }
    }

    // L button: reload weapon
    if (pressed & SCE_CTRL_LTRIGGER) {
        if (hud) {
            void *multi = *(void **)(hud + 0x98);
            if (multi) {
                static void (*reload_bullets)(void *);
                if (!reload_bullets)
                    reload_bullets = (void (*)(void *))macho_sym("__ZN12MultiElement13ReloadBulletsEv");
                if (reload_bullets)
                    reload_bullets(multi);
            }
        }
    }
    hud_rect r;
    if (hud) {
        for (unsigned i = 0; i < sizeof(hud_map) / sizeof(hud_map[0]); i++) {
            if (!(pad.buttons & hud_map[i].buttons))
                continue;
            for (int k = 0; k < 6 && hud_map[i].offs[k]; k++)
                if (hud_element(hud, hud_map[i].offs[k], &r)) {
                    add_touch(1000 + i, (r.x1 + r.x2) / 2, (r.y1 + r.y2) / 2);
                    break;
                }
        }
        // Left stick -> AnalogStick: touch-down at its center, then drag by the deflection.
        static int stick_down;
        float lx = axis(pad.lx), ly = axis(pad.ly);
        if ((lx || ly) && hud_element(hud, HUD_STICK, &r)) {
            float cx = (r.x1 + r.x2) / 2, cy = (r.y1 + r.y2) / 2, rad = (r.x2 - r.x1) / 2;
            add_touch(2000, stick_down ? cx + lx * rad : cx, stick_down ? cy + ly * rad : cy);
            stick_down = 1;
        } else {
            stick_down = 0;
        }
    }
    cam_update(hud);
}
