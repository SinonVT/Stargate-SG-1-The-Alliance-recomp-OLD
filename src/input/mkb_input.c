/*
 * mkb_input.c -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * A hidden window on its own thread registers for Raw Input with
 * RIDEV_INPUTSINK, so mouse deltas and key presses arrive whether or
 * not the game's own window is the one with focus, and no matter which
 * window the runtime happens to present through (the GDI debug window,
 * the FMV window, a D3D11 swap chain -- the input layer should not
 * have to care).
 *
 * The poll side drains the accumulated deltas and turns them into a
 * stick deflection that is proportional to the movement since the last
 * poll: the title integrates deflection over its own frame, so the
 * total rotation comes out proportional to total mouse travel at any
 * frame rate, which is the property mouse aim has to keep.
 *
 * Small movements get a floor of RECOMP_MKB_DZ of full throw, because
 * every console title applies its own dead zone to the stick, and a
 * mouse correction smaller than it would be silently eaten.
 */

/* Raw input needs a modern _WIN32_WINNT; the toolkit's platform header
 * includes windows.h first, so these have to land before anything else. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define _CRT_SECURE_NO_WARNINGS

#include "mkb_input.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern volatile LONG g_sg1_menu_open;   /* SG1: in-game settings overlay is open */

/* ---- configuration ----------------------------------------------------- */

typedef struct {
    double sens;        /* multiplier on the aim deflection          */
    double dz;          /* assumed game deadzone, 0..1               */
    double acc;         /* aim acceleration exponent, 1 = linear     */
    long   full;        /* mouse counts for full throw, per poll     */
    int    invert_y;    /* invert the vertical aim axis              */
    int    swap_sticks; /* mouse aims with the left stick instead    */
} MkbConfig;

static MkbConfig s_cfg = { 1.0, 0.18, 1.0, 8, 0, 0 };

/* One "key=value" line; returns 1 when the key was recognised. */
static int mkb_apply_kv(const char *key, const char *val)
{
    double d;
    long   l;

    if (!strcmp(key, "sens")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 100.0) s_cfg.sens = d;
    } else if (!strcmp(key, "dz")) {
        d = strtod(val, NULL); if (d >= 0.0 && d < 0.9) s_cfg.dz = d;
    } else if (!strcmp(key, "acc")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.acc = d;
    } else if (!strcmp(key, "full")) {
        l = strtol(val, NULL, 0); if (l > 0 && l < 1000) s_cfg.full = l;
    } else if (!strcmp(key, "invert_y")) {
        s_cfg.invert_y = atoi(val) != 0;
    } else if (!strcmp(key, "swap_sticks")) {
        s_cfg.swap_sticks = atoi(val) != 0;
    } else {
        return 0;
    }
    return 1;
}

static void mkb_load_config(void)
{
    static const char *kv[] = {
        "RECOMP_MKB_SENS", "sens",
        "RECOMP_MKB_DZ",   "dz",
        "RECOMP_MKB_ACC",  "acc",
        "RECOMP_MKB_FULL", "full",
        "RECOMP_MKB_INVERT_Y",  "invert_y",
        "RECOMP_MKB_SWAP_STICKS", "swap_sticks",
        NULL
    };
    char path[MAX_PATH];
    const char *ini;
    DWORD n;
    FILE *f;
    int i;

    for (i = 0; kv[i]; i += 2) {
        const char *v = getenv(kv[i]);
        if (v && *v)
            mkb_apply_kv(kv[i + 1], v);
    }

    ini = getenv("RECOMP_MKB_INI");
    if (ini && *ini) {
        snprintf(path, sizeof path, "%s", ini);
    } else {
        n = GetModuleFileNameA(NULL, path, sizeof path - 12);
        if (!n || n >= sizeof path - 12)
            return;
        snprintf(path + n, 12, "\\..\\mkb.ini");   /* beside the .exe */
    }
    f = fopen(path, "r");
    if (!f)
        return;
    {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            char *k, *v;
            if (!eq) continue;
            *eq = 0;
            k = line; v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            while (*v == ' ' || *v == '\t') v++;
            k[strcspn(k, " \t\r\n")] = 0;
            v[strcspn(v, " \t\r\n")] = 0;
            if (*k && *v)
                mkb_apply_kv(k, v);
        }
    }
    fclose(f);
}

/* ---- raw input thread -------------------------------------------------- */

/* Accumulators, written by the input thread and drained by the poll.
 * InterlockedExchange is the whole protocol: the input thread adds
 * with InterlockedAdd, the poll takes everything with Exchange(0). */
static volatile LONG s_dx, s_dy;
static volatile LONG s_raw_events, s_abs_events;
static volatile LONG s_wheel;        /* wheel steps since last poll   */
static volatile LONG s_gdx, s_gdy;   /* gameplay (native mouse-look) copy */
static volatile LONG s_mdx, s_mdy, s_mlvl, s_mrlvl;   /* menu-side copy, never cleared by aim */
static volatile LONG s_mouse_btn;    /* 1 L, 2 R, 4 M, 8 X1, 16 X2    */
static volatile unsigned char s_vk_down[256];
/* SG1: the game reads the (emulated) pad only about once per frame, so a quick tap of a key can start
 * and end between two reads and is never seen. Remember when each key went down and report it as
 * held for a minimum time (RECOMP_KEY_HOLD_MS, default 90 ms). Long presses are unaffected. */
static volatile LONGLONG s_vk_t[256];
extern int mkb_text_mode(void);
static int mk_held(unsigned k)
{
    static LONGLONG hold_ticks;
    LARGE_INTEGER now;
    if (k >= 256) return 0;
    if (s_vk_down[k]) return 1;
    if (!s_vk_t[k]) return 0;
    if (!hold_ticks) {
        LARGE_INTEGER f; const char *e = getenv("RECOMP_KEY_HOLD_MS");
        long ms = e ? atol(e) : 90;
        QueryPerformanceFrequency(&f);
        hold_ticks = f.QuadPart * (ms < 0 ? 0 : ms) / 1000;
        if (!hold_ticks) hold_ticks = 1;
    }
    QueryPerformanceCounter(&now);
    return (now.QuadPart - s_vk_t[k]) < hold_ticks;
}

static volatile LONG s_active = 1;   /* F9 toggles this               */
static volatile LONG s_thread_up;

/* F9 is read on the input thread, so the toggle works even while the
 * game is stalled on the poll side. */
#define MKB_TOGGLE_VK  VK_F9


/* ---- real keyboard events for the game (typing, console) ---- */
static volatile LONG s_console_typing, s_con_tick;
typedef struct { unsigned short vk; unsigned char down, rep; unsigned short ch; } KbEvt;
static KbEvt s_kq[256];
static volatile LONG s_kq_w, s_kq_r;
static void kq_push(unsigned vk, int down, int rep, unsigned ch)
{
    LONG w = s_kq_w;
    if (w - s_kq_r >= 256) return;
    s_kq[w & 255].vk = (unsigned short)vk; s_kq[w & 255].down = (unsigned char)down;
    s_kq[w & 255].rep = (unsigned char)rep; s_kq[w & 255].ch = (unsigned short)ch;
    MemoryBarrier();
    s_kq_w = w + 1;
    if (s_console_typing) s_con_tick = (LONG)GetTickCount();
}

/* Text mode: while a menu is on screen (the game draws it every frame and tells us)
 * or the console is open for typing, letters must reach the game as letters, not
 * as pad buttons -- so the keyboard-as-pad layers stand down. */
static volatile LONG s_menu_tick;
static volatile int mkb_txt;
static volatile int s_ingame;                    /* a match is loaded (set from the guest log) */
void mkb_set_ingame(int on) { s_ingame = on ? 1 : 0; fprintf(stderr, "[MKB] in game: %d\n", s_ingame); fflush(stderr); }
int mkb_in_game(void) { return s_ingame; }
static int s_startdbg;
void mkb_note_menu(void) { s_menu_tick = (LONG)GetTickCount(); }
static volatile LONG s_blk[256];
void mkb_set_console(int on) { s_console_typing = on ? 1 : 0; s_con_tick = (LONG)GetTickCount(); if (!on) { s_blk[0x0D] = 1; s_blk[0x1B] = 1; } }
int mkb_key_blocked(int vk) { return (unsigned)vk < 256 && s_blk[vk]; }
int mkb_text_mode(void)
{
    static int last = -1;
    int menu = s_menu_tick && (LONG)GetTickCount() - s_menu_tick < 250;
    if (s_console_typing && (LONG)GetTickCount() - s_con_tick > 15000) s_console_typing = 0;   /* failsafe: chat/console never opened */
    int m = (menu || s_console_typing) ? 1 : 0;
    if (m != last) { last = m; fprintf(stderr, "  [MKB] text mode %s (menu=%d console=%d)\n", m ? "on: keyboard->pad off" : "off: keyboard->pad on", menu, (int)s_console_typing); fflush(stderr); }
    return m;
}

static void mkb_handle_raw(HRAWINPUT h)
{
    BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof buf;
    RAWINPUT *ri = (RAWINPUT *)buf;

    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER))
            == (UINT)-1)
        return;

    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE *m = &ri->data.mouse;

        s_raw_events++;
        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) { if (m->lLastX) { InterlockedAdd(&s_mdx, m->lLastX); InterlockedAdd(&s_gdx, m->lLastX); } if (m->lLastY) { InterlockedAdd(&s_mdy, m->lLastY); InterlockedAdd(&s_gdy, m->lLastY); } }
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) InterlockedExchange(&s_mlvl, 1);
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)   InterlockedExchange(&s_mlvl, 0);
        if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) InterlockedExchange(&s_mrlvl, 1);
        if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)   InterlockedExchange(&s_mrlvl, 0);
        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (m->lLastX) InterlockedAdd(&s_dx, m->lLastX);
            if (m->lLastY) InterlockedAdd(&s_dy, m->lLastY);
        } else {
            /* SG1: remote desktop, VM and tablet mice report absolute
             * positions (0..65535); turn the change into relative counts. */
            static LONG ax = -1, ay = -1;
            s_abs_events++;
            if (ax >= 0) {
                int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
                LONG ddx = (LONG)((m->lLastX - ax) * (long long)w / 65535);
                LONG ddy = (LONG)((m->lLastY - ay) * (long long)h / 65535);
                if (ddx) InterlockedAdd(&s_dx, ddx);
                if (ddx) { InterlockedAdd(&s_mdx, ddx); InterlockedAdd(&s_gdx, ddx); }
                if (ddy) { InterlockedAdd(&s_mdy, ddy); InterlockedAdd(&s_gdy, ddy); }
                if (ddy) InterlockedAdd(&s_dy, ddy);
            }
            ax = m->lLastX; ay = m->lLastY;
        }
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
            InterlockedOr(&s_mouse_btn, 1);
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
            InterlockedAnd(&s_mouse_btn, ~1);
        if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
            InterlockedOr(&s_mouse_btn, 2);
        if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
            InterlockedAnd(&s_mouse_btn, ~2);
        if (m->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN)
            { InterlockedOr(&s_mouse_btn, 4); kq_push(0x04, 1, 0, 0); }   /* IK_MiddleMouse for the game's MiddleMouse= binding (ZoomToggle) */
        if (m->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
            { InterlockedAnd(&s_mouse_btn, ~4); kq_push(0x04, 0, 0, 0); }
        if (m->usButtonFlags & RI_MOUSE_BUTTON_4_DOWN)
            InterlockedOr(&s_mouse_btn, 8);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_4_UP)
            InterlockedAnd(&s_mouse_btn, ~8);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_5_DOWN)
            InterlockedOr(&s_mouse_btn, 16);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_5_UP)
            InterlockedAnd(&s_mouse_btn, ~16);
        if (m->usButtonFlags & RI_MOUSE_WHEEL) {
            SHORT d = (SHORT)m->usButtonData;
            int wn = d / WHEEL_DELTA, wi, wc = wn < 0 ? -wn : wn; unsigned wvk = wn > 0 ? 0xED : 0xEC;   /* IK_MouseWheelUp / Down */
            InterlockedAdd(&s_wheel, wn);
            { extern volatile LONG g_sg1_mwheel; InterlockedAdd(&g_sg1_mwheel, wn); }   /* also counted for the SG1 overlay menus (voice menu scrolls) */
            if (wc > 4) wc = 4;
            { extern volatile LONG g_sg1_voice_open; if (g_sg1_voice_open) wc = 0; }   /* voice menu open: the wheel selects a line, no weapon switch */
            for (wi = 0; wi < wc; wi++) { kq_push(wvk, 1, 0, 0); kq_push(wvk, 0, 0, 0); }   /* the game's MouseWheelUp/Down bindings (weapon select) */
        }
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD *k = &ri->data.keyboard;
        UINT vk = k->VKey;

        if (vk == MKB_TOGGLE_VK) {
            if (k->Message == WM_KEYDOWN && !s_vk_down[vk & 0xFF]) {
                LONG now = !s_active;
                InterlockedExchange(&s_active, now);
                fprintf(stderr, "  [MKB] %s\n", now ? "captured"
                                                    : "released (F9)");
                fflush(stderr);
            }
        }
        if (vk && vk < 256 && s_active && vk != MKB_TOGGLE_VK) {
            int dn = (k->Message == WM_KEYDOWN || k->Message == WM_SYSKEYDOWN);
            unsigned ch = 0;
            int rep = dn && s_vk_down[vk];
            if (dn && !s_vk_down[0x11]) {
                BYTE ks[256]; WCHAR wb[4];
                memset(ks, 0, sizeof ks);
                if (s_vk_down[0x10]) ks[VK_SHIFT] = 0x80;
                if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;
                if (ToUnicode(vk, k->MakeCode, ks, wb, 4, 4) == 1 && wb[0] >= 0x20 && wb[0] < 0x7F)
                    ch = wb[0];
            }
            kq_push(vk, dn, rep, ch);
        }
        if (vk && vk < 256) {
            if (k->Message == WM_KEYDOWN || k->Message == WM_SYSKEYDOWN) {
                if (!s_vk_down[vk] && !mkb_text_mode()) { LARGE_INTEGER q; QueryPerformanceCounter(&q); s_vk_t[vk] = q.QuadPart; }
                s_vk_down[vk] = 1;
            }
            else
                s_vk_down[vk] = 0;
        }
    }
}

static LRESULT CALLBACK mkb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {
        mkb_handle_raw((HRAWINPUT)l);
        return 0;
    }
    if (m == WM_CLOSE || m == WM_DESTROY)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI mkb_thread(LPVOID unused)
{
    WNDCLASSA wc;
    HWND hwnd;
    RAWINPUTDEVICE rid[2];
    MSG msg;

    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = mkb_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "XboxRecompMKB";
    RegisterClassA(&wc);
    /* Not visible: it exists to own the WM_INPUT delivery, nothing else. */
    hwnd = CreateWindowExA(0, "XboxRecompMKB", "mkb", WS_OVERLAPPED,
                           0, 0, 0, 0, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        fprintf(stderr, "  [MKB] window creation failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    memset(rid, 0, sizeof rid);
    rid[0].usUsagePage = 0x01;  rid[0].usUsage = 0x02;  /* mouse     */
    rid[1].usUsagePage = 0x01;  rid[1].usUsage = 0x06;  /* keyboard  */
    rid[0].dwFlags = RIDEV_INPUTSINK;   /* deliver without focus */
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        fprintf(stderr, "  [MKB] RegisterRawInputDevices failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    InterlockedExchange(&s_thread_up, 1);
    fprintf(stderr, "  [MKB] mouse+keyboard captured (F9 releases; "
            "sens=%.2f dz=%.2f full=%ld)\n",
            s_cfg.sens, s_cfg.dz, s_cfg.full);
    fflush(stderr);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    InterlockedExchange(&s_thread_up, 0);
    return 0;
}

/* ---- poll side --------------------------------------------------------- */

int mkb_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        /* Opt-in, like RECOMP_KEYBOARD: a keyboard silently acting as
         * player 1 is surprising when a real pad is what you meant to use.
         * RECOMP_MKB=1 turns the layer on. */
        const char *v = getenv("RECOMP_MKB");
        on = v && *v && *v != '0';
        if (on) {
            fprintf(stderr, "[MKB] mouse+keyboard pad on (F9 toggles capture)\n");
            mkb_load_config();
            if (InterlockedCompareExchange(&s_thread_up, 0, 0) == 0) {
                static LONG started = 0;
                if (InterlockedCompareExchange(&started, 1, 0) == 0)
                    CloseHandle(CreateThread(NULL, 0, mkb_thread,
                                             NULL, 0, NULL));
            }
        }
    }
    return on;
}

/* The recompiled game's process is the foreground one. Raw input with
 * INPUTSINK keeps arriving while the user is in another app, and the
 * mouse must go on behaving normally there. */
static int game_is_foreground(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Hide and confine the pointer while captured. Aiming with raw deltas
 * does not move the pointer by itself, but Windows still moves it from
 * the same physical movement, so it would wander across the screen and
 * end up clicking other apps' title bars. */
static volatile LONG s_cap_now;
int mkb_capturing(void) { return (int)s_cap_now; }
static void mkb_manage_cursor(int capture)
{
    s_cap_now = capture ? 1 : 0;
    static int hidden;          /* how many ShowCursor(FALSE) we owe  */
    static unsigned long last;

    unsigned long now = GetTickCount();
    /* holding a Windows key lets go of the pointer so you can click out (SG1: not Alt any more - Alt = quick weapon switch) */
    if (capture && ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))) { capture = 0; last = 0; }
    if (now - last < 150 && (capture ? hidden : !hidden) == 0)
        return;
    last = now;

    if (capture) {
        RECT r;
        HWND fg = GetForegroundWindow();
        if (fg && GetClientRect(fg, &r)) {   /* client area only: keeps the pointer inside the picture */
            POINT tl = { r.left, r.top }, br = { r.right, r.bottom };
            ClientToScreen(fg, &tl); ClientToScreen(fg, &br);
            r.left = tl.x; r.top = tl.y; r.right = br.x; r.bottom = br.y;
            ClipCursor(&r);
        } else if (fg && GetWindowRect(fg, &r))
            ClipCursor(&r);
        if (hidden < 5) { ShowCursor(FALSE); hidden++; }
    } else {
        if (hidden > 0) { ShowCursor(TRUE); hidden--; }
        ClipCursor(NULL);
    }
}

/* Radial deadzone compensation: an input radius in [0..1] comes out as
 * [dz..1], so the smallest mouse correction still clears the title's
 * own deadzone, and the response stays linear above it. */
static void mkb_aim_vector(double ax, double ay, SHORT *out_x, SHORT *out_y)
{
    double r = sqrt(ax * ax + ay * ay);
    double len, fx, fy;
    long v;

    if (r <= 0.0) { *out_x = 0; *out_y = 0; return; }
    if (r > 1.0) r = 1.0;
    len = s_cfg.dz + (1.0 - s_cfg.dz) * pow(r, s_cfg.acc);
    len *= s_cfg.sens;
    if (len > 1.0) len = 1.0;
    fx = ax * len / sqrt(ax * ax + ay * ay);
    fy = ay * len / sqrt(ax * ax + ay * ay);
    v = (long)(fx * 32767.0);
    *out_x = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    v = (long)(fy * 32767.0);
    *out_y = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

#define MK_VK(k)  (!mkb_txt && (unsigned)(k) < 256 && mk_held((unsigned)(k)) && !s_blk[(k)])


/* Menu cursor feed: mouse counts since the last call and the left button
 * level, independent of the aim path. Returns 0 when capture is off or the
 * game is not the foreground window. */
int mkb_menu_poll(int *dx, int *dy, int *left)
{
    static int en = -1;
    if (en < 0) { const char *e = getenv("RECOMP_MKB_MENU"); en = !(e && e[0] == '0'); }
    *dx = *dy = *left = 0;
    if (!en || !mkb_enabled() || !s_thread_up || !s_active) return 0;
    if (g_sg1_menu_open) { InterlockedExchange(&s_mdx, 0); InterlockedExchange(&s_mdy, 0); return 0; }
    if (!game_is_foreground()) { InterlockedExchange(&s_mdx, 0); InterlockedExchange(&s_mdy, 0); return 0; }
    *dx = (int)InterlockedExchange(&s_mdx, 0);
    *dy = (int)InterlockedExchange(&s_mdy, 0);
    *left = (int)s_mlvl | ((int)s_mrlvl << 1);   /* bit0 left, bit1 right */
    return 1;
}


/* Native mouse look: raw counts go to the engine as IK_MouseX/Y axis events (the
 * game's own MouseX=Axis aMouseX binding and MouseSensitivity then apply), instead
 * of being faked as a joystick. RECOMP_MKB_NATIVE=0 goes back to the stick path. */
int mkb_native_aim(void)
{
    static int en = -1;
    if (en < 0) { const char *e = getenv("RECOMP_MKB_NATIVE"); en = !(e && e[0] == '0'); }
    return en;
}

/* ---- SG1: settings store + overlay state ------------------------------------------------------
 * sg1_settings.txt (key=value lines, next to the exe) holds what the in-game menu changes. */
volatile LONG g_sg1_menu_open;                  /* the Insert overlay is up: the game gets no keyboard/mouse */
int g_mkb_invert_y;
static char s_cfg_k[32][32], s_cfg_v[32][64];
static int s_cfg_n, s_cfg_loaded;
static void cfg_load(void)
{
    FILE *f;
    char line[128];
    s_cfg_loaded = 1;
    f = fopen("sg1_settings.txt", "r");
    if (!f) return;
    while (fgets(line, sizeof line, f) && s_cfg_n < 32) {
        char *eq = strchr(line, '=');
        size_t n;
        if (!eq) continue;
        *eq++ = 0;
        n = strlen(eq);
        while (n && (eq[n - 1] == '\n' || eq[n - 1] == '\r' || eq[n - 1] == ' ')) eq[--n] = 0;
        strncpy(s_cfg_k[s_cfg_n], line, 31); s_cfg_k[s_cfg_n][31] = 0;
        strncpy(s_cfg_v[s_cfg_n], eq, 63); s_cfg_v[s_cfg_n][63] = 0;
        s_cfg_n++;
    }
    fclose(f);
}
const char *sg1_cfg_get(const char *k)
{
    int i;
    if (!s_cfg_loaded) cfg_load();
    for (i = 0; i < s_cfg_n; i++) if (!strcmp(s_cfg_k[i], k)) return s_cfg_v[i];
    return NULL;
}
void sg1_cfg_set(const char *k, const char *v)
{
    int i;
    FILE *f;
    if (!s_cfg_loaded) cfg_load();
    for (i = 0; i < s_cfg_n; i++) if (!strcmp(s_cfg_k[i], k)) break;
    if (i == s_cfg_n) { if (s_cfg_n >= 32) return; s_cfg_n++; strncpy(s_cfg_k[i], k, 31); s_cfg_k[i][31] = 0; }
    strncpy(s_cfg_v[i], v, 63); s_cfg_v[i][63] = 0;
    f = fopen("sg1_settings.txt", "w");
    if (!f) return;
    for (i = 0; i < s_cfg_n; i++) fprintf(f, "%s=%s\n", s_cfg_k[i], s_cfg_v[i]);
    fclose(f);
}
int sg1_cfg_int(const char *k, int def) { const char *v = sg1_cfg_get(k); return v ? atoi(v) : def; }
float sg1_cfg_float(const char *k, float def) { const char *v = sg1_cfg_get(k); return v ? (float)atof(v) : def; }

/* Native-aim scale: env RECOMP_MKB_NATIVE_SCALE, else the saved value, else 50. */
static float s_mscale = -1.0f;
float mkb_scale_get(void)
{
    if (s_mscale < 0) {
        const char *e = getenv("RECOMP_MKB_NATIVE_SCALE");
        s_mscale = e ? (float)atof(e) : sg1_cfg_float("mouse_scale", 50.0f);
        if (s_mscale <= 0) s_mscale = 50.0f;
        g_mkb_invert_y = sg1_cfg_int("invert_y", 0);
    }
    return s_mscale;
}
void mkb_scale_mul(float f)
{
    char b[32];
    float v = mkb_scale_get() * f;
    if (v < 0.5f) v = 0.5f;
    if (v > 2000.0f) v = 2000.0f;
    s_mscale = v;
    snprintf(b, sizeof b, "%.3f", (double)v);
    sg1_cfg_set("mouse_scale", b);
    fprintf(stderr, "[MKB] native mouse scale %.2f\n", (double)v);
}
/* SG1: real game controllers on/off (menu item). Off = XInput pads are ignored and the keyboard and
 * mouse are the only input, so a pad cannot overlap them. RECOMP_CONTROLLERS=0/1 overrides. */
static int s_ctl_on = -1;
int sg1_controllers_on(void)
{
    if (s_ctl_on < 0) {
        const char *e = getenv("RECOMP_CONTROLLERS");
        s_ctl_on = e ? !(e[0] == '0') : (sg1_cfg_int("controllers", 0) != 0);
    }
    return s_ctl_on;
}
void sg1_controllers_toggle(void)
{
    s_ctl_on = !sg1_controllers_on();
    sg1_cfg_set("controllers", s_ctl_on ? "1" : "0");
}

void mkb_invert_y_toggle(void)
{
    mkb_scale_get();
    g_mkb_invert_y = !g_mkb_invert_y;
    sg1_cfg_set("invert_y", g_mkb_invert_y ? "1" : "0");
}

int mkb_game_mouse(int *dx, int *dy)
{
    int menu = s_menu_tick && (LONG)GetTickCount() - s_menu_tick < 250;
    LONG x, y;
    *dx = *dy = 0;
    x = InterlockedExchange(&s_gdx, 0);
    y = InterlockedExchange(&s_gdy, 0);
    if (!mkb_native_aim() || !mkb_enabled() || !s_thread_up || !s_active) return 0;
    if (!game_is_foreground() || menu || s_console_typing || g_sg1_menu_open) return 0;
    *dx = (int)x; *dy = (int)y;
    return (x || y);
}

/* ---- SG1: set the in-game name from the selected profile ----
 * The multiplayer URL takes ?Name= from the engine's default URL, which the
 * profile menu never updates. Once a match is loaded the console command
 * "SetName <profile>" fixes both the player name and the default URL, so the
 * recompiled build types it into the console by itself. */
static char s_prof[40];
static char s_cmd[128];                                       /* generic console command to type (FOV etc.) */
static volatile LONG s_nm_due, s_nm_done;                 /* tick at which to type, 0 = idle */
static KbEvt s_nm_seq[320]; static int s_nm_n, s_nm_i; static DWORD s_nm_t;
void mkb_set_profile(const char *name)
{
    int i, n = 0;
    for (i = 0; name[i] && n < 30; i++) {
        char c = name[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') s_prof[n++] = c; else break;
    }
    s_prof[n] = 0;
    fprintf(stderr, "[MKB] profile name '%s'\n", s_prof); fflush(stderr);
}
/* Type "<cmd>" into the game console after delay_ms (re-arming replaces a pending one). */
void mkb_type_command(const char *cmd, int delay_ms)
{
    LONG due;
    if (s_nm_n > 0) return;                       /* already typing */
    snprintf(s_cmd, sizeof s_cmd, "%s", cmd);
    due = (LONG)(GetTickCount() + (DWORD)delay_ms); if (!due) due = 1;
    s_nm_due = due; s_nm_n = 0; s_nm_i = 0;
}
void mkb_arm_name(int delay_ms)
{
    LONG due;
    /* SG1: the profile name now reaches the URL from script (patch_profile_name.py, Engine.u PlayerProfile.Initialize), so typing
     * "SetName" into the console is off unless RECOMP_TYPE_SETNAME=1 (fallback for installs without that patch). */
    { static int on = -1; if (on < 0) { const char *e = getenv("RECOMP_TYPE_SETNAME"); on = (e && e[0] == '1'); } if (!on) return; }
    if (!s_prof[0]) return;
    if (delay_ms < 0) { if (s_nm_done) return; delay_ms = -delay_ms; } else s_nm_done = 0;
    due = (LONG)(GetTickCount() + (DWORD)delay_ms); if (!due) due = 1;
    s_nm_due = due; s_nm_n = 0; s_nm_i = 0;
    fprintf(stderr, "[MKB] will set in-game name in %d ms\n", delay_ms); fflush(stderr);
}
static void nm_add(unsigned vk, int down, unsigned ch) { if (s_nm_n < 318) { s_nm_seq[s_nm_n].vk = (unsigned short)vk; s_nm_seq[s_nm_n].down = (unsigned char)down; s_nm_seq[s_nm_n].rep = 0; s_nm_seq[s_nm_n].ch = (unsigned short)ch; s_nm_n++; } }
static volatile LONG s_nm_end;
volatile LONG g_sg1_mwheel;
volatile LONG g_sg1_voice_open;    /* V voice menu up: the game gets no mouse wheel and no left button (the rest still works) */
volatile LONG g_sg1_lb_suppress;   /* voice menu closed by a left click: keep that press away from the game (fire) until released */
int mkb_menu_wheel(void) { return (int)InterlockedExchange(&g_sg1_mwheel, 0); }
/* SG1 voice menu: one key press for the game (its User.ini binding runs, e.g. NumPad1=Taunt MSGID_Ok) - no console typing */
void mkb_tap_key(unsigned vk) { kq_push(vk, 1, 0, 0); kq_push(vk, 0, 0, 0); }
int mkb_name_busy(void) { return s_nm_n > 0 || s_nm_due || (s_nm_end && (LONG)(GetTickCount() - (DWORD)s_nm_end) < 0); }
static void autocmd_check(void)
{   /* RECOMP_AUTOCMD="open M01A?Listen": typed into the game console once, RECOMP_AUTOCMD_DELAY seconds (default 30) after start */
    static int st; static DWORD t0; const char *e, *d; int sec;
    if (st == 2) return;
    e = getenv("RECOMP_AUTOCMD");
    if (!e || !*e) { st = 2; return; }
    if (!st) { t0 = GetTickCount(); st = 1; }
    d = getenv("RECOMP_AUTOCMD_DELAY"); sec = d ? atoi(d) : 30; if (sec < 0) sec = 0;
    if (GetTickCount() - t0 >= (DWORD)sec * 1000u) { st = 2; mkb_type_command(e, 100); }
}
static int nm_next(int *vk, int *down, int *rep, int *ch)
{
    DWORD now = GetTickCount();
    autocmd_check();
    if (s_nm_due && !s_nm_n) {
        if ((LONG)(now - (DWORD)s_nm_due) < 0) return 0;
        {   const char *cmd = s_cmd[0] ? s_cmd : "SetName "; int i;
            nm_add(0x67, 1, 0); nm_add(0x67, 0, 0);
            nm_add(0x20, 1, ' '); nm_add(0x20, 0, 0);          /* first char is swallowed */
            for (i = 0; cmd[i]; i++) { unsigned c = (unsigned char)cmd[i]; unsigned v = (c >= 'a' && c <= 'z') ? c - 32 : (c == ' ' ? 0x20 : c == '.' ? 0xBE : c == '=' ? 0xBB : c == '\\' ? 0xDC : c == '/' ? 0xBF : c == '?' ? 0xBF : c == '-' ? 0xBD : c == '\'' ? 0xDE : c == '(' ? 0x39 : c == ')' ? 0x30 : c); nm_add(v, 1, c); nm_add(v, 0, 0); }
            if (!s_cmd[0]) for (i = 0; s_prof[i]; i++) { unsigned c = (unsigned char)s_prof[i]; unsigned v = (c >= 'a' && c <= 'z') ? c - 32 : c; nm_add(v, 1, c); nm_add(v, 0, 0); }
            nm_add(0x0D, 1, 0); nm_add(0x0D, 0, 0);
        }
        s_nm_i = 0; s_nm_t = now;
        fprintf(stderr, "[MKB] typing %s%s\n", s_cmd[0] ? s_cmd : "SetName ", s_cmd[0] ? "" : s_prof); fflush(stderr);
    }
    if (!s_nm_n) return 0;
    if (s_nm_i >= s_nm_n) { s_nm_n = 0; s_nm_due = 0; s_nm_done = 1; s_cmd[0] = 0; s_nm_end = (LONG)(now + 100); return 0; }
    {   static int ms = -1;      /* key spacing in ms; was 35, now 1. RECOMP_TYPE_MS overrides it (raise it if typed commands lose characters) */
        if (ms < 0) { const char *e = getenv("RECOMP_TYPE_MS"); ms = e ? atoi(e) : 1; if (ms < 1) ms = 1; if (ms > 200) ms = 200; }
        if (now - s_nm_t < (s_nm_i < 2 ? 40u : (unsigned)ms)) return 0;
    }
    s_nm_t = now;
    *vk = s_nm_seq[s_nm_i].vk; *down = s_nm_seq[s_nm_i].down; *rep = 0; *ch = s_nm_seq[s_nm_i].ch;
    s_nm_i++;
    return 1;
}

/* Next real keyboard event for the game. Returns 0 when none (or capture is off). */
int mkb_kbd_pop(int *vk, int *down, int *rep, int *ch)
{
    static int en = -1;
    if (en < 0) { const char *e = getenv("RECOMP_MKB_KEYS"); en = !(e && e[0] == '0'); }
    if (!en || !mkb_enabled() || !s_thread_up) return 0;
    autocmd_check();   /* RECOMP_AUTOCMD: arms the console command once the delay has passed */
    if (s_nm_due && s_active && game_is_foreground() && nm_next(vk, down, rep, ch)) return 1;
    if (s_kq_r == s_kq_w) return 0;
    if (!s_active || !game_is_foreground()) { s_kq_r = s_kq_w; return 0; }
    {
        KbEvt *e = &s_kq[s_kq_r & 255];
        *vk = e->vk; *down = e->down; *rep = e->rep; *ch = e->ch;
        MemoryBarrier();
        s_kq_r++;
        /* while the name is being typed, keys you are physically holding must not leak into the console */
        if (mkb_name_busy() && *down) return 0;
        if (g_sg1_menu_open && *down) return 0;     /* the overlay owns the keyboard; key-ups still pass so nothing sticks */
    }
    return 1;
}

int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    SHORT aim_x = 0, aim_y = 0, mov_x = 0, mov_y = 0;
    WORD b = 0;
    BYTE analog[8];
    LONG dx, dy, wheel, btn;
    int any = 0, i;
    static DWORD packet;

    if (!pState || dwPort != 0 || !mkb_enabled())
        return 0;
    mkb_txt = mkb_text_mode();
    { int bk; for (bk = 0; bk < 256; bk++) { if (mkb_txt && s_vk_down[bk]) s_blk[bk] = 1; else if (!s_vk_down[bk]) s_blk[bk] = 0; } }
    if (!s_thread_up)
        return 0;

    if (g_sg1_menu_open) {
        mkb_manage_cursor(0);
        InterlockedExchange(&s_dx, 0);
        InterlockedExchange(&s_dy, 0);
        return 0;
    }

    if (!s_active) {
        mkb_manage_cursor(0);
        return 0;
    }

    /* Foreground gate: while the user is elsewhere the mouse belongs to
     * whoever has it, and the game should read a pad at rest. */
    if (!game_is_foreground()) {
        mkb_manage_cursor(0);
        InterlockedExchange(&s_dx, 0);
        InterlockedExchange(&s_dy, 0);
        return 0;
    }
    mkb_manage_cursor(1);

    dx    = InterlockedExchange(&s_dx, 0);
    dy    = InterlockedExchange(&s_dy, 0);
    {   /* SG1: the emulated USB pad is polled about every millisecond, far
         * faster than the game reads it, so most polls see no mouse movement
         * and the game usually reads a centred stick. Report the movement of
         * the last ~40 ms instead, so a read at any moment sees it. */
        static struct { LONGLONG t; LONG dx, dy; } hist[128];
        static int hn;
        static LONGLONG freq;
        LARGE_INTEGER now;
        LONGLONG win;
        LONG sx = 0, sy = 0;
        int k;
        if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
        QueryPerformanceCounter(&now);
        win = freq * 40 / 1000;
        if (dx || dy) {
            hist[hn & 127].t = now.QuadPart; hist[hn & 127].dx = dx; hist[hn & 127].dy = dy;
            hn++;
        }
        for (k = 0; k < 128 && k < hn; k++) {
            int i = (hn - 1 - k) & 127;
            if (now.QuadPart - hist[i].t > win) break;
            sx += hist[i].dx; sy += hist[i].dy;
        }
        dx = sx; dy = sy;
    }
    wheel = InterlockedExchange(&s_wheel, 0);
    btn   = (LONG)s_mouse_btn;   /* SG1: level, not a one-poll pulse: the game reads the pad far less often than we poll */
    if (g_sg1_voice_open) btn &= ~0x01;
    else if (g_sg1_lb_suppress) { if (btn & 0x01) btn &= ~0x01; else g_sg1_lb_suppress = 0; }

    memset(analog, 0, sizeof analog);

    /* ---- mouse buttons, as the Xbox pad sees them ---- */
    if (btn & 0x01) analog[XBOX_BUTTON_RTRIGGER] = 255;  /* fire      */
    if (btn & 0x02) analog[XBOX_BUTTON_LTRIGGER] = 255;  /* ADS/aim   */
    if (btn & 0x04) b |= 0;                              /* M: below  */

    /* WASD / arrows / letters: left to the game's own keyboard bindings (System\User.ini) */

    /* ---- mouse: aim ---- */
    if ((dx || dy) && !mkb_native_aim()) {
        double ax =  (double)dx / (double)s_cfg.full;
        double ay = -(double)dy / (double)s_cfg.full;
        if (s_cfg.invert_y) ay = -ay;
        mkb_aim_vector(ax, ay, &aim_x, &aim_y);
        {
            static int shown;
            if (shown < 25) {
                shown++;
                fprintf(stderr, "  [MKB] aim counts dx=%ld dy=%ld -> stick %d,%d (raw events %ld, absolute %ld)\n",
                        (long)dx, (long)dy, (int)aim_x, (int)aim_y, (long)s_raw_events, (long)s_abs_events);
            }
        }
    }
    {
        static DWORD last_report;
        DWORD tnow = GetTickCount();
        if (tnow - last_report > 10000) {
            last_report = tnow;
            fprintf(stderr, "  [MKB] status: raw mouse events %ld (absolute %ld), foreground ok\n",
                    (long)s_raw_events, (long)s_abs_events);
        }
    }

    /* ---- keyboard: buttons (Black's layout) ---- */
    if (btn & 0x08)       analog[XBOX_BUTTON_WHITE] = 255; /* X1        */
    if (btn & 0x10)       analog[XBOX_BUTTON_BLACK] = 255; /* X2        */
    if (MK_VK(VK_RETURN) || MK_VK(VK_ESCAPE)) { b |= XBOX_GAMEPAD_START; if (!s_startdbg) fprintf(stderr, "[STARTDBG] mkb start edge\n"); s_startdbg = 1; } else s_startdbg = 0;
    if (MK_VK(VK_TAB))                         b |= XBOX_GAMEPAD_BACK;
    /* LShift handled by the keyboard layer (Walking in User.ini); no pad LEFT_THUMB (= duck toggle) */
    /* LCtrl handled by the keyboard layer (Duck in User.ini); no pad RIGHT_THUMB */

    /* ---- wheel / middle button: cycle weapon ---- */
    (void)wheel;   /* wheel = weapon select, middle button = Zoom: both native bindings */

    /* ---- merge on top of whatever the host pad produced ---- */
    if (b)        { pState->Gamepad.wButtons |= b; any = 1; }
    for (i = 0; i < 8; i++) {
        if (analog[i]) {
            if (analog[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = analog[i];
            any = 1;
        }
    }
    if (aim_x || aim_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbLX = aim_x;
            pState->Gamepad.sThumbLY = aim_y;
        } else {
            pState->Gamepad.sThumbRX = aim_x;
            pState->Gamepad.sThumbRY = aim_y;
        }
        any = 1;
    }
    if (mov_x || mov_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbRX = mov_x;
            pState->Gamepad.sThumbRY = mov_y;
        } else {
            pState->Gamepad.sThumbLX = mov_x;
            pState->Gamepad.sThumbLY = mov_y;
        }
        any = 1;
    }

    /* Edge detection: a title polling for changes ignores a state whose
     * packet number did not move. */
    if (any)
        pState->dwPacketNumber = ++packet;
    return any;
}
