// iOS touch controls. Included by runtime/plat.c on iOS only: uses its statics (ren, win, tex_w/h,
// vx/vy, vmove, mouse_button, up_pending, down_ms, send_key).
//
//   tap                  left click (on release, so a drag is never a click)
//   one-finger drag      pan: the world follows the finger 1:1
//   press-and-hold       arms a selection box (a ring shows): drag = box from where the finger landed;
//                         lift without moving = right click (query)
//   two-finger tap       right click
//   two-finger drag      grab and move the world: it follows the fingers and stops when they stop
//                         (each PAN_STEP of travel holds an arrow / Ctrl+arrow key for PAN_PULSE_MS)
//   two-finger twist     turn the world with the fingers (Left/Right pulses per TWIST_STEP)
//   pinch / spread       zoom out / in with the fingers (3D: continuous, plat_frame_hook; else one = / - step
//                         per PINCH_STEP of spread)
//   left column (levels)  MENU (Esc), PAUSE (P), SKIP (Space), WORLD (Return) at the top; R-CLICK / HOLD:BOX
//                         at the bottom for the left thumb: tap = right click at the cursor (deselect); held:
//                         a one-finger drag draws a selection box, a tap on the game right-clicks there
//                         (SHIFT / CTRL / ALT buttons: off, THUMB_MODS)
//   edge scrolling       the game scrolls while its cursor sits at a screen edge; after a touch ends near
//                         an edge the cursor is moved inward so the map doesn't keep drifting
//   iPad pointer         absolute, like the system cursor
//   Apple Pencil         a finger (SDL_PEN_TOUCHID touches; pen->mouse events off); palm ignored while it
//                         touches; hover moves the cursor
//
// Cursor positioning. The game reads only relative mouse motion and keeps its own cursors (one per
// screen, plus copies), each clamped to a rect: the whole screen in menus, the 512x384 view in a
// software-rendered level. When the game's cursor is not where we think ("out of sync"), a touch
// first sends a huge up-left motion ("homing") that pins every cursor to the rect's corner, then the
// distance from that corner to the finger. Frames drawn while homing are not presented, so the cursor
// never visibly jumps to the corner. In sync (the usual case inside a level), the move is direct.

#include <stdarg.h>
// ---- touch log (debug "touchlog"): the last touches and what they turned into, to diagnose on a device ----
#define TLOG_N 160
static struct { uint32_t t; char s[72]; } tlog[TLOG_N];
static int tlog_n, tlog_hover;
static uint32_t tlog_cam(void) { return *(volatile uint32_t *)PTR(0x89d1ecu); }
void ios_haptic(float intensity);   // ios/haptics_ios.m
void ios_haptic_prepare(void);
#define HAPTIC_STRENGTH 1.0f   // UIKit heavy impact, full strength (light 50% and medium 70% were too faint on the iPhone)

static void tlogf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int i = tlog_n++ % TLOG_N;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    tlog[i].t = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    vsnprintf(tlog[i].s, sizeof tlog[i].s, fmt, ap);
    va_end(ap);
}
static void haptic(const char *why) { tlogf("haptic %s", why); ios_haptic(HAPTIC_STRENGTH); }

#define THUMB_STRIP 100      // points reserved left of the game picture for the button column (levels)
#define LEFT_COLUMN 1        // the button column on the left (the game picture shrinks for it in levels)
#define THUMB_MODS 0         // SHIFT/CTRL/ALT buttons (none in the column at the moment)
#define HOME_MS 40           // homing motion -> target motion (separate DirectInput batches)
#define SETTLE_MS 40         // target motion -> button press
#define HIDE_AFTER_MS 30     // keep holding the frame this long after the target motion
#define TAP_SLOP 22.0f       // points a finger may wander and still be a tap
#define LONG_MS 500          // long press -> right click
#define JOY_DEAD 28.0f       // two-finger travel before panning starts (points)
#define TWIST_DEAD 0.30f     // two-finger twist before rotating starts (radians, ~17 degrees)
#define PAN_STEP 10.0f       // finger travel (points) per pan pulse
#define PAN_PULSE_MS 45      // key hold per pan pulse
#define TWIST_STEP 0.06f     // finger turn (radians) per rotate pulse
#define TWIST_PULSE_MS 45
#define PINCH_STEP 1.15f     // finger spread ratio per zoom step
#define ZOOM_PULSE_MS 90     // how long one zoom step holds the zoom key

// ---- the game's mouse object: cursor at +0x20, clamp rect at +0x38 (x0, y0, x1, y1) ----
static uint32_t mouse_obj(void) {
    extern const char *g_exe_name;
    if (!strcasecmp(g_exe_name, "popTB.exe")) return 0x973e94u;
    if (!strcasecmp(g_exe_name, "D3DPopTB.exe")) return 0xd0595cu;   // same object layout
    return 0;
}
static void cursor_rect(int *x0, int *y0, int *x1, int *y1) {
    *x0 = 0; *y0 = 0; *x1 = tex_w; *y1 = tex_h;
    uint32_t o = mouse_obj();
    if (!o) return;
    int a = (int32_t)rd32(o + 0x38), b = (int32_t)rd32(o + 0x3c), c = (int32_t)rd32(o + 0x40), d = (int32_t)rd32(o + 0x44);
    if (a < 0 || b < 0 || c <= a || d <= b || c > 2048 || d > 2048) return;
    *x0 = a; *y0 = b; *x1 = c; *y1 = d;
}
// the game's mode: 1 in menus, world select and cutscenes, 3 while a level is loaded (also with its
// in-game menu open); the word after it is 312 in a level. Unknown executables: always "in a level".
static int in_level(void) {
    extern const char *g_exe_name;
    uint32_t a;
    if (!strcasecmp(g_exe_name, "popTB.exe")) a = 0x599ab4u;
    else if (!strcasecmp(g_exe_name, "D3DPopTB.exe")) a = 0x5d45a8u;
    else return 1;
    return rd32(a) == 3;
}
static int cursor_in_sync(void) {
    uint32_t o = mouse_obj();
    return o && (int32_t)rd32(o + 0x20) == (int)vx && (int32_t)rd32(o + 0x24) == (int)vy;
}

static void raw_delta(int dx, int dy) {
    if (dx) { di_mouse_dx += dx; di_push_mouse(0, (uint32_t)dx); }
    if (dy) { di_mouse_dy += dy; di_push_mouse(4, (uint32_t)dy); }
}
static void post_move(void) {
    msgq_post(g_hwnd, 0x200, mk_state(), (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
}
static void win_to_game(float wx, float wy, float *x, float *y) {
    map_from_window(wx, wy, x, y);
}

int g_home_count, g_touch_count, g_drag_resyncs;   // diagnostics (debug "stat")
static int home_phase;              // 1: homing motion sent, target pending
static float home_tx, home_ty;
static uint32_t home_ms, settle_ms;
static int click_pending = -1;      // button clicked (press + release) once the cursor has settled
static int press_pending = -1;      // pointer button pressed once settled (released by the pointer)
static int release_after;           // pointer released before its press went out

static int touch_owned;             // the cursor was last placed by a finger (not the iPad pointer)
// Hardware mouse / keyboard (iPad, iPhone with a mouse): with a mouse connected the pointer is locked
// (relative mode, as on the Mac) and mouse events take the regular desktop path in plat.c, untouched.
// The on-screen buttons hide while the mouse or keyboard is in use and come back on the next touch.
static int kbm_relative;            // relative mouse mode is on (a mouse is connected)
static int kbm_active;              // the last input came from the mouse or keyboard
int g_mice;                         // diagnostics
// left strip kept free of the game picture: in a level, in touch mode (mouse/keyboard: full size)
static float touch_left_reserve(void) { return LEFT_COLUMN && in_level() && !kbm_active ? THUMB_STRIP : 0; }
int g_last_click_mods = -1;         // diagnostics: modifier mask of the last touch click
static uint32_t last_touch_ms;       // last finger activity
static void cursor_to(float gx, float gy) {
    g_touch_count++;
    int x0, y0, x1, y1; cursor_rect(&x0, &y0, &x1, &y1);
    if (gx < x0) gx = (float)x0; if (gx > x1 - 1) gx = (float)(x1 - 1);
    if (gy < y0) gy = (float)y0; if (gy > y1 - 1) gy = (float)(y1 - 1);
    if (home_phase) { home_tx = gx; home_ty = gy; return; }
    if (cursor_in_sync()) { vmove(gx, gy); post_move(); settle_ms = now_ms(); return; }
    if (di_mouse_btn[0] || di_mouse_btn[1] || di_mouse_btn[2] || in_level()) {
        // No homing in a level or while a button is held: the sweep to the corner would sit on the screen
        // edge for a few frames (the game edge-scrolls the map) or press on the minimap (the camera jumps).
        // In a level there is one cursor; the game is only behind on our last moves (e.g. a hovering pencil
        // just before it touches down) or clamped one. Carry on from its own cursor once it has taken all
        // our moves.
        uint32_t o = mouse_obj();
        if (o && di_mouse_count() == 0) { vx = (float)(int32_t)rd32(o + 0x20); vy = (float)(int32_t)rd32(o + 0x24); }
        vmove(gx, gy); post_move(); settle_ms = now_ms();
        g_drag_resyncs++;
        return;
    }
    raw_delta(-4096, -4096);
    g_home_count++;
    tlogf("homing (button %d)", di_mouse_btn[0]);
    home_phase = 1; home_ms = now_ms(); home_tx = gx; home_ty = gy;
}
static int cursor_settled(void) { return !home_phase && now_ms() - settle_ms >= SETTLE_MS; }
static uint32_t hide_until;
static int frame_held(void) { return home_phase || (int32_t)(hide_until - now_ms()) > 0; }
// a selection change shortly after the player's own tap or box gets a light haptic (sel_tick); changes at
// other times (selected units dying in a fight) don't
#define SEL_WINDOW_MS 800
static uint32_t sel_until;
static void click(int b) {
    if (up_pending[b]) mouse_button(b, 0);
    click_pending = b; tlogf("click %d", b);
    if (b == 0) sel_until = now_ms() + SEL_WINDOW_MS;
}

// ---- held keys (scrolling, rotation, zoom) ----
enum { HK_UP = 1, HK_DOWN = 2, HK_LEFT = 4, HK_RIGHT = 8, HK_CTRL = 16, HK_ZIN = 32, HK_ZOUT = 64 };   // zoom bits unused (zoom_step)
static const SDL_Scancode hk_sc[7] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                                      SDL_SCANCODE_LCTRL, SDL_SCANCODE_EQUALS, SDL_SCANCODE_MINUS};
static unsigned held;
static void set_held(unsigned want) {
    unsigned rel = held & ~want, pre = want & ~held;
    for (int i = 0; i < 7; i++) if (i != 4 && (rel & (1u << i))) send_key(hk_sc[i], 0);
    if (rel & HK_CTRL) send_key(SDL_SCANCODE_LCTRL, 0);
    if (pre & HK_CTRL) send_key(SDL_SCANCODE_LCTRL, 1);
    for (int i = 0; i < 7; i++) if (i != 4 && (pre & (1u << i))) send_key(hk_sc[i], 1);
    held = want;
}

// ---- modifiers (SHIFT / CTRL / ALT for the next tap or button; double-tap locks) ----
enum { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4 };
static const SDL_Scancode mod_sc[3] = {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL, SDL_SCANCODE_LALT};
static unsigned mod_arm, mod_lock;   // armed for the next action / locked on
static unsigned mod_hold, mod_used;  // finger held on the button / the hold modified an action
static unsigned mods_down;           // modifier keys currently pressed for an action
static uint32_t mods_down_ms, mods_release_ms;
static uint32_t mod_tap_ms[3];
#define MOD_LEAD_MS 40               // modifier down -> click (separate input batch)
static unsigned mods_active(void) { return mod_arm | mod_lock | mod_hold; }
static void mods_used(unsigned m) { mod_used |= m & mod_hold; }
static void mods_press(unsigned m) {
    for (int i = 0; i < 3; i++) if ((m & (1u << i)) && !(mods_down & (1u << i))) send_key(mod_sc[i], 1);
    mods_down |= m; mods_down_ms = now_ms();
}
static void mods_release(void) {
    for (int i = 0; i < 3; i++) if (mods_down & (1u << i)) send_key(mod_sc[i], 0);
    mods_down = 0;
    mod_arm = 0;   // one-shot modifiers are used up; locked ones stay
}
static void mod_tap(int i) {
    unsigned bit = 1u << i; uint32_t t = now_ms();
    if (mod_lock & bit) { mod_lock &= ~bit; mod_arm &= ~bit; }
    else if ((mod_arm & bit) && t - mod_tap_ms[i] < 400) { mod_lock |= bit; mod_arm &= ~bit; }
    else if (mod_arm & bit) mod_arm &= ~bit;
    else mod_arm |= bit;
    mod_tap_ms[i] = t;
}

// ---- buttons ----
enum { B_KEY, B_MOD, B_RMB };
typedef struct { const char *label; SDL_Scancode sc; int kind, mod; } Btn;   // mod: modifier index (B_MOD)
static const Btn btns[] = {
    {"MENU", SDL_SCANCODE_ESCAPE, B_KEY, -1}, {"PAUSE", SDL_SCANCODE_P, B_KEY, -1},
    {"SKIP", SDL_SCANCODE_SPACE, B_KEY, -1}, {"WORLD", SDL_SCANCODE_RETURN, B_KEY, -1},
    {"R-CLICK", SDL_SCANCODE_UNKNOWN, B_RMB, -1},
};
#define NBTN ((int)(sizeof btns / sizeof btns[0]))
#define KEY_H 38       // column: key buttons at the top
#define KEY_GAP 8
#define RMB_H 110      // R-CLICK / HOLD:BOX at the bottom, in reach of the left thumb
static int rmb_held, rmb_used;      // R-CLICK is held / was used for something while held
static int btn_down = -1;
static unsigned btn_mods;            // modifiers pressed with the current key button
static uint32_t btn_down_ms;
static SDL_FRect btn_rect(int i) {   // window points: a column in the strip left of the game picture
    SDL_Rect safe; int ww, wh;
    SDL_GetWindowSize(win, &ww, &wh);
    if (!SDL_GetWindowSafeArea(win, &safe)) safe = (SDL_Rect){0, 0, ww, wh};
    float w = THUMB_STRIP - 16, x = safe.x + 8;
    if (btns[i].kind == B_RMB) return (SDL_FRect){x, safe.y + safe.h - 16 - RMB_H, w, RMB_H};
    return (SDL_FRect){x, safe.y + 8 + i * (KEY_H + KEY_GAP), w, KEY_H};
}
static int btn_at(float x, float y) {
    if (!in_level() || !LEFT_COLUMN) return -1;   // buttons exist only in a level
    for (int i = 0; i < NBTN; i++) {
        if (!THUMB_MODS && btns[i].kind == B_MOD) continue;
        SDL_FRect r = btn_rect(i);
        if (x >= r.x - 2 && x < r.x + r.w + 2 && y >= r.y - 2 && y < r.y + r.h + 2) return i;
    }
    return -1;
}
static int btn_key_held;             // a finger is still on the key button
static void btn_press(int b) {
    btn_down_ms = now_ms();
    btn_down = b; btn_key_held = 1;
    btn_mods = mods_active();
    mods_used(btn_mods);
    if (btn_mods) mods_press(btn_mods);   // key events are ordered, so the modifier is seen first
    send_key(btns[b].sc, 1);
}
static void btn_release(void) {
    send_key(btns[btn_down].sc, 0);
    if (btn_mods) mods_release();
    btn_mods = 0; btn_down = -1;
}
static void draw_hold_ring(float ps);
static void touch_draw_overlay(void) {
    if (!in_level() || kbm_active) return;
    int ww, wh, ow, oh;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetWindowSizeInPixels(win, &ow, &oh);   // the render output size is letterboxed on wide screens
    float ps = ww ? (float)ow / ww : 1;
    int lw, lh; SDL_RendererLogicalPresentation mode;
    SDL_GetRenderLogicalPresentation(ren, &lw, &lh, &mode);
    SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    float k = ps * 1.25f;   // 8x8 debug font -> 10 points
    if (!LEFT_COLUMN) return;
    for (int i = 0; i < NBTN; i++) {
        if (!THUMB_MODS && btns[i].kind == B_MOD) continue;
        SDL_FRect r = btn_rect(i);
        SDL_FRect p = {r.x * ps, r.y * ps, r.w * ps, r.h * ps};
        unsigned bit = btns[i].kind == B_MOD ? 1u << btns[i].mod : 0;
        int lit = i == btn_down || (btns[i].kind == B_RMB && rmb_held) || (bit && ((mod_arm | mod_hold) & bit));
        if (bit && (mod_lock & bit)) SDL_SetRenderDrawColor(ren, 210, 90, 30, 230);         // locked
        else if (lit) SDL_SetRenderDrawColor(ren, 200, 150, 40, 210);   // pressed / armed / held
        else SDL_SetRenderDrawColor(ren, 0, 0, 0, 120);
        SDL_RenderFillRect(ren, &p);
        SDL_SetRenderDrawColor(ren, 230, 190, 90, 200);
        SDL_RenderRect(ren, &p);
        if (bit && (mod_lock & bit)) { SDL_FRect q = {p.x + ps, p.y + ps, p.w - 2 * ps, p.h - 2 * ps}; SDL_RenderRect(ren, &q); }
        int two = btns[i].kind == B_RMB;   // "R-CLICK" / "HOLD:BOX" on two lines
        float kk = k;
        float th = 8 * kk, lh = two ? th * 1.6f : 0;
        SDL_SetRenderScale(ren, kk, kk);
        SDL_SetRenderDrawColor(ren, 245, 225, 160, 255);
        for (int ln = 0; ln <= two; ln++) {
            const char *txt = ln ? "HOLD:BOX" : btns[i].label;
            float tw = (float)strlen(txt) * 8 * kk, ty = p.y + (p.h - th - lh) / 2 + ln * lh;
            SDL_RenderDebugText(ren, (p.x + (p.w - tw) / 2) / kk, ty / kk, txt);
        }
        SDL_SetRenderScale(ren, 1, 1);
    }
    draw_hold_ring(ps);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_SetRenderLogicalPresentation(ren, lw, lh, mode);
}

// ---- gestures ----
typedef struct { SDL_FingerID id; float x0, y0, x, y; } Finger;
static Finger fing[2];
static int nfing;
enum { G_NONE, G_ONE, G_TWO, G_DONE };
// fingers resting on on-screen buttons; they don't take part in gestures, so a modifier can be held
// with one hand while the other taps or drags the game
typedef struct { SDL_FingerID id; int btn; uint32_t t0; } BtnFinger;
static BtnFinger bfing[4];
static int nbfing;
static void bfinger_up(int k) {
    BtnFinger bf = bfing[k]; bfing[k] = bfing[--nbfing];
    if (btns[bf.btn].kind == B_RMB) {
        rmb_held = 0;
        if (!rmb_used) click(1);   // tapped on its own: right click where the cursor is (deselect)
    } else if (btns[bf.btn].kind == B_MOD) {
        int m = btns[bf.btn].mod; unsigned bit = 1u << m;
        mod_hold &= ~bit;
        if (!(mod_used & bit) && now_ms() - bf.t0 < 400) mod_tap(m);   // short tap: arm / lock / unlock
        mod_used &= ~bit;
    } else if (bf.btn == btn_down) btn_key_held = 0;   // key goes up in touch_tick (>= 80 ms after down)
}
static int gesture, g_moved, g_long;
static uint32_t g_t0;
static float g_d0, g_cx0, g_cy0, g_a0;
// grab-the-world panning/turning: held keys run until these times, extended by finger travel
static uint32_t pan_until[4];        // 0 up, 1 down, 2 left, 3 right (of the camera move)
static float g_px, g_py, g_pa;       // last judged centroid / angle
// one-finger drag = left button held (selection box); the cursor follows the finger once pressed
static int g_drag, drag_dirty;
static float drag_tx, drag_ty;
int g_edge_px = 4;        // game pixels from the cursor rect's edge that count as "at the edge" (debug: edge N)
// The game scrolls the map while its cursor sits on the screen's last pixel, also while a selection box is
// dragged, and a finger at the bezel lands there easily: the map would race off. Box drags stay this far in.
static float drag_rx, drag_ry;   // finger position (game pixels) without the inset
static void drag_inset(float *x, float *y) {
    int x0, y0, x1, y1; cursor_rect(&x0, &y0, &x1, &y1);
    float m = (float)g_edge_px;
    if (*x < x0 + m) *x = x0 + m; if (*x > x1 - 1 - m) *x = x1 - 1 - m;
    if (*y < y0 + m) *y = y0 + m; if (*y > y1 - 1 - m) *y = y1 - 1 - m;
}
static uint32_t drag_release_ms;
static int g_two_mode;             // two fingers: 0 undecided, 1 pinch (zoom), 2 sideways (rotate)
static int g_two_dirty;
// zoom steps: a short press of = or - per step; steps in the same direction extend it
static SDL_Scancode zoom_key; static uint32_t zoom_until;
int g_zoom_in, g_zoom_out;   // diagnostics (debug "stat")
static void zoom_step(int in) {
    if (in) g_zoom_in++; else g_zoom_out++;
    SDL_Scancode k = in ? SDL_SCANCODE_EQUALS : SDL_SCANCODE_MINUS;
    uint32_t t = now_ms();
    if (zoom_key && zoom_key != k) { send_key(zoom_key, 0); zoom_key = 0; }
    if (!zoom_key) { send_key(k, 1); zoom_key = k; zoom_until = t; }
    zoom_until = (zoom_until > t ? zoom_until : t) + ZOOM_PULSE_MS;
}

static Finger *finger_of(SDL_FingerID id) {
    for (int i = 0; i < nfing; i++) if (fing[i].id == id) return &fing[i];
    return NULL;
}
static void finger_drop(SDL_FingerID id) {
    for (int i = 0; i < nfing; i++) if (fing[i].id == id) { fing[i] = fing[--nfing]; return; }
}
static float dist2(void) { float dx = fing[1].x - fing[0].x, dy = fing[1].y - fing[0].y; return SDL_sqrtf(dx * dx + dy * dy); }

static int cam_direct(void);
static void cam_move(float dgx, float dgy);
static void cam_turn(float radians);
static int zoom_direct(void);
static float zoom_now(void);
static float zoom_v(float u);
static float zoom_u(float v);
static void zoom_set(float u);
static float g_zd0, g_zv0;   // pinch zoom: finger spread and zoom (log scale) when the pinch engaged
// iOS delivers each finger's motion as its own event, so between the two the spread briefly looks
// wrong; the two-finger gesture is judged once per frame on the settled positions.
// grab: the world follows the point (cx, cy) (window points) from (g_px, g_py), then g_px/g_py move along.
// 3D build: 1:1 camera move; otherwise arrow-key pulses (fingers down -> the ground comes towards you,
// i.e. scroll forwards; fingers right -> the ground moves right, the camera strafes left)
static void grab_pan(float cx, float cy) {
    if (cam_direct()) {
        float gx0, gy0, gx1, gy1;
        win_to_game(g_px, g_py, &gx0, &gy0); win_to_game(cx, cy, &gx1, &gy1);
        cam_move(gx1 - gx0, gy1 - gy0);
        g_px = cx; g_py = cy;
        return;
    }
    uint32_t t = now_ms();
    #define EXTEND(k, ms) pan_until[k] = (pan_until[k] > t ? pan_until[k] : t) + (ms)
    while (cy - g_py > PAN_STEP) { EXTEND(0, PAN_PULSE_MS); g_py += PAN_STEP; }
    while (g_py - cy > PAN_STEP) { EXTEND(1, PAN_PULSE_MS); g_py -= PAN_STEP; }
    while (cx - g_px > PAN_STEP) { EXTEND(2, PAN_PULSE_MS); g_px += PAN_STEP; }
    while (g_px - cx > PAN_STEP) { EXTEND(3, PAN_PULSE_MS); g_px -= PAN_STEP; }
    #undef EXTEND
}
static int g_pan1, g_pan1_dirty;
static int g_pen1;                 // the one-finger gesture is the Apple Pencil (drags draw boxes)
// a press-and-hold has armed a selection box: a ring around the finger until it moves or lifts
static void draw_hold_ring(float ps) {
    if (gesture != G_ONE || !g_long || g_moved || !nfing) return;
    float cx = fing[0].x * ps, cy = fing[0].y * ps;
    for (int r = 0; r < 3; r++) {
        float rad = (34.0f + r) * ps;
        SDL_FPoint pts[49];
        for (int i = 0; i <= 48; i++) {
            float a = (float)i * 2 * SDL_PI_F / 48;
            pts[i] = (SDL_FPoint){cx + SDL_cosf(a) * rad, cy + SDL_sinf(a) * rad};
        }
        SDL_SetRenderDrawColor(ren, 245, 205, 90, 230);
        SDL_RenderLines(ren, pts, 49);
    }
}   // one-finger pan (a plain finger drag), finger moved since the last frame

static void two_finger_tick(void) {
    if (!g_two_dirty || gesture != G_TWO || nfing != 2) return;
    g_two_dirty = 0;
    float d = dist2();
    float cdx = (fing[0].x + fing[1].x) / 2 - g_cx0, cdy = (fing[0].y + fing[1].y) / 2 - g_cy0;
    float da = SDL_atan2f(fing[1].y - fing[0].y, fing[1].x - fing[0].x) - g_a0;
    while (da > SDL_PI_F) da -= 2 * SDL_PI_F;
    while (da < -SDL_PI_F) da += 2 * SDL_PI_F;
    float spread = SDL_fabsf(d - g_d0), pan = SDL_sqrtf(cdx * cdx + cdy * cdy);
    if (!g_two_mode) {   // the first clear movement decides, and stays until the fingers lift
        if (spread > g_d0 * (PINCH_STEP - 1) && spread > pan) g_two_mode = 1;
        else if (SDL_fabsf(da) > TWIST_DEAD) g_two_mode = 3;
        else if (pan > JOY_DEAD) g_two_mode = 2;
        if (g_two_mode) tlogf("two-finger mode %d (1 zoom 2 pan 3 twist)", g_two_mode);
        if (g_two_mode == 1) { g_zd0 = d; g_zv0 = zoom_v(zoom_now()); }   // pinch counts from here too
        if (g_two_mode == 2 || g_two_mode == 3) {
            // count movement from here: the dead zone travel would otherwise land in one frame as a jump
            g_px = (fing[0].x + fing[1].x) / 2; g_py = (fing[0].y + fing[1].y) / 2;
            g_pa = SDL_atan2f(fing[1].y - fing[0].y, fing[1].x - fing[0].x);
        }
    }
    if (g_two_mode == 1 && zoom_direct() && g_zd0 > 0) {
        // 1:1: the picture scales with the finger spread (log scale along the game's zoom stops)
        g_moved = 1;
        float v = g_zv0 - SDL_logf(d / g_zd0), vmax = zoom_v(3);
        if (v < 0 || v > vmax) {   // at an end: rebase, so turning back responds at once
            v = v < 0 ? 0 : vmax;
            g_zv0 = v; g_zd0 = d;
        }
        zoom_set(zoom_u(v));
    } else if (g_two_mode == 1) {
        // one zoom step per PINCH_STEP of spread; the base moves by whole steps so nothing is lost
        g_moved = 1;
        while (d > g_d0 * PINCH_STEP) { zoom_step(1); g_d0 *= PINCH_STEP; }
        while (d < g_d0 / PINCH_STEP) { zoom_step(0); g_d0 /= PINCH_STEP; }
    } else if (g_two_mode == 2 || g_two_mode == 3) {
        g_moved = 1;
        uint32_t t = now_ms();
        float cx = (fing[0].x + fing[1].x) / 2, cy = (fing[0].y + fing[1].y) / 2;
        float a = SDL_atan2f(fing[1].y - fing[0].y, fing[1].x - fing[0].x);
        #define EXTEND(k, ms) pan_until[k] = (pan_until[k] > t ? pan_until[k] : t) + (ms)
        if (g_two_mode == 2) {
            grab_pan(cx, cy);
        } else if (cam_direct()) {
            // twist, 1:1: turn the heading by exactly the fingers' rotation since the last frame
            float d = a - g_pa;
            while (d > SDL_PI_F) d -= 2 * SDL_PI_F;
            while (d < -SDL_PI_F) d += 2 * SDL_PI_F;
            cam_turn(d);
            g_pa = a;
        } else {
            float d = a - g_pa;
            while (d > SDL_PI_F) d -= 2 * SDL_PI_F;
            while (d < -SDL_PI_F) d += 2 * SDL_PI_F;
            while (d > TWIST_STEP) { EXTEND(2, TWIST_PULSE_MS); g_pa += TWIST_STEP; d -= TWIST_STEP; }
            while (d < -TWIST_STEP) { EXTEND(3, TWIST_PULSE_MS); g_pa -= TWIST_STEP; d += TWIST_STEP; }
        }
        #undef EXTEND
    }
}

// ---- direct camera (D3DPopTB.exe 1.01) ----
// The local player's view block: 0x89d1ec = camera centre (u16 X, u16 Z; the world wraps at 65536),
// 0x89d1f8 high 16 bits = heading (2048 per turn). Scrolling forward moves along
// (sin t, -cos t), t = (1023 - heading) * 2pi/2048. Writing the centre moves the view on the next frame,
// which lets a two-finger drag follow the fingers exactly instead of at the scroll keys' fixed speed.
// Scale (world units per game pixel of finger travel) was measured at the default zoom; ground further
// away moves less on screen, so it is a compromise between near and far.
#define CAM_KX 10.0f   // sideways
#define CAM_KY 30.0f   // forwards/backwards (foreshortened)
static int cam_direct(void) {
    extern const char *g_exe_name;
    return !strcasecmp(g_exe_name, "D3DPopTB.exe") && rd8(0x89c6f0u) == 0;   // local player 0 (single player)
}
static float cam_fx, cam_fz;   // sub-unit remainders
static void cam_move(float dgx, float dgy) {   // finger movement in game pixels
    uint32_t v = rd32(0x89d1ecu);
    float t = (1023.0f - (float)(rd32(0x89d1f8u) >> 16)) * 2 * SDL_PI_F / 2048;
    float fx = SDL_sinf(t), fz = -SDL_cosf(t), rx = SDL_cosf(t), rz = SDL_sinf(t);
    float fwd = dgy * CAM_KY, side = dgx * CAM_KX;   // fingers down -> forward; fingers right -> camera left
    cam_fx += fwd * fx + side * rx; cam_fz += fwd * fz + side * rz;
    int ix = (int)cam_fx, iz = (int)cam_fz;
    cam_fx -= ix; cam_fz -= iz;
    uint32_t x = ((v & 0xffffu) + (uint32_t)ix) & 0xffffu, z = ((v >> 16) + (uint32_t)iz) & 0xffffu;
    wr32(0x89d1ecu, (z << 16) | x);
}

// heading += fingers' turn (the game's own rotation does heading = (heading + d) & 0x7ff)
#define CAM_TURN_SIGN (-1.0f)
static float cam_fa;
static void cam_turn(float radians) {
    cam_fa += CAM_TURN_SIGN * radians * 2048.0f / (2 * SDL_PI_F);
    int d = (int)cam_fa; cam_fa -= d;
    if (!d) return;
    uint32_t w = rd32(0x89d1f8u);
    uint32_t h = ((w >> 16) + (uint32_t)d) & 0x7ffu;
    wr32(0x89d1f8u, (w & 0xffffu) | (h << 16));
}

// ---- direct zoom (D3DPopTB.exe 1.01) ----
// The game's zoom keys step through camera presets (94-byte records at 0x88f0c0, group [0x89c6ee] of 5,
// current one [0x89c6ec]); from close to far they are 3, 0, 2, 4, and each step changes distance and tilt.
// The scripted camera blends presets with f_41c700(z, 0x4000): preset 0 of the group towards preset 2
// (z > 0) or 3 (z < 0), then rebuilds the view. Pinch zoom calls it with presets copied into slots 0 and 2
// for the stretch between any two neighbours, from the game thread between frames (plat_frame_hook), and
// puts the slots back right after. Position on the chain: u = 0 (preset 3) .. 3 (preset 4).
#define ZP_BASE 0x88f0c0u
#define ZP_SIZE 94
static const int zchain[4] = {3, 0, 2, 4};
static float zoom_want = -1;        // chain position to apply on the next frame (< 0: nothing)
static float zoom_have = -1;        // last applied
int g_hook_n[4], g_zoom_applied;    // diagnostics
static uint32_t zslot(int i) { return ZP_BASE + (uint32_t)((int8_t)rd8(0x89c6eeu) * 5 + i) * ZP_SIZE; }
static int zoom_direct(void) {   // our zoom may drive the camera: 3D game, in a level, no scripted camera
    return cam_direct() && in_level() && !(rd8(0x969bd7u) & 1);
}
// How much the picture scales across each stretch (measured: people on screen 200 -> 70 px from preset 3
// to 0; 0 -> 2 is mostly tilt, so it gets a short stretch; 75 -> 32 px from 2 to 4), as natural logs, so a
// pinch that doubles the finger spread doubles the picture.
static const float zweight[3] = {1.03f, 0.40f, 0.83f};
static float zoom_v(float u) {
    float v = 0;
    for (int k = 0; k < 3; k++) { float t = u - (float)k; v += zweight[k] * (t < 0 ? 0 : t > 1 ? 1 : t); }
    return v;
}
static float zoom_u(float v) {
    for (int k = 0; k < 3; k++) { if (v <= zweight[k]) return (float)k + (v < 0 ? 0 : v / zweight[k]); v -= zweight[k]; }
    return 3;
}
static uint32_t zoom_live;          // the view's distance field after our last apply
static void zoom_set(float u) { zoom_want = u; }
static float zoom_now(void) {    // chain position of the current view
    if (zoom_have >= 0 && rd32(0x88f00cu) == zoom_live) return zoom_have;   // still ours (no key/preset since)
    zoom_have = -1;
    int idx = rd8(0x89c6ecu);
    for (int k = 0; k < 4; k++) if (zchain[k] == idx) return (float)k;
    return 1;
}
static void zoom_apply(CPU *R, float u) {
    if (u < 0) u = 0;
    if (u > 3) u = 3;
    int k = u >= 2 ? 2 : (int)u;
    float t = u - (float)k;
    uint8_t s0[ZP_SIZE], s2[ZP_SIZE], a[ZP_SIZE], b[ZP_SIZE];
    memcpy(a, PTR(zslot(zchain[k])), ZP_SIZE); memcpy(b, PTR(zslot(zchain[k + 1])), ZP_SIZE);
    memcpy(s0, PTR(zslot(0)), ZP_SIZE); memcpy(s2, PTR(zslot(2)), ZP_SIZE);
    memcpy(PTR(zslot(0)), a, ZP_SIZE); memcpy(PTR(zslot(2)), b, ZP_SIZE);
    int z = (int)(t * 0x4000 + 0.5f);
    guest_call(R, 0x41c700u, 2, (uint32_t)z, 0x4000u);
    memcpy(PTR(zslot(0)), s0, ZP_SIZE); memcpy(PTR(zslot(2)), s2, ZP_SIZE);
    wr8(0x89c6ecu, (uint8_t)zchain[(int)(u + 0.5f)]);   // the zoom keys carry on from the nearest preset
    zoom_have = u; zoom_live = rd32(0x88f00cu);
    g_zoom_applied++;
}
// called from the DirectDraw present paths (game thread, between frames)
// debug "watch ADDR [1|2|4]": log changes of a guest value once per frame into the touch log; "watch 0" clears
static uint32_t w_addr[8], w_val[8]; static int w_sz[8], w_n;
static uint32_t w_rd(int i) { return w_sz[i] == 4 ? rd32(w_addr[i]) : w_sz[i] == 2 ? rd16(w_addr[i]) : rd8(w_addr[i]); }
void touch_debug_watch(FILE *out, uint32_t a, int sz) {
    if (!a) w_n = 0;
    else if (w_n < 8) { w_addr[w_n] = a; w_sz[w_n] = sz == 4 || sz == 2 ? sz : 1; w_val[w_n] = w_rd(w_n); w_n++; }
    fprintf(out, "dbg watch %08x (%d)\n", a, w_n);
}
// The 3D game's selected-units count (the number drawn by the cursor; 0 = nothing selected). A light haptic
// when the player's tap or box changes it to a non-zero value. Switching between two single units keeps it
// at 1, so that one goes without.
#define SEL_COUNT 0x89daf9u
static void sel_tick(void) {
    static int last = -1;
    if (!cam_direct() || !in_level()) { last = -1; return; }
    int n = rd8(SEL_COUNT);
    if (last >= 0 && n != last && n > 0 && (int32_t)(now_ms() - sel_until) < 0) haptic("select");
    last = n;
}
void touch_frame_hook(CPU *R, int site) {
    if (site >= 0 && site < 4) g_hook_n[site]++;
    sel_tick();
    for (int i = 0; i < w_n; i++) {
        uint32_t v = w_rd(i);
        if (v != w_val[i]) { tlogf("watch %08x %x -> %x", w_addr[i], w_val[i], v); w_val[i] = v; }
    }
    if (zoom_want < 0) return;
    float u = zoom_want; zoom_want = -1;
    if (zoom_direct()) zoom_apply(R, u);
}
void touch_debug_zoom(FILE *out, float u) {
    zoom_want = u;
    fprintf(out, "dbg zoomu %.2f (now %.2f, idx %d, hooks %d/%d/%d/%d, applied %d)\n", u, zoom_now(), rd8(0x89c6ecu),
            g_hook_n[0], g_hook_n[1], g_hook_n[2], g_hook_n[3], g_zoom_applied);
}

// Slow edge scroll while a selection box is dragged (3D build): the cursor stays off the edge pixels (the
// game's own edge scroll is far too fast for a finger), so the camera is moved here instead, faster the
// closer the finger is to the edge: 0 at DRAG_EDGE_BAND game pixels in, DRAG_EDGE_SPEED at the bezel.
#define DRAG_EDGE_BAND 30.0f    // game pixels
#define DRAG_EDGE_SPEED 100.0f  // game pixels of view per second at the bezel (cam_move units)
int g_drag_scroll_ms;           // diagnostics
static void drag_edge_tick(uint32_t t) {
    static uint32_t last;
    if (last && (int32_t)(t - last) <= 0) return;   // input is pumped from more than one thread
    float dt = last ? (float)(t - last) / 1000.0f : 0; last = t;
    if (dt > 0.05f) dt = 0.05f;
    if (!g_drag || !di_mouse_btn[0] || !cam_direct() || !in_level() || dt <= 0) return;
    int x0, y0, x1, y1; cursor_rect(&x0, &y0, &x1, &y1);
    float b = DRAG_EDGE_BAND, ex = 0, ey = 0;
    if (drag_rx < x0 + b) ex = -(x0 + b - drag_rx) / b; else if (drag_rx > x1 - 1 - b) ex = (drag_rx - (x1 - 1 - b)) / b;
    if (drag_ry < y0 + b) ey = -(y0 + b - drag_ry) / b; else if (drag_ry > y1 - 1 - b) ey = (drag_ry - (y1 - 1 - b)) / b;
    if (ex > 1) ex = 1; if (ex < -1) ex = -1; if (ey > 1) ey = 1; if (ey < -1) ey = -1;
    if (!ex && !ey) return;
    // look towards the edge: the world moves the other way, like fingers dragging it back
    cam_move(-ex * DRAG_EDGE_SPEED * dt, -ey * DRAG_EDGE_SPEED * dt);
    g_drag_scroll_ms += (int)(dt * 1000);
}

// keys for grab panning/turning, from the pulse timers (called every tick)
static void pan_keys_tick(void) {
    if (!(gesture == G_ONE && g_pan1) && (gesture != G_TWO || (g_two_mode != 2 && g_two_mode != 3))) {
        for (int k = 0; k < 4; k++) pan_until[k] = 0;
        return;
    }
    uint32_t t = now_ms();
    unsigned want = 0;
    if ((int32_t)(pan_until[0] - t) > 0) want |= HK_UP;
    else if ((int32_t)(pan_until[1] - t) > 0) want |= HK_DOWN;
    int l = (int32_t)(pan_until[2] - t) > 0, r = (int32_t)(pan_until[3] - t) > 0;
    if (l != r) want |= (l ? HK_LEFT : HK_RIGHT) | (g_two_mode == 2 || g_pan1 ? HK_CTRL : 0);   // pans strafe
    set_held(want);
}

// Apple Pencil: SDL delivers it as touches of its own device (SDL_PEN_TOUCHID; the mouse events it would
// also make are switched off, SDL_HINT_PEN_MOUSE_EVENTS), so it works exactly like a finger: RIGHT CLICK,
// drags, taps. While it touches the game, other fingers on the game are ignored (a resting palm); fingers
// on the buttons still work.
static int pen_down;
// end a finger gesture without its tap/click (the pencil takes over)
static void gesture_abandon(void) {
    set_held(0);
    if (g_drag) {
        g_drag = 0;
        if (press_pending >= 0) release_after = 1; else drag_release_ms = now_ms() + SETTLE_MS;
    }
    g_pan1 = 0;
    nfing = 0;
    gesture = G_NONE;
}
static void on_finger(const SDL_Event *e) {
    last_touch_ms = now_ms(); touch_owned = 1;
    int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
    float x = e->tfinger.x * ww, y = e->tfinger.y * wh;   // window points
    int is_pen = e->tfinger.touchID == SDL_PEN_TOUCHID;
    if (e->type != SDL_EVENT_FINGER_MOTION)
        tlogf("%s %s %d @%.0f,%.0f nf%d g%d pen%d rmb%d bf%d cam%08x", e->type == SDL_EVENT_FINGER_DOWN ? "DOWN" : "UP",
              is_pen ? "PEN" : "fng", (int)(e->tfinger.fingerID % 1000), x, y, nfing, gesture, pen_down, rmb_held, nbfing, tlog_cam());
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        // a lost finger-up would leave a ghost finger and swallow later taps: if this is the only finger
        // the system knows about, start from a clean state. Not for the pencil: it is a touch device of its
        // own, so it would always look alone and drop the fingers holding buttons (RIGHT CLICK).
        if (!is_pen && !pen_down) {
            int nsys = 0; SDL_Finger **fl = SDL_GetTouchFingers(e->tfinger.touchID, &nsys);
            SDL_free(fl);
            if (e->tfinger.touchID == 0xD0D0) nsys = 99;   // injected by the debug channel: no resync
            if (nsys <= 1) { rmb_used = 1; while (nbfing) bfinger_up(nbfing - 1); }   // (no right click from a ghost)
            if (nsys - nbfing <= 1 && nfing) {
                set_held(0);
                nfing = 0;
                gesture = G_NONE;
            }
        }
        int b = btn_at(x, y);
        if (b >= 0 && nbfing < 4) {
            bfing[nbfing++] = (BtnFinger){e->tfinger.fingerID, b, now_ms()};
            if (btns[b].kind == B_RMB) { rmb_held = 1; rmb_used = 0; }
            else if (btns[b].kind == B_MOD) { mod_hold |= 1u << btns[b].mod; mod_used &= ~(1u << btns[b].mod); }
            else { if (btn_down >= 0) btn_release(); btn_press(b); }
            return;
        }
        if (is_pen) {   // the pencil always drives: a finger gesture in progress (a palm) gives way
            if (nfing) gesture_abandon();
            pen_down = 1;
        } else if (pen_down) { tlogf("  palm ignored"); return; }   // palm rejection: no finger gestures while the pencil touches
        if (nfing == 0) {
            fing[nfing++] = (Finger){e->tfinger.fingerID, x, y, x, y};
            gesture = G_ONE; g_moved = g_long = g_pan1 = 0; g_t0 = now_ms(); g_pen1 = is_pen;
            if (in_level()) ios_haptic_prepare();
            float gx, gy; win_to_game(x, y, &gx, &gy); cursor_to(gx, gy);   // cursor follows the finger (tooltips)
        } else if (nfing == 1 && gesture == G_ONE && !g_drag && !g_pen1) {
            // a second finger turns a one-finger pan (or a touch) into a two-finger gesture (pinch, twist)
            g_pan1 = 0;
            fing[nfing++] = (Finger){e->tfinger.fingerID, x, y, x, y};
            set_held(0);
            gesture = G_TWO; g_moved = 0; g_two_mode = 0; g_two_dirty = 0; g_t0 = now_ms();
            g_d0 = dist2(); if (g_d0 < 1) g_d0 = 1;
            g_cx0 = (fing[0].x + fing[1].x) / 2; g_cy0 = (fing[0].y + fing[1].y) / 2;
            g_a0 = SDL_atan2f(fing[1].y - fing[0].y, fing[1].x - fing[0].x);
            g_px = g_cx0; g_py = g_cy0; g_pa = g_a0;
        } else if (nfing < 2) fing[nfing++] = (Finger){e->tfinger.fingerID, x, y, x, y};
        return;
    }
    for (int k = 0; k < nbfing; k++)
        if (bfing[k].id == e->tfinger.fingerID) {
            if (e->type != SDL_EVENT_FINGER_MOTION) bfinger_up(k);
            return;
        }
    Finger *f = finger_of(e->tfinger.fingerID);
    if (!f) return;
    if (e->type == SDL_EVENT_FINGER_MOTION) {
        f->x = x; f->y = y;
        // one-finger gestures follow the finger that started them: another one landing meanwhile (a thumb
        // resting near the minimap) must not move the box or the cursor
        if (gesture == G_ONE && f == &fing[0]) {
            float dx = f->x - f->x0, dy = f->y - f->y0;
            if (!g_moved && dx * dx + dy * dy > TAP_SLOP * TAP_SLOP) {
                if (g_pen1 || rmb_held || g_long) {
                    // selection box: the pencil, the R-CLICK button held, or a press-and-hold first.
                    // Press the left button where the finger landed (the cursor is already there).
                    if (rmb_held) rmb_used = 1;
                    if (!g_long) haptic("box");   // a press-and-hold buzzed already, when it armed
                    g_moved = 1; g_drag = 1; drag_release_ms = 0;
                    if (up_pending[0]) mouse_button(0, 0);
                    press_pending = 0; release_after = 0;
                } else {
                    // a plain finger drag grabs the world from where the finger landed
                    g_moved = 1; g_pan1 = 1;
                    g_px = f->x0; g_py = f->y0;
                }
            }
            if (g_pan1) g_pan1_dirty = 1;   // applied once per frame in touch_tick
            if (g_drag) {
                win_to_game(f->x, f->y, &drag_tx, &drag_ty);
                drag_rx = drag_tx; drag_ry = drag_ty;   // unclamped, for the slow edge scroll
                drag_inset(&drag_tx, &drag_ty); drag_dirty = 1;
            }
        } else if (gesture == G_TWO && nfing == 2) {
            g_two_dirty = 1;   // judged once per frame in two_finger_tick, after both fingers updated
        }
        return;
    }
    // finger up / cancelled
    int cancelled = e->type == SDL_EVENT_FINGER_CANCELED;
    if (is_pen) pen_down = 0;
    if (gesture == G_ONE && f != &fing[0]) { finger_drop(e->tfinger.fingerID); return; }   // a stray extra finger
    finger_drop(e->tfinger.fingerID);
    switch (gesture) {
    case G_ONE:
        set_held(0);
        if (g_drag) {
            g_drag = 0;
            sel_until = now_ms() + SETTLE_MS + SEL_WINDOW_MS;
            if (press_pending >= 0) release_after = 1;   // lifted before the press went out
            else {
                // final position in its own input batch, then release over it
                if (drag_dirty && di_mouse_btn[0]) { cursor_to(drag_tx, drag_ty); drag_dirty = 0; }
                drag_release_ms = now_ms() + SETTLE_MS;
            }
        } else if (g_pan1) {
            g_pan1 = 0;
        } else if (!cancelled && !g_moved && g_long) {
            click(1);   // press-and-hold without moving: right click (query), on lift
        } else if (!cancelled && !g_moved) {
            if (rmb_held) { rmb_used = 1; click(1); }   // R-CLICK held: a tap right-clicks there (query)
            else click(0);
        }
        gesture = G_NONE;
        break;
    case G_TWO:
        set_held(0);
        if (!cancelled && !g_moved && now_ms() - g_t0 < 400) {   // two-finger tap: right click between the fingers
            float gx, gy; win_to_game(g_cx0, g_cy0, &gx, &gy); cursor_to(gx, gy); click(1);
        }
        gesture = nfing ? G_DONE : G_NONE;
        break;
    case G_DONE:
        if (nfing == 0) gesture = G_NONE;
        break;
    }
}

// ---- edge scrolling ----
int g_edge_moves;         // diagnostics
static void edge_tick(uint32_t t) {
    if (!g_edge_px || !touch_owned || nfing || home_phase || click_pending >= 0 || press_pending >= 0) return;
    if (di_mouse_btn[0] || di_mouse_btn[1] || di_mouse_btn[2] || t - last_touch_ms < 120 || !in_level()) return;
    int x0, y0, x1, y1; cursor_rect(&x0, &y0, &x1, &y1);
    float nx = vx, ny = vy, m = (float)g_edge_px;
    if (nx < x0 + m) nx = x0 + m; if (nx > x1 - 1 - m) nx = x1 - 1 - m;
    if (ny < y0 + m) ny = y0 + m; if (ny > y1 - 1 - m) ny = y1 - 1 - m;
    if (nx == vx && ny == vy) return;
    if (!cursor_in_sync()) return;   // the next touch re-homes anyway
    vmove(nx, ny); post_move();
    g_edge_moves++;
}

// iPad trackpad / mouse: absolute position; a press homes first so every screen's cursor is in sync
static int on_pointer(const SDL_Event *e) {
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        touch_owned = 0;
        float gx, gy; win_to_game(e->motion.x, e->motion.y, &gx, &gy);
        if (home_phase) { home_tx = gx; home_ty = gy; return 1; }
        int x0, y0, x1, y1; cursor_rect(&x0, &y0, &x1, &y1);
        if (gx < x0) gx = (float)x0; if (gx > x1 - 1) gx = (float)(x1 - 1);
        if (gy < y0) gy = (float)y0; if (gy > y1 - 1) gy = (float)(y1 - 1);
        vmove(gx, gy); post_move();
        return 1;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (b < 0) return 1;
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            float gx, gy; win_to_game(e->button.x, e->button.y, &gx, &gy);
            if (up_pending[b]) mouse_button(b, 0);
            cursor_to(gx, gy);
            press_pending = b; release_after = 0;
        } else if (press_pending == b) release_after = 1;
        else if (now_ms() - down_ms[b] < MIN_CLICK_MS) up_pending[b] = 1;
        else mouse_button(b, 0);
        return 1;
    }
    return 0;
}

// returns 1 when the event was handled here
static void kbm_update(void) {
    int n = 0; SDL_MouseID *m = SDL_GetMice(&n); SDL_free(m);
    g_mice = n;
    int want = n > 0 && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS);
    if (want != kbm_relative) {
        SDL_SetWindowRelativeMouseMode(win, want);
        kbm_relative = want;
        if (!want) kbm_active = 0;
    }
}
static int touch_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_PEN_MOTION:
        // a hovering pencil (where the hardware senses it) moves the cursor, so tooltips show under its tip
        if (!(e->pmotion.pen_state & SDL_PEN_INPUT_DOWN) && !nfing && gesture == G_NONE && !home_phase) {
            float gx, gy; win_to_game(e->pmotion.x, e->pmotion.y, &gx, &gy);
            touch_owned = 1; last_touch_ms = now_ms(); kbm_active = 0;
            tlog_hover++;
            cursor_to(gx, gy);
        }
        return 1;
    case SDL_EVENT_PEN_PROXIMITY_IN: case SDL_EVENT_PEN_PROXIMITY_OUT: case SDL_EVENT_PEN_DOWN: case SDL_EVENT_PEN_UP:
    case SDL_EVENT_PEN_BUTTON_DOWN: case SDL_EVENT_PEN_BUTTON_UP: case SDL_EVENT_PEN_AXIS:
        return 1;   // contact comes as touch events (SDL_PEN_TOUCHID)
    case SDL_EVENT_FINGER_DOWN:
        kbm_active = 0;
        /* fall through */
    case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_CANCELED:
        on_finger(e); return 1;
    case SDL_EVENT_MOUSE_ADDED: case SDL_EVENT_MOUSE_REMOVED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED: case SDL_EVENT_WINDOW_FOCUS_LOST:
        kbm_update(); return 0;
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
        if (e->motion.which == SDL_PEN_MOUSEID) return 1;   // pencil: handled as touches (in case SDL still sends these)
        if (kbm_relative) { kbm_active = 1; touch_owned = 0; return 0; }   // desktop path, like the Mac
        return e->type == SDL_EVENT_MOUSE_WHEEL ? 0 : on_pointer(e);
    case SDL_EVENT_KEY_DOWN:
        if (!SDL_TextInputActive(win)) kbm_active = 1;   // (the on-screen keyboard isn't a hardware keyboard)
        return 0;
    }
    return 0;
}

// The game's cursor is hidden while playing by touch or pencil, and shown while a real mouse or trackpad is
// in use: only mouse input clears touch_owned (on_pointer; the relative-mode path with a mouse connected),
// a touch sets it. Last input wins: a touch hides the cursor again, moving the mouse brings it back. (A
// hardware keyboard alone doesn't bring it back.)
static int touch_cursor_hidden(void) { return touch_owned; }

// iOS keyboard: shown while the game has a text field open (save game names): D3DPopTB.exe keeps the
// active field at 0xa6905c (0 when none). Its letters arrive as key presses plus text (WM_KEYDOWN +
// WM_CHAR), like a desktop keyboard; Return confirms, as in the game.
static void text_tick(uint32_t t) {
    extern const char *g_exe_name;
    static uint32_t last; static int shown;
    if (t - last < 100) return;
    last = t;
    int want = !strcasecmp(g_exe_name, "D3DPopTB.exe") && rd32(0xa6905cu) != 0;
    if (want == shown) return;
    shown = want;
    if (want) {
        // the field is the row just tapped (where the cursor is); iOS slides the view up so that row stays
        // above the keyboard
        float x0, y0, x1, y1;
        map_to_window(0, vy - 14, &x0, &y0);
        map_to_window((float)tex_w, vy + 30, &x1, &y1);   // (some room under the row)
        SDL_Rect r = {(int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0)};
        SDL_SetTextInputArea(win, &r, 0);
        SDL_StartTextInput(win);
    } else SDL_StopTextInput(win);
    tlogf("keyboard %s", want ? "shown" : "hidden");
}
static void touch_tick(void) {
    uint32_t t = now_ms();
    text_tick(t);
    static uint32_t pen_check;
    if (pen_down && t - pen_check > 500) {   // a lost pencil-up must not leave fingers ignored
        pen_check = t;
        int n = 0; SDL_Finger **fl = SDL_GetTouchFingers(SDL_PEN_TOUCHID, &n);
        if (fl && n == 0) pen_down = 0;   // (no list: SDL hasn't seen a real pencil, e.g. injected for tests)
        SDL_free(fl);
    }
    static uint32_t kbm_check;
    if (t - kbm_check > 1000) { kbm_check = t; kbm_update(); }   // mice present at launch, focus changes
    if (home_phase && t - home_ms >= HOME_MS) {
        home_phase = 0;
        int x0, y0, x1, y1; cursor_rect(&x0, &y0, &x1, &y1);
        vx = (float)x0; vy = (float)y0;   // where homing left the game's cursors
        vmove(home_tx, home_ty);          // the exact distance from there to the target
        post_move();
        settle_ms = t;
        hide_until = t + HIDE_AFTER_MS;
    }
    if (cursor_settled() && (click_pending >= 0 || press_pending >= 0)) {
        // armed modifiers go down first, in an earlier input batch than the button
        unsigned m = mods_active();
        if (m && (mods_down & m) != m) mods_press(m);
        mods_used(m);
        if (!m || t - mods_down_ms >= MOD_LEAD_MS) {
            if (click_pending >= 0) {
                int b = click_pending; click_pending = -1;
                g_last_click_mods = (int)m;
                down_ms[b] = t; mouse_button(b, 1); up_pending[b] = 1;
            }
            if (press_pending >= 0) {
                int b = press_pending; press_pending = -1;
                down_ms[b] = t; mouse_button(b, 1);
                if (release_after) up_pending[b] = 1;
            }
            if (m) mods_release_ms = t + MIN_CLICK_MS + 80;
        }
    }
    // modifiers of a click go up once its buttons are released
    if (mods_down && btn_down < 0 && click_pending < 0 && press_pending < 0 && (int32_t)(t - mods_release_ms) >= 0
        && !di_mouse_btn[0] && !di_mouse_btn[1] && !di_mouse_btn[2]) mods_release();
    // press-and-hold arms a selection box (drag) or a right click (lift without moving); shown by a ring
    if (gesture == G_ONE && !g_moved && !g_long && nfing == 1 && t - g_t0 >= LONG_MS) { g_long = 1; if (rmb_held) rmb_used = 1; haptic("hold"); }
    if (gesture == G_ONE && g_pan1 && g_pan1_dirty && nfing >= 1) {
        g_pan1_dirty = 0;
        grab_pan(fing[0].x, fing[0].y);
        // the cursor (and what the game draws with it, e.g. the selected-units count) follows the finger,
        // kept off the edge pixels so the game's own edge scroll doesn't start; no button is held
        if (!home_phase) {
            float gx, gy; win_to_game(fing[0].x, fing[0].y, &gx, &gy); drag_inset(&gx, &gy);
            cursor_to(gx, gy);
        }
    }
    two_finger_tick();
    pan_keys_tick();
    // drag: follow the finger once the press is out (each move in its own input batch)
    if (g_drag && drag_dirty && di_mouse_btn[0] && !home_phase && t - down_ms[0] >= SETTLE_MS) {
        cursor_to(drag_tx, drag_ty); drag_dirty = 0;
    }
    drag_edge_tick(t);
    if (drag_release_ms && (int32_t)(t - drag_release_ms) >= 0) {
        drag_release_ms = 0;
        if (di_mouse_btn[0]) mouse_button(0, 0);
    }
    if (zoom_key && (int32_t)(t - zoom_until) >= 0) { send_key(zoom_key, 0); zoom_key = 0; }
    if (btn_down >= 0 && !btn_key_held && t - btn_down_ms >= 80) btn_release();
    edge_tick(t);
}

// debug: inject a finger event (window points), so multi-finger timing can be tested in the simulator
void touch_debug_finger(int down, int id, float x, float y, int pen) {
    SDL_Event e; memset(&e, 0, sizeof e);
    int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
    e.type = down == 1 ? SDL_EVENT_FINGER_DOWN : down == 0 ? SDL_EVENT_FINGER_UP : SDL_EVENT_FINGER_MOTION;
    e.tfinger.touchID = pen ? SDL_PEN_TOUCHID : 0xD0D0; e.tfinger.fingerID = (SDL_FingerID)(1000 + id);
    e.tfinger.x = x / ww; e.tfinger.y = y / wh;
    SDL_PushEvent(&e);
}
void touch_debug_log(FILE *out) {
    int n = tlog_n < TLOG_N ? tlog_n : TLOG_N;
    fprintf(out, "dbg touchlog: %d entries, pen hover moves %d, now cam%08x\n", tlog_n, tlog_hover, tlog_cam());
    for (int k = tlog_n - n; k < tlog_n; k++) fprintf(out, "%10u %s\n", tlog[k % TLOG_N].t, tlog[k % TLOG_N].s);
}
void touch_debug_layout(FILE *out) {
    int ww, wh, ow, oh, pw, ph; SDL_Rect safe = {0};
    SDL_GetWindowSize(win, &ww, &wh); SDL_GetWindowSizeInPixels(win, &pw, &ph);
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh); SDL_GetWindowSafeArea(win, &safe);
    SDL_FRect r = btn_rect(NBTN - 1), m = btn_rect(0);
    fprintf(out, "dbg layout: win %dx%d px %dx%d out %dx%d safe %d,%d %dx%d rclick %.0f,%.0f %.0fx%.0f menu %.0f,%.0f\n",
            ww, wh, pw, ph, ow, oh, safe.x, safe.y, safe.w, safe.h, r.x, r.y, r.w, r.h, m.x, m.y);
    fprintf(out, "dbg rclick held %d used %d, button fingers %d, fingers %d, gesture %d, pan1 %d, moved %d long %d\n",
            rmb_held, rmb_used, nbfing, nfing, gesture, g_pan1, g_moved, g_long);
}
void touch_script_tap(float gx, float gy) { cursor_to(gx, gy); click(0); }
