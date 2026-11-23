// iOS touch controls. Included by runtime/plat.c on iOS only: uses its statics (ren, win, tex_w/h,
// vx/vy, vmove, mouse_button, up_pending, down_ms, send_key).
//
//   tap                  left click (on release, so a drag is never a click)
//   long press           right click (query / cancel)
//   two-finger tap       right click
//   one-finger drag      selection box (left button held from where the finger landed), as with a mouse
//   two-finger drag      grab and move the world: it follows the fingers and stops when they stop
//                         (each PAN_STEP of travel holds an arrow / Ctrl+arrow key for PAN_PULSE_MS)
//   two-finger twist     turn the world with the fingers (Left/Right pulses per TWIST_STEP)
//   pinch / spread       zoom out / in, one step per PINCH_STEP of finger travel
//   buttons (levels only) row 1: Menu (Esc), Pause (P), Skip (Space), Shaman (.), Save (F9), World (Return)
//                         row 2: command groups 1-6
//                         thumb column, bottom left (left hand): SHIFT / CTRL / ALT. Hold while tapping with the other
//                         hand, or tap to apply to the next tap/button (double-tap locks)
//   edge scrolling       the game scrolls while its cursor sits at a screen edge; after a touch ends near
//                         an edge the cursor is moved inward so the map doesn't keep drifting
//   iPad pointer         absolute, like the system cursor
//
// Cursor positioning. The game reads only relative mouse motion and keeps its own cursors (one per
// screen, plus copies), each clamped to a rect: the whole screen in menus, the 512x384 view in a
// software-rendered level. When the game's cursor is not where we think ("out of sync"), a touch
// first sends a huge up-left motion ("homing") that pins every cursor to the rect's corner, then the
// distance from that corner to the finger. Frames drawn while homing are not presented, so the cursor
// never visibly jumps to the corner. In sync (the usual case inside a level), the move is direct.

#define THUMB_STRIP 100      // points reserved left of the game picture for the thumb column (levels)
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

int g_home_count, g_touch_count;   // diagnostics (debug "stat")
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
static float touch_left_reserve(void) { return in_level() && !kbm_active ? THUMB_STRIP : 0; }
int g_last_click_mods = -1;         // diagnostics: modifier mask of the last touch click
static uint32_t last_touch_ms;       // last finger activity
static void cursor_to(float gx, float gy) {
    g_touch_count++;
    int x0, y0, x1, y1; cursor_rect(&x0, &y0, &x1, &y1);
    if (gx < x0) gx = (float)x0; if (gx > x1 - 1) gx = (float)(x1 - 1);
    if (gy < y0) gy = (float)y0; if (gy > y1 - 1) gy = (float)(y1 - 1);
    if (home_phase) { home_tx = gx; home_ty = gy; return; }
    if (cursor_in_sync()) { vmove(gx, gy); post_move(); settle_ms = now_ms(); return; }
    raw_delta(-4096, -4096);
    g_home_count++;
    home_phase = 1; home_ms = now_ms(); home_tx = gx; home_ty = gy;
}
static int cursor_settled(void) { return !home_phase && now_ms() - settle_ms >= SETTLE_MS; }
static uint32_t hide_until;
static int frame_held(void) { return home_phase || (int32_t)(hide_until - now_ms()) > 0; }
static void click(int b) { if (up_pending[b]) mouse_button(b, 0); click_pending = b; }

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
enum { B_KEY, B_MOD };
typedef struct { const char *label; SDL_Scancode sc; int kind, row; float w; } Btn;
static const Btn btns[] = {
    {"MENU", SDL_SCANCODE_ESCAPE, B_KEY, 0, 66}, {"PAUSE", SDL_SCANCODE_P, B_KEY, 0, 66},
    {"SKIP", SDL_SCANCODE_SPACE, B_KEY, 0, 66}, {"SHAMAN", SDL_SCANCODE_PERIOD, B_KEY, 0, 66},
    {"SAVE", SDL_SCANCODE_F9, B_KEY, 0, 66}, {"WORLD", SDL_SCANCODE_RETURN, B_KEY, 0, 66},
    {"SHIFT", SDL_SCANCODE_LSHIFT, B_MOD, 2, 84}, {"CTRL", SDL_SCANCODE_LCTRL, B_MOD, 2, 84},
    {"ALT", SDL_SCANCODE_LALT, B_MOD, 2, 84},
    {"1", SDL_SCANCODE_1, B_KEY, 1, 34}, {"2", SDL_SCANCODE_2, B_KEY, 1, 34}, {"3", SDL_SCANCODE_3, B_KEY, 1, 34},
    {"4", SDL_SCANCODE_4, B_KEY, 1, 34}, {"5", SDL_SCANCODE_5, B_KEY, 1, 34}, {"6", SDL_SCANCODE_6, B_KEY, 1, 34},
};
#define NBTN ((int)(sizeof btns / sizeof btns[0]))
#define BTN_H 30
#define BTN_GAP 6
#define GROUP_GAP 0
#define THUMB_H 54     // modifier buttons: a column in the bottom-right thumb corner
#define THUMB_GAP 10
static int btn_down = -1;
static unsigned btn_mods;            // modifiers pressed with the current key button
static uint32_t btn_down_ms;
static SDL_FRect btn_rect(int i) {   // window points, right-aligned rows inside the safe area
    SDL_Rect safe; int ww, wh;
    SDL_GetWindowSize(win, &ww, &wh);
    if (!SDL_GetWindowSafeArea(win, &safe)) safe = (SDL_Rect){0, 0, ww, wh};
    if (btns[i].row == 2) {
        // thumb column for the LEFT hand (the right hand taps the game), SHIFT nearest the corner, in
        // the strip the game picture leaves free on the left in a level (touch_left_reserve)
        float w = btns[i].w, x = safe.x + (THUMB_STRIP - w) / 2;
        int slot = i - 6;   // 0 SHIFT (bottom), 1 CTRL, 2 ALT (top)
        SDL_FRect r = {x, safe.y + safe.h - 24 - THUMB_H - slot * (THUMB_H + THUMB_GAP), w, THUMB_H};
        return r;
    }
    float right = safe.x + safe.w - 6;
    for (int j = NBTN - 1; j > i; j--)
        if (btns[j].row == btns[i].row) {
            right -= btns[j].w + BTN_GAP;
            if (btns[j].kind != btns[j - 1].kind) right -= GROUP_GAP;
        }
    SDL_FRect r = {right - btns[i].w, safe.y + 6 + btns[i].row * (BTN_H + BTN_GAP), btns[i].w, BTN_H};
    return r;
}
static int btn_at(float x, float y) {
    if (!in_level()) return -1;   // buttons exist only in a level
    for (int i = 0; i < NBTN; i++) {
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
    for (int i = 0; i < NBTN; i++) {
        SDL_FRect r = btn_rect(i);
        SDL_FRect p = {r.x * ps, r.y * ps, r.w * ps, r.h * ps};
        unsigned bit = btns[i].kind == B_MOD ? 1u << (i - 6) : 0;
        if (bit && (mod_lock & bit)) SDL_SetRenderDrawColor(ren, 210, 90, 30, 230);         // locked
        else if (i == btn_down || (bit && ((mod_arm | mod_hold) & bit))) SDL_SetRenderDrawColor(ren, 200, 150, 40, 210);   // pressed / armed / held
        else SDL_SetRenderDrawColor(ren, 0, 0, 0, 120);
        SDL_RenderFillRect(ren, &p);
        SDL_SetRenderDrawColor(ren, 230, 190, 90, 200);
        SDL_RenderRect(ren, &p);
        if (bit && (mod_lock & bit)) { SDL_FRect q = {p.x + ps, p.y + ps, p.w - 2 * ps, p.h - 2 * ps}; SDL_RenderRect(ren, &q); }
        float kk = btns[i].row == 2 ? k * 1.4f : k;
        float tw = (float)strlen(btns[i].label) * 8 * kk, th = 8 * kk;
        SDL_SetRenderScale(ren, kk, kk);
        SDL_SetRenderDrawColor(ren, 245, 225, 160, 255);
        SDL_RenderDebugText(ren, (p.x + (p.w - tw) / 2) / kk, (p.y + (p.h - th) / 2) / kk, btns[i].label);
        SDL_SetRenderScale(ren, 1, 1);
    }
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
    if (btns[bf.btn].kind == B_MOD) {
        int m = bf.btn - 6; unsigned bit = 1u << m;
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

// iOS delivers each finger's motion as its own event, so between the two the spread briefly looks
// wrong; the two-finger gesture is judged once per frame on the settled positions.
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
    }
    if (g_two_mode == 1) {
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
            // grab: fingers down -> the ground comes towards you (scroll forwards); fingers right ->
            // the ground moves right (camera strafes left)
            while (cy - g_py > PAN_STEP) { EXTEND(0, PAN_PULSE_MS); g_py += PAN_STEP; }
            while (g_py - cy > PAN_STEP) { EXTEND(1, PAN_PULSE_MS); g_py -= PAN_STEP; }
            while (cx - g_px > PAN_STEP) { EXTEND(2, PAN_PULSE_MS); g_px += PAN_STEP; }
            while (g_px - cx > PAN_STEP) { EXTEND(3, PAN_PULSE_MS); g_px -= PAN_STEP; }
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

// keys for grab panning/turning, from the pulse timers (called every tick)
static void pan_keys_tick(void) {
    if (gesture != G_TWO || (g_two_mode != 2 && g_two_mode != 3)) {
        for (int k = 0; k < 4; k++) pan_until[k] = 0;
        return;
    }
    uint32_t t = now_ms();
    unsigned want = 0;
    if ((int32_t)(pan_until[0] - t) > 0) want |= HK_UP;
    else if ((int32_t)(pan_until[1] - t) > 0) want |= HK_DOWN;
    int l = (int32_t)(pan_until[2] - t) > 0, r = (int32_t)(pan_until[3] - t) > 0;
    if (l != r) want |= (l ? HK_LEFT : HK_RIGHT) | (g_two_mode == 2 ? HK_CTRL : 0);
    set_held(want);
}

static void on_finger(const SDL_Event *e) {
    last_touch_ms = now_ms(); touch_owned = 1;
    int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
    float x = e->tfinger.x * ww, y = e->tfinger.y * wh;   // window points
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        // a lost finger-up would leave a ghost finger and swallow later taps: if this is the only finger
        // the system knows about, start from a clean state
        int nsys = 0; SDL_Finger **fl = SDL_GetTouchFingers(e->tfinger.touchID, &nsys);
        SDL_free(fl);
        if (e->tfinger.touchID == 0xD0D0) nsys = 99;   // injected by the debug channel: no resync
        if (nsys <= 1) while (nbfing) bfinger_up(nbfing - 1);
        if (nsys - nbfing <= 1 && nfing) {
            set_held(0);
            nfing = 0;
            gesture = G_NONE;
        }
        int b = btn_at(x, y);
        if (b >= 0 && nbfing < 4) {
            bfing[nbfing++] = (BtnFinger){e->tfinger.fingerID, b, now_ms()};
            if (btns[b].kind == B_MOD) { mod_hold |= 1u << (b - 6); mod_used &= ~(1u << (b - 6)); }
            else { if (btn_down >= 0) btn_release(); btn_press(b); }
            return;
        }
        if (nfing == 0) {
            fing[nfing++] = (Finger){e->tfinger.fingerID, x, y, x, y};
            gesture = G_ONE; g_moved = g_long = 0; g_t0 = now_ms();
            float gx, gy; win_to_game(x, y, &gx, &gy); cursor_to(gx, gy);   // cursor follows the finger (tooltips)
        } else if (nfing == 1 && gesture == G_ONE && !g_drag) {
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
        if (gesture == G_ONE && !g_long) {
            float dx = f->x - f->x0, dy = f->y - f->y0;
            if (!g_moved && dx * dx + dy * dy > TAP_SLOP * TAP_SLOP) {
                // drag: press the left button where the finger landed (the cursor is already there)
                g_moved = 1; g_drag = 1; drag_release_ms = 0;
                if (up_pending[0]) mouse_button(0, 0);
                press_pending = 0; release_after = 0;
            }
            if (g_drag) { win_to_game(f->x, f->y, &drag_tx, &drag_ty); drag_dirty = 1; }
        } else if (gesture == G_TWO && nfing == 2) {
            g_two_dirty = 1;   // judged once per frame in two_finger_tick, after both fingers updated
        }
        return;
    }
    // finger up / cancelled
    int cancelled = e->type == SDL_EVENT_FINGER_CANCELED;
    finger_drop(e->tfinger.fingerID);
    switch (gesture) {
    case G_ONE:
        set_held(0);
        if (g_drag) {
            g_drag = 0;
            if (press_pending >= 0) release_after = 1;   // lifted before the press went out
            else {
                // final position in its own input batch, then release over it
                if (drag_dirty && di_mouse_btn[0]) { cursor_to(drag_tx, drag_ty); drag_dirty = 0; }
                drag_release_ms = now_ms() + SETTLE_MS;
            }
        } else if (!cancelled && !g_moved && !g_long) click(0);
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
int g_edge_px = 4;        // game pixels from the cursor rect's edge that count as "at the edge" (debug: edge N)
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
    case SDL_EVENT_FINGER_DOWN:
        kbm_active = 0;
        /* fall through */
    case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_CANCELED:
        on_finger(e); return 1;
    case SDL_EVENT_MOUSE_ADDED: case SDL_EVENT_MOUSE_REMOVED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED: case SDL_EVENT_WINDOW_FOCUS_LOST:
        kbm_update(); return 0;
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
        if (kbm_relative) { kbm_active = 1; touch_owned = 0; return 0; }   // desktop path, like the Mac
        return e->type == SDL_EVENT_MOUSE_WHEEL ? 0 : on_pointer(e);
    case SDL_EVENT_KEY_DOWN:
        kbm_active = 1; return 0;
    }
    return 0;
}

static void touch_tick(void) {
    uint32_t t = now_ms();
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
    if (gesture == G_ONE && !g_moved && !g_long && nfing == 1 && t - g_t0 >= LONG_MS) { g_long = 1; click(1); }
    two_finger_tick();
    pan_keys_tick();
    // drag: follow the finger once the press is out (each move in its own input batch)
    if (g_drag && drag_dirty && di_mouse_btn[0] && !home_phase && t - down_ms[0] >= SETTLE_MS) {
        cursor_to(drag_tx, drag_ty); drag_dirty = 0;
    }
    if (drag_release_ms && (int32_t)(t - drag_release_ms) >= 0) {
        drag_release_ms = 0;
        if (di_mouse_btn[0]) mouse_button(0, 0);
    }
    if (zoom_key && (int32_t)(t - zoom_until) >= 0) { send_key(zoom_key, 0); zoom_key = 0; }
    if (btn_down >= 0 && !btn_key_held && t - btn_down_ms >= 80) btn_release();
    edge_tick(t);
}

// debug: inject a finger event (window points), so multi-finger timing can be tested in the simulator
void touch_debug_finger(int down, int id, float x, float y) {
    SDL_Event e; memset(&e, 0, sizeof e);
    int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
    e.type = down == 1 ? SDL_EVENT_FINGER_DOWN : down == 0 ? SDL_EVENT_FINGER_UP : SDL_EVENT_FINGER_MOTION;
    e.tfinger.touchID = 0xD0D0; e.tfinger.fingerID = (SDL_FingerID)(1000 + id);
    e.tfinger.x = x / ww; e.tfinger.y = y / wh;
    SDL_PushEvent(&e);
}
void touch_debug_layout(FILE *out) {
    int ww, wh, ow, oh, pw, ph; SDL_Rect safe = {0};
    SDL_GetWindowSize(win, &ww, &wh); SDL_GetWindowSizeInPixels(win, &pw, &ph);
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh); SDL_GetWindowSafeArea(win, &safe);
    SDL_FRect r = btn_rect(6), m = btn_rect(0);
    fprintf(out, "dbg layout: win %dx%d px %dx%d out %dx%d safe %d,%d %dx%d shift %.0f,%.0f menu %.0f,%.0f\n",
            ww, wh, pw, ph, ow, oh, safe.x, safe.y, safe.w, safe.h, r.x, r.y, m.x, m.y);
}
void touch_script_tap(float gx, float gy) { cursor_to(gx, gy); click(0); }
