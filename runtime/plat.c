// SDL3 platform: window, frame presentation, events -> Win32 messages and DirectInput.
#include "rt.h"
#include "input.h"
#include "gpu.h"
#include <SDL3/SDL.h>
#include <pthread.h>
#include <TargetConditionals.h>

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_Texture *overlay, *hires;   // hybrid present: CPU pixels over the GPU scene image
static SDL_GPUTexture *hires_src;
static int tex_w = 640, tex_h = 480;
static uint32_t *rgba;
int g_mouse_x = 320, g_mouse_y = 240;
uint32_t g_start_ms;
static float vx = 320, vy = 240;   // virtual cursor in game coordinates (the game draws its own)

// moves the virtual cursor and generates the relative deltas DirectInput delivers to the game
static void vmove(float nx, float ny) {
    if (nx < 0) nx = 0; if (ny < 0) ny = 0;
    if (nx > tex_w - 1) nx = (float)(tex_w - 1); if (ny > tex_h - 1) ny = (float)(tex_h - 1);
    int ox = (int)vx, oy = (int)vy;
    vx = nx; vy = ny;
    int dx = (int)vx - ox, dy = (int)vy - oy;
    g_mouse_x = (int)vx; g_mouse_y = (int)vy;
    if (dx) { di_mouse_dx += dx; di_push_mouse(0, (uint32_t)dx); }
    if (dy) { di_mouse_dy += dy; di_push_mouse(4, (uint32_t)dy); }
}

// input script: "ms:key:NAME;ms:click:x,y;ms:rclick:x,y;ms:move:x,y;ms:quit"
typedef struct { uint32_t t; int kind; int a, b; } ScriptEv;
static ScriptEv script[256];
static int nscript, iscript;
static int ev_cmp(const void *a, const void *b) { const ScriptEv *x = a, *y = b; return x->t < y->t ? -1 : x->t > y->t; }
void plat_script(const char *spec) {
    char buf[4096]; snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *it = strtok_r(buf, ";", &save); it && nscript < 256; it = strtok_r(NULL, ";", &save)) {
        char kind[32] = "", arg[64] = "";
        unsigned t = 0;
        if (sscanf(it, "%u:%31[^:]:%63s", &t, kind, arg) < 2) continue;
        ScriptEv *e = &script[nscript++];
        e->t = t;
        if (!strcmp(kind, "key")) { e->kind = 1; e->a = (int)SDL_GetScancodeFromName(arg); }
        else if (!strcmp(kind, "click") || !strcmp(kind, "rclick")) {
            // button: move at t, press at t+80, release after hold ms (x,y[,hold], default 120)
            // the game applies deltas only at the end of each DirectInput batch: move first, press after
            int right = kind[0] == 'r', hold = 120;
            e->kind = 4; sscanf(arg, "%d,%d,%d", &e->a, &e->b, &hold);
            ScriptEv *d = &script[nscript++];
            *d = *e; d->t = t + 80; d->kind = right ? 3 : 2;
            ScriptEv *u = &script[nscript++];
            *u = *e; u->t = t + 80 + (uint32_t)hold; u->kind = right ? 7 : 6;
        }
        else if (!strcmp(kind, "move")) { e->kind = 4; sscanf(arg, "%d,%d", &e->a, &e->b); }
        else if (!strcmp(kind, "quit")) e->kind = 5;
        else if (!strcmp(kind, "scan")) e->kind = 8;
        else if (!strcmp(kind, "snap")) e->kind = 9;
        else if (!strcmp(kind, "diff")) e->kind = 10;
        else if (!strcmp(kind, "ttap")) { e->kind = 11; sscanf(arg, "%d,%d", &e->a, &e->b); }   // touch tap (iOS)
    }
    qsort(script, nscript, sizeof script[0], ev_cmp);
}
// debug ("ms:scan:"): finds the game's own cursor variable. Keeps the guest addresses whose 32-bit or
// 16-bit value pair equals the virtual cursor (x, y), intersected with the previous scan.
static uint32_t scan_cand[4096]; static uint8_t scan_kind[4096]; static int nscan = -1;
static void cursor_scan(void) {
    extern uint32_t brk_ptr;
    int x = (int)vx, y = (int)vy, n = 0;
    uint32_t ranges[2][2] = {{0x00400000u, 0x01000000u}, {0x01000000u, brk_ptr}};
    if (nscan < 0) {
        for (int r = 0; r < 2; r++)
            for (uint32_t a = ranges[r][0]; a + 8 <= ranges[r][1] && n < 4096; a += 2) {
                if (a >= 0x00400000u + 0x400000u && r == 0) break;
                if ((a & 3) == 0 && (int32_t)rd32(a) == x && (int32_t)rd32(a + 4) == y) { scan_cand[n] = a; scan_kind[n++] = 4; }
                else if ((int16_t)rd16(a) == x && (int16_t)rd16(a + 2) == y) { scan_cand[n] = a; scan_kind[n++] = 2; }
            }
    } else {
        for (int i = 0; i < nscan; i++) {
            uint32_t a = scan_cand[i];
            int ok = scan_kind[i] == 4 ? ((int32_t)rd32(a) == x && (int32_t)rd32(a + 4) == y)
                                       : ((int16_t)rd16(a) == x && (int16_t)rd16(a + 2) == y);
            if (ok) { scan_cand[n] = a; scan_kind[n++] = scan_kind[i]; }
        }
    }
    nscan = n;
    fprintf(stderr, "scan at (%d,%d) game cursor (%d,%d) size %dx%d: %d candidates", x, y,
            (int32_t)rd32(0x5c4d40), (int32_t)rd32(0x5c4d44), tex_w, tex_h, n);
    for (int i = 0; i < n && i < 24; i++) fprintf(stderr, " %08x/%d", scan_cand[i], scan_kind[i]);
    fprintf(stderr, "\n");
}
// debug ("ms:snap:" then "ms:diff:"): lists guest int pairs (32- or 16-bit) whose change since the snapshot
// has the same signs as the motion sent to the game since then (finds cursors kept by other screens).
static uint8_t *snap_buf; static uint32_t snap_end; static int snap_x, snap_y;
static void cursor_snap(void) {
    extern uint32_t brk_ptr;
    snap_end = brk_ptr; free(snap_buf);
    snap_buf = malloc(snap_end - 0x00400000u);
    memcpy(snap_buf, PTR(0x00400000u), snap_end - 0x00400000u);
    snap_x = (int)vx; snap_y = (int)vy;
    fprintf(stderr, "snap at (%d,%d)\n", snap_x, snap_y);
}
static void cursor_diff(void) {
    if (!snap_buf) return;
    int dx = (int)vx - snap_x, dy = (int)vy - snap_y, n = 0;
    fprintf(stderr, "diff for motion (%d,%d):", dx, dy);
    for (uint32_t a = 0x00400000u; a + 8 <= snap_end && n < 40; a += 2) {
        const uint8_t *o = snap_buf + (a - 0x00400000u);
        int ox, oy, nx, ny, w;
        if ((a & 3) == 0) { ox = (int32_t)(o[0] | o[1] << 8 | o[2] << 16 | (uint32_t)o[3] << 24); oy = (int32_t)(o[4] | o[5] << 8 | o[6] << 16 | (uint32_t)o[7] << 24);
                            nx = (int32_t)rd32(a); ny = (int32_t)rd32(a + 4); w = 4; }
        else { ox = (int16_t)(o[0] | o[1] << 8); oy = (int16_t)(o[2] | o[3] << 8); nx = (int16_t)rd16(a); ny = (int16_t)rd16(a + 2); w = 2; }
        int cx = nx - ox, cy = ny - oy;
        if (!cx || !cy || (cx > 0) != (dx > 0) || (cy > 0) != (dy > 0)) continue;
        if (abs(cx) > 4 * abs(dx) + 4 || abs(cy) > 4 * abs(dy) + 4 || nx < -2000 || nx > 4000) continue;
        fprintf(stderr, " %08x/%d:(%d,%d)->(%d,%d)", a, w, ox, oy, nx, ny); n++;
        if (w == 4) a += 2;
    }
    fprintf(stderr, "\n");
}
// debug: commands read from $HOME/Documents/poptb_cmd.txt (checked twice a second, then deleted):
//   find X Y T   fresh search for 32/16-bit pairs within +-T of (X,Y)     filt X Y T   keep those still matching
//   peek ADDR N  dumps N 32-bit words                                     poke ADDR V  writes a 32-bit word
//   rect         lists int quads that look like a clamp rectangle (x0<x1<=640, y0<y1<=480)
static void mouse_button(int b, int down);
static uint32_t mk_state(void);
static void send_key(SDL_Scancode sc, int down);
SDL_Scancode g_hold_sc; uint32_t g_hold_until;
static uint32_t dc[8192]; static uint8_t dk[8192]; static int ndc;
static int near(int v, int t, int tol) { return v >= t - tol && v <= t + tol; }
static void debug_cmds(void) {
    static uint32_t last;
    if (now_ms() - last < 500) return;
    last = now_ms();
    char p[1100]; snprintf(p, sizeof p, "%s/Documents/poptb_cmd.txt", getenv("HOME") ? getenv("HOME") : ".");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[256];
    char op[1100]; snprintf(op, sizeof op, "%s/Documents/poptb_dbg.txt", getenv("HOME") ? getenv("HOME") : ".");
    FILE *out = fopen(op, "a"); if (!out) out = stderr;
    extern uint32_t brk_ptr;
    while (fgets(line, sizeof line, f)) {
        int x = 0, y = 0, t = 0; unsigned a = 0, v = 0;
        if (sscanf(line, "find %d %d %d", &x, &y, &t) == 3 || sscanf(line, "filt %d %d %d", &x, &y, &t) == 3) {
            int fresh = line[0] == 'f' && line[1] == 'i' && line[2] == 'n', n = 0;
            if (fresh) {
                for (uint32_t ad = 0x00400000u; ad + 8 <= brk_ptr && n < 8192; ad += 2) {
                    if ((ad & 3) == 0 && near((int32_t)rd32(ad), x, t) && near((int32_t)rd32(ad + 4), y, t)) { dc[n] = ad; dk[n++] = 4; }
                    else if (near((int16_t)rd16(ad), x, t) && near((int16_t)rd16(ad + 2), y, t)) { dc[n] = ad; dk[n++] = 2; }
                }
            } else {
                for (int i = 0; i < ndc; i++) {
                    uint32_t ad = dc[i];
                    int cx = dk[i] == 4 ? (int32_t)rd32(ad) : (int16_t)rd16(ad), cy = dk[i] == 4 ? (int32_t)rd32(ad + 4) : (int16_t)rd16(ad + 2);
                    if (near(cx, x, t) && near(cy, y, t)) { dc[n] = ad; dk[n++] = dk[i]; }
                }
            }
            ndc = n;
            fprintf(out, "dbg %s (%d,%d)+-%d: %d", fresh ? "find" : "filt", x, y, t, n);
            for (int i = 0; i < n && i < 40; i++)
                fprintf(out, " %08x/%d=(%d,%d)", dc[i], dk[i], dk[i] == 4 ? (int32_t)rd32(dc[i]) : (int16_t)rd16(dc[i]),
                        dk[i] == 4 ? (int32_t)rd32(dc[i] + 4) : (int16_t)rd16(dc[i] + 2));
            fprintf(out, "\n");
        } else if (sscanf(line, "peek %x %u", &a, &v) == 2) {
            fprintf(out, "dbg peek %08x:", a);
            for (unsigned i = 0; i < v && i < 64; i++) fprintf(out, " %d", (int32_t)rd32(a + 4 * i));
            fprintf(out, "\n");
        } else if (sscanf(line, "poke %x %d", &a, (int *)&v) == 2) {
            wr32(a, v); fprintf(out, "dbg poke %08x = %d\n", a, (int)v);
        } else if (!strncmp(line, "quad ", 5)) {
            int q[4]; extern uint32_t brk_ptr;
            if (sscanf(line + 5, "%d %d %d %d", &q[0], &q[1], &q[2], &q[3]) == 4) {
                fprintf(out, "dbg quad:");
                for (uint32_t ad = 0x00400000u; ad + 16 <= brk_ptr; ad += 4)
                    if ((int32_t)rd32(ad) == q[0] && (int32_t)rd32(ad + 4) == q[1] && (int32_t)rd32(ad + 8) == q[2] && (int32_t)rd32(ad + 12) == q[3])
                        fprintf(out, " %08x", ad);
                fprintf(out, "\n");
            }
        } else if (!strncmp(line, "dump ", 5)) {
            // the game image and its static data (0x400000-0x1000000) -> Documents/dump_NAME.bin
            char nm[64] = "", dp[1200]; sscanf(line + 5, "%63s", nm);
            snprintf(dp, sizeof dp, "%s/Documents/dump_%s.bin", getenv("HOME") ? getenv("HOME") : ".", nm);
            FILE *df = fopen(dp, "wb");
            extern uint32_t brk_ptr;
            uint32_t end = brk_ptr > 0x01000000u ? brk_ptr : 0x01000000u;
            if (end - 0x00400000u > 0x10000000u) end = 0x10400000u;   // cap 256 MB
            if (df) { fwrite(PTR(0x00400000u), 1, end - 0x00400000u, df); fclose(df); }
            fprintf(out, "dbg dump %s %s\n", nm, df ? "ok" : "failed");
        } else if (!strncmp(line, "stall ", 6)) {
            void dsound_debug_stall(int); int on = atoi(line + 6); dsound_debug_stall(on);
            fprintf(out, "dbg stall %d\n", on);
        } else if (!strncmp(line, "stat", 4)) {
#if TARGET_OS_IPHONE
            extern int g_home_count, g_touch_count, g_zoom_in, g_zoom_out, g_edge_moves;
            extern int g_mice;
            fprintf(out, "dbg stat: cursor moves %d, homings %d, cursor (%d,%d), zoom steps in %d out %d, edge moves %d, keys shift %d ctrl %d alt %d, mice %d relative %d\n",
                    g_touch_count, g_home_count, (int)vx, (int)vy, g_zoom_in, g_zoom_out, g_edge_moves,
                    di_keys[0x2A] != 0, di_keys[0x1D] != 0, di_keys[0x38] != 0, g_mice, (int)SDL_GetWindowRelativeMouseMode(win));
            extern int g_last_click_mods, g_drag_scroll_ms; extern int g_drag_resyncs; fprintf(out, "dbg last click mods %d, drag edge scroll %d ms, drag resyncs %d\n", g_last_click_mods, g_drag_scroll_ms, g_drag_resyncs);
        } else if (!strncmp(line, "zoomu ", 6)) {
            void touch_debug_zoom(FILE *, float); touch_debug_zoom(out, (float)atof(line + 6));
        } else if (!strncmp(line, "edge ", 5)) {
            extern int g_edge_px; g_edge_px = atoi(line + 5);
            fprintf(out, "dbg edge %d\n", g_edge_px);
#endif
        } else if (!strncmp(line, "finger ", 7)) {
#if TARGET_OS_IPHONE
            char what[8]; int id; float fx, fy;
            if (sscanf(line + 7, "%7s %d %f %f", what, &id, &fx, &fy) == 4) {
                void touch_debug_finger(int, int, float, float);
                touch_debug_finger(!strcmp(what, "down") ? 1 : !strcmp(what, "up") ? 0 : 2, id, fx, fy);
                fprintf(out, "dbg finger %s %d\n", what, id);
            }
#endif
        } else if (!strncmp(line, "mouse ", 6)) {
            // mouse down|up|move X Y (game pixels): raw left-button drive, for testing drags
            char what[8]; float mx, my;
            if (sscanf(line + 6, "%7s %f %f", what, &mx, &my) == 3) {
                vmove(mx, my);
                msgq_post(g_hwnd, 0x200, mk_state(), (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
                if (!strcmp(what, "down")) mouse_button(0, 1);
                else if (!strcmp(what, "up")) mouse_button(0, 0);
                fprintf(out, "dbg mouse %s %.0f %.0f\n", what, mx, my);
            }
        } else if (!strncmp(line, "layout", 6)) {
#if TARGET_OS_IPHONE
            void touch_debug_layout(FILE *); touch_debug_layout(out);
#endif
        } else if (!strncmp(line, "hold ", 5)) {
            // hold NAME MS: key down now, up after exactly MS (released in plat_pump)
            extern SDL_Scancode g_hold_sc; extern uint32_t g_hold_until;
            char nm[64] = ""; int ms = 0; sscanf(line + 5, "%63s %d", nm, &ms);
            g_hold_sc = SDL_GetScancodeFromName(nm); g_hold_until = now_ms() + (uint32_t)ms;
            send_key(g_hold_sc, 1);
            fprintf(out, "dbg hold %s %d\n", nm, ms);
        } else if (!strncmp(line, "keydown ", 8) || !strncmp(line, "keyup ", 6)) {
            int dn = line[3] == 'd'; char nm[64] = "";
            sscanf(line + (dn ? 8 : 6), "%63s", nm);
            SDL_Event ev; memset(&ev, 0, sizeof ev);
            ev.type = dn ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP; ev.key.scancode = SDL_GetScancodeFromName(nm); ev.key.down = dn;
            SDL_PushEvent(&ev);
            fprintf(out, "dbg %s %s\n", dn ? "keydown" : "keyup", nm);
        } else if (!strncmp(line, "key ", 4)) {
            char nm[64] = ""; sscanf(line + 4, "%63s", nm);
            SDL_Event ev; memset(&ev, 0, sizeof ev);
            ev.type = SDL_EVENT_KEY_DOWN; ev.key.scancode = SDL_GetScancodeFromName(nm); ev.key.down = true; SDL_PushEvent(&ev);
            ev.type = SDL_EVENT_KEY_UP; ev.key.down = false; SDL_PushEvent(&ev);
            fprintf(out, "dbg key %s\n", nm);
        } else if (!strncmp(line, "rect", 4)) {
            int n = 0;
            fprintf(out, "dbg rect:");
            for (uint32_t ad = 0x00400000u; ad + 16 <= 0x00800000u && n < 60; ad += 4) {
                int a0 = (int32_t)rd32(ad), a1 = (int32_t)rd32(ad + 4), a2 = (int32_t)rd32(ad + 8), a3 = (int32_t)rd32(ad + 12);
                if (a0 >= 0 && a1 >= 0 && a2 > a0 + 100 && a3 > a1 + 100 && a2 <= 640 && a3 <= 480 && (a0 || a1)) {
                    fprintf(out, " %08x=(%d,%d,%d,%d)", ad, a0, a1, a2, a3); n++;
                }
            }
            fprintf(out, "\n");
        }
    }
    fclose(f);
    remove(p);
    if (out != stderr) fclose(out);
}
// ---- where the game picture goes ----
// Mac: SDL's letterboxed logical presentation. iOS: placed here, so the picture can leave a strip on
// the left for the thumb buttons (touch_left_reserve, in levels) and stay clear of camera cutouts at
// the sides (horizontal safe area). Everything that converts between window points and game pixels
// goes through map_from_window / map_to_window.
#if TARGET_OS_IPHONE
static float touch_left_reserve(void);
static SDL_FRect game_dst = {0, 0, 1, 1};   // output pixels
static float px_per_pt = 1;
static void layout_game(void) {
    int ww, wh, pw, ph;
    SDL_GetWindowSize(win, &ww, &wh); SDL_GetWindowSizeInPixels(win, &pw, &ph);
    if (ww <= 0 || wh <= 0) return;
    px_per_pt = (float)pw / ww;
    SDL_Rect safe;
    if (!SDL_GetWindowSafeArea(win, &safe)) safe = (SDL_Rect){0, 0, ww, wh};
    float l = safe.x + touch_left_reserve(), r = (float)(safe.x + safe.w);   // full height: the home
    float s = SDL_min((r - l) / tex_w, (float)wh / tex_h);                   // indicator is just a line
    float w = tex_w * s, h = tex_h * s;
    game_dst = (SDL_FRect){(l + (r - l - w) / 2) * px_per_pt, (wh - h) / 2 * px_per_pt, w * px_per_pt, h * px_per_pt};
}
static void map_from_window(float wx, float wy, float *x, float *y) {
    *x = (wx * px_per_pt - game_dst.x) * tex_w / game_dst.w;
    *y = (wy * px_per_pt - game_dst.y) * tex_h / game_dst.h;
}
static void map_to_window(float x, float y, float *wx, float *wy) {
    *wx = (game_dst.x + x * game_dst.w / tex_w) / px_per_pt;
    *wy = (game_dst.y + y * game_dst.h / tex_h) / px_per_pt;
}
#define GAME_DST (&game_dst)
#else
static void layout_game(void) {}
static void map_from_window(float wx, float wy, float *x, float *y) { SDL_RenderCoordinatesFromWindow(ren, wx, wy, x, y); }
static void map_to_window(float x, float y, float *wx, float *wy) { SDL_RenderCoordinatesToWindow(ren, x, y, wx, wy); }
#define GAME_DST NULL
#endif
static void set_logical(int w, int h) {
#if TARGET_OS_IPHONE
    (void)w; (void)h; SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
#else
    SDL_SetRenderLogicalPresentation(ren, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
#endif
}
static void game_to_window(int gx, int gy, float *wx, float *wy) {
    map_to_window((float)gx + 0.5f, (float)gy + 0.5f, wx, wy);
}
static void script_tick(void) {
    uint32_t t = now_ms() - g_start_ms;
    while (iscript < nscript && script[iscript].t <= t) {
        ScriptEv *e = &script[iscript++];
        SDL_Event ev; memset(&ev, 0, sizeof ev);
        float wx, wy;
        switch (e->kind) {
        case 1:
            ev.type = SDL_EVENT_KEY_DOWN; ev.key.scancode = (SDL_Scancode)e->a; ev.key.down = true; SDL_PushEvent(&ev);
            ev.type = SDL_EVENT_KEY_UP; ev.key.down = false; SDL_PushEvent(&ev);
            break;
        case 2: case 3: case 4:
            game_to_window(e->a, e->b, &wx, &wy);
            vmove((float)e->a, (float)e->b);
            msgq_post(g_hwnd, 0x200, 0, (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
            if (e->kind == 4) break;
            memset(&ev, 0, sizeof ev);
            ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN; ev.button.x = wx; ev.button.y = wy;
            ev.button.button = e->kind == 2 ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT; ev.button.down = true; SDL_PushEvent(&ev);
            break;
        case 6: case 7:
            memset(&ev, 0, sizeof ev);
            ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
            ev.button.button = e->kind == 6 ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT; ev.button.down = false; SDL_PushEvent(&ev);
            break;
        case 5: fprintf(stderr, "script: end\n"); exit(0);
        case 8: cursor_scan(); break;
        case 9: cursor_snap(); break;
        case 10: cursor_diff(); break;
#if TARGET_OS_IPHONE
        case 11: { void touch_script_tap(float, float); touch_script_tap((float)e->a, (float)e->b); break; }
#endif
        }
    }
}

void plat_init(void) {
#if TARGET_OS_IPHONE
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");   // touches are handled as gestures (ios/touch_ios.inc)
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) rt_fatal("SDL_Init: %s", SDL_GetError());
#if TARGET_OS_IPHONE
    win = SDL_CreateWindow("Populous: The Beginning", 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY);
#else
    win = SDL_CreateWindow("Populous: The Beginning", 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
#endif
    ren = SDL_CreateGPURenderer(NULL, win);
    if (!ren) { WARN("GPU renderer unavailable: %s", SDL_GetError()); ren = SDL_CreateRenderer(win, NULL); }
    LOG("renderer: %s", SDL_GetRendererName(ren));
    SDL_SetRenderVSync(ren, 0);
    set_logical(tex_w, tex_h);
    layout_game();
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);
    SDL_HideCursor();
#if !TARGET_OS_IPHONE
    SDL_SetWindowRelativeMouseMode(win, true);   // iOS: touches and the iPad pointer are absolute
#endif
    g_start_ms = now_ms();
}

SDL_GPUDevice *plat_gpu_device(int *window_pixel_h) {
    if (window_pixel_h) SDL_GetWindowSizeInPixels(win, NULL, window_pixel_h);
    return SDL_GetGPURendererDevice(ren);
}

static void set_size(int w, int h) {
    if (w == tex_w && h == tex_h && rgba) return;
    if (tex) SDL_DestroyTexture(tex);
    if (overlay) SDL_DestroyTexture(overlay);
    tex = overlay = NULL;
    set_logical(w, h);
    free(rgba); rgba = malloc((size_t)w * h * 4);
    tex_w = w; tex_h = h;
}
static SDL_Texture *stream_tex(SDL_Texture **t, SDL_PixelFormat fmt, SDL_BlendMode blend) {
    if (!*t) {
        *t = SDL_CreateTexture(ren, fmt, SDL_TEXTUREACCESS_STREAMING, tex_w, tex_h);
        SDL_SetTextureScaleMode(*t, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(*t, blend);
    }
    return *t;
}
static inline uint32_t rgb565_to_xrgb(uint32_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

#if TARGET_OS_IPHONE
static int frame_held(void);
static void touch_draw_overlay(void);
#endif
// rgba holds the game-resolution frame (alpha ignored); presents, then takes the requested screenshots
static uint32_t last_present;
static void finish_frame(int w, int h) {
    extern const char *g_shot_path, *g_shot_dir; extern int g_shot_frame, g_shot_every;
    static int frames;
    frames++;
    char p[1024] = "";
    if (g_shot_dir && g_shot_every && frames % g_shot_every == 0)
        snprintf(p, sizeof p, "%s/shot_%06u_f%05d.bmp", g_shot_dir, now_ms() - g_start_ms, frames);
    if (g_shot_path && frames == g_shot_frame) snprintf(p, sizeof p, "%s", g_shot_path);
    if (p[0]) {
        // POPTB_SHOT_WINDOW: what the window shows (full output resolution) instead of the game-resolution frame
        SDL_Surface *s = getenv("POPTB_SHOT_WINDOW") ? SDL_RenderReadPixels(ren, NULL)
                                                    : SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, rgba, w * 4);
        if (s) { SDL_SaveBMP(s, p); SDL_DestroySurface(s); }
        if (g_shot_path && frames == g_shot_frame) fprintf(stderr, "screenshot saved to %s (frame %d)\n", p, frames);
    }
#if TARGET_OS_IPHONE
    touch_draw_overlay();
#endif
    SDL_RenderPresent(ren);
    last_present = now_ms();
    if (rt_verbose > 1) { static int n; if (n++ % 10 == 0) LOG("[%u] present #%d %dx%d", now_ms() % 100000, n, w, h); }
}

void plat_present(const uint8_t *pix, int w, int h, int pitch, const uint32_t *pal) {
#if TARGET_OS_IPHONE
    if (frame_held()) return;   // the cursor is being re-homed: keep showing the previous frame
#endif
    set_size(w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *s = pix + (size_t)y * pitch;
        uint32_t *d = rgba + (size_t)y * w;
        if (pal) for (int x = 0; x < w; x++) d[x] = pal[s[x]];
        else for (int x = 0; x < w; x++) d[x] = rgb565_to_xrgb((uint32_t)(s[2 * x] | s[2 * x + 1] << 8));
    }
    SDL_Texture *t = stream_tex(&tex, SDL_PIXELFORMAT_XRGB8888, SDL_BLENDMODE_NONE);
    SDL_UpdateTexture(t, NULL, rgba, w * 4);
    SDL_RenderClear(ren);
    layout_game();
    SDL_RenderTexture(ren, t, NULL, GAME_DST);
    finish_frame(w, h);
}

void plat_present_hybrid(const uint16_t *pix, int w, int h, int pitch, const uint8_t *mask) {
#if TARGET_OS_IPHONE
    if (frame_held()) return;
#endif
    set_size(w, h);
    int iw, ih;
    SDL_GPUTexture *img = gpu_image(&iw, &ih);
    if (img != hires_src || (hires && (hires->w != iw || hires->h != ih))) {
        if (hires) SDL_DestroyTexture(hires);
        hires = NULL;
        hires_src = img;
        if (img) {
            SDL_PropertiesID p = SDL_CreateProperties();
            SDL_SetPointerProperty(p, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, img);
            SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
            SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, iw);
            SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, ih);
            hires = SDL_CreateTextureWithProperties(ren, p);
            SDL_DestroyProperties(p);
            if (!hires) WARN("hybrid present: %s", SDL_GetError());
            else { SDL_SetTextureScaleMode(hires, SDL_SCALEMODE_LINEAR); SDL_SetTextureBlendMode(hires, SDL_BLENDMODE_NONE); }
        }
    }
    for (int y = 0; y < h; y++) {
        const uint16_t *s = (const uint16_t *)((const uint8_t *)pix + (size_t)y * pitch);
        const uint8_t *m = mask + (size_t)y * w;
        uint32_t *d = rgba + (size_t)y * w;
        for (int x = 0; x < w; x++) d[x] = rgb565_to_xrgb(s[x]) | (m[x] ? 0xff000000u : 0);
    }
    SDL_Texture *t = stream_tex(&overlay, SDL_PIXELFORMAT_ARGB8888, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(t, NULL, rgba, w * 4);
    SDL_RenderClear(ren);
    layout_game();
    if (hires) SDL_RenderTexture(ren, hires, NULL, GAME_DST);
    SDL_RenderTexture(ren, t, NULL, GAME_DST);
    finish_frame(w, h);
}

// ---- key tables: SDL scancode -> DIK (PC set 1 scancode) and VK ----
typedef struct { SDL_Scancode sc; uint8_t dik, vk; } KeyMap;
static const KeyMap keymap[] = {
    {SDL_SCANCODE_ESCAPE, 0x01, 0x1B}, {SDL_SCANCODE_1, 0x02, '1'}, {SDL_SCANCODE_2, 0x03, '2'}, {SDL_SCANCODE_3, 0x04, '3'},
    {SDL_SCANCODE_4, 0x05, '4'}, {SDL_SCANCODE_5, 0x06, '5'}, {SDL_SCANCODE_6, 0x07, '6'}, {SDL_SCANCODE_7, 0x08, '7'},
    {SDL_SCANCODE_8, 0x09, '8'}, {SDL_SCANCODE_9, 0x0A, '9'}, {SDL_SCANCODE_0, 0x0B, '0'}, {SDL_SCANCODE_MINUS, 0x0C, 0xBD},
    {SDL_SCANCODE_EQUALS, 0x0D, 0xBB}, {SDL_SCANCODE_BACKSPACE, 0x0E, 0x08}, {SDL_SCANCODE_TAB, 0x0F, 0x09},
    {SDL_SCANCODE_Q, 0x10, 'Q'}, {SDL_SCANCODE_W, 0x11, 'W'}, {SDL_SCANCODE_E, 0x12, 'E'}, {SDL_SCANCODE_R, 0x13, 'R'},
    {SDL_SCANCODE_T, 0x14, 'T'}, {SDL_SCANCODE_Y, 0x15, 'Y'}, {SDL_SCANCODE_U, 0x16, 'U'}, {SDL_SCANCODE_I, 0x17, 'I'},
    {SDL_SCANCODE_O, 0x18, 'O'}, {SDL_SCANCODE_P, 0x19, 'P'}, {SDL_SCANCODE_LEFTBRACKET, 0x1A, 0xDB},
    {SDL_SCANCODE_RIGHTBRACKET, 0x1B, 0xDD}, {SDL_SCANCODE_RETURN, 0x1C, 0x0D}, {SDL_SCANCODE_LCTRL, 0x1D, 0x11},
    {SDL_SCANCODE_A, 0x1E, 'A'}, {SDL_SCANCODE_S, 0x1F, 'S'}, {SDL_SCANCODE_D, 0x20, 'D'}, {SDL_SCANCODE_F, 0x21, 'F'},
    {SDL_SCANCODE_G, 0x22, 'G'}, {SDL_SCANCODE_H, 0x23, 'H'}, {SDL_SCANCODE_J, 0x24, 'J'}, {SDL_SCANCODE_K, 0x25, 'K'},
    {SDL_SCANCODE_L, 0x26, 'L'}, {SDL_SCANCODE_SEMICOLON, 0x27, 0xBA}, {SDL_SCANCODE_APOSTROPHE, 0x28, 0xDE},
    {SDL_SCANCODE_GRAVE, 0x29, 0xC0}, {SDL_SCANCODE_LSHIFT, 0x2A, 0x10}, {SDL_SCANCODE_BACKSLASH, 0x2B, 0xDC},
    {SDL_SCANCODE_Z, 0x2C, 'Z'}, {SDL_SCANCODE_X, 0x2D, 'X'}, {SDL_SCANCODE_C, 0x2E, 'C'}, {SDL_SCANCODE_V, 0x2F, 'V'},
    {SDL_SCANCODE_B, 0x30, 'B'}, {SDL_SCANCODE_N, 0x31, 'N'}, {SDL_SCANCODE_M, 0x32, 'M'}, {SDL_SCANCODE_COMMA, 0x33, 0xBC},
    {SDL_SCANCODE_PERIOD, 0x34, 0xBE}, {SDL_SCANCODE_SLASH, 0x35, 0xBF}, {SDL_SCANCODE_RSHIFT, 0x36, 0x10},
    {SDL_SCANCODE_KP_MULTIPLY, 0x37, 0x6A}, {SDL_SCANCODE_LALT, 0x38, 0x12}, {SDL_SCANCODE_SPACE, 0x39, 0x20},
    {SDL_SCANCODE_CAPSLOCK, 0x3A, 0x14}, {SDL_SCANCODE_F1, 0x3B, 0x70}, {SDL_SCANCODE_F2, 0x3C, 0x71},
    {SDL_SCANCODE_F3, 0x3D, 0x72}, {SDL_SCANCODE_F4, 0x3E, 0x73}, {SDL_SCANCODE_F5, 0x3F, 0x74}, {SDL_SCANCODE_F6, 0x40, 0x75},
    {SDL_SCANCODE_F7, 0x41, 0x76}, {SDL_SCANCODE_F8, 0x42, 0x77}, {SDL_SCANCODE_F9, 0x43, 0x78}, {SDL_SCANCODE_F10, 0x44, 0x79},
    {SDL_SCANCODE_NUMLOCKCLEAR, 0x45, 0x90}, {SDL_SCANCODE_SCROLLLOCK, 0x46, 0x91}, {SDL_SCANCODE_KP_7, 0x47, 0x67},
    {SDL_SCANCODE_KP_8, 0x48, 0x68}, {SDL_SCANCODE_KP_9, 0x49, 0x69}, {SDL_SCANCODE_KP_MINUS, 0x4A, 0x6D},
    {SDL_SCANCODE_KP_4, 0x4B, 0x64}, {SDL_SCANCODE_KP_5, 0x4C, 0x65}, {SDL_SCANCODE_KP_6, 0x4D, 0x66},
    {SDL_SCANCODE_KP_PLUS, 0x4E, 0x6B}, {SDL_SCANCODE_KP_1, 0x4F, 0x61}, {SDL_SCANCODE_KP_2, 0x50, 0x62},
    {SDL_SCANCODE_KP_3, 0x51, 0x63}, {SDL_SCANCODE_KP_0, 0x52, 0x60}, {SDL_SCANCODE_KP_PERIOD, 0x53, 0x6E},
    {SDL_SCANCODE_F11, 0x57, 0x7A}, {SDL_SCANCODE_F12, 0x58, 0x7B}, {SDL_SCANCODE_KP_ENTER, 0x9C, 0x0D},
    {SDL_SCANCODE_RCTRL, 0x9D, 0x11}, {SDL_SCANCODE_KP_DIVIDE, 0xB5, 0x6F}, {SDL_SCANCODE_RALT, 0xB8, 0x12},
    {SDL_SCANCODE_PAUSE, 0xC5, 0x13}, {SDL_SCANCODE_HOME, 0xC7, 0x24}, {SDL_SCANCODE_UP, 0xC8, 0x26},
    {SDL_SCANCODE_PAGEUP, 0xC9, 0x21}, {SDL_SCANCODE_LEFT, 0xCB, 0x25}, {SDL_SCANCODE_RIGHT, 0xCD, 0x27},
    {SDL_SCANCODE_END, 0xCF, 0x23}, {SDL_SCANCODE_DOWN, 0xD0, 0x28}, {SDL_SCANCODE_PAGEDOWN, 0xD1, 0x22},
    {SDL_SCANCODE_INSERT, 0xD2, 0x2D}, {SDL_SCANCODE_DELETE, 0xD3, 0x2E}, {SDL_SCANCODE_LGUI, 0xDB, 0x5B},
    {SDL_SCANCODE_RGUI, 0xDC, 0x5C},
};
static const KeyMap *key_of(SDL_Scancode sc) {
    for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++) if (keymap[i].sc == sc) return &keymap[i];
    return NULL;
}

static void mouse_to_game(float wx, float wy) {
    float x, y;
    map_from_window(wx, wy, &x, &y);
    int ix = (int)x, iy = (int)y;
    if (ix < 0) ix = 0; if (iy < 0) iy = 0;
    if (ix >= tex_w) ix = tex_w - 1; if (iy >= tex_h) iy = tex_h - 1;
    g_mouse_x = ix; g_mouse_y = iy;
}
static uint32_t mk_state(void) {
    uint32_t s = 0;
    if (di_mouse_btn[0]) s |= 1; if (di_mouse_btn[1]) s |= 2; if (di_mouse_btn[2]) s |= 0x10;
    const bool *k = SDL_GetKeyboardState(NULL);
    // di_keys also holds modifiers injected by touch controls (DIK 0x2A/0x36 shift, 0x1D/0x9D ctrl)
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT] || di_keys[0x2A] || di_keys[0x36]) s |= 4;
    if (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL] || di_keys[0x1D] || di_keys[0x9D]) s |= 8;
    return s;
}

// A trackpad tap delivers press and release in the same instant. The input thread drains the whole
// DirectInput buffer when signalled and the menus act on the button state at the end of it, so such
// a click is lost; the release is held until the press is at least MIN_CLICK_MS old.
#define MIN_CLICK_MS 50
static uint32_t down_ms[3];
static int up_pending[3];
static void mouse_button(int b, int down) {
    static const uint32_t msgs[3][2] = {{0x202, 0x201}, {0x205, 0x204}, {0x208, 0x207}};
    up_pending[b] = 0;
    di_mouse_btn[b] = down ? 0x80 : 0;
    di_push_mouse(12 + b, down ? 0x80 : 0);
    msgq_post(g_hwnd, msgs[b][down], mk_state(), (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
}

// sends a key as the keyboard would (DirectInput + WM_KEYDOWN/UP, WM_CHAR for control keys)
static void send_key(SDL_Scancode sc, int down) {
    const KeyMap *k = key_of(sc);
    if (!k) return;
    di_push_key(k->dik, down);
    uint32_t lp = 1 | ((uint32_t)(k->dik & 0x7f) << 16) | ((k->dik & 0x80) ? 1u << 24 : 0) | (down ? 0 : 0xC0000000u);
    msgq_post(g_hwnd, down ? 0x100 : 0x101, k->vk, lp);
    if (down && (k->vk == 0x0D || k->vk == 0x08 || k->vk == 0x1B || k->vk == 0x09)) msgq_post(g_hwnd, 0x102, k->vk, lp);
}

#if TARGET_OS_IPHONE
#include "../ios/touch_ios.inc"
void plat_frame_hook(CPU *R, int site) { touch_frame_hook(R, site); }
#else
void plat_frame_hook(CPU *R, int site) { (void)R; (void)site; }
#endif

void plat_pump(void) {
    SDL_Event e;
    if (!pthread_main_np()) return;   // Cocoa only accepts events on the main thread
    if (nscript) script_tick();
    if (getenv("POPTB_DEBUG_CMDS")) debug_cmds();
    if (g_hold_sc && (int32_t)(now_ms() - g_hold_until) >= 0) { send_key(g_hold_sc, 0); g_hold_sc = 0; }
#if TARGET_OS_IPHONE
    touch_tick();
#endif
    for (int b = 0; b < 3; b++)
        if (up_pending[b] && now_ms() - down_ms[b] >= MIN_CLICK_MS) mouse_button(b, 0);
    while (SDL_PollEvent(&e)) {
#if TARGET_OS_IPHONE
        if (touch_event(&e)) continue;
#endif
        switch (e.type) {
        case SDL_EVENT_QUIT:
            msgq_post(g_hwnd, 0x10, 0, 0);   // WM_CLOSE
            break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            int down = e.type == SDL_EVENT_KEY_DOWN;
            // Cmd+Q / Cmd+Enter: Mac shortcuts
            if (down && (e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_Q) { fprintf(stderr, "quitting (Cmd+Q)\n"); exit(0); }
            if (down && (e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_RETURN) {
                SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)); break;
            }
            const KeyMap *k = key_of(e.key.scancode);
            if (!k) break;
            if (!(down && e.key.repeat)) di_push_key(k->dik, down);
            uint32_t lp = 1 | ((uint32_t)(k->dik & 0x7f) << 16) | ((k->dik & 0x80) ? 1u << 24 : 0) | (down ? 0 : 0xC0000000u);
            if (down && e.key.repeat) lp |= 1u << 30;
            int alt = (e.key.mod & SDL_KMOD_ALT) != 0;
            msgq_post(g_hwnd, alt ? (down ? 0x104 : 0x105) : (down ? 0x100 : 0x101), k->vk, lp);
            // WM_CHAR for control keys (text comes from SDL_EVENT_TEXT_INPUT)
            if (down && (k->vk == 0x0D || k->vk == 0x08 || k->vk == 0x1B || k->vk == 0x09)) msgq_post(g_hwnd, 0x102, k->vk, lp);
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
            for (const unsigned char *c = (const unsigned char *)e.text.text; *c; c++)
                if (*c >= 32 && *c < 128) msgq_post(g_hwnd, 0x102, *c, 1);
            break;
        case SDL_EVENT_MOUSE_MOTION: {
            // deltas in window points -> game pixels
            float x0, y0, x1, y1;
            map_from_window(0, 0, &x0, &y0);
            map_from_window(e.motion.xrel, e.motion.yrel, &x1, &y1);
            vmove(vx + (x1 - x0), vy + (y1 - y0));
            msgq_post(g_hwnd, 0x200, mk_state(), (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            int down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            if (b < 0) break;
            if (down) {
                if (up_pending[b]) mouse_button(b, 0);
                down_ms[b] = now_ms();
            } else if (now_ms() - down_ms[b] < MIN_CLICK_MS) { up_pending[b] = 1; break; }
            mouse_button(b, down);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            int dz = (int)(e.wheel.y * 120);
            di_mouse_dz += dz;
            di_push_mouse(8, (uint32_t)dz);
            msgq_post(g_hwnd, 0x20A, ((uint32_t)(dz & 0xffff) << 16) | mk_state(), (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            msgq_post(g_hwnd, 0x1c, 1, 0);
#if !TARGET_OS_IPHONE
            SDL_SetWindowRelativeMouseMode(win, true);
#endif
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            memset(di_keys, 0, sizeof di_keys);
#if !TARGET_OS_IPHONE
            SDL_SetWindowRelativeMouseMode(win, false);
#endif
            break;
        default: break;
        }
    }
}
