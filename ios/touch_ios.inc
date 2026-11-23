// iOS touch controls. Included by runtime/plat.c on iOS only: uses its statics (ren, win, tex_w/h,
// vx/vy, vmove, mouse_button, up_pending, down_ms, send_key).
//
//   tap                  left click (on release, so a drag is never a click)
//   long press           right click (query / cancel)
//   two-finger tap       right click
//   one-finger drag      scroll the map, joystick style (arrow keys / Ctrl+arrows held)
//   two-finger sideways  rotate the camera (Left/Right held)
//   pinch / spread       zoom out / in, one step per PINCH_STEP of finger travel
//   button strip         Menu (Esc), Pause (P), Skip flyby (Space), Shaman (.), Save (F9); in levels only
//   iPad pointer         absolute, like the system cursor
//
// Cursor positioning. The game reads only relative mouse motion and keeps its own cursors (one per
// screen, plus copies), each clamped to a rect: the whole screen in menus, the 512x384 view in a
// software-rendered level. When the game's cursor is not where we think ("out of sync"), a touch
// first sends a huge up-left motion ("homing") that pins every cursor to the rect's corner, then the
// distance from that corner to the finger. Frames drawn while homing are not presented, so the cursor
// never visibly jumps to the corner. In sync (the usual case inside a level), the move is direct.

#define HOME_MS 40           // homing motion -> target motion (separate DirectInput batches)
#define SETTLE_MS 40         // target motion -> button press
#define HIDE_AFTER_MS 30     // keep holding the frame this long after the target motion
#define TAP_SLOP 22.0f       // points a finger may wander and still be a tap
#define LONG_MS 500          // long press -> right click
#define JOY_DEAD 28.0f       // drag distance before scrolling/rotating starts (points)
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
    SDL_RenderCoordinatesFromWindow(ren, wx, wy, x, y);
}

int g_home_count, g_touch_count;   // diagnostics (debug "stat")
static int home_phase;              // 1: homing motion sent, target pending
static float home_tx, home_ty;
static uint32_t home_ms, settle_ms;
static int click_pending = -1;      // button clicked (press + release) once the cursor has settled
static int press_pending = -1;      // pointer button pressed once settled (released by the pointer)
static int release_after;           // pointer released before its press went out

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

// ---- button strip ----
typedef struct { const char *label; SDL_Scancode sc; } Btn;
static const Btn btns[] = {
    {"MENU", SDL_SCANCODE_ESCAPE}, {"PAUSE", SDL_SCANCODE_P}, {"SKIP", SDL_SCANCODE_SPACE},
    {"SHAMAN", SDL_SCANCODE_PERIOD}, {"SAVE", SDL_SCANCODE_F9},
};
#define NBTN ((int)(sizeof btns / sizeof btns[0]))
static int btn_down = -1;
static uint32_t btn_down_ms;
static SDL_FRect btn_rect(int i) {   // window points, top-right inside the safe area
    SDL_Rect safe; int ww, wh;
    SDL_GetWindowSize(win, &ww, &wh);
    if (!SDL_GetWindowSafeArea(win, &safe)) safe = (SDL_Rect){0, 0, ww, wh};
    const float w = 66, h = 30, gap = 6;
    SDL_FRect r = {safe.x + safe.w - 6 - (NBTN - i) * (w + gap) + gap, safe.y + 6, w, h};
    return r;
}
static int btn_at(float x, float y) {
    if (!in_level()) return -1;   // buttons exist only in a level
    for (int i = 0; i < NBTN; i++) {
        SDL_FRect r = btn_rect(i);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return i;
    }
    return -1;
}
static void touch_draw_overlay(void) {
    if (!in_level()) return;
    int ww, wh, ow, oh;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    float ps = ww ? (float)ow / ww : 1;
    int lw, lh; SDL_RendererLogicalPresentation mode;
    SDL_GetRenderLogicalPresentation(ren, &lw, &lh, &mode);
    SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    float k = ps * 1.25f;   // 8x8 debug font -> 10 points
    for (int i = 0; i < NBTN; i++) {
        SDL_FRect r = btn_rect(i);
        SDL_FRect p = {r.x * ps, r.y * ps, r.w * ps, r.h * ps};
        if (i == btn_down) SDL_SetRenderDrawColor(ren, 200, 150, 40, 200);
        else SDL_SetRenderDrawColor(ren, 0, 0, 0, 120);
        SDL_RenderFillRect(ren, &p);
        SDL_SetRenderDrawColor(ren, 230, 190, 90, 200);
        SDL_RenderRect(ren, &p);
        float tw = (float)strlen(btns[i].label) * 8 * k, th = 8 * k;
        SDL_SetRenderScale(ren, k, k);
        SDL_SetRenderDrawColor(ren, 245, 225, 160, 255);
        SDL_RenderDebugText(ren, (p.x + (p.w - tw) / 2) / k, (p.y + (p.h - th) / 2) / k, btns[i].label);
        SDL_SetRenderScale(ren, 1, 1);
    }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_SetRenderLogicalPresentation(ren, lw, lh, mode);
}

// ---- gestures ----
typedef struct { SDL_FingerID id; float x0, y0, x, y; } Finger;
static Finger fing[2];
static int nfing;
enum { G_NONE, G_ONE, G_TWO, G_BUTTON, G_DONE };
static int gesture, g_moved, g_long;
static uint32_t g_t0;
static float g_d0, g_cx0, g_cy0;
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
    float d = dist2(), cdx = (fing[0].x + fing[1].x) / 2 - g_cx0;
    float spread = SDL_fabsf(d - g_d0);
    if (!g_two_mode) {
        if (spread > g_d0 * (PINCH_STEP - 1) && spread > SDL_fabsf(cdx)) g_two_mode = 1;
        else if (SDL_fabsf(cdx) > JOY_DEAD && SDL_fabsf(cdx) > spread) g_two_mode = 2;
    }
    if (g_two_mode == 1) {
        // one zoom step per PINCH_STEP of spread; the base moves by whole steps so nothing is lost
        g_moved = 1;
        while (d > g_d0 * PINCH_STEP) { zoom_step(1); g_d0 *= PINCH_STEP; }
        while (d < g_d0 / PINCH_STEP) { zoom_step(0); g_d0 /= PINCH_STEP; }
    } else if (g_two_mode == 2) {
        g_moved = 1;
        set_held(cdx < -JOY_DEAD ? HK_LEFT : cdx > JOY_DEAD ? HK_RIGHT : 0);
    }
}

static void on_finger(const SDL_Event *e) {
    int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
    float x = e->tfinger.x * ww, y = e->tfinger.y * wh;   // window points
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        // a lost finger-up would leave a ghost finger and swallow later taps: if this is the only finger
        // the system knows about, start from a clean state
        int nsys = 0; SDL_Finger **fl = SDL_GetTouchFingers(e->tfinger.touchID, &nsys);
        SDL_free(fl);
        if (nsys <= 1 && nfing) {
            set_held(0);
            nfing = 0;
            if (gesture != G_BUTTON) gesture = G_NONE;
        }
        if (nfing == 0) {
            int b = btn_at(x, y);
            fing[nfing++] = (Finger){e->tfinger.fingerID, x, y, x, y};
            if (b >= 0) { gesture = G_BUTTON; btn_down = b; btn_down_ms = now_ms(); send_key(btns[b].sc, 1); return; }
            gesture = G_ONE; g_moved = g_long = 0; g_t0 = now_ms();
            float gx, gy; win_to_game(x, y, &gx, &gy); cursor_to(gx, gy);   // cursor follows the finger (tooltips)
        } else if (nfing == 1 && gesture == G_ONE) {
            fing[nfing++] = (Finger){e->tfinger.fingerID, x, y, x, y};
            set_held(0);
            gesture = G_TWO; g_moved = 0; g_two_mode = 0; g_two_dirty = 0; g_t0 = now_ms();
            g_d0 = dist2(); if (g_d0 < 1) g_d0 = 1;
            g_cx0 = (fing[0].x + fing[1].x) / 2; g_cy0 = (fing[0].y + fing[1].y) / 2;
        } else if (nfing < 2) fing[nfing++] = (Finger){e->tfinger.fingerID, x, y, x, y};
        return;
    }
    Finger *f = finger_of(e->tfinger.fingerID);
    if (!f) return;
    if (e->type == SDL_EVENT_FINGER_MOTION) {
        f->x = x; f->y = y;
        if (gesture == G_ONE && !g_long) {
            float dx = f->x - f->x0, dy = f->y - f->y0;
            if (!g_moved && dx * dx + dy * dy > TAP_SLOP * TAP_SLOP) g_moved = 1;
            if (g_moved) {
                unsigned want = 0;
                if (dy < -JOY_DEAD) want |= HK_UP; else if (dy > JOY_DEAD) want |= HK_DOWN;
                if (dx < -JOY_DEAD) want |= HK_CTRL | HK_LEFT; else if (dx > JOY_DEAD) want |= HK_CTRL | HK_RIGHT;
                set_held(want);
            }
        } else if (gesture == G_TWO && nfing == 2) {
            g_two_dirty = 1;   // judged once per frame in two_finger_tick, after both fingers updated
        }
        return;
    }
    // finger up / cancelled
    int cancelled = e->type == SDL_EVENT_FINGER_CANCELED;
    finger_drop(e->tfinger.fingerID);
    switch (gesture) {
    case G_BUTTON:
        if (nfing == 0) {   // the key goes up in touch_tick, at least 80 ms after it went down
            gesture = G_NONE;
        }
        break;
    case G_ONE:
        set_held(0);
        if (!cancelled && !g_moved && !g_long) click(0);
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

// iPad trackpad / mouse: absolute position; a press homes first so every screen's cursor is in sync
static int on_pointer(const SDL_Event *e) {
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
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
static int touch_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_CANCELED:
        on_finger(e); return 1;
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        return on_pointer(e);
    }
    return 0;
}

static void touch_tick(void) {
    uint32_t t = now_ms();
    if (home_phase && t - home_ms >= HOME_MS) {
        home_phase = 0;
        int x0, y0, x1, y1; cursor_rect(&x0, &y0, &x1, &y1);
        vx = (float)x0; vy = (float)y0;   // where homing left the game's cursors
        vmove(home_tx, home_ty);          // the exact distance from there to the target
        post_move();
        settle_ms = t;
        hide_until = t + HIDE_AFTER_MS;
    }
    if (cursor_settled()) {
        if (click_pending >= 0) {
            int b = click_pending; click_pending = -1;
            down_ms[b] = t; mouse_button(b, 1); up_pending[b] = 1;
        }
        if (press_pending >= 0) {
            int b = press_pending; press_pending = -1;
            down_ms[b] = t; mouse_button(b, 1);
            if (release_after) up_pending[b] = 1;
        }
    }
    if (gesture == G_ONE && !g_moved && !g_long && nfing == 1 && t - g_t0 >= LONG_MS) { g_long = 1; click(1); }
    two_finger_tick();
    if (zoom_key && (int32_t)(t - zoom_until) >= 0) { send_key(zoom_key, 0); zoom_key = 0; }
    if (btn_down >= 0 && gesture != G_BUTTON && t - btn_down_ms >= 80) { send_key(btns[btn_down].sc, 0); btn_down = -1; }
}

void touch_script_tap(float gx, float gy) { cursor_to(gx, gy); click(0); }
