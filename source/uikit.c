/**
 * @file uikit.c
 * @brief UIKit / EAGL / MediaPlayer host classes, UIApplicationMain and the main run loop.
 * @details UIApplicationMain recreates what MainWindow.nib does on iOS (UIWindow 320x480 with an
 *          EAGLView subview, outlets window/glView on ZombieInfectionAppDelegate), sends
 *          applicationDidFinishLaunching: and then runs the run loop forever: input -> UITouch
 *          events on the EAGLView, accelerometer, NSTimers (EAGLView's drawView:), deferred UIKit
 *          callbacks (alerts, movie notifications).
 *
 *          UIView host state lives in the ivar range every UIView subclass of the game leaves to
 *          UIKit (+4..+36): frame, subviews, superview, flags.
 */
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gles.h"
#include "imports.h"
#include "input.h"
#include "log.h"
#include "macho.h"
#include "objc.h"
#include "objc_rt.h"
#include "patch.h"

#define VIEW_W 320.0f
#define VIEW_H 480.0f

static id app, app_delegate, key_window, gl_view, eagl_layer, current_context;

static double now_s(void) { return sceKernelGetProcessTimeWide() / 1e6; }

// The game's own device orientation (CTouchPad::s_currentDeviceOrientation): 1 or 3. It decides
// how the game rotates its rendering and its touch input, so the screen blit and to_view() follow it.
static int guest_orientation(void) {
    static int *o, last;
    if (!o)
        o = (int *)macho_sym("__ZN9CTouchPad26s_currentDeviceOrientationE");
    int v = o && *o == 1 ? 1 : 3;
    if (v != last)
        l_info("game device orientation: %d", v);
    last = v;
    return v;
}

// ---------------------------------------------------------------- deferred calls

typedef struct {
    id target;
    SEL sel;
    uint32_t a, b;
} deferred;
static deferred pending[32];
static int npending;

static void defer(id target, SEL sel, uint32_t a, uint32_t b) {
    if (npending < 32)
        pending[npending++] = (deferred){target, sel, a, b};
}

typedef struct {
    id target;
    const char *notif;
} deferred_notif;
static deferred_notif pending_notifs[16];
static int npending_notifs;

static void defer_notification(const char *name, id object) {
    if (npending_notifs < 16)
        pending_notifs[npending_notifs++] = (deferred_notif){object, name};
}

// ---------------------------------------------------------------- UIView

typedef struct {
    objc_class *isa;
    CGRect frame;
    id subviews, superview;
    uint32_t flags; // bit0 hidden
} ui_view;

static void view_init(id self, CGRect r) {
    ui_view *v = (ui_view *)self;
    v->frame = r;
    v->subviews = ns_array_new();
}

HM(v_init) {
    view_init(self, (CGRect){{0, 0}, {VIEW_W, VIEW_H}});
    return RET_ID(self);
}
HM(v_init_frame) {
    view_init(self, (CGRect){{ARG_F(a), ARG_F(b)}, {ARG_F(c), ARG_F(a3)}});
    return RET_ID(self);
}
HM(v_init_coder) { return v_init(self, sel, 0, 0, 0, 0, 0, 0, 0, 0); }
HM_STRET(v_frame) { *(CGRect *)ret = ((ui_view *)self)->frame; }
HM_STRET(v_bounds) { *(CGRect *)ret = (CGRect){{0, 0}, ((ui_view *)self)->frame.s}; }
HM_STRET(v_center) {
    CGRect r = ((ui_view *)self)->frame;
    *(CGPoint *)ret = (CGPoint){r.o.x + r.s.w / 2, r.o.y + r.s.h / 2};
}
HM_STRET(v_transform) {
    static const float identity[6] = {1, 0, 0, 1, 0, 0};
    memcpy(ret, identity, sizeof(identity));
}
HM(v_set_frame) {
    ((ui_view *)self)->frame = (CGRect){{ARG_F(a), ARG_F(b)}, {ARG_F(c), ARG_F(a3)}};
    return 0;
}
HM(v_set_bounds) {
    ((ui_view *)self)->frame.s = (CGSize){ARG_F(c), ARG_F(a3)};
    return 0;
}
HM(v_set_center) {
    ui_view *v = (ui_view *)self;
    v->frame.o = (CGPoint){ARG_F(a) - v->frame.s.w / 2, ARG_F(b) - v->frame.s.h / 2};
    return 0;
}
HM(v_add_subview) {
    ui_view *v = (ui_view *)self, *sub = (ui_view *)a;
    if (!v->subviews)
        v->subviews = ns_array_new();
    ns_array_add(v->subviews, (id)sub);
    if (sub)
        sub->superview = self;
    return 0;
}
HM(v_remove_from_superview) {
    ui_view *v = (ui_view *)self;
    if (v->superview)
        objc_send(((ui_view *)v->superview)->subviews, "removeObject:", (uint32_t)self, 0, 0, 0);
    v->superview = NULL;
    return 0;
}
HM(v_subviews) {
    ui_view *v = (ui_view *)self;
    return RET_ID(v->subviews ? v->subviews : (v->subviews = ns_array_new()));
}
HM(v_superview) { return RET_ID(((ui_view *)self)->superview); }
HM(v_set_hidden) {
    ((ui_view *)self)->flags = (((ui_view *)self)->flags & ~1) | (a & 1);
    return 0;
}
HM(v_is_hidden) { return ((ui_view *)self)->flags & 1; }
HM(v_layer) { return RET_ID(eagl_layer); }
HM(v_window) { return RET_ID(key_window); }
HM(v_alpha) { return RET_F(1.0f); }
HM(v_noop) { return 0; }
HM(v_self) { return RET_ID(self); }
HM(v_yes) { return 1; }
HM(v_nil) { return 0; }

// ---------------------------------------------------------------- UIApplication / UIDevice / UIScreen

static uint32_t status_orientation = 3; // UIInterfaceOrientationLandscapeRight

HM(app_shared) { return RET_ID(app); }
HM(app_delegate_get) { return RET_ID(app_delegate); }
HM(app_set_delegate) {
    app_delegate = (id)a;
    return 0;
}
HM(app_set_orientation) {
    status_orientation = a;
    l_info("UIApplication: status bar orientation %u", a);
    return 0;
}
HM(app_orientation) { return status_orientation; }
HM(app_key_window) { return RET_ID(key_window); }
HM(app_open_url) {
    l_info("UIApplication openURL: %s (ignored)", ns_cstr((id)a));
    return 0;
}
HM(win_make_key) {
    key_window = self;
    return 0;
}

static id device_obj, screen_obj;
HM(dev_current) { return RET_ID(device_obj ? device_obj : (device_obj = objc_alloc((objc_class *)self))); }
HM(dev_model) { return RET_ID(ns_string("iPhone")); }
HM(dev_system_version) { return RET_ID(ns_string("3.1.3")); }
HM(dev_udid) { return RET_ID(ns_string("0000000000000000000000000000000000000000")); }
HM(dev_orientation) { return status_orientation; }
HM(scr_main) { return RET_ID(screen_obj ? screen_obj : (screen_obj = objc_alloc((objc_class *)self))); }
HM_STRET(scr_bounds) { *(CGRect *)ret = (CGRect){{0, 0}, {VIEW_W, VIEW_H}}; }

// ---------------------------------------------------------------- EAGLContext / CAEAGLLayer

HM(ctx_init_api) {
    ((uint32_t *)self)[1] = a;
    l_info("EAGLContext initWithAPI:%u", a);
    return a == 1 ? RET_ID(self) : 0; // only kEAGLRenderingAPIOpenGLES1
}
HM(ctx_set_current) {
    current_context = (id)a;
    return 1;
}
HM(ctx_current) { return RET_ID(current_context); }
HM(ctx_storage_from_drawable) {
    gles_bind_drawable();
    return 1;
}
HM(ctx_present) {
    gles_present(guest_orientation() == 1);
    return 1;
}

// ---------------------------------------------------------------- NSTimer

typedef struct {
    objc_class *isa;
    id target, user_info;
    SEL sel;
    double interval, next;
    uint32_t repeats, valid;
} ns_timer;
static ns_timer *timers[32];
static int ntimers;

// Invalidated timers are recycled once the game has had time to drop its pointer (EAGLView swaps
// its timer every frame: allocating a new one each time leaked ~14 MB per hour).
static ns_timer *dead[64];
static double dead_at[64];
static int ndead;

static ns_timer *timer_alloc(objc_class *cls) {
    if (ndead && now_s() - dead_at[0] > 2.0) {
        ns_timer *t = dead[0];
        memmove(dead, dead + 1, --ndead * sizeof(dead[0]));
        memmove(dead_at, dead_at + 1, ndead * sizeof(dead_at[0]));
        memset(t, 0, sizeof(*t));
        t->isa = cls;
        return t;
    }
    return (ns_timer *)objc_alloc(cls);
}

// +scheduledTimerWithTimeInterval:(double r2:r3) target: selector: userInfo: repeats:
HM(timer_scheduled) {
    ns_timer *t = timer_alloc((objc_class *)self);
    t->interval = ARG_D(a, b);
    t->target = (id)c;
    t->sel = (SEL)a3;
    t->user_info = (id)a4;
    t->repeats = a5 & 0xFF;
    t->valid = 1;
    t->next = now_s() + t->interval;
    if (ntimers < 32)
        timers[ntimers++] = t;
    // EAGLView recreates its timer every frame to compensate frame time: log only the first few
    // (a flushed log line costs ~9 ms on ux0 and was halving the frame rate).
    static int logged;
    if (logged++ < 4)
        l_info("NSTimer: every %.4fs -> -[%s %s]%s", t->interval, objc_class_name(t->target), t->sel,
               t->repeats ? " (repeats)" : "");
    return RET_ID(t);
}
HM(timer_invalidate) {
    ((ns_timer *)self)->valid = 0;
    return 0;
}
HM(timer_is_valid) { return ((ns_timer *)self)->valid; }
HM(timer_user_info) { return RET_ID(((ns_timer *)self)->user_info); }

static int run_timers(void) {
    int fired = 0;
    for (int i = 0; i < ntimers; i++) {
        ns_timer *t = timers[i];
        if (t->valid && now_s() >= t->next) {
            t->next = now_s() + t->interval;
            fired = 1;
            objc_send(t->target, t->sel, (uint32_t)t, 0, 0, 0);
            if (!t->repeats)
                t->valid = 0;
        }
        if (!t->valid) {
            timers[i--] = timers[--ntimers];
            if (ndead < 64) {
                dead_at[ndead] = now_s();
                dead[ndead++] = t;
            }
        }
    }
    return fired;
}

// ---------------------------------------------------------------- UIAccelerometer

typedef struct {
    objc_class *isa;
    uint32_t pad;
    double x, y, z, ts;
} ui_accel;
static id accel_obj, accel_delegate;
static double accel_interval = 1.0 / 30, accel_next;

HM(acc_shared) { return RET_ID(accel_obj ? accel_obj : (accel_obj = objc_alloc((objc_class *)self))); }
HM(acc_set_interval) {
    accel_interval = ARG_D(a, b);
    return 0;
}
HM(acc_set_delegate) {
    accel_delegate = (id)a;
    return 0;
}
HM(acc_x) { return RET_D(((ui_accel *)self)->x); }
HM(acc_y) { return RET_D(((ui_accel *)self)->y); }
HM(acc_z) { return RET_D(((ui_accel *)self)->z); }
HM(acc_ts) { return RET_D(((ui_accel *)self)->ts); }

// Device held still. The game only uses tilt to flip its orientation (|tiltX| > 20 switches
// 1 <-> 3), so any non-zero X would rotate the screen and the controls mid-game.
static void run_accelerometer(void) {
    if (!accel_delegate || now_s() < accel_next)
        return;
    accel_next = now_s() + accel_interval;
    static ui_accel *ev;
    if (!ev)
        ev = (ui_accel *)objc_alloc(objc_host_class("UIAcceleration"));
    ev->x = 0.0;
    ev->y = 0.0;
    ev->z = -1.0;
    ev->ts = now_s();
    objc_send(accel_delegate, "accelerometer:didAccelerate:", (uint32_t)accel_obj, (uint32_t)ev, 0, 0);
}

// ---------------------------------------------------------------- UITouch / UIEvent

typedef struct {
    objc_class *isa;
    CGPoint loc, prev;
    uint32_t phase, taps;
    double ts;
    int vita_id;
} ui_touch;

HM_STRET(t_location) { *(CGPoint *)ret = ((ui_touch *)self)->loc; }
HM_STRET(t_prev_location) { *(CGPoint *)ret = ((ui_touch *)self)->prev; }
HM(t_tap_count) { return ((ui_touch *)self)->taps; }
HM(t_phase) { return ((ui_touch *)self)->phase; }
HM(t_timestamp) { return RET_D(((ui_touch *)self)->ts); }
HM(t_view) { return RET_ID(gl_view); }

static id event_obj, event_touches;
HM(ev_all_touches) { return RET_ID(event_touches); }
HM(ev_touches_for_view) { return RET_ID(event_touches); }

static ui_touch *active[SCE_TOUCH_MAX_REPORT];
static int nactive;

// Landscape point (0..480 x 0..320, as input.c maps it) -> portrait EAGLView coordinates: the
// inverse of CTouchPad::TransformCoordinatesByOrientation (3: x'=y, y'=320-x; 1: x'=480-y, y'=x),
// and consistent with the rotation gles_present applies to the framebuffer.
static CGPoint to_view(int lx, int ly) {
    return guest_orientation() == 1 ? (CGPoint){ly, VIEW_H - lx} : (CGPoint){VIEW_W - ly, lx};
}

static void send_touches(const char *sel, ui_touch **list, int n) {
    if (!n || !gl_view)
        return;
    id set = ns_array_new();
    ((objc_object *)set)->isa = objc_host_class("NSSet");
    for (int i = 0; i < n; i++)
        ns_array_add(set, (id)list[i]);
    event_touches = set;
    objc_send(gl_view, sel, (uint32_t)set, (uint32_t)event_obj, 0, 0);
}

static void run_touches(void) {
    ui_touch *began[SCE_TOUCH_MAX_REPORT], *moved[SCE_TOUCH_MAX_REPORT], *ended[SCE_TOUCH_MAX_REPORT];
    int nb = 0, nm = 0, ne = 0;
    double ts = now_s();
    // Ended: active touches whose Vita id disappeared.
    for (int i = 0; i < nactive; i++) {
        int still = 0;
        for (int j = 0; j < touch_count; j++)
            still |= touches[j].id == active[i]->vita_id;
        if (!still) {
            active[i]->phase = 3; // UITouchPhaseEnded
            active[i]->ts = ts;
            ended[ne++] = active[i];
            active[i--] = active[--nactive];
        }
    }
    for (int j = 0; j < touch_count; j++) {
        CGPoint p = to_view(touches[j].gx, touches[j].gy);
        ui_touch *t = NULL;
        for (int i = 0; i < nactive; i++)
            if (active[i]->vita_id == touches[j].id)
                t = active[i];
        if (!t) {
            t = (ui_touch *)objc_alloc(objc_host_class("UITouch"));
            *t = (ui_touch){t->isa, p, p, 0 /* UITouchPhaseBegan */, 1, ts, touches[j].id};
            active[nactive++] = t;
            began[nb++] = t;
        } else if (p.x != t->loc.x || p.y != t->loc.y) {
            t->prev = t->loc;
            t->loc = p;
            t->phase = 1; // UITouchPhaseMoved
            t->ts = ts;
            moved[nm++] = t;
        }
    }
    send_touches("touchesBegan:withEvent:", began, nb);
    send_touches("touchesMoved:withEvent:", moved, nm);
    send_touches("touchesEnded:withEvent:", ended, ne);
}

// ---------------------------------------------------------------- UIAlertView / misc UIKit

typedef struct {
    objc_class *isa;
    CGRect frame;
    id subviews, superview;
    uint32_t flags;
} ui_alert; // UIView layout; the delegate is kept apart
static id alert_delegates[8], alert_views[8];
static int nalerts;

static uint64_t alert_init(id self, SEL sel, uint32_t title, uint32_t msg, uint32_t delegate, ...) {
    view_init(self, (CGRect){{0, 0}, {0, 0}});
    l_info("UIAlertView: \"%s\" / \"%s\"", ns_cstr((id)title), ns_cstr((id)msg));
    if (nalerts < 8) {
        alert_views[nalerts] = self;
        alert_delegates[nalerts++] = (id)delegate;
    }
    return RET_ID(self);
}
// Alerts are answered at once with the cancel button (index 0): there is no UI to show them.
HM(alert_show) {
    for (int i = 0; i < nalerts; i++)
        if (alert_views[i] == self && alert_delegates[i]) {
            if (objc_responds(alert_delegates[i], "alertView:clickedButtonAtIndex:"))
                defer(alert_delegates[i], "alertView:clickedButtonAtIndex:", (uint32_t)self, 0);
            if (objc_responds(alert_delegates[i], "alertView:didDismissWithButtonIndex:"))
                defer(alert_delegates[i], "alertView:didDismissWithButtonIndex:", (uint32_t)self, 0);
        }
    return 0;
}

static id color_obj;
HM(color_any) { return RET_ID(color_obj ? color_obj : (color_obj = objc_alloc((objc_class *)self))); }

// ---------------------------------------------------------------- MediaPlayer

// Cinematics are skipped: preload and "playback finished" are reported right away.
// ponytail: no video playback; upgrade to SceAvPlayer (H.264 .m4v plays natively) for cinematics.
HM(movie_init_url) {
    l_info("MPMoviePlayerController: %s (skipped)", ns_cstr((id)a));
    defer_notification("MPMoviePlayerContentPreloadDidFinishNotification", self);
    return RET_ID(self);
}
HM(movie_play) {
    defer_notification("MPMoviePlayerPlaybackDidFinishNotification", self);
    return 0;
}
static id music_obj;
HM(music_player) { return RET_ID(music_obj ? music_obj : (music_obj = objc_alloc((objc_class *)self))); }
HM(media_empty) { return RET_ID(ns_array_new()); }

// ---------------------------------------------------------------- UIApplicationMain + run loop

static void run_loop(void) {
    l_info("run loop started");
    for (;;) {
        input_poll();
        patch_input();
        run_touches();
        run_accelerometer();
        for (int i = 0; i < npending; i++) {
            deferred d = pending[i];
            objc_send(d.target, d.sel, d.a, d.b, 0, 0);
        }
        npending = 0;
        for (int i = 0; i < npending_notifs; i++)
            ns_post_notification(pending_notifs[i].notif, pending_notifs[i].target);
        npending_notifs = 0;
        int fired = run_timers();
        ime_poll();
        patch_frame();
        if (!fired)
            sceKernelDelayThread(1000);
    }
}

static int ui_application_main(int argc, char **argv, id principal, id delegate_name) {
    l_info("UIApplicationMain: emulating MainWindow.nib");
    app = objc_alloc(objc_host_class("UIApplication"));
    event_obj = objc_alloc(objc_host_class("UIEvent"));
    eagl_layer = objc_alloc(objc_host_class("CAEAGLLayer"));

    objc_class *dc = objc_get_class(delegate_name ? ns_cstr(delegate_name) : "ZombieInfectionAppDelegate");
    app_delegate = (id)objc_send(objc_alloc(dc), "init", 0, 0, 0, 0);
    id window = (id)objc_send(objc_alloc(objc_host_class("UIWindow")), "init", 0, 0, 0, 0);
    gl_view = (id)objc_send(objc_alloc(objc_get_class("EAGLView")), "initWithCoder:", 0, 0, 0, 0);
    if (!gl_view)
        fatal_error("-[EAGLView initWithCoder:] returned nil");
    objc_send(window, "addSubview:", (uint32_t)gl_view, 0, 0, 0);
    objc_send(app_delegate, "setWindow:", (uint32_t)window, 0, 0, 0);
    objc_send(app_delegate, "setGlView:", (uint32_t)gl_view, 0, 0, 0);
    key_window = window;

    l_info("-> applicationDidFinishLaunching:");
    objc_send(app_delegate, "applicationDidFinishLaunching:", (uint32_t)app, 0, 0, 0);
    // The window becomes visible: UIKit lays out the GL view (it creates its framebuffer there).
    objc_send(gl_view, "layoutSubviews", 0, 0, 0, 0);
    if (objc_responds(app_delegate, "applicationDidBecomeActive:"))
        objc_send(app_delegate, "applicationDidBecomeActive:", (uint32_t)app, 0, 0, 0);
    run_loop();
    return 0;
}

static const host_method uikit_methods[] = {
    {"UIView", '-', "init", v_init}, {"UIView", '-', "initWithFrame:", v_init_frame},
    {"UIView", '-', "initWithCoder:", v_init_coder}, {"UIView", '-', "frame", v_frame},
    {"UIView", '-', "bounds", v_bounds}, {"UIView", '-', "center", v_center},
    {"UIView", '-', "transform", v_transform}, {"UIView", '-', "setFrame:", v_set_frame},
    {"UIView", '-', "setBounds:", v_set_bounds}, {"UIView", '-', "setCenter:", v_set_center},
    {"UIView", '-', "addSubview:", v_add_subview}, {"UIView", '-', "removeFromSuperview", v_remove_from_superview},
    {"UIView", '-', "subviews", v_subviews}, {"UIView", '-', "superview", v_superview},
    {"UIView", '-', "setHidden:", v_set_hidden}, {"UIView", '-', "isHidden", v_is_hidden},
    {"UIView", '-', "layer", v_layer}, {"UIView", '-', "window", v_window}, {"UIView", '-', "alpha", v_alpha},
    {"UIView", '-', "setNeedsDisplay", v_noop}, {"UIView", '-', "setNeedsLayout", v_noop},
    {"UIView", '-', "layoutSubviews", v_noop}, {"UIView", '-', "setTransform:", v_noop},
    {"UIView", '-', "setAlpha:", v_noop}, {"UIView", '-', "setBackgroundColor:", v_noop},
    {"UIView", '-', "setMultipleTouchEnabled:", v_noop}, {"UIView", '-', "setUserInteractionEnabled:", v_noop},
    {"UIView", '-', "setOpaque:", v_noop}, {"UIView", '-', "setTag:", v_noop},
    {"UIView", '-', "setClipsToBounds:", v_noop}, {"UIView", '-', "setExclusiveTouch:", v_noop},
    {"UIView", '-', "setContentMode:", v_noop}, {"UIView", '-', "becomeFirstResponder", v_yes},
    {"UIView", '-', "resignFirstResponder", v_yes}, {"UIView", '-', "touchesBegan:withEvent:", v_noop},
    {"UIView", '-', "touchesMoved:withEvent:", v_noop}, {"UIView", '-', "touchesEnded:withEvent:", v_noop},
    {"UIView", '-', "touchesCancelled:withEvent:", v_noop},
    {"UIWindow", '-', "makeKeyAndVisible", win_make_key},
    {"UIViewController", '-', "initWithNibName:bundle:", v_init}, {"UIViewController", '-', "init", v_init},
    {"UIViewController", '-', "view", v_self}, {"UIViewController", '-', "setView:", v_noop},
    {"UIViewController", '-', "dismissModalViewControllerAnimated:", v_noop},
    {"UIViewController", '-', "presentModalViewController:animated:", v_noop},
    {"UILabel", '-', "setText:", v_noop}, {"UILabel", '-', "setFont:", v_noop},
    {"UILabel", '-', "setTextColor:", v_noop}, {"UILabel", '-', "setTextAlignment:", v_noop},
    {"UIButton", '+', "buttonWithType:", v_init},
    {"UIActivityIndicatorView", '-', "initWithActivityIndicatorStyle:", v_init},
    {"UIActivityIndicatorView", '-', "startAnimating", v_noop}, {"UIActivityIndicatorView", '-', "stopAnimating", v_noop},
    {"UIAlertView", '-', "initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:", alert_init},
    {"UIAlertView", '-', "show", alert_show},
    {"UIColor", '+', "blackColor", color_any}, {"UIColor", '+', "whiteColor", color_any},
    {"UIColor", '+', "clearColor", color_any}, {"UIColor", '+', "grayColor", color_any},
    {"UIColor", '+', "orangeColor", color_any}, {"UIColor", '+', "colorWithRed:green:blue:alpha:", color_any},

    {"UIApplication", '+', "sharedApplication", app_shared}, {"UIApplication", '-', "delegate", app_delegate_get},
    {"UIApplication", '-', "setDelegate:", app_set_delegate},
    {"UIApplication", '-', "setStatusBarOrientation:", app_set_orientation},
    {"UIApplication", '-', "setStatusBarOrientation:animated:", app_set_orientation},
    {"UIApplication", '-', "statusBarOrientation", app_orientation},
    {"UIApplication", '-', "setStatusBarHidden:", v_noop}, {"UIApplication", '-', "setStatusBarHidden:animated:", v_noop},
    {"UIApplication", '-', "setIdleTimerDisabled:", v_noop},
    {"UIApplication", '-', "setNetworkActivityIndicatorVisible:", v_noop},
    {"UIApplication", '-', "keyWindow", app_key_window}, {"UIApplication", '-', "openURL:", app_open_url},
    {"UIDevice", '+', "currentDevice", dev_current}, {"UIDevice", '-', "model", dev_model},
    {"UIDevice", '-', "systemVersion", dev_system_version}, {"UIDevice", '-', "uniqueIdentifier", dev_udid},
    {"UIDevice", '-', "orientation", dev_orientation},
    {"UIDevice", '-', "beginGeneratingDeviceOrientationNotifications", v_noop},
    {"UIScreen", '+', "mainScreen", scr_main}, {"UIScreen", '-', "bounds", scr_bounds},
    {"UIScreen", '-', "applicationFrame", scr_bounds},

    {"EAGLContext", '-', "initWithAPI:", ctx_init_api}, {"EAGLContext", '+', "setCurrentContext:", ctx_set_current},
    {"EAGLContext", '+', "currentContext", ctx_current},
    {"EAGLContext", '-', "renderbufferStorage:fromDrawable:", ctx_storage_from_drawable},
    {"EAGLContext", '-', "presentRenderbuffer:", ctx_present},
    {"CAEAGLLayer", '-', "setOpaque:", v_noop}, {"CAEAGLLayer", '-', "setDrawableProperties:", v_noop},

    {"NSTimer", '+', "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", timer_scheduled},
    {"NSTimer", '-', "invalidate", timer_invalidate}, {"NSTimer", '-', "isValid", timer_is_valid},
    {"NSTimer", '-', "userInfo", timer_user_info},

    {"UIAccelerometer", '+', "sharedAccelerometer", acc_shared},
    {"UIAccelerometer", '-', "setUpdateInterval:", acc_set_interval},
    {"UIAccelerometer", '-', "setDelegate:", acc_set_delegate},
    {"UIAcceleration", '-', "x", acc_x}, {"UIAcceleration", '-', "y", acc_y}, {"UIAcceleration", '-', "z", acc_z},
    {"UIAcceleration", '-', "timestamp", acc_ts},

    {"UITouch", '-', "locationInView:", t_location}, {"UITouch", '-', "previousLocationInView:", t_prev_location},
    {"UITouch", '-', "tapCount", t_tap_count}, {"UITouch", '-', "phase", t_phase},
    {"UITouch", '-', "timestamp", t_timestamp}, {"UITouch", '-', "view", t_view},
    {"UIEvent", '-', "allTouches", ev_all_touches}, {"UIEvent", '-', "touchesForView:", ev_touches_for_view},

    {"MPMoviePlayerController", '-', "initWithContentURL:", movie_init_url},
    {"MPMoviePlayerController", '-', "play", movie_play}, {"MPMoviePlayerController", '-', "stop", v_noop},
    {"MPMoviePlayerController", '-', "setMovieControlMode:", v_noop},
    {"MPMoviePlayerController", '-', "setScalingMode:", v_noop},
    {"MPMusicPlayerController", '+', "iPodMusicPlayer", music_player},
    {"MPMusicPlayerController", '+', "applicationMusicPlayer", music_player},
    {"MPMusicPlayerController", '-', "playbackState", v_nil},
    {"MPMusicPlayerController", '-', "nowPlayingItem", v_nil},
    {"MPMusicPlayerController", '-', "beginGeneratingPlaybackNotifications", v_noop},
    {"MPMediaQuery", '+', "songsQuery", music_player}, {"MPMediaQuery", '+', "playlistsQuery", music_player},
    {"MPMediaQuery", '+', "albumsQuery", music_player}, {"MPMediaQuery", '+', "artistsQuery", music_player},
    {"MPMediaQuery", '-', "items", media_empty}, {"MPMediaQuery", '-', "collections", media_empty},
};

static int sc_reachability_flags(void *target, uint32_t *flags) {
    *flags = 0; // not reachable: the game stays offline
    return 1;
}
static void *sc_null(void) { return NULL; }

const import_entry uikit_imports[] = {
    F(UIApplicationMain, ui_application_main),
    F(SCNetworkReachabilityCreateWithName, sc_null),
    F(SCNetworkReachabilityGetFlags, sc_reachability_flags),
};
const int uikit_imports_count = sizeof(uikit_imports) / sizeof(uikit_imports[0]);

void uikit_init(void) {
    objc_host_class("NSSet");
    objc_register(uikit_methods, sizeof(uikit_methods) / sizeof(uikit_methods[0]));
}
