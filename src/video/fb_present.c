/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 * RECOMP_D3D_PRESENT=1 shows the same frames through a D3D11 swap chain of
 * the window's own instead (gpu_present.c): scaled on the GPU, vsync-paced.
 *
 * The window opens at RECOMP_WINDOW_SCALE times the frame (default 2, made
 * smaller until it fits the desktop) and can be resized; the frame is
 * letterboxed to its 4:3 shape. F11 or Alt+Enter toggles borderless
 * fullscreen, RECOMP_FULLSCREEN=1 starts in it. Closing the window ends the
 * process, as the SDL window does.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Which keys are down, for the pad stand-in in src/input: this window
 * records them in the shared key table (src/input/keyboard.c).
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them. */
#include "keyboard.h"   /* src/input */
#include "gpu_present.h"
#include "recomp_exit_hook.h"   /* src/platform */
#include <stdbool.h>
#include "pvideo_scanout.h"
#include "../nv2a/nv2a_mmio_hook.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */
/* SG1: full-resolution copy of the same frame, when the renderer runs above 640x480. The renderer
 * installs the hook (d3d11_backend.c); 0 from it means "use the normal frame". */
int (*xbox_fb_hires_hook)(uint32_t va, const uint32_t **pix, uint32_t *w, uint32_t *h);
static uint32_t     *s_hi[2];
static uint32_t      s_hi_w[2], s_hi_h[2];
static size_t        s_hi_cap[2];
static uint32_t     *s_hi_show;            /* window thread's copy */
static size_t        s_hi_show_cap;

/* The last PVIDEO frame shown. A frame stays on screen until the next one or
 * STOP, but once its bank is consumed the title may reuse the memory, so the
 * pixels are kept here rather than read again. */
static uint8_t      *s_pvideo_source;
static size_t        s_pvideo_bytes;
static unsigned      s_pvideo_bank;
static uint32_t      s_pvideo_regs[0x1000 / 4];

/* Draw the video overlay over s_rgb: a newly submitted bank if there is one
 * (copied, drawn, then handed back to the title), else the last frame. */
static int s_ov_active;      /* the video overlay drew into s_rgb this frame */
static void fb_overlay(void)
{
    uint32_t regs[0x1000 / 4];
    unsigned bank;
    bool presented = false;

    s_ov_active = 0;
    if (!s_rgb || !nv2a_hook_pvideo_snapshot(regs)) return;
    if ((regs[NV_PVIDEO_STOP / 4] & 1u) ||
        (s_pvideo_source &&
         regs[(NV_PVIDEO_SIZE_IN + s_pvideo_bank * 4) / 4] == UINT32_MAX)) {
        free(s_pvideo_source);
        s_pvideo_source = NULL;
        if (regs[NV_PVIDEO_STOP / 4] & 1u) return;
    }
    for (bank = 0; bank < 2; ++bank) {
        uint64_t physical = (uint64_t)regs[(NV_PVIDEO_BASE + bank * 4) / 4] +
                            regs[(NV_PVIDEO_OFFSET + bank * 4) / 4];
        unsigned pitch = regs[(NV_PVIDEO_FORMAT + bank * 4) / 4] & NV_PVIDEO_FORMAT_PITCH;
        unsigned height = (regs[(NV_PVIDEO_SIZE_IN + bank * 4) / 4] >> 16) & 0x7ff;
        uint64_t bytes = (uint64_t)pitch * height;
        uintptr_t va;
        MEMORY_BASIC_INFORMATION mapping;
        uint8_t *snapshot;

        if (!(regs[NV_PVIDEO_BUFFER / 4] & (1u << (bank * 4u)))) continue;
        if (!bytes || physical + bytes > 0x04000000u) continue;
        va = (uintptr_t)(0x80000000u + (uint32_t)physical) +
             (uintptr_t)xbox_GetMemoryOffset();
        if (!VirtualQuery((const void *)va, &mapping, sizeof(mapping)) ||
            mapping.State != MEM_COMMIT ||
            (mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            bytes > mapping.RegionSize - (size_t)(va - (uintptr_t)mapping.BaseAddress))
            continue;
        snapshot = (uint8_t *)malloc((size_t)bytes);
        if (!snapshot) continue;
        memcpy(snapshot, (const void *)va, (size_t)bytes);
        if (!pvideo_scanout(regs, snapshot, (size_t)bytes, s_rgb,
                            s_fb_width, s_fb_height, bank)) {
            free(snapshot);
            continue;
        }
        free(s_pvideo_source);
        s_pvideo_source = snapshot;
        s_pvideo_bytes = (size_t)bytes;
        s_pvideo_bank = bank;
        memcpy(s_pvideo_regs, regs, sizeof(s_pvideo_regs));
        presented = true; s_ov_active = 1;
        nv2a_hook_pvideo_consume(bank);
    }
    if (!presented && s_pvideo_source) {
        s_ov_active = 1;
        pvideo_scanout(s_pvideo_regs, s_pvideo_source, s_pvideo_bytes, s_rgb,
                       s_fb_width, s_fb_height, s_pvideo_bank);
    }
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Vertical anti-aliasing factor of the surface being shown: 2 for 2x2, where
 * the surface has twice the rows as well as twice the columns, and 1 for
 * everything else (2x1 included). Only the pushbuffer executor knows it. */
static uint32_t s_fb_aa_sy = 1;

void xbox_FramebufferWindowSetAA(uint32_t sy)
{
    s_fb_aa_sy = sy ? sy : 1;
}

/* Set while the NPC spawner types its console commands: keep presenting so the game is not frozen and the typing is visible. */
volatile int g_sg1_show_typing;

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    {   extern int mkb_name_busy(void);
        if (getenv("RECOMP_HIDE_NAME_TYPING") && mkb_name_busy() && !g_sg1_show_typing) return;   /* old behaviour (frozen screen while the name is typed) only if the env var is set */
    }
    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    {   /* SG1 diag: log each flip surface the first time it is seen */
        static uint32_t seen[32]; static int ns; int k, found = 0;
        for (k = 0; k < ns; k++) if (seen[k] == fb_va) { found = 1; break; }
        if (!found && ns < 32) { seen[ns++] = fb_va; fprintf(stderr, "[PRESENT] new flip surface va %08X pitch %u\n", fb_va, pitch); }
    }
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* 2x-wide anti-aliased 32-bit surface: every other pixel, as in
             * fb_convert. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    s_hi_w[next] = 0;
    if (xbox_fb_hires_hook) {
        const uint32_t *hp; uint32_t hw, hh;
        if (xbox_fb_hires_hook(fb_va, &hp, &hw, &hh) && hw && hh) {
            size_t need = (size_t)hw * hh;
            if (s_hi_cap[next] < need) {
                free(s_hi[next]);
                s_hi[next] = (uint32_t *)malloc(need * 4);
                s_hi_cap[next] = s_hi[next] ? need : 0;
            }
            if (s_hi[next]) { memcpy(s_hi[next], hp, need * 4); s_hi_w[next] = hw; s_hi_h[next] = hh; }
        }
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

/* SG1: short note shown in the title bar (current render resolution) */
static wchar_t       s_note[64];
static volatile LONG s_note_dirty;
void xbox_FramebufferWindowSetNote(const char *text)
{
    int i;
    for (i = 0; text && text[i] && i < 63; i++) s_note[i] = (wchar_t)(unsigned char)text[i];
    s_note[i] = 0;
    InterlockedExchange(&s_note_dirty, 1);
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}


/* ---- SG1: in-game settings overlay (Insert) ---------------------------------------------------
 * Drawn with GDI into a private DIB and composited onto the copy of the frame the window shows, so it
 * works for both the 640x480 and the high-resolution path. Keyboard only: Up/Down select, Left/Right
 * change, Insert or Esc close. While it is open mkb_input gives the game no keyboard or mouse. */
extern volatile LONG g_sg1_menu_open;
extern float mkb_scale_get(void);
extern void mkb_scale_mul(float f);
extern int g_mkb_invert_y;
extern void mkb_invert_y_toggle(void);
extern int sg1_controllers_on(void);
extern void sg1_controllers_toggle(void);
/* backend (d3d11_backend.c) items: resolution, anisotropy, mipmaps */
int g_sg1_bm_count;
void (*g_sg1_bm_text)(int item, char *buf, int n);
void (*g_sg1_bm_change)(int item, int dir);

static int s_mn_sel;

static int mn_key_edge(int vk, int *prev)
{
    int d = (GetAsyncKeyState(vk) & 0x8000) != 0, e = d && !*prev;
    *prev = d;
    return e;
}

/* Menu pages. Entry codes: 0..2 = front-end items (mouse, invert Y, controllers), 100+k = back-end item k
 * (d3d11_backend.c), 900 = open the debug page, 901 = back to the main page. */
static int s_mn_page;
static const int s_mn_main[] = { 0, 1, 2, 114, 902, 903, 900 };
static const int s_mn_hud[]  = { 117, 118, 901 };   /* page 3: HUD options */
static const int s_mn_gfx[]  = { 3, 100, 101, 102, 103, 113, 116, 104, 108, 109, 110, 901 };   /* page 4: graphics */
static const int s_mn_dbg[]  = { 115, 111, 112, 105, 106, 107, 901 };

/* Page 2: secret NPC spawner (Del). Types console commands into the game: summon StargateContent.<name>.
 * Pawns spawned during play get no controller and no weapons (Pawn.PostBeginPlay / StargatePawn.PostBeginPlay only do that while
 * Level.bStartup is set), so each spawn briefly sets bStartup and gives the class a controller class (ctrl). ctrl NULL = no brain. */
static const struct { const char *name, *ctrl; } s_npc_def[] = {
    { "JaffaHorusL", "JaffaBotLight" },  { "JaffaHorusM", "JaffaBotMedium" },  { "JaffaHorusH", "JaffaBotHeavy" },
    { "JaffaSerpentL", "JaffaBotLight" }, { "JaffaSerpentM", "JaffaBotMedium" }, { "JaffaSerpentH", "JaffaBotHeavy" },
    { "JaffaJackalL", "JaffaBotLight" },  { "JaffaJackalM", "JaffaBotMedium" },  { "JaffaJackalH", "JaffaBotHeavy" },
    { "JaffaCaptain", "JaffaBotMedium" }, { "JaffaGrunt", "JaffaBotLight" },     { "GoauldFirstPrime", "JaffaBotHeavy" },
    { "HaakenGrunt", "HaakenBot" },       { "HaakenWarrior", "HaakenBot" },      { "HaakenLeader", "HaakenLeaderBot" },
    { "Goauld", "BotGoauld" },            { "Anubis", NULL },
    { "Replicator", "BotEnemy" },         { "MediumGreenReplicator", "BotEnemy" }, { "MediumPurpleReplicator", "BotEnemy" },
    { "BigReplicator", "BotEnemy" },      { "ReplicatorQueen", "ReplicatorQueenBot" },
    { "Ptero", "PteroBot" },              { "PteroBoss", "PteroBossBot" },
};
#define NPC_COUNT ((int)(sizeof s_npc_def / sizeof *s_npc_def))
extern void mkb_type_command(const char *cmd, int delay_ms);
extern int mkb_name_busy(void);
/* Experimental: the replicator classes have no mesh in this build. Assign one with the console's `set`, then summon. */
static const struct { const char *cls, *set_cmd, *summon, *kill, *skin; } s_exp[] = {
    { "ReplicatorQueen",       "set ReplicatorQueen Mesh SkeletalMesh'replicator_queen.replicator_queen'", "summon StargateContent.ReplicatorQueen", "killall StargateContent.ReplicatorQueen", NULL },
    { "Replicator",            "set Replicator Mesh SkeletalMesh'ReplicatorAnims.repmesh17'",             "summon StargateContent.Replicator", "killall StargateContent.Replicator", NULL },
    { "BigReplicator",         "set BigReplicator Mesh SkeletalMesh'ReplicatorAnims.bigrep'",             "summon StargateContent.BigReplicator", "killall StargateContent.BigReplicator", NULL },
    /* the medium ones used to come out as the small mesh; the mesh only references the green texture, so purple gets a skin */
    { "MediumGreenReplicator",  "set MediumGreenReplicator Mesh SkeletalMesh'ReplicatorAnims.repmesh18'",  "summon StargateContent.MediumGreenReplicator",  "killall StargateContent.MediumGreenReplicator",  NULL },
    { "MediumPurpleReplicator", "set MediumPurpleReplicator Mesh SkeletalMesh'ReplicatorAnims.repmesh18'", "summon StargateContent.MediumPurpleReplicator", "killall StargateContent.MediumPurpleReplicator", "set MediumPurpleReplicator Skins (Texture'ReplicatorTex.RepTexPink')" },
};
#define EXP_COUNT ((int)(sizeof s_exp / sizeof *s_exp))
#define CHAIN_MAX 10
static const char *s_chain[CHAIN_MAX]; static int s_chain_n, s_chain_i;
static char s_chain_buf[4][112];
static void mn_chain_tick(void)
{   /* types the queued console commands one after another, each once the previous has finished */
    if (s_chain_i < s_chain_n && !mkb_name_busy()) mkb_type_command(s_chain[s_chain_i++], 30);
    if (s_chain_i >= s_chain_n && !mkb_name_busy()) g_sg1_show_typing = 0;   /* chain finished */
}
static void mn_chain_add(const char *c) { if (s_chain_n < CHAIN_MAX) s_chain[s_chain_n++] = c; }
static int s_ai_on;   /* row 0 of the list: while on, Level.bStartup stays true, so spawned pawns get a controller and their default weapons */
static void mn_spawn_npc(int row)      /* row 0 = the AI switch, rows 1.. = NPCs */
{
    int i = row - 1, k, redo = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;      /* Shift: redo the one-time setup steps */
    static int mesh_done[EXP_COUNT], brain_done[NPC_COUNT];
    const char *name, *ctrl;
    s_chain_n = 0; s_chain_i = 0;
    g_sg1_show_typing = 1;          /* let the player see the console while it types */
    if (row == 0) {
        if (!redo) s_ai_on = !s_ai_on;        /* Shift+Right just sends the current state again (a new map resets the flag) */
        mn_chain_add(s_ai_on ? "set LevelInfo bStartup true" : "set LevelInfo bStartup false");
        return;
    }
    if (i < 0 || i >= NPC_COUNT) return;
    name = s_npc_def[i].name; ctrl = s_ai_on ? s_npc_def[i].ctrl : NULL;
    snprintf(s_chain_buf[0], sizeof s_chain_buf[0], "summon StargateContent.%s", name);
    for (k = 0; k < EXP_COUNT; k++) if (!strcmp(name, s_exp[k].cls)) {
        if (!mesh_done[k] || redo) {   /* the mesh stays assigned for the rest of the run */
            mesh_done[k] = 1;
            if (s_ai_on) mn_chain_add("set LevelInfo bStartup false");   /* the bare meshless pawn must not get a brain (it would wander off unseen) */
            mn_chain_add(s_exp[k].summon); mn_chain_add(s_exp[k].set_cmd);   /* summon first so the class is loaded, then set the mesh */
            if (s_exp[k].skin) mn_chain_add(s_exp[k].skin);
            mn_chain_add(s_exp[k].kill);                                     /* remove the meshless one so the real one does not collide with it */
        }
        break;
    }
    /* the log shows bStartup does not stay true after the mesh setup, so re-arm it right before every AI spawn (one short extra command) */
    if (s_ai_on) mn_chain_add("set LevelInfo bStartup true");
    if (ctrl) {
        if (!brain_done[i] || redo) {
            brain_done[i] = 1;
            snprintf(s_chain_buf[1], sizeof s_chain_buf[1], "set %s ControllerClass Class'%s.%s'", name, strcmp(ctrl, "BotEnemy") ? "StargateContent" : "StargateGame", ctrl);   /* BotEnemy lives in StargateGame; the wrong package gave None, i.e. no brain at all */
            mn_chain_add(s_chain_buf[1]);
        }
        mn_chain_add(s_chain_buf[0]);
    } else mn_chain_add(s_chain_buf[0]);
}
/* Game resolution: types SETRES WxH into the game console (needs the HD display modes). */
extern int g_sg1_mode_w, g_sg1_mode_h;
static const struct { int w, h; } s_gres[3] = { { 1920, 1080 }, { 1280, 720 }, { 640, 480 } };
static int s_gres_sel = -1;
static void gres_init(void) { if (s_gres_sel < 0) { int i; s_gres_sel = 1; for (i = 0; i < 3; i++) if (g_sg1_mode_w == s_gres[i].w && g_sg1_mode_h == s_gres[i].h) s_gres_sel = i; } }
/* SG1: voice menu (V in game): each row presses a key whose User.ini binding is  Taunt <MSGID>  (NumPad0-9, F13-F17;
 * written by tools\controls_preset.py; patch_coop_voicetaunt.py makes ServerTaunt call the voice pack), so nothing is typed
 * into the console. Only IDs that all four SG-1 voice packs have. Wheel = select, left click = say. */
static const struct { const char *label, *id; unsigned vk; } s_voice[] = {
    { "Taunt",              "MSGID_Taunt",            0x61 }, { "OK / affirmative",   "MSGID_Ok",               0x62 },
    { "Negative",           "MSGID_NotOk",            0x63 }, { "Ready",              "MSGID_Ready",            0x64 },
    { "Enemy spotted",      "MSGID_SpotEnemy",        0x65 }, { "Take cover",         "MSGID_TakeCover",        0x66 },
    { "Enemy killed",       "MSGID_EnemyKilled",      0x67 }, { "Area clear",         "MSGID_AllEnemiesKilled", 0x68 },
    { "Under fire",         "MSGID_UnderFire",        0x69 }, { "Team under fire",    "MSGID_TeamUnderFire",    0x60 },
    { "Man down",           "MSGID_ManDown",          0x7C }, { "In cover",           "MSGID_InCoverPosition",  0x7D },
    { "Holding fire",       "MSGID_HoldingFire",      0x7E }, { "Firing",             "MSGID_Firing",           0x7F },
    { "Friendly fire!",     "MSGID_FriendlyFire",     0x80 },
};
#define VOICE_COUNT ((int)(sizeof s_voice / sizeof *s_voice))
static void mn_say(int row)
{
    extern void mkb_tap_key(unsigned vk);
    if (row < 0 || row >= VOICE_COUNT) return;
    mkb_tap_key(s_voice[row].vk);
}
static int mn_items(void) { if (s_mn_page == 5) return VOICE_COUNT; return s_mn_page == 4 ? (int)(sizeof s_mn_gfx / sizeof *s_mn_gfx) : s_mn_page == 3 ? (int)(sizeof s_mn_hud / sizeof *s_mn_hud) : s_mn_page == 2 ? NPC_COUNT + 1 : s_mn_page ? (int)(sizeof s_mn_dbg / sizeof *s_mn_dbg) : (int)(sizeof s_mn_main / sizeof *s_mn_main); }
static int mn_code(int i) { if (s_mn_page == 5) return 2000 + i; return s_mn_page == 4 ? s_mn_gfx[i] : s_mn_page == 3 ? s_mn_hud[i] : s_mn_page == 2 ? 1000 + i : s_mn_page ? s_mn_dbg[i] : s_mn_main[i]; }

static void mn_text(int i, char *b, int n)
{
    int c = mn_code(i);
    b[0] = 0;
    if (c >= 2000) { int k = c - 2000; snprintf(b, n, "%s", s_voice[k].label); return; }
    if (c >= 1000) { if (c == 1000) snprintf(b, n, "AI (bStartup)  < %s >   Right = toggle", s_ai_on ? "ON" : "off"); else if (c - 1001 < NPC_COUNT) snprintf(b, n, "%s%s", s_npc_def[c - 1001].name, s_npc_def[c - 1001].ctrl ? "" : "  (no AI)"); return; }
    if (c == 3) { gres_init(); snprintf(b, n, "Game res (EXPERIMENTAL) < %dx%d >", s_gres[s_gres_sel].w, s_gres[s_gres_sel].h); }
    else if (c == 0) snprintf(b, n, "Mouse sensitivity   < %.1f >", (double)mkb_scale_get());
    else if (c == 1) { mkb_scale_get(); snprintf(b, n, "Invert mouse Y       < %s >", g_mkb_invert_y ? "on" : "off"); }
    else if (c == 2) snprintf(b, n, "Game controllers    < %s >", sg1_controllers_on() ? "on" : "off");
    else if (c == 900) snprintf(b, n, "Debug menu          >");
    else if (c == 902) snprintf(b, n, "HUD options         >");
    else if (c == 903) snprintf(b, n, "Graphics            >");
    else if (c == 901) snprintf(b, n, "<  Back");
    else if (c >= 100 && c < 200 && g_sg1_bm_text && c - 100 < g_sg1_bm_count) g_sg1_bm_text(c - 100, b, n);
}

static void mn_change(int i, int dir)
{
    int c = mn_code(i);
    if (c == 3) { char cmd[32]; gres_init(); s_gres_sel = (s_gres_sel + (dir > 0 ? 1 : 2)) % 3; snprintf(cmd, sizeof cmd, "SETRES %dx%d", s_gres[s_gres_sel].w, s_gres[s_gres_sel].h); mkb_type_command(cmd, 700); }   /* typed into the console once you stop pressing */
    else if (c == 0) mkb_scale_mul(dir > 0 ? 1.25f : 1.0f / 1.25f);
    else if (c == 1) mkb_invert_y_toggle();
    else if (c == 2) sg1_controllers_toggle();
    else if (c == 900) { if (dir > 0) { s_mn_page = 1; s_mn_sel = 0; } }
    else if (c == 902) { if (dir > 0) { s_mn_page = 3; s_mn_sel = 0; } }
    else if (c == 903) { if (dir > 0) { s_mn_page = 4; s_mn_sel = 0; } }
    else if (c == 901) { s_mn_page = 0; s_mn_sel = 0; }
    else if (c >= 100 && c < 200 && g_sg1_bm_change && c - 100 < g_sg1_bm_count) g_sg1_bm_change(c - 100, dir);
}

/* SG1: the game's gamma ramp (Brightness/Contrast/Gamma). The game writes it to the DAC palette; the
 * recomp captures it (hook in sub_00456DBE) and applies it to the finished frame here. */
uint8_t g_sg1_gamma[768];
volatile int g_sg1_gamma_set;
int g_sg1_gamma_on = 1;
static void sg1_apply_gamma(uint32_t *px, uint32_t w, uint32_t h)
{
    static uint8_t lut[768];
    static int ident = 1, seen;
    size_t i, n = (size_t)w * h;
    if (!g_sg1_gamma_on || !px) return;
    if (g_sg1_gamma_set) {
        int k;
        g_sg1_gamma_set = 0;
        memcpy(lut, g_sg1_gamma, 768);
        ident = 1;
        for (k = 0; k < 256; k++)
            if (lut[k] != k || lut[256 + k] != k || lut[512 + k] != k) ident = 0;
        seen = 1;
        if (getenv("RECOMP_GAMMA_LOG"))
            fprintf(stderr, "[GAMMA] ramp R[0,64,128,192,255]=%u,%u,%u,%u,%u G[128]=%u B[128]=%u identity=%d\n",
                    lut[0], lut[64], lut[128], lut[192], lut[255], lut[256 + 128], lut[512 + 128], ident);
    }
    if (!seen || ident) return;
    for (i = 0; i < n; i++) {
        uint32_t c = px[i];
        px[i] = (c & 0xFF000000u) | ((uint32_t)lut[(c >> 16) & 255] << 16) | ((uint32_t)lut[256 + ((c >> 8) & 255)] << 8) | lut[512 + (c & 255)];
    }
}

static void sg1_menu_frame(HWND hwnd, uint32_t *px, uint32_t w, uint32_t h)
{
    static int p_ins, p_esc, p_up, p_dn, p_lt, p_rt;
    int open = (int)g_sg1_menu_open;
    int fg = GetForegroundWindow() == hwnd;
    int ins = mn_key_edge(VK_INSERT, &p_ins), esc = mn_key_edge(VK_ESCAPE, &p_esc);
    int up = mn_key_edge(VK_UP, &p_up), dn = mn_key_edge(VK_DOWN, &p_dn);
    int lt = mn_key_edge(VK_LEFT, &p_lt), rt = mn_key_edge(VK_RIGHT, &p_rt);
    int ret = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    static int p_pgup;
    int pgup = mn_key_edge(VK_DELETE, &p_pgup);   /* Del opens the secret NPC spawner */
    static int p_ret;
    int rete = ret && !p_ret;
    p_ret = ret;
    /* Voice menu (V): an overlay only - the game keeps the keyboard and mouse (move, look, aim). It takes just the mouse wheel
     * (select, no weapon switch) and the left button (say, no shot) - see g_sg1_voice_open in mkb_input.c. */
    static int p_v, p_lb, s_voice_open, s_voice_sel;   /* s_voice_sel: the row the voice menu was left on (kept for the next open) */
    int vkey = mn_key_edge('V', &p_v), lclick = mn_key_edge(VK_LBUTTON, &p_lb);
    mn_chain_tick();
    if (!fg) return;
    {   extern int mkb_in_game(void); extern int mkb_text_mode(void); extern int mkb_menu_wheel(void);
        extern volatile LONG g_sg1_voice_open, g_sg1_lb_suppress;
        if (s_voice_open && (!mkb_in_game() || mkb_text_mode() || open || ins || esc)) { s_voice_open = 0; g_sg1_voice_open = 0; }
        if (vkey && !open && !mkb_text_mode() && (s_voice_open || mkb_in_game())) {
            s_voice_open = !s_voice_open; g_sg1_voice_open = s_voice_open; mkb_menu_wheel();
            if (s_voice_open) s_mn_sel = s_voice_sel;
        }
        if (s_voice_open) {
            int wh = mkb_menu_wheel();
            if (wh) s_mn_sel = ((s_mn_sel - wh) % VOICE_COUNT + VOICE_COUNT) % VOICE_COUNT;
            if (s_mn_sel >= VOICE_COUNT || s_mn_sel < 0) s_mn_sel = 0;
            s_voice_sel = s_mn_sel;
            if (lclick) { mn_say(s_mn_sel); s_voice_open = 0; g_sg1_voice_open = 0; g_sg1_lb_suppress = 1; return; }
            s_mn_page = 5; open = 1;                         /* draw page 5 below; the Insert menu stays closed (g_sg1_menu_open = 0) */
            ins = esc = up = dn = lt = rt = rete = pgup = 0;
        }
    }
    if (pgup) { if (open && s_mn_page == 2) { open = 0; g_sg1_menu_open = 0; } else { open = 1; g_sg1_menu_open = 1; s_mn_page = 2; s_mn_sel = 0; } ins = 0; esc = 0; }
    if (open && esc && s_mn_page == 2) { open = 0; g_sg1_menu_open = 0; esc = 0; ins = 0; }
    if (open && esc && s_mn_page) { s_mn_page = 0; s_mn_sel = 0; esc = 0; ins = 0; }   /* Esc on the debug page goes back */
    if (ins || (open && esc)) { open = !open; g_sg1_menu_open = open; if (open) { s_mn_sel = 0; s_mn_page = 0; } }
    if (!open || !px || w < 160 || h < 120) return;
    {
        int n = mn_items(), r;
        if (up) s_mn_sel = (s_mn_sel + n - 1) % n;
        if (dn) s_mn_sel = (s_mn_sel + 1) % n;
        if (s_mn_page == 2) {
            if (rt) { mn_spawn_npc(s_mn_sel); open = 0; g_sg1_menu_open = 0; return; }
        } else {
        if (lt) { if (s_mn_page && mn_code(s_mn_sel) == 901) mn_change(s_mn_sel, +1); else mn_change(s_mn_sel, -1); }
        if (rt || rete) mn_change(s_mn_sel, +1);
        }
        n = mn_items();                               /* a page change above (Debug menu) changes the row count */
        if (s_mn_sel >= n) s_mn_sel = 0;
        {
            int fpx = (int)h / 28; if (fpx < 14) fpx = 14; if (fpx > 40) fpx = 40;
            int pw = fpx * (s_mn_page == 5 ? 11 : s_mn_page == 1 ? 40 : s_mn_page == 2 ? 34 : s_mn_page == 3 ? 30 : s_mn_page == 4 ? 40 : 22), x0 = fpx, y0 = fpx, x, y;
            static int s_top;
            int rows = n, ph;
            { int vis = ((int)h - 2 * y0) / fpx - 4; if (vis < 4) vis = 4; if (rows > vis) rows = vis; }   /* long lists scroll instead of running off the screen */
            if (s_mn_sel < s_top) s_top = s_mn_sel;
            if (s_mn_sel >= s_top + rows) s_top = s_mn_sel - rows + 1;
            if (s_top > n - rows) s_top = n - rows;
            if (s_top < 0) s_top = 0;
            ph = fpx * (rows + (s_mn_page == 5 ? 2 : 4));
            BITMAPINFO bi; void *bits = NULL; HBITMAP bmp; HDC dc; HFONT fnt, old; RECT rc; uint32_t *dib;
            if (pw > (int)w - 2 * x0) pw = (int)w - 2 * x0;
            if (ph > (int)h - 2 * y0) ph = (int)h - 2 * y0;
            if (pw < 100 || ph < 60) return;
            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = pw; bi.bmiHeader.biHeight = -ph;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            dc = CreateCompatibleDC(NULL);
            bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (!dc || !bmp || !bits) { if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); return; }
            SelectObject(dc, bmp);
            memset(bits, 0, (size_t)pw * ph * 4);
            fnt = CreateFontA(-fpx * 3 / 4, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
            old = (HFONT)SelectObject(dc, fnt);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            { const char *tt = s_mn_page == 5 ? "VOICE" : s_mn_page == 2 ? "NPC SPAWNER  (Right = spawn, Del/Esc closes)" : s_mn_page == 4 ? "GRAPHICS  (Esc = back, Insert closes)" : s_mn_page == 3 ? "HUD  (Esc = back, Insert closes)" : s_mn_page ? "DEBUG  (Esc = back, Insert closes)" : "SETTINGS  (Insert closes)"; TextOutA(dc, fpx / 2, fpx / 3, tt, (int)strlen(tt)); }
            for (r = s_top; r < s_top + rows; r++) {
                char line[96];
                int ty = fpx * (r - s_top + 2);
                mn_text(r, line, sizeof line);
                if (r == s_mn_sel) {
                    HBRUSH hb = CreateSolidBrush(RGB(40, 70, 170));
                    rc.left = 4; rc.right = pw - 4; rc.top = ty - 2; rc.bottom = ty + fpx;
                    FillRect(dc, &rc, hb); DeleteObject(hb);
                    SetTextColor(dc, RGB(255, 255, 120));
                } else SetTextColor(dc, RGB(235, 235, 235));
                TextOutA(dc, fpx / 2, ty, line, (int)strlen(line));
            }
            SetTextColor(dc, RGB(150, 150, 150));
            if (s_mn_page != 5) { char hl[96]; if (mn_code(s_mn_sel) == 3) snprintf(hl, sizeof hl, "Without the HD .bat, changing this will crash you!"); else if (s_mn_page == 2) snprintf(hl, sizeof hl, "Row 1 = AI switch. Shift = redo setup (%d/%d)", s_mn_sel + 1, n); else if (rows < n) snprintf(hl, sizeof hl, "Up/Down select  (%d/%d)", s_mn_sel + 1, n); else snprintf(hl, sizeof hl, "Up/Down select  Left/Right change"); TextOutA(dc, fpx / 2, fpx * (rows + 2) + fpx / 3, hl, (int)strlen(hl)); }
            SelectObject(dc, old); DeleteObject(fnt);
            GdiFlush();
            dib = (uint32_t *)bits;
            for (y = 0; y < ph; y++) {
                uint32_t *dst = px + (size_t)(y0 + y) * w + x0;
                const uint32_t *src = dib + (size_t)y * pw;
                for (x = 0; x < pw; x++) {
                    uint32_t d = src[x] & 0xFFFFFF, f = dst[x];
                    uint32_t fr = (((f >> 16) & 255) * 90) >> 8, fgc = (((f >> 8) & 255) * 90) >> 8, fb = ((f & 255) * 90) >> 8;
                    uint32_t rr = fr + ((d >> 16) & 255), gg = fgc + ((d >> 8) & 255), bb = fb + (d & 255);
                    if (rr > 255) rr = 255; if (gg > 255) gg = 255; if (bb > 255) bb = 255;
                    dst[x] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
                }
            }
            DeleteObject(bmp); DeleteDC(dc);
        }
    }
}

/* Alt+Enter / F11: borderless fullscreen on the window's monitor, and back.
 * (DXGI's own Alt+Enter, exclusive mode at the frame's 640x480, is off --
 * gpu_present.c; both presenters letterbox to any size.) */
static void fb_toggle_fullscreen(HWND h)
{
    static int full;
    static RECT saved;
    static LONG_PTR saved_style;
    if (!full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &saved);
        saved_style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        full = 0;
    }
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* First press only (bit 30: the key was already down), and with Alt held
     * (bit 29) for Enter; neither reaches the key table. */
    /* F11 no longer toggles fullscreen: the game binds it (Brightness +). */
    if (m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30))) {
        fb_toggle_fullscreen(h);
        return 0;
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN)
        return 0;                       /* auto-repeat of Alt+Enter */
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {   /* SG1: no Windows arrow while the game owns the mouse */
        extern int mkb_capturing(void);
        if (mkb_capturing()) { SetCursor(NULL); return TRUE; }
    }
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        /* Closing the window ends the run. Only stopping this thread left
         * every other one running -- audio played on, and the process had
         * to be ended from Task Manager. The guest has no exit path of its
         * own to ask. TerminateProcess rather than ExitProcess: the guest
         * threads are still running, and ExitProcess's cleanup can wait on
         * a lock one of them held. The exit hook does the reporting that
         * would otherwise be skipped. RECOMP_CLOSE_KEEPS_RUNNING=1 only
         * closes the window, as before. */
        InterlockedExchange(&s_fb_running, 0);
        if (!getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
            fprintf(stderr, "  [FBWIN] window closed: exiting\n");
            recomp_exit_hook_run("window");
            fflush(stdout);
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        xbox_InputKeySet((int)w, 1);
        /* A Shift, Ctrl or Alt is also its side's key, for bindings to
         * "lshift" and the like; wParam stays the either-side one. */
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 1);
        }
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us.
         * F10 is the exception: Windows treats it as a menu key and would
         * freeze the game in its menu loop until you click, and the game
         * binds it (Gamma +). */
        if (w == 0x79) return 0;
        if (w == VK_MENU) return 0;   /* SG1: Alt alone (quick weapon switch) must not start Windows' menu mode, which froze the game */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 0);
            /* With both Shifts down Windows sends one key-up, for the
             * last released, so the other side would stay held. Autorepeat
             * sets a Shift still down again. */
            if (w == VK_SHIFT) {
                xbox_InputKeySet(VK_LSHIFT, 0);
                xbox_InputKeySet(VK_RSHIFT, 0);
            }
        }
        if (w == 0x79 || w == VK_MENU) return 0;   /* F10 / Alt: keep Windows' menu mode out */
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Mouse buttons go in the same table, for bindings that name them.
     * Capturing the mouse while one is down delivers its release even
     * outside the window. */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        {
            int down = 0;
            int vk = xbox_InputWin32MouseVk(m, (uint32_t)w, &down);
            if (vk)
                xbox_InputKeySet(vk, down);
            if (down)
                SetCapture(h);
            else if (!(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)))
                ReleaseCapture();
        }
        return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch
                           * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* A 2x-wide anti-aliased 32-bit surface (pitch = 2 * width * 4):
             * take every other pixel. X-Men Legends renders its 640-wide
             * frame into a 1280-wide surface, which read as "8 bytes per
             * pixel" and showed black. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

/* GDI: the frame letterboxed into the client area, like the D3D11
 * presenter; the borders are cleared when the size changes. */
static void gdi_blit(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    static LONG lw, lh;
    RECT c;
    LONG cw, ch, w, h;
    GetClientRect(hwnd, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return;
    if (cw != lw || ch != lh) {
        FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        lw = cw; lh = ch;
    }
    if ((long long)cw * s_fb_height <= (long long)ch * s_fb_width) {
        w = cw; h = (LONG)((long long)cw * s_fb_height / s_fb_width);
    } else {
        h = ch; w = (LONG)((long long)ch * s_fb_width / s_fb_height);
    }
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);     /* HALFTONE wants it after the mode */
    StretchDIBits(hdc, (cw - w) / 2, (ch - h) / 2, w, h,
                  0, 0, (int)s_fb_width, (int)s_fb_height,
                  s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
}

/* The converted frame in s_rgb, on screen: D3D11 when it is up (which waits
 * for vsync, so returns 1 when the loop is already paced), else GDI. */
static int fb_show(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    if (gpu_present_active())
        return gpu_present_frame_sized(s_rgb, s_fb_width, s_fb_height);
    gdi_blit(hwnd, hdc, bi);
    return 0;
}

static int fb_show_hi(HWND hwnd, HDC hdc, const uint32_t *px, uint32_t w, uint32_t h)
{
    if (gpu_present_active())
        return gpu_present_frame_sized(px, w, h);
    {
        BITMAPINFO b2;
        RECT c; LONG cw, ch, dw, dh;
        memset(&b2, 0, sizeof b2);
        b2.bmiHeader.biSize = sizeof(b2.bmiHeader);
        b2.bmiHeader.biWidth = (LONG)w; b2.bmiHeader.biHeight = -(LONG)h;
        b2.bmiHeader.biPlanes = 1; b2.bmiHeader.biBitCount = 32; b2.bmiHeader.biCompression = BI_RGB;
        GetClientRect(hwnd, &c);
        cw = c.right - c.left; ch = c.bottom - c.top;
        if (cw <= 0 || ch <= 0) return 0;
        if ((long long)cw * h <= (long long)ch * w) { dw = cw; dh = (LONG)((long long)cw * h / w); }
        else { dh = ch; dw = (LONG)((long long)ch * w / h); }
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, NULL);
        StretchDIBits(hdc, (cw - dw) / 2, (ch - dh) / 2, dw, dh, 0, 0, (int)w, (int)h, px, &b2, DIB_RGB_COLORS, SRCCOPY);
    }
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    int fb_pinned;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        /* RECOMP_WINDOW_SCALE (1-8, default 2, as in the SDL window): the
         * client area is the frame times this, made smaller until the window
         * fits the desktop work area, and centred on it. */
        const char *e = getenv("RECOMP_WINDOW_SCALE");
        int scale = e ? atoi(e) : 2;
        RECT work;
        if (scale < 1 || scale > 8)
            scale = 2;
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
        }
        for (;; scale--) {
            r.left = 0; r.top = 0;
            r.right = (LONG)s_fb_width * scale; r.bottom = (LONG)s_fb_height * scale;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            if (scale <= 1 || (r.right - r.left <= work.right - work.left
                               && r.bottom - r.top <= work.bottom - work.top))
                break;
        }
        hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                               work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
    }
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && *fs && *fs != '0')
            fb_toggle_fullscreen(hwnd);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
    /* A window pinned to one address shows that memory and nothing else. */
    fb_pinned = getenv("RECOMP_FB_VA") != NULL;

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    if (gpu_present_enabled())
        gpu_present_init(hwnd, s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        int paced = 0;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            if (!fb_pinned)
                fb_overlay();
            {   /* SG1: full-resolution frame when there is one and no video is playing */
                uint32_t hw = s_hi_w[idx], hh = s_hi_h[idx];
                if (hw && hh && !s_ov_active && s_hi[idx]) {
                    size_t need = (size_t)hw * hh;
                    if (s_hi_show_cap < need) { free(s_hi_show); s_hi_show = (uint32_t *)malloc(need * 4); s_hi_show_cap = s_hi_show ? need : 0; }
                    if (s_hi_show) {
                        memcpy(s_hi_show, s_hi[idx], need * 4);
                        sg1_apply_gamma(s_hi_show, hw, hh);
                        sg1_menu_frame(hwnd, s_hi_show, hw, hh);
                        paced = fb_show_hi(hwnd, hdc, s_hi_show, hw, hh);
                        goto shown;
                    }
                }
            }
            sg1_apply_gamma(s_rgb, s_fb_width, s_fb_height);
            sg1_menu_frame(hwnd, s_rgb, s_fb_width, s_fb_height);
            paced = fb_show(hwnd, hdc, &bi);
            shown: ;
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            if (!fb_pinned)
                fb_overlay();
            sg1_apply_gamma(s_rgb, s_fb_width, s_fb_height);
            sg1_menu_frame(hwnd, s_rgb, s_fb_width, s_fb_height);
            paced = fb_show(hwnd, hdc, &bi);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000 || InterlockedExchange(&s_note_dirty, 0)) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | %ls | FPS: %.1f | draws: %ld", s_title, s_note,
                           (t0 && now > t0) ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        if (!paced)
            Sleep(16);                  /* D3D11's Present waited for vsync */
    }

    gpu_present_shutdown();
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    free(s_pvideo_source);
    s_pvideo_source = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#endif /* _WIN32: POSIX hosts get the SDL2 window in fb_present_sdl.c */
