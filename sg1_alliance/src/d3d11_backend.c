/*
 * SG-1 GPU back end: a Direct3D 11 renderer for the pushbuffer executor.
 *
 * The executor (nv2a_pb_exec.c) decodes the title's NV2A commands, transforms
 * vertices on the CPU and hands this file triangle lists with every vertex
 * attribute, four texture stages, the register-combiner words and the render
 * state (nv2a_backend.h, Nv2aBackendEx). One pixel shader runs the NV2A
 * register combiners from a constant buffer -- a line-for-line port of
 * nv2a_combiner.c -- so no shader is generated per draw.
 *
 * Render targets live on the GPU, keyed by their guest address. A texture
 * whose address names a recently drawn render target samples that target
 * directly (bloom and other render-to-texture passes). Textures from guest
 * memory are decoded once and re-uploaded only when their bytes change. At
 * each presented flip the finished target is copied into guest memory so the
 * existing framebuffer window can show it.
 *
 * Switch: RECOMP_GPU=1 (see main.c). RECOMP_GPU_DEBUG=1 prints statistics.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdarg.h>

#include "nv2a_backend.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ------------------------------------------------------------------ HLSL */

static const char g_hlsl[] =
"cbuffer CB : register(b0)\n"
"{\n"
"    float4 g_surf;\n"          /* x=W y=H z=1/depth_max */
"    uint4  g_misc;\n"          /* x=use_rc y=alpha_test z=alpha_func w=alpha_ref */
"    uint4  g_icw[2]; uint4 g_aicw[2]; uint4 g_ocw[2]; uint4 g_aocw[2];\n"
"    uint4  g_f0[2];  uint4 g_f1[2];\n"
"    uint4  g_fin;\n"           /* final0 final1 final_c0 final_c1 */
"    uint4  g_ctl;\n"           /* control stage_program */
"    float4 g_fogc;\n"
"    float4 g_tex[4];\n"        /* xy = uv scale, z = valid, w = alpha_one */
"    float4 g_cube;\n"          /* x..w: stage s samples a cube map */
"};\n"
"Texture2D T0 : register(t0); Texture2D T1 : register(t1);\n"
"Texture2D T2 : register(t2); Texture2D T3 : register(t3);\n"
"TextureCube C0 : register(t4); TextureCube C1 : register(t5);\n"
"TextureCube C2 : register(t6); TextureCube C3 : register(t7);\n"
"SamplerState S0 : register(s0); SamplerState S1 : register(s1);\n"
"SamplerState S2 : register(s2); SamplerState S3 : register(s3);\n"
"\n"
"struct VSIn {\n"
"    float4 pos : TEXCOORD0; float4 d0 : TEXCOORD1; float4 d1 : TEXCOORD2;\n"
"    float4 fog : TEXCOORD3;\n"
"    float4 t0 : TEXCOORD4; float4 t1 : TEXCOORD5;\n"
"    float4 t2 : TEXCOORD6; float4 t3 : TEXCOORD7;\n"
"};\n"
"struct PSIn {\n"
"    float4 sv : SV_Position;\n"
"    float4 d0 : TEXCOORD1; float4 d1 : TEXCOORD2; float4 fog : TEXCOORD3;\n"
"    float4 t0 : TEXCOORD4; float4 t1 : TEXCOORD5;\n"
"    float4 t2 : TEXCOORD6; float4 t3 : TEXCOORD7;\n"
"};\n"
"\n"
"PSIn VSMain(VSIn i)\n"
"{\n"
"    PSIn o;\n"
"    float w = i.pos.w;\n"
"    o.sv = float4((i.pos.x / g_surf.x * 2.0 - 1.0) * w,\n"
"                  (1.0 - i.pos.y / g_surf.y * 2.0) * w,\n"
"                  i.pos.z * g_surf.z * w, w);\n"
"    o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog;\n"
"    o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
"    return o;\n"
"}\n"
"\n"
"float4 samp(uint st, float2 uv)\n"
"{\n"
"    float4 c;\n"
"    if (st == 0) c = T0.Sample(S0, uv);\n"
"    else if (st == 1) c = T1.Sample(S1, uv);\n"
"    else if (st == 2) c = T2.Sample(S2, uv);\n"
"    else c = T3.Sample(S3, uv);\n"
"    if (g_tex[st].w > 0.5) c.a = 1.0;\n"
"    return c;\n"
"}\n"
"float4 unp(uint c)\n"
"{\n"
"    return float4((float)((c >> 16) & 255u), (float)((c >> 8) & 255u),\n"
"                  (float)(c & 255u), (float)(c >> 24)) / 255.0;\n"
"}\n"
"uint rd(uint4 a[2], uint s) { return a[s >> 2][s & 3u]; }\n"
"float mapin(float x, uint b)\n"
"{\n"
"    float p = max(x, 0.0);\n"
"    uint m = (b >> 5) & 7u;\n"
"    if (m == 0u) return p;\n"
"    if (m == 1u) return 1.0 - saturate(x);\n"
"    if (m == 2u) return 2.0 * p - 1.0;\n"
"    if (m == 3u) return -2.0 * p + 1.0;\n"
"    if (m == 4u) return p - 0.5;\n"
"    if (m == 5u) return -p + 0.5;\n"
"    if (m == 6u) return x;\n"
"    return -x;\n"
"}\n"
"float mapout(float x, uint m)\n"
"{\n"
"    if (m == 0x08u) return x - 0.5;\n"
"    if (m == 0x10u) return x * 2.0;\n"
"    if (m == 0x18u) return (x - 0.5) * 2.0;\n"
"    if (m == 0x20u) return x * 4.0;\n"
"    if (m == 0x30u) return x * 0.5;\n"
"    return x;\n"
"}\n"
"static float4 R[16];\n"
"float3 rgb_in(uint b)\n"
"{\n"
"    float4 r = R[b & 15u];\n"
"    float3 s = ((b & 0x10u) != 0u) ? float3(r.a, r.a, r.a) : r.rgb;\n"
"    return float3(mapin(s.x, b), mapin(s.y, b), mapin(s.z, b));\n"
"}\n"
"float alpha_in(uint b)\n"
"{\n"
"    float4 r = R[b & 15u];\n"
"    return mapin(((b & 0x10u) != 0u) ? r.a : r.b, b);\n"
"}\n"
"\n"
"float4 rc_eval(float4 v0, float4 v1, float4 fog, float4 t0, float4 t1, float4 t2, float4 t3)\n"
"{\n"
"    uint n = g_ctl.x & 255u;\n"
"    uint flags = g_ctl.x >> 8;\n"
"    uint stp = g_ctl.y;\n"
"    uint s;\n"
"    if (n > 8u) n = 8u;\n"
"    [unroll] for (s = 0u; s < 16u; s++) R[s] = float4(0, 0, 0, 0);\n"
"    R[3] = fog; R[4] = v0; R[5] = v1;\n"
"    R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;\n"
"    R[12].a = ((stp & 31u) != 0u) ? t0.a : 1.0;\n"
"    for (s = 0u; s < n; s++) {\n"
"        uint icw = rd(g_icw, s), ocw = rd(g_ocw, s);\n"
"        uint aicw = rd(g_aicw, s), aocw = rd(g_aocw, s);\n"
"        uint fl = ocw >> 12, afl = aocw >> 12;\n"
"        R[1] = unp(rd(g_f0, ((flags & 0x010u) != 0u) ? s : 0u));\n"
"        R[2] = unp(rd(g_f1, ((flags & 0x100u) != 0u) ? s : 0u));\n"
"        bool mux_cd = ((flags & 1u) != 0u) ? (R[12].a >= 0.5)\n"
"                    : ((((int)(R[12].a * 255.0)) & 1) != 0);\n"
"        float3 A = rgb_in(icw >> 24), B = rgb_in(icw >> 16);\n"
"        float3 C = rgb_in(icw >> 8),  D = rgb_in(icw);\n"
"        float aA = alpha_in(aicw >> 24), aB = alpha_in(aicw >> 16);\n"
"        float aC = alpha_in(aicw >> 8),  aD = alpha_in(aicw);\n"
"        float dab = dot(A, B), dcd = dot(C, D);\n"
"        float3 pab = ((fl & 2u) != 0u) ? float3(dab, dab, dab) : A * B;\n"
"        float3 pcd = ((fl & 1u) != 0u) ? float3(dcd, dcd, dcd) : C * D;\n"
"        float3 sm = ((fl & 4u) != 0u) ? (mux_cd ? pcd : pab) : pab + pcd;\n"
"        uint om = fl & 0x38u;\n"
"        float3 ab = clamp(float3(mapout(pab.x, om), mapout(pab.y, om), mapout(pab.z, om)), -1.0, 1.0);\n"
"        float3 cd = clamp(float3(mapout(pcd.x, om), mapout(pcd.y, om), mapout(pcd.z, om)), -1.0, 1.0);\n"
"        float3 ms = clamp(float3(mapout(sm.x, om), mapout(sm.y, om), mapout(sm.z, om)), -1.0, 1.0);\n"
"        float apab = aA * aB, apcd = aC * aD;\n"
"        float asm_ = ((afl & 4u) != 0u) ? (mux_cd ? apcd : apab) : apab + apcd;\n"
"        uint aom = afl & 0x38u;\n"
"        float aab = clamp(mapout(apab, aom), -1.0, 1.0);\n"
"        float acd = clamp(mapout(apcd, aom), -1.0, 1.0);\n"
"        float ams = clamp(mapout(asm_, aom), -1.0, 1.0);\n"
"        uint d;\n"
"        d = (ocw >> 4) & 15u;\n"
"        if (d != 0u) { R[d].xyz = ab; if ((fl & 0x80u) != 0u) R[d].w = ab.z; }\n"
"        d = ocw & 15u;\n"
"        if (d != 0u) { R[d].xyz = cd; if ((fl & 0x40u) != 0u) R[d].w = cd.z; }\n"
"        d = (ocw >> 8) & 15u;\n"
"        if (d != 0u) { R[d].xyz = ms; }\n"
"        d = (aocw >> 4) & 15u; if (d != 0u) R[d].w = aab;\n"
"        d = aocw & 15u;        if (d != 0u) R[d].w = acd;\n"
"        d = (aocw >> 8) & 15u; if (d != 0u) R[d].w = ams;\n"
"        R[0] = float4(0, 0, 0, 0);\n"
"    }\n"
"    float4 outc;\n"
"    if (g_fin.x != 0u || g_fin.y != 0u) {\n"
"        uint f0 = g_fin.x, f1 = g_fin.y, ff = f1 & 255u;\n"
"        R[1] = unp(g_fin.z); R[2] = unp(g_fin.w);\n"
"        float3 a3 = ((ff & 0x40u) != 0u) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"        float3 b3 = ((ff & 0x20u) != 0u) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"        float3 sm3 = a3 + b3;\n"
"        if ((ff & 0x80u) != 0u) sm3 = saturate(sm3);\n"
"        R[14] = float4(sm3, 0.0);\n"
"        float3 E = rgb_in(f1 >> 24), F = rgb_in(f1 >> 16);\n"
"        R[15] = float4(E * F, 0.0);\n"
"        float3 A = rgb_in(f0 >> 24), B = rgb_in(f0 >> 16);\n"
"        float3 C = rgb_in(f0 >> 8),  D = rgb_in(f0);\n"
"        outc = float4(D + C * (1.0 - A) + B * A, alpha_in(f1 >> 8));\n"
"    } else {\n"
"        outc = R[12];\n"
"    }\n"
"    return saturate(outc);\n"
"}\n"
"\n"
"float4 fetch_stage(uint st, float4 c)\n"
"{\n"
"    uint mode = (g_ctl.y >> (st * 5u)) & 31u;\n"
"    float q = (c.w != 0.0) ? c.w : 1.0;\n"
"    if (mode == 0u) return float4(0, 0, 0, 1);\n"
"    if (mode == 3u) {\n"
"        if (g_cube[st] < 0.5) return float4(1, 1, 1, 1);   /* no cube map available: neutral */\n"
"        if (st == 0u) return C0.Sample(S0, c.xyz);\n"
"        if (st == 1u) return C1.Sample(S1, c.xyz);\n"
"        if (st == 2u) return C2.Sample(S2, c.xyz);\n"
"        return C3.Sample(S3, c.xyz);\n"
"    }\n"
"    if (mode >= 1u && mode <= 3u) {\n"
"        if (g_tex[st].z < 0.5) return float4(1, 1, 1, 1);\n"
"        return samp(st, (c.xy / q) * g_tex[st].xy);\n"
"    }\n"
"    if (mode == 4u) return saturate(c);\n"
"    return float4(0, 0, 0, 0);\n"
"}\n"
"\n"
"float2 dimof(uint st)\n"
"{\n"
"    uint w, h;\n"
"    if (st == 0u) T0.GetDimensions(w, h);\n"
"    else if (st == 1u) T1.GetDimensions(w, h);\n"
"    else if (st == 2u) T2.GetDimensions(w, h);\n"
"    else T3.GetDimensions(w, h);\n"
"    return float2((float)w, (float)h);\n"
"}\n"
"\n"
"float3 dot_map(uint st, float3 c)\n"
"{\n"
"    uint m = (g_ctl.z >> ((st - 1u) * 4u)) & 15u;\n"
"    return (m == 0u) ? c : (c * 2.0 - 1.0);\n"
"}\n"
"\n"
"float4 PSMain(PSIn i) : SV_Target\n"
"{\n"
"    float4 col;\n"
"    if (g_misc.x != 0u) {\n"
"        float4 tc[4] = { i.t0, i.t1, i.t2, i.t3 };\n"
"        float4 tv[4];\n"
"        float ou = 0.0, nzu = 1.0;\n"
"        [unroll] for (uint st = 0u; st < 4u; st++) {\n"
"            uint mode = (g_ctl.y >> (st * 5u)) & 31u;\n"
"            if (st > 0u && mode == 0x11u) {\n"
"                float3 n = dot_map(st, tv[0].rgb);\n"
"                ou = dot(tc[st].xy, n.xy); nzu = n.z;\n"
"                tv[st] = float4(saturate(dot(tc[st].xyz, n)).xxx, 1.0);\n"
"            } else if (st > 0u && mode == 9u) {\n"
"                float3 n = dot_map(st, tv[0].rgb);\n"
"                float ov = dot(tc[st].xy, n.xy);\n"
"                /* the game's rows are (shift_x, 0, screen_x) / (0, shift_y, screen_y): use the pixel's own\n"
"                   screen position (perspective-correct interpolation of the vertex copy of it is wrong) */\n"
"                float2 uv = (i.sv.xy / dimof(st)) * float2(nzu, n.z) + float2(ou / g_surf.x, ov / g_surf.y);\n"
"                tv[st] = (g_tex[st].z < 0.5) ? float4(1, 1, 1, 1) : samp(st, uv);\n"
"            } else tv[st] = (st > 0u && g_ctl.w == 2u) ? float4(1, 1, 1, 1) : fetch_stage(st, tc[st]);\n"
"        }\n"
"        float4 t0 = tv[0], t1 = tv[1], t2 = tv[2], t3 = tv[3];\n"
"        col = rc_eval(i.d0, i.d1, float4(g_fogc.rgb, i.fog.x), t0, t1, t2, t3);\n"
"        if (g_ctl.w == 3u) col = float4(t0.rgb, 1.0);\n"
"        if (g_ctl.w == 4u) col = float4(saturate(tc[0].xy), 0.0, 1.0);\n"
"    } else {\n"
"        col = saturate(i.d0);\n"
"        if (g_tex[0].z > 0.5) col *= samp(0u, i.t0.xy * g_tex[0].xy);\n"
"    }\n"
"    if (g_fogc.w > 0.5) col.rgb = lerp(g_fogc.rgb, col.rgb, saturate(i.fog.x));\n"   /* SG1: program-batch distance fog */
"    if (g_misc.y != 0u) {\n"
"        int v = (int)(col.a * 255.0 + 0.5), r = (int)(g_misc.w & 255u);\n"
"        bool apass = true;\n"
"        uint f = g_misc.z;\n"
"        if (f == 0x200u) apass = false;\n"
"        else if (f == 0x201u) apass = v < r;\n"
"        else if (f == 0x202u) apass = v == r;\n"
"        else if (f == 0x203u) apass = v <= r;\n"
"        else if (f == 0x204u) apass = v > r;\n"
"        else if (f == 0x205u) apass = v != r;\n"
"        else if (f == 0x206u) apass = v >= r;\n"
"        if (!apass) discard;\n"
"    }\n"
"    if (g_ctl.w == 1u) return float4(1, 0, 1, 1);\n"
"    return col;\n"
"}\n";

/* --------------------------------------------------------------- state */

typedef struct {
    float    surf[4];
    uint32_t misc[4];
    uint32_t icw[8], aicw[8], ocw[8], aocw[8], f0[8], f1[8];
    uint32_t fin[4];
    uint32_t ctl[4];
    float    fogc[4];
    float    tex[4][4];
    float    cube[4];
} Cb;

typedef struct {
    uint32_t va, w, h, bpp;
    uint32_t pw, ph;                    /* real size: logical size times the resolution scale */
    ID3D11Texture2D          *tex, *staging;
    ID3D11Texture2D          *stg[2][2];   /* async readback ring: [slot][which] */
    uint8_t                   stg_idx[2], stg_have[2];
    ID3D11RenderTargetView   *rtv;
    ID3D11ShaderResourceView *srv;
    uint64_t last_draw;                 /* frame of the last draw/clear */
    uint64_t last_use;
} Rt;

typedef struct {
    uint32_t va, w, h;
    ID3D11Texture2D        *tex;
    ID3D11DepthStencilView *dsv;
    uint64_t last_use;
} Depth;

typedef struct {
    uint32_t offset, w, h, pitch, color, texel_uv;
    uint64_t hash, checked;             /* checked: frame of the last hash */
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
    uint64_t last_use;
    int mips;                           /* full mip chain generated on the GPU */
} Tex;

#define MAX_RT    32
#define MAX_DEPTH 16
#define MAX_TEX   1024
#define MAX_STATE 256

static ID3D11Device           *g_dev;
static ID3D11DeviceContext    *g_ctx;
static ID3D11VertexShader     *g_vs;
static ID3D11PixelShader      *g_ps;
static ID3D11InputLayout      *g_layout;
static ID3D11Buffer           *g_cb, *g_vb;
static uint32_t                g_vb_bytes, g_vb_pos;
static LONGLONG                g_t_total, g_t_vb, g_t_tex, g_t_call;
static LONGLONG qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static ID3D11RasterizerState  *g_rs;
/* SG1: rasterizer states with the title's polygon offset (D3D ZBIAS), cached by value. Without it,
 * decals and projected shadows z-fight with the surface under them. SG1_NO_ZBIAS=1 turns it off. */
static ID3D11RasterizerState *rs_for(const Nv2aRenderState *s)
{
    static struct { float sc, bi, dm; ID3D11RasterizerState *st; } c[32];
    static int n, off = -1;
    D3D11_RASTERIZER_DESC rd;
    int i;
    if (off < 0) off = getenv("SG1_NO_ZBIAS") != NULL;
    if (off || !s->poly_offset_fill || (s->poly_offset_scale == 0.0f && s->poly_offset_bias == 0.0f)) return g_rs;
    for (i = 0; i < n; i++) if (c[i].sc == s->poly_offset_scale && c[i].bi == s->poly_offset_bias && c[i].dm == s->depth_max) return c[i].st;
    if (n == 32) return g_rs;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.DepthBias = (INT)(s->poly_offset_bias * (16777216.0f / (s->depth_max > 0.0f ? s->depth_max : 16777215.0f)));   /* title bias is in z units of its own buffer */
    rd.SlopeScaledDepthBias = s->poly_offset_scale;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &c[n].st))) return g_rs;
    c[n].sc = s->poly_offset_scale; c[n].bi = s->poly_offset_bias; c[n].dm = s->depth_max;
    return c[n++].st;
}
static ID3D11ShaderResourceView *g_white;
static ID3D11ShaderResourceView *g_clear;
static int                     g_init_failed;
static unsigned                g_logframe, g_logn, g_dumpn;
static int                     g_lastrthit;
static uint64_t                g_frame;
static Rt    g_rt[MAX_RT];
static Depth g_depth[MAX_DEPTH];
static Tex   g_tex[MAX_TEX];

typedef struct { uint32_t key[6]; ID3D11BlendState *s; } BlendEnt;
typedef struct { uint32_t key[10]; ID3D11DepthStencilState *s; } DsEnt;
typedef struct { uint32_t key[4]; ID3D11SamplerState *s; } SmpEnt;
static BlendEnt g_blend[MAX_STATE]; static int g_nblend;
static DsEnt    g_ds[MAX_STATE];    static int g_nds;
static SmpEnt   g_smp[MAX_STATE];   static int g_nsmp;

static int g_debug;
static uint32_t g_st_draws, g_st_tris, g_st_texup, g_st_rtuse, g_st_clears, g_st_resolve;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[GPU11] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

/* ---------------------------------------------------------------- init */

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(g_hlsl, sizeof g_hlsl - 1, "sg1_gpu.hlsl", NULL, NULL,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &err);
    if (FAILED(hr)) {
        logf_("shader %s failed: %s", entry,
              err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "(no message)");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static int ensure_init(void)
{
    D3D_FEATURE_LEVEL fl;
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    ID3DBlob *vsb, *psb;
    D3D11_INPUT_ELEMENT_DESC il[8];
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    int i;

    if (g_init_failed) return 0;
    if (g_dev) return 1;
    g_debug = getenv("RECOMP_GPU_DEBUG") != NULL;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 2,
                                 D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx))) {
        logf_("D3D11CreateDevice failed");
        g_init_failed = 1;
        return 0;
    }
    vsb = compile("VSMain", "vs_5_0");
    psb = compile("PSMain", "ps_5_0");
    if (!vsb || !psb) { g_init_failed = 1; return 0; }
    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_ps);
    for (i = 0; i < 8; i++) {
        il[i].SemanticName = "TEXCOORD";
        il[i].SemanticIndex = (UINT)i;
        il[i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[i].InputSlot = 0;
        il[i].AlignedByteOffset = (UINT)(i * 16);
        il[i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        il[i].InstanceDataStepRate = 0;
    }
    if (FAILED(ID3D11Device_CreateInputLayout(g_dev, il, 8,
                   ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb),
                   &g_layout))) {
        logf_("CreateInputLayout failed");
        g_init_failed = 1;
        return 0;
    }
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (UINT)sizeof(Cb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb);

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs);

    {   /* a 1x1 white texture for stages with nothing bound */
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd;
        ID3D11Texture2D *t = NULL;
        uint32_t px = 0xFFFFFFFFu;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = &px; sd.SysMemPitch = 4; sd.SysMemSlicePitch = 0;
        ID3D11Device_CreateTexture2D(g_dev, &td, &sd, &t);
        if (t) {
            ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t, NULL, &g_white);
            ID3D11Texture2D_Release(t);
        }
    }
    {   /* a 1x1 fully transparent texture, used in place of the hidden banner */
        D3D11_TEXTURE2D_DESC td; D3D11_SUBRESOURCE_DATA sd; ID3D11Texture2D *t = NULL; uint32_t px = 0;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = &px; sd.SysMemPitch = 4; sd.SysMemSlicePitch = 0;
        ID3D11Device_CreateTexture2D(g_dev, &td, &sd, &t);
        if (t) { ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t, NULL, &g_clear); ID3D11Texture2D_Release(t); }
    }
    logf_("D3D11 back end ready (feature level 0x%X)", (unsigned)fl);
    return 1;
}

/* --------------------------------------------------- render targets/depth */

/* ---- SG1 resolution scale: render targets are allocated at logical size times g_rscale; vertex
 * positions are normalised by the logical size in the vertex shader, so only the viewport and the
 * target change. F12 cycles the presets; RECOMP_RESOLUTION=480|720|1080|1440|2160 (output height)
 * or a plain factor sets the start-up value; the last choice is kept in resolution.txt. */
static const struct { float scale; const char *name; } g_res_presets[] = {
    { 1.0f, "480p (original)" }, { 1.5f, "720p" }, { 2.25f, "1080p" }, { 3.0f, "1440p" }, { 4.5f, "2160p (4K)" } };
#define N_RES_PRESETS 5
static float g_rscale = 1.0f;
static int   g_res_idx = 0, g_res_init;

static void res_set_idx(int idx)
{
    g_res_idx = ((idx % N_RES_PRESETS) + N_RES_PRESETS) % N_RES_PRESETS;
    g_rscale = g_res_presets[g_res_idx].scale;
}

static void res_init(void)
{
    const char *e = getenv("RECOMP_RESOLUTION");
    int i;
    g_res_init = 1;
    res_set_idx(1);   /* default: 720p */
    if (!e || !*e) {
        FILE *f = fopen("resolution.txt", "r");
        static char buf[32];
        if (f) { if (fgets(buf, sizeof buf, f)) e = buf; fclose(f); }
    }
    if (e && *e) {
        double v = atof(e);
        double sc = v >= 100.0 ? v / 480.0 : v;
        int best = 0; double bd = 1e9;
        for (i = 0; i < N_RES_PRESETS; i++) {
            double d = fabs((double)g_res_presets[i].scale - sc);
            if (d < bd) { bd = d; best = i; }
        }
        res_set_idx(best);
    }
    fprintf(stderr, "[RES] resolution %s (scale %.2f); Home/End change it\n", g_res_presets[g_res_idx].name, (double)g_rscale);
}

static uint32_t pw_of(uint32_t w)
{
    uint32_t v;
    if (!g_res_init) res_init();
    v = (uint32_t)((float)w * g_rscale + 0.5f);
    return v ? v : 1;
}

float sg1_res_scale(void) { if (!g_res_init) res_init(); return g_rscale; }

static Rt *rt_find(uint32_t va)
{
    int i;
    for (i = 0; i < MAX_RT; i++)
        if (g_rt[i].tex && g_rt[i].va == va) return &g_rt[i];
    return NULL;
}

static void rt_free(Rt *r)
{
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->staging) ID3D11Texture2D_Release(r->staging);
    { int a, b; for (a = 0; a < 2; a++) for (b = 0; b < 2; b++) if (r->stg[a][b]) ID3D11Texture2D_Release(r->stg[a][b]); }
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    memset(r, 0, sizeof *r);
}

static int sg1_letterbox_on(void)   /* RECOMP_LETTERBOX_OFF=1 restores the old behaviour */
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMP_LETTERBOX_OFF"); v = !(e && e[0] == '1'); }
    return v;
}

static Rt *rt_get(const Nv2aSurface *s)
{
    Rt *r = rt_find(s->color_va);
    int i, victim = 0;
    D3D11_TEXTURE2D_DESC td;

    /* SG1: cutscenes alternate a letterboxed 640x289 surface and a 640x480 one at the SAME address (it is the same
     * buffer). Throwing the target away on every change lost the picture and froze the window on the old menu frame.
     * A shorter full-width surface now draws into the top of the existing target (viewport), and a taller one grows
     * the target and keeps what was drawn. Only full-width screen-sized targets; shadow/effect targets are untouched. */
    if (r && s->width == 640 && r->w == 640 && r->pw == pw_of(s->width) && s->height >= 200 && r->h >= 200 && s->height != r->h
            && sg1_letterbox_on()) {
        if (s->height < r->h) { r->last_use = g_frame; return r; }
        {
            D3D11_TEXTURE2D_DESC gd;
            ID3D11Texture2D *nt = NULL;
            ID3D11RenderTargetView *nrtv = NULL;
            ID3D11ShaderResourceView *nsrv = NULL;
            int a, b;
            memset(&gd, 0, sizeof gd);
            gd.Width = pw_of(s->width); gd.Height = pw_of(s->height);
            gd.MipLevels = gd.ArraySize = 1;
            gd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            gd.SampleDesc.Count = 1;
            gd.Usage = D3D11_USAGE_DEFAULT;
            gd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &gd, NULL, &nt))) {
                D3D11_BOX box;
                ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)nt, NULL, &nrtv);
                ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)nt, NULL, &nsrv);
                box.left = 0; box.top = 0; box.front = 0; box.right = r->pw; box.bottom = r->ph; box.back = 1;
                ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)nt, 0, 0, 0, 0, (ID3D11Resource *)r->tex, 0, &box);
                if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
                if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
                if (r->staging) ID3D11Texture2D_Release(r->staging);
                for (a = 0; a < 2; a++) for (b = 0; b < 2; b++) if (r->stg[a][b]) ID3D11Texture2D_Release(r->stg[a][b]);
                ID3D11Texture2D_Release(r->tex);
                r->tex = nt; r->rtv = nrtv; r->srv = nsrv; r->staging = NULL;
                memset(r->stg, 0, sizeof r->stg); memset(r->stg_idx, 0, sizeof r->stg_idx); memset(r->stg_have, 0, sizeof r->stg_have);
                r->h = s->height; r->ph = pw_of(s->height);
                r->last_use = g_frame;
                { static int gn; if (gn++ < 20) fprintf(stderr, "[RT-GROW] va %08X now %ux%u (content kept)\n", r->va, r->w, r->h); }
                return r;
            }
        }
    }
    if (r && (r->w != s->width || r->h != s->height || r->pw != pw_of(s->width) || r->ph != pw_of(s->height)))
        { rt_free(r); r = NULL; }
    if (r) { r->last_use = g_frame; return r; }
    for (i = 0; i < MAX_RT; i++) {
        if (!g_rt[i].tex) { victim = i; break; }
        if (g_rt[i].last_use < g_rt[victim].last_use) victim = i;
    }
    r = &g_rt[victim];
    if (r->tex) rt_free(r);
    { static int rn; if (rn++ < 300) fprintf(stderr, "[RT-NEW] frame %u va %08X %ux%u\n", (unsigned)g_frame, s->color_va, s->width, s->height); }
    memset(&td, 0, sizeof td);
    td.Width = pw_of(s->width); td.Height = pw_of(s->height);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &r->tex))) {
        logf_("render target %ux%u failed", s->width, s->height);
        memset(r, 0, sizeof *r);
        return NULL;
    }
    ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)r->tex, NULL, &r->rtv);
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)r->tex, NULL, &r->srv);
    r->va = s->color_va; r->w = s->width; r->h = s->height; r->pw = pw_of(s->width); r->ph = pw_of(s->height);
    r->bpp = s->bytes_per_pixel;
    r->last_use = g_frame;
    r->last_draw = 0;
    if (g_debug) logf_("new RT 0x%08X %ux%u bpp %u", r->va, r->w, r->h, r->bpp);
    return r;
}

static Depth *depth_get(uint32_t va, uint32_t w, uint32_t h)
{
    int i, victim = 0;
    D3D11_TEXTURE2D_DESC td;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv;
    Depth *d;

    if (!va) return NULL;
    for (i = 0; i < MAX_DEPTH; i++)
        if (g_depth[i].tex && g_depth[i].va == va && g_depth[i].w == w && g_depth[i].h == h) {
            g_depth[i].last_use = g_frame;
            return &g_depth[i];
        }
    for (i = 0; i < MAX_DEPTH; i++) {
        if (!g_depth[i].tex) { victim = i; break; }
        if (g_depth[i].last_use < g_depth[victim].last_use) victim = i;
    }
    d = &g_depth[victim];
    if (d->tex) {
        if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
        ID3D11Texture2D_Release(d->tex);
        memset(d, 0, sizeof *d);
    }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &d->tex))) {
        logf_("depth %ux%u failed", w, h);
        memset(d, 0, sizeof *d);
        return NULL;
    }
    memset(&dv, 0, sizeof dv);
    dv.Format = td.Format;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)d->tex, &dv, &d->dsv);
    d->va = va; d->w = w; d->h = h; d->last_use = g_frame;
    return d;
}

/* -------------------------------------------------------------- textures */

static uint32_t texel_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D:
        return 2;
    default:
        return 4;
    }
}

static size_t tex_bytes(const Nv2aTexture *t, uint32_t texel_uv)
{
    if (t->color == 0x0C || t->color == 0x0E || t->color == 0x0F) {
        uint32_t bb = t->color == 0x0C ? 8u : 16u;
        return (size_t)((t->width + 3) / 4) * ((t->height + 3) / 4) * bb;
    }
    if (texel_uv && t->pitch)
        return (size_t)t->pitch * t->height;
    return (size_t)t->width * t->height * texel_bytes(t->color);
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* SG1 settings the in-game menu changes (see the menu callbacks below) */
extern int sg1_cfg_int(const char *k, int def);
extern void sg1_cfg_set(const char *k, const char *v);
static int g_aniso = -1;                 /* 1 = off, 2..16 */
static int g_mips = -1;
extern int g_sg1_tg0;                          /* defined in the executor: shadow texgen */
extern int g_sg1_lightfloor;                  /* defined in the executor: ambient floor level 0-4 */
extern int g_sg1_texmat;                      /* defined in the executor */
extern int g_sg1_gamma_on;                    /* defined in fb_present.c: apply the game's gamma ramp */
static int g_dbg_hide;                   /* debug: hide one class of draws (by blend mode), not saved */
static int g_dbg_tex, g_dbg_log;         /* debug: dump decoded textures to texdump\\, verbose draw logging (both default off) */
static struct { uint32_t off, fmt, w, h, lv, flt, draws, tris; double area; float uvspan; uint32_t t1f, t1w, t1h, t2f, t2w, prog, nst, fin0, fin1, ocw0, icw0, uvs1, tv; } g_ta[96];
static int g_ta_n;
extern int g_sg1_hdpack;
extern int g_sg1_hideph;                      /* Menu: hide the PLACE-HOLDER banner texture */
extern uint64_t g_sg1_ph_hash[8]; extern int g_sg1_ph_n;
extern int g_sg1_fov;
extern int g_sg1_hudscale;
extern int g_sg1_xhair;
extern int g_sg1_whitepct;
int g_sg1_cube = 1;   /* Debug menu: sample cube maps (off = neutral white, the old behaviour) */
static struct { float z; int have; uint32_t off, fmt, w, h, lv, t1f, t1w, t1h, prog; float u, v, density, uvmax; int blend; uint32_t n; } g_pk[3];
static int g_haze = 1;                   /* heat-haze / refraction texture shaders (0 = skip those draws) */
static volatile int g_req_flush, g_req_res;
int g_sg1_custom = 1;                    /* Debug/menu: load user textures from texpack\custom */
static void settings_init(void)
{
    const char *e;
    if (g_aniso >= 0) return;
    e = getenv("RECOMP_ANISO");
    g_aniso = e ? atoi(e) : sg1_cfg_int("aniso", 1);
    if (g_aniso < 1) g_aniso = 1;
    if (g_aniso > 16) g_aniso = 16;
    e = getenv("RECOMP_MIPMAPS");
    g_mips = e ? !(e[0] == '0') : sg1_cfg_int("mips", 1) != 0;
    g_sg1_tg0 = sg1_cfg_int("shadows", 1) ? 1 : 0;
    g_sg1_gamma_on = sg1_cfg_int("gamma_ramp", 1) ? 1 : 0;
    g_sg1_lightfloor = sg1_cfg_int("light_floor", 1);
    g_sg1_texmat = sg1_cfg_int("texmat", 1) ? 1 : 0;
    g_sg1_cube = sg1_cfg_int("cubemaps", 1) ? 1 : 0;
    g_sg1_hdpack = sg1_cfg_int("hdpack", 1) ? 1 : 0;
    g_sg1_hideph = sg1_cfg_int("hideph", 0) ? 1 : 0;
    if (g_sg1_ph_n == 0) g_sg1_ph_hash[g_sg1_ph_n++] = 0x59355B879916C320ull;   /* the PLACE-HOLDER banner (256x256 DXT5) */
    { const char *ph = getenv("RECOMP_PH_HASH");   /* hex hashes, comma separated (also baked in below) */
      while (ph && *ph && g_sg1_ph_n < 8) { char *end; unsigned long long v = strtoull(ph, &end, 16); if (end == ph) break; g_sg1_ph_hash[g_sg1_ph_n++] = v; ph = (*end == ',') ? end + 1 : end; } }
    g_sg1_custom = sg1_cfg_int("customtex", 0) ? 1 : 0;
    g_sg1_fov = sg1_cfg_int("fov", 75);
    e = getenv("RECOMP_HUD_SCALE");
    g_sg1_hudscale = e ? atoi(e) : sg1_cfg_int("hudscale", 100);
    if (g_sg1_hudscale < 50 || g_sg1_hudscale > 150) g_sg1_hudscale = 100;
    e = getenv("RECOMP_XHAIR_SCALE");
    g_sg1_xhair = e ? atoi(e) : sg1_cfg_int("xhair", 100);
    if (g_sg1_xhair < 25 || g_sg1_xhair > 200) g_sg1_xhair = 100;
    g_sg1_whitepct = sg1_cfg_int("whitepct", 10);
    if (g_sg1_whitepct < 0 || g_sg1_whitepct > 100) g_sg1_whitepct = 10;
    if (g_sg1_fov < 60 || g_sg1_fov > 120) g_sg1_fov = 75;
    if (g_sg1_lightfloor < 0 || g_sg1_lightfloor > 4) g_sg1_lightfloor = 0;
    e = getenv("RECOMP_DEBUG_TEXDUMP");
    g_dbg_tex = e ? !(e[0] == '0') : sg1_cfg_int("dbg_texdump", 0) != 0;
    e = getenv("RECOMP_DEBUG_LOGS");
    g_dbg_log = e ? !(e[0] == '0') : sg1_cfg_int("dbg_logs", 0) != 0;
    e = getenv("RECOMP_HAZE");
    g_haze = e ? !(e[0] == '0') : sg1_cfg_int("haze", 1) != 0;
}

static int tex_want_mips(void)
{
    settings_init();
    return g_mips;
}



/* ---- SG1: HD texture pack. Dump: texpack\dump\<id>.png ; Replace: texpack\<id>.png (any size, PNG). id = f<fmt>_<w>x<h>_<hash> ---- */
#include <wincodec.h>
#include <objbase.h>
static const GUID sg1_CLSID_WICFactory = {0xcacaf262,0x9370,0x4615,{0xa1,0x3b,0x9f,0x55,0x39,0xda,0x4c,0x0a}};
static const GUID sg1_IID_WICFactory   = {0xec5ec8a9,0xc395,0x4314,{0x9c,0x77,0x54,0xd7,0xa9,0x35,0xff,0x70}};
static const GUID sg1_PF_BGRA          = {0x6fddc324,0x4e03,0x4bfe,{0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,0x0f}};
int g_sg1_whitepct = 10;   /* Debug menu: brightness (percent) given to props whose vertex colours are all white */
int g_sg1_fov = 75;       /* Menu: field of view, sent to the game with the FOV console command */
int g_sg1_hudscale = 100; /* Menu: HUD size in percent (the DrawActor hook in recomp_0019.c scales the HUD widgets) */
int g_sg1_xhair = 100;  /* Menu: crosshair size in percent (small 2D quads near the screen centre are scaled about their own centre in be_draw) */
int sg1_dbg_logs(void) { settings_init(); return g_dbg_log; }   /* RECOMP_DEBUG_LOGS=1 or the Debug menu: gates the HUD logs in main.c / the DrawActor hook */
int sg1_hud_scale_pct(void) { settings_init(); return g_sg1_hudscale; }
int sg1_barfix_on(void) { return 1; }   /* HUD progress bars always follow their fill (the retail renderer never applied the TexScaler; see main.c sg1_bar_apply) */
int g_sg1_hdpack = 1;     /* Menu: load replacement PNGs */
int g_sg1_hideph = 0;     /* Menu: hide the PLACE-HOLDER banner (matched by pixel hash, see [TEX256] in the debug log) */
uint64_t g_sg1_ph_hash[8]; int g_sg1_ph_n;
static unsigned g_hd_loaded;

static uint32_t hd_crc_tab[256]; static int hd_crc_init;
static uint32_t hd_crc(uint32_t c, const unsigned char *p, size_t n)
{
    size_t i;
    if (!hd_crc_init) { uint32_t k, j, v; for (k = 0; k < 256; k++) { v = k; for (j = 0; j < 8; j++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1; hd_crc_tab[k] = v; } hd_crc_init = 1; }
    c = ~c;
    for (i = 0; i < n; i++) c = hd_crc_tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void hd_be32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static void hd_chunk(FILE *f, const char *type, const unsigned char *d, uint32_t n)
{
    unsigned char h[8]; uint32_t c;
    hd_be32(h, n); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    c = hd_crc(0, (const unsigned char *)type, 4);
    if (n) { /* continue crc over data */
        uint32_t x = ~c; size_t i; for (i = 0; i < n; i++) x = hd_crc_tab[(x ^ d[i]) & 255] ^ (x >> 8); c = ~x;
    }
    hd_be32(h, c); fwrite(h, 1, 4, f);
}
/* px = BGRA (as decoded), writes an RGBA PNG using stored (uncompressed) deflate blocks */
static int hd_write_png(const char *fn, const uint32_t *px, uint32_t w, uint32_t h)
{
    size_t rowb = (size_t)w * 4 + 1, rawn = rowb * h, i, pos = 0;
    unsigned char *raw = (unsigned char *)malloc(rawn), *z; size_t zn, zp = 2;
    uint32_t a = 1, b = 0, y, x; unsigned char ih[13]; FILE *f;
    if (!raw) return 0;
    for (y = 0; y < h; y++) {
        unsigned char *r = raw + y * rowb; r[0] = 0;
        for (x = 0; x < w; x++) { uint32_t v = px[(size_t)y * w + x]; r[1 + x * 4] = (unsigned char)(v >> 16); r[2 + x * 4] = (unsigned char)(v >> 8); r[3 + x * 4] = (unsigned char)v; r[4 + x * 4] = (unsigned char)(v >> 24); }
    }
    zn = 2 + rawn + ((rawn + 65534) / 65535) * 5 + 4 + 8;
    z = (unsigned char *)malloc(zn);
    if (!z) { free(raw); return 0; }
    z[0] = 0x78; z[1] = 0x01;
    while (pos < rawn) {
        size_t n = rawn - pos > 65535 ? 65535 : rawn - pos;
        z[zp++] = (pos + n >= rawn) ? 1 : 0; z[zp++] = (unsigned char)n; z[zp++] = (unsigned char)(n >> 8); z[zp++] = (unsigned char)~n; z[zp++] = (unsigned char)(~n >> 8);
        memcpy(z + zp, raw + pos, n); zp += n; pos += n;
    }
    for (i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    hd_be32(z + zp, (b << 16) | a); zp += 4;
    f = fopen(fn, "wb");
    if (!f) { free(raw); free(z); return 0; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    hd_be32(ih, w); hd_be32(ih + 4, h); ih[8] = 8; ih[9] = 6; ih[10] = 0; ih[11] = 0; ih[12] = 0;
    hd_chunk(f, "IHDR", ih, 13); hd_chunk(f, "IDAT", z, (uint32_t)zp); hd_chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    return 1;
}
/* returns malloc'd BGRA pixels or NULL */
static uint32_t *hd_load_png(const char *path, uint32_t *ow, uint32_t *oh)
{
    WCHAR wp[MAX_PATH]; IWICImagingFactory *fac = NULL; IWICBitmapDecoder *dec = NULL; IWICBitmapFrameDecode *fr = NULL; IWICFormatConverter *cv = NULL;
    UINT w = 0, h = 0; uint32_t *buf = NULL; int ok = 0;
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return NULL;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wp, MAX_PATH);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(&sg1_CLSID_WICFactory, NULL, CLSCTX_INPROC_SERVER, &sg1_IID_WICFactory, (void **)&fac))) return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(fac, wp, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))
        && SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr))
        && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(fac, &cv))
        && SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &sg1_PF_BGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))
        && SUCCEEDED(IWICBitmapSource_GetSize((IWICBitmapSource *)cv, &w, &h)) && w >= 1 && h >= 1 && w <= 4096 && h <= 4096) {
        buf = (uint32_t *)malloc((size_t)w * h * 4);
        if (buf && SUCCEEDED(IWICBitmapSource_CopyPixels((IWICBitmapSource *)cv, NULL, w * 4, w * h * 4, (BYTE *)buf))) ok = 1;
    }
    if (cv) IUnknown_Release((IUnknown *)cv);
    if (fr) IUnknown_Release((IUnknown *)fr);
    if (dec) IUnknown_Release((IUnknown *)dec);
    IUnknown_Release((IUnknown *)fac);
    if (!ok) { free(buf); return NULL; }
    *ow = w; *oh = h;
    return buf;
}
typedef struct { uint64_t hash; uint32_t code, w, h; char *file; } HdEnt;
static HdEnt *g_hd_ent; static int g_hd_nent, g_hd_idx_done;
static int hd_cmp(const void *a, const void *b)
{
    const HdEnt *x = (const HdEnt *)a, *y = (const HdEnt *)b;
    return x->hash < y->hash ? -1 : x->hash > y->hash ? 1 : 0;
}
static void hd_index_load(void)
{   /* texpack\index.txt: "f<fmt>_<w>x<h>_<hash> <file relative to texpack>" per line (built by tools\make_hdpack.py --all) */
    FILE *f; char line[320]; int cap = 0;
    g_hd_idx_done = 1;
    f = fopen("texpack\\index.txt", "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned code, w, h; unsigned long long hs; char fn[260];
        if (sscanf(line, "f%X_%ux%u_%llX %259s", &code, &w, &h, &hs, fn) != 5) continue;
        if (g_hd_nent == cap) { cap = cap ? cap * 2 : 4096; g_hd_ent = (HdEnt *)realloc(g_hd_ent, cap * sizeof(HdEnt)); if (!g_hd_ent) { g_hd_nent = 0; break; } }
        g_hd_ent[g_hd_nent].hash = hs; g_hd_ent[g_hd_nent].code = code; g_hd_ent[g_hd_nent].w = w; g_hd_ent[g_hd_nent].h = h;
        g_hd_ent[g_hd_nent].file = _strdup(fn); g_hd_nent++;
    }
    fclose(f);
    if (g_hd_nent) qsort(g_hd_ent, g_hd_nent, sizeof(HdEnt), hd_cmp);
    fprintf(stderr, "[HDPACK] index: %d entries\n", g_hd_nent);
}
static const char *hd_index_find(uint32_t code, uint32_t w, uint32_t h, uint64_t hash)
{
    int lo = 0, hi = g_hd_nent - 1;
    if (!g_hd_idx_done) hd_index_load();
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_hd_ent[mid].hash < hash) lo = mid + 1; else if (g_hd_ent[mid].hash > hash) hi = mid - 1;
        else {
            int i = mid;
            while (i > 0 && g_hd_ent[i - 1].hash == hash) i--;
            for (; i < g_hd_nent && g_hd_ent[i].hash == hash; i++)
                if (g_hd_ent[i].code == code && g_hd_ent[i].w == w && g_hd_ent[i].h == h) return g_hd_ent[i].file;
            return NULL;
        }
    }
    return NULL;
}
static int hd_eligible(const Tex *e, const Nv2aTexture *t)
{
    return !e->texel_uv && (t->color == 0x06 || t->color == 0x0C || t->color == 0x0E || t->color == 0x0F) && t->width >= 16 && t->height >= 16;
}
static void hd_id(char *b, size_t n, const Tex *e, const Nv2aTexture *t)
{
    snprintf(b, n, "f%02X_%ux%u_%016llX", t->color, t->width, t->height, (unsigned long long)e->hash);
}

static int tex_upload(Tex *e, const Nv2aTexture *t)
{
    uint32_t *px = (uint32_t *)malloc((size_t)t->width * t->height * 4);
    int ok;
    uint32_t uw = t->width, uh = t->height;
    settings_init();

    if (!px) return 0;
    ok = nv2a_backend_decode_texture(t, px);
    if (g_dbg_log || g_dbg_tex) {   /* SG1 texture diagnostics: which formats fail to decode, which decode to black */
        static unsigned nlog; static uint32_t seen[512]; static int nseen;
        uint32_t key = (t->color << 24) ^ (t->width << 12) ^ t->height ^ (t->offset >> 4), k; int dup = 0;
        for (k = 0; k < (uint32_t)nseen; k++) if (seen[k] == key) { dup = 1; break; }
        if (!dup && nseen < 512) seen[nseen++] = key;
        if (!dup && nlog < 400) {
            unsigned long long sr = 0, sg = 0, sb = 0, sa = 0; size_t n = (size_t)t->width * t->height, i, st = n > 4096 ? n / 4096 : 1, c = 0;
            if (ok) for (i = 0; i < n; i += st) { uint32_t v = px[i]; sb += v & 255; sg += (v >> 8) & 255; sr += (v >> 16) & 255; sa += v >> 24; c++; }
            if (!c) c = 1;
            if (g_dbg_log) fprintf(stderr, "[TEXFMT] fmt=0x%02X %ux%u pitch=%u uv=%u off=%08X %s avgRGBA=%llu,%llu,%llu,%llu\n",
                    t->color, t->width, t->height, t->pitch, 0u, t->offset, ok ? "ok" : "DECODE-FAILED",
                    sr / c, sg / c, sb / c, sa / c);
            fflush(stderr); nlog++;
            if (g_dbg_tex && ok && nlog <= 150 && t->width <= 1024 && t->height <= 1024) {   /* dump for viewing: tex\\NNN_fmtXX_off.bmp */
                char fn[128]; FILE *f; uint32_t hdr[14]; uint32_t x, y, w = t->width, h = t->height;
                CreateDirectoryA("texdump", NULL);
                snprintf(fn, sizeof fn, "texdump\\%03u_f%02X_%ux%u_%08X.bmp", nlog, t->color, w, h, t->offset);
                f = fopen(fn, "wb");
                if (f) {
                    uint32_t fsz = 54 + w * h * 4; unsigned char bh[54]; memset(bh, 0, 54); (void)hdr;
                    bh[0] = 'B'; bh[1] = 'M'; memcpy(bh + 2, &fsz, 4); bh[10] = 54; bh[14] = 40;
                    memcpy(bh + 18, &w, 4); { int32_t nh = -(int32_t)h; memcpy(bh + 22, &nh, 4); } bh[26] = 1; bh[28] = 32;
                    fwrite(bh, 1, 54, f);
                    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
                        uint32_t v = px[(size_t)y * w + x], a = v >> 24; unsigned char o[4];
                        /* alpha over a checker so transparent areas are visible */
                        unsigned bg = (((x >> 3) + (y >> 3)) & 1) ? 90 : 150;
                        o[0] = (unsigned char)(((v & 255) * a + bg * (255 - a)) / 255);
                        o[1] = (unsigned char)((((v >> 8) & 255) * a + bg * (255 - a)) / 255);
                        o[2] = (unsigned char)((((v >> 16) & 255) * a + bg * (255 - a)) / 255); o[3] = 255;
                        fwrite(o, 1, 4, f);
                    }
                    fclose(f);
                }
            }
        }
    }
    if (g_dbg_log && t->color == 0x0F && t->width == 256 && t->height == 256) {   /* find the PLACE-HOLDER banner: its address is t_*_<off>_fmt0F.bmp from RECOMP_TEX_DUMP */
        static unsigned n256;
        if (n256 < 300) { fprintf(stderr, "[TEX256] off=%08X hash=%016llX\n", t->offset, (unsigned long long)e->hash); fflush(stderr); n256++; }
    }
    if (ok && t->color == 0x0C) {   /* SG1: DXT1 pure black = transparent (window holes in room-interior meshes) */
        static int bk = -1;
        if (bk < 0) { const char *ev = getenv("RECOMP_DXT1_BLACKKEY"); bk = (ev && ev[0] == '1');   /* off by default: did not fix the window */ }
        if (bk) {
            size_t i2, n2 = (size_t)t->width * t->height;
            for (i2 = 0; i2 < n2; i2++) if ((px[i2] & 0x00FFFFFFu) == 0) px[i2] = 0;
        }
    }
    if (!ok) {
        free(px);
        return 0;
    }
    if (ok && (g_sg1_hdpack || g_sg1_custom) && hd_eligible(e, t)) {   /* SG1: HD texture pack (originals from the .utx packages) + user textures */
        char fn[260]; uint32_t rw2 = 0, rh2 = 0, *rp;
        const char *ix = hd_index_find(t->color, t->width, t->height, e->hash);
        if (ix) {
            const char *base = strrchr(ix, '/');
            base = base ? base + 1 : ix;
            if (g_sg1_custom) {      /* user's own version: texpack\custom\<same file name as in texpack\hd> */
                snprintf(fn, sizeof fn, "texpack\\custom\\%s", base);
                rp = hd_load_png(fn, &rw2, &rh2);
                if (rp) { free(px); px = rp; uw = rw2; uh = rh2; g_hd_loaded++; ix = NULL; }
            }
            if (ix && g_sg1_hdpack) {
                snprintf(fn, sizeof fn, "texpack\\%s", ix);
                rp = hd_load_png(fn, &rw2, &rh2);
                if (rp) { free(px); px = rp; uw = rw2; uh = rh2; g_hd_loaded++; }
            }
        }
    }
    if (e->tex) {
        D3D11_TEXTURE2D_DESC od;
        ID3D11Texture2D_GetDesc(e->tex, &od);
        if (od.Width != uw || od.Height != uh) {
            if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
            ID3D11Texture2D_Release(e->tex); e->tex = NULL; e->mips = 0;
        }
    }
    if (!e->tex) {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd;
        memset(&td, 0, sizeof td);
        td.Width = uw; td.Height = uh;
        td.ArraySize = 1;
        td.MipLevels = (tex_want_mips() && uw >= 8 && uh >= 8) ? 0 : 1;   /* 0 = full chain */
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = px; sd.SysMemPitch = uw * 4; sd.SysMemSlicePitch = 0;
        if (td.MipLevels == 0) {
            td.BindFlags |= D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &e->tex))) {
                td.MipLevels = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.MiscFlags = 0;
                if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, &sd, &e->tex))) { free(px); return 0; }
            } else {
                ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)e->tex, 0, NULL, px, uw * 4, 0);
                e->mips = 1;
            }
        } else if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, &sd, &e->tex))) {
            free(px);
            return 0;
        }
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)e->tex, NULL, &e->srv);
        if (e->mips && e->srv) ID3D11DeviceContext_GenerateMips(g_ctx, e->srv);
    } else {
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)e->tex, 0, NULL,
                                              px, uw * 4, 0);
        if (e->mips && e->srv) ID3D11DeviceContext_GenerateMips(g_ctx, e->srv);
    }
    free(px);
    g_st_texup++;
    return 1;
}

/* ---- SG1: cube maps (lighting / environment cubes on the characters' upper stages) ---- */
typedef struct { uint32_t offset, w, h, color, stride; uint64_t hash, checked, last_use; ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; } CubeTex;
static CubeTex g_cubes[24];

static ID3D11ShaderResourceView *cube_srv(const Nv2aTexture *t)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    CubeTex *e = NULL;
    int i, victim = 0, f;
    uint64_t hsh;
    if (!g_sg1_cube || !t->cube || !t->face_stride || t->width == 0 || t->width > 512 || t->height == 0 || t->height > 512) return NULL;
    hsh = hash_bytes(mem + t->offset, (size_t)t->face_stride * 6u);
    for (i = 0; i < 24; i++)
        if (g_cubes[i].srv && g_cubes[i].offset == t->offset && g_cubes[i].w == t->width && g_cubes[i].h == t->height
                && g_cubes[i].color == t->color && g_cubes[i].stride == t->face_stride) { e = &g_cubes[i]; break; }
    if (e && e->hash == hsh) { e->last_use = g_frame; return e->srv; }
    if (!e) {
        for (i = 0; i < 24; i++) {
            if (!g_cubes[i].srv) { victim = i; break; }
            if (g_cubes[i].last_use < g_cubes[victim].last_use) victim = i;
        }
        e = &g_cubes[victim];
    }
    {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd[6];
        uint32_t *px[6];
        ID3D11Texture2D *tx = NULL;
        ID3D11ShaderResourceView *sv = NULL;
        int ok = 1;
        for (f = 0; f < 6; f++) px[f] = NULL;
        for (f = 0; f < 6 && ok; f++) {
            Nv2aTexture ft = *t;
            ft.offset = t->offset + (uint32_t)f * t->face_stride;
            ft.cube = 0; ft.face_stride = 0; ft.levels = 1;
            px[f] = (uint32_t *)malloc((size_t)t->width * t->height * 4);
            if (!px[f] || !nv2a_backend_decode_texture(&ft, px[f])) ok = 0;
            sd[f].pSysMem = px[f]; sd[f].SysMemPitch = t->width * 4; sd[f].SysMemSlicePitch = 0;
        }
        if (ok) {
            memset(&td, 0, sizeof td);
            td.Width = t->width; td.Height = t->height; td.MipLevels = 1; td.ArraySize = 6;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, sd, &tx)))
                ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)tx, NULL, &sv);
        }
        for (f = 0; f < 6; f++) free(px[f]);
        if (!sv) { if (tx) ID3D11Texture2D_Release(tx); return NULL; }
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        e->tex = tx; e->srv = sv;
        e->offset = t->offset; e->w = t->width; e->h = t->height; e->color = t->color; e->stride = t->face_stride; e->hash = hsh;
        e->last_use = g_frame;
        if (g_dbg_log) fprintf(stderr, "[CUBE] created %ux%u fmt %02X face_stride %u off %08X\n", t->width, t->height, t->color, t->face_stride, t->offset);
    }
    return e->srv;
}

/* The SRV for a texture stage. `cur` is the render target being drawn into
 * (a target cannot be sampled while bound). Returns the white texture for
 * anything unusable. *alpha1 is set when the format has no alpha. */
static ID3D11ShaderResourceView *tex_srv(const Nv2aTexture *t, uint32_t texel_uv,
                                         const Rt *cur, int *alpha1, int *rt_hit)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    Rt *r;
    Tex *e = NULL;
    int i, victim = 0;
    size_t nb;
    uint64_t h;

    *alpha1 = (t->color == 0x1E || t->color == 0x07 || t->color == 0x1C) ? 1 : 0;
    *rt_hit = 0;
    if (t->width == 0 || t->height == 0 || t->width > 4096 || t->height > 4096)
        return g_white;
    r = rt_find(t->offset);
    if (r && r != cur && r->w == t->width && r->h == t->height
            && g_frame - r->last_draw <= 6) {   /* SG1: was 2; when the game updates the shadow map less often than every frame the receiver fell back to a plain texture and flickered */
        r->last_use = g_frame;
        *rt_hit = 1;
        g_st_rtuse++;
        return r->srv;
    }
    if (r && r == cur)
        return g_white;

    for (i = 0; i < MAX_TEX; i++) {
        if (g_tex[i].tex || g_tex[i].offset) {
            if (g_tex[i].offset == t->offset && g_tex[i].w == t->width
                    && g_tex[i].h == t->height && g_tex[i].pitch == t->pitch
                    && g_tex[i].color == t->color && g_tex[i].texel_uv == texel_uv) {
                e = &g_tex[i];
                break;
            }
        }
    }
    nb = tex_bytes(t, texel_uv);
    if (!e) {
        for (i = 0; i < MAX_TEX; i++) {
            if (!g_tex[i].offset) { victim = i; break; }
            if (g_tex[i].last_use < g_tex[victim].last_use) victim = i;
        }
        e = &g_tex[victim];
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        memset(e, 0, sizeof *e);
        e->offset = t->offset; e->w = t->width; e->h = t->height;
        e->pitch = t->pitch; e->color = t->color; e->texel_uv = texel_uv;
        h = hash_bytes(mem + t->offset, nb);
        e->hash = h;
        e->checked = g_frame;
        if (!tex_upload(e, t)) {
            e->offset = 0;
            return g_white;
        }
    } else if (e->checked != g_frame) {
        h = hash_bytes(mem + t->offset, nb);
        e->checked = g_frame;
        if (h != e->hash) {
            e->hash = h;
            if (!tex_upload(e, t))
                return g_white;
        }
    }
    e->last_use = g_frame;
    if (g_sg1_hideph && g_sg1_ph_n && g_clear && e->color == 0x0F && e->w == 256 && e->h == 256) {
        int k;
        for (k = 0; k < g_sg1_ph_n; k++) if (g_sg1_ph_hash[k] == e->hash) return g_clear;
    }
    return e->srv ? e->srv : g_white;
}

/* ------------------------------------------------------------ state maps */

static D3D11_BLEND blend_map(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return alpha ? D3D11_BLEND_BLEND_FACTOR : D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op_map(uint32_t e)
{
    switch (e) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_COMPARISON_FUNC cmp_map(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    default:    return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP stencil_op_map(uint32_t o)
{
    switch (o) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11BlendState *get_blend(const Nv2aRenderState *rs)
{
    uint32_t key[6];
    int i;
    D3D11_BLEND_DESC bd;
    UINT8 mask = 0;

    key[0] = rs->blend_enable ? 1u : 0u;
    key[1] = rs->blend_src; key[2] = rs->blend_dst; key[3] = rs->blend_eq;
    key[4] = rs->color_mask; key[5] = 0;
    for (i = 0; i < g_nblend; i++)
        if (!memcmp(g_blend[i].key, key, sizeof key)) return g_blend[i].s;
    if (g_nblend >= MAX_STATE) return NULL;
    if (rs->color_mask & 0x00010000u) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (rs->color_mask & 0x00000100u) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (rs->color_mask & 0x00000001u) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (rs->color_mask & 0x01000000u) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = rs->blend_enable ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = blend_map(rs->blend_src, 0);
    bd.RenderTarget[0].DestBlend = blend_map(rs->blend_dst, 0);
    bd.RenderTarget[0].BlendOp = blend_op_map(rs->blend_eq);
    bd.RenderTarget[0].SrcBlendAlpha = blend_map(rs->blend_src, 1);
    bd.RenderTarget[0].DestBlendAlpha = blend_map(rs->blend_dst, 1);
    bd.RenderTarget[0].BlendOpAlpha = blend_op_map(rs->blend_eq);
    bd.RenderTarget[0].RenderTargetWriteMask = mask;
    memcpy(g_blend[g_nblend].key, key, sizeof key);
    if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bd, &g_blend[g_nblend].s)))
        return NULL;
    return g_blend[g_nblend++].s;
}

static ID3D11DepthStencilState *get_ds(const Nv2aRenderState *rs, int has_depth)
{
    uint32_t key[10];
    int i;
    D3D11_DEPTH_STENCIL_DESC dd;

    memset(key, 0, sizeof key);
    key[0] = has_depth && rs->depth_test_enable ? 1u : 0u;
    key[1] = rs->depth_func;
    key[2] = has_depth && rs->depth_write ? 1u : 0u;
    key[3] = has_depth && rs->stencil_enable ? 1u : 0u;
    key[4] = rs->stencil_func;
    key[5] = rs->stencil_func_mask | (rs->stencil_write_mask << 8);
    key[6] = rs->stencil_fail; key[7] = rs->stencil_zfail; key[8] = rs->stencil_zpass;
    for (i = 0; i < g_nds; i++)
        if (!memcmp(g_ds[i].key, key, sizeof key)) return g_ds[i].s;
    if (g_nds >= MAX_STATE) return NULL;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = key[0] ? TRUE : FALSE;
    dd.DepthWriteMask = key[2] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = cmp_map(rs->depth_func);
    dd.StencilEnable = key[3] ? TRUE : FALSE;
    dd.StencilReadMask = (UINT8)(rs->stencil_func_mask & 0xFF);
    dd.StencilWriteMask = (UINT8)(rs->stencil_write_mask & 0xFF);
    dd.FrontFace.StencilFailOp = dd.BackFace.StencilFailOp = stencil_op_map(rs->stencil_fail);
    dd.FrontFace.StencilDepthFailOp = dd.BackFace.StencilDepthFailOp = stencil_op_map(rs->stencil_zfail);
    dd.FrontFace.StencilPassOp = dd.BackFace.StencilPassOp = stencil_op_map(rs->stencil_zpass);
    dd.FrontFace.StencilFunc = dd.BackFace.StencilFunc = cmp_map(rs->stencil_func);
    memcpy(g_ds[g_nds].key, key, sizeof key);
    if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &dd, &g_ds[g_nds].s)))
        return NULL;
    return g_ds[g_nds++].s;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_map(uint32_t a)
{
    switch (a) {
    case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

static int g_smp_nomip;   /* SG1: set for 2x-modulate (lightmap / shadow) draws: those textures must not use mip levels */
static ID3D11SamplerState *get_sampler(const Nv2aTexture *t, uint32_t border_on, uint32_t border)
{
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    uint32_t lin = (mag == 2 || min == 2 || min == 4 || min == 6) ? 1u : 0u;
    static int force = -1;
    int aniso;
    uint32_t key[4];
    if (force < 0) { const char *e = getenv("RECOMP_FORCE_LINEAR"); force = !(e && e[0] == '0'); }
    settings_init();
    aniso = g_aniso;
    if (force) lin = 1u;
    int i;
    D3D11_SAMPLER_DESC sd;

    key[0] = lin; key[1] = t->addr_u | (t->addr_v << 8);
    key[2] = border_on ? border : 0u;
    key[3] = (uint32_t)aniso | (g_smp_nomip ? 0x10000u : 0u);
    for (i = 0; i < g_nsmp; i++)
        if (!memcmp(g_smp[i].key, key, sizeof key)) return g_smp[i].s;
    if (g_nsmp >= MAX_STATE) return NULL;
    memset(&sd, 0, sizeof sd);
    sd.Filter = lin ? (aniso > 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR) : D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = addr_map(t->addr_u);
    sd.AddressV = addr_map(t->addr_v);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = lin && aniso > 1 ? (UINT)aniso : 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = g_smp_nomip ? 0.0f : D3D11_FLOAT32_MAX;
    if (lin && aniso <= 1 && !g_smp_nomip) {   /* SG1: trilinear without anisotropic filtering picks a blurry mip on walls and floors seen at an angle; bias towards the sharper level (RECOMP_LODBIAS overrides, 0 = off) */
        const char *lb = getenv("RECOMP_LODBIAS");
        sd.MipLODBias = lb ? (float)atof(lb) : -0.6f;
    }
    if (border_on) {
        sd.BorderColor[0] = (float)((border >> 16) & 255) / 255.0f;
        sd.BorderColor[1] = (float)((border >> 8) & 255) / 255.0f;
        sd.BorderColor[2] = (float)(border & 255) / 255.0f;
        sd.BorderColor[3] = (float)(border >> 24) / 255.0f;
    }
    memcpy(g_smp[g_nsmp].key, key, sizeof key);
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_smp[g_nsmp].s)))
        return NULL;
    return g_smp[g_nsmp++].s;
}

/* ---------------------------------------------------------------- calls */

static void argb_to_float(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 255) / 255.0f;
    o[1] = (float)((c >> 8) & 255) / 255.0f;
    o[2] = (float)(c & 255) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    Rt *r;
    if (!ensure_init() || !s->width || !s->height) return;
    r = rt_get(s);
    if (!r) return;
    if (g_dbg_log && (s->width < 640 || s->height < 480)) {   /* [RTCLEAR]: small offscreen targets (shadow / effect textures) */
        static unsigned nrc;
        if (nrc < 300) { nrc++; fprintf(stderr, "[RTCLEAR] f%llu va=%08X %ux%u flags=%02X argb=%08X zs=%08X\n", (unsigned long long)g_frame, s->color_va, s->width, s->height, flags, argb, zstencil); }
    }
    if (flags & 0xF0u) {
        float c[4];
        argb_to_float(argb, c);
        {   /* SG1: a letterboxed cutscene clears only its clip rectangle (e.g. 640x289); the bars below keep their black.
             * Clearing the whole target painted the bars with the scene's fog colour (a green block). */
            int part = 0;
            if (sg1_letterbox_on() && s->width == 640 && r->w == 640 && s->height < r->h && s->height >= 200) {
                ID3D11DeviceContext1 *c1 = NULL;
                if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(g_ctx, &IID_ID3D11DeviceContext1, (void **)&c1)) && c1) {
                    D3D11_RECT rc;
                    rc.left = (LONG)pw_of(s->clip_x); rc.top = (LONG)pw_of(s->clip_y);
                    rc.right = rc.left + (LONG)pw_of(s->width); rc.bottom = rc.top + (LONG)pw_of(s->height);
                    ID3D11DeviceContext1_ClearView(c1, (ID3D11View *)r->rtv, c, &rc, 1);
                    ID3D11DeviceContext1_Release(c1);
                    part = 1;
                    { static int cn; if (cn++ < 10) fprintf(stderr, "[CLEAR-PART] clip %ux%u at (%u,%u) of RT %ux%u\n", s->width, s->height, s->clip_x, s->clip_y, r->w, r->h); }
                }
            }
            if (!part) ID3D11DeviceContext_ClearRenderTargetView(g_ctx, r->rtv, c);
        }
        r->last_draw = g_frame;
    }
    if ((flags & 0x3u) && rs->zeta_va) {
        Depth *d = depth_get(rs->zeta_va, r->pw, r->ph);
        if (d) {
            UINT cf = 0;
            if (flags & 1u) cf |= D3D11_CLEAR_DEPTH;
            if (flags & 2u) cf |= D3D11_CLEAR_STENCIL;
            ID3D11DeviceContext_ClearDepthStencilView(g_ctx, d->dsv, cf,
                (float)(zstencil >> 8) / 16777215.0f, (UINT8)(zstencil & 255u));
        }
    }
    g_st_clears++;
}

static void dump_rt_bmp(Rt *r, const char *name)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t y;
    FILE *bf;
    if (!r || r->w > 1280 || r->pw != r->w) return;
    if (!r->staging) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = r->w; td.Height = r->h; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &r->staging))) return;
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)r->staging, (ID3D11Resource *)r->tex);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)r->staging, 0, D3D11_MAP_READ, 0, &m)))
        return;
    bf = fopen(name, "wb");
    if (bf) {
        uint32_t sz = 54 + r->w * r->h * 4, w = r->w;
        int32_t h = -(int32_t)r->h;
        uint8_t hd[54];
        memset(hd, 0, sizeof hd);
        hd[0] = 'B'; hd[1] = 'M';
        memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
        memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4);
        hd[26] = 1; hd[28] = 32;
        fwrite(hd, 1, 54, bf);
        for (y = 0; y < r->h; y++)
            fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 1, (size_t)r->w * 4, bf);
        fclose(bf);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)r->staging, 0);
}

/* Heat haze: the game copies the finished scene to a 640x480 texture and then refracts it. That copy
 * lives in guest memory we may not have (or have a frame late), so use the render target itself:
 * snapshot what has been drawn so far into a private texture and let the shader sample that. */
static ID3D11Texture2D          *g_hz_tex;
static ID3D11ShaderResourceView *g_hz_srv;
static uint32_t                  g_hz_w, g_hz_h;
static ID3D11ShaderResourceView *haze_scene(const Rt *r)
{
    if (!r || !r->tex) return NULL;
    if (g_hz_tex && (g_hz_w != r->pw || g_hz_h != r->ph)) {
        if (g_hz_srv) ID3D11ShaderResourceView_Release(g_hz_srv);
        ID3D11Texture2D_Release(g_hz_tex);
        g_hz_tex = NULL; g_hz_srv = NULL;
    }
    if (!g_hz_tex) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = r->pw; td.Height = r->ph;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_hz_tex))) { g_hz_tex = NULL; return NULL; }
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_hz_tex, NULL, &g_hz_srv);
        g_hz_w = r->pw; g_hz_h = r->ph;
    }
    if (!g_hz_srv) return NULL;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_hz_tex, (ID3D11Resource *)r->tex);
    return g_hz_srv;
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatchEx *b)
{
    Rt *r;
    Depth *d;
    Cb cb;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11ShaderResourceView *srv[4];
    ID3D11ShaderResourceView *csrv[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *smp[4];
    ID3D11RenderTargetView *rtv;
    D3D11_VIEWPORT vp;
    float bf[4];
    UINT stride = sizeof(Nv2aXfVertex), off = 0;
    uint32_t bytes, st;
    LONGLONG t_a, t_b, t_c, t_d;
    float depth_max;
    const Nv2aRenderState *rs = b->state;
    Nv2aTexture t0;
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *ds;

    if (!ensure_init() || !b->count || !s->width || !s->height) return;
    if (g_dbg_log && s->width <= 256 && s->height <= 256 && s->width != 160) {   /* [SMALLRT]: draws into small render targets (shadow maps) */
        static unsigned nsm;
        if (nsm < 80) { nsm++; fprintf(stderr, "[SMALLRT] f%llu va=%08X %ux%u pitch=%u bpp=%u n=%u zeta=%08X\n", (unsigned long long)g_frame, s->color_va, s->width, s->height, s->pitch, s->bytes_per_pixel, b->count, b->state->zeta_va); }
    }
    r = rt_get(s);
    if (!r) return;
    d = depth_get(rs->zeta_va, r->pw, r->ph);

    bytes = b->count * (uint32_t)sizeof(Nv2aXfVertex);
    t_a = qpc();
    if (bytes > g_vb_bytes) {
        D3D11_BUFFER_DESC bd;
        if (g_vb) ID3D11Buffer_Release(g_vb);
        g_vb = NULL;
        g_vb_pos = 0;
        memset(&bd, 0, sizeof bd);
        g_vb_bytes = bytes < (32u << 20) ? (32u << 20) : bytes;
        bd.ByteWidth = g_vb_bytes;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_vb))) {
            g_vb_bytes = 0;
            return;
        }
    }
    {
        D3D11_MAP mt = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (g_vb_pos + bytes > g_vb_bytes) { mt = D3D11_MAP_WRITE_DISCARD; g_vb_pos = 0; }
        if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_vb, 0, mt, 0, &m)))
            return;
        memcpy((uint8_t *)m.pData + g_vb_pos, b->vertices, bytes);
        if (sg1_letterbox_on() && r->h > s->height && s->width == 640 && s->height >= 200 && (s->clip_x || s->clip_y)) {
            /* SG1: vertex positions are absolute in the surface; the letterbox clip starts at (clip_x, clip_y) and the
             * viewport below starts there too, so make the positions relative to it (the picture sits at row 95, not 0). */
            Nv2aXfVertex *dv = (Nv2aXfVertex *)((uint8_t *)m.pData + g_vb_pos);
            uint32_t vi;
            for (vi = 0; vi < b->count; vi++) {
                dv[vi].pos[0] = b->vertices[vi].pos[0] - (float)s->clip_x;
                dv[vi].pos[1] = b->vertices[vi].pos[1] - (float)s->clip_y;
            }
            { static int lbn; if (lbn++ < 3) fprintf(stderr, "[LB-SHIFT] draws in clip %ux%u at (%u,%u) are shifted to the clip origin\n", s->width, s->height, s->clip_x, s->clip_y); }
        }
        {   /* SG1: a lit prop whose vertex colours were never computed is pure white (the engine only fills them
             * when a light near it changes, e.g. you shoot next to it): draw it with a moderate level instead of full bright */
            static int wf = -1;
            if (wf < 0) { const char *e = getenv("RECOMP_WHITEFIX"); wf = !(e && e[0] == '0'); }
            if (wf && b->use_rc && ((b->rc.stage_program & 0xFFFFFu) == 0x18C21u || (b->rc.stage_program & 0xFFFFFu) == 0x00001u) && !rs->blend_enable && b->count >= 30 && b->count <= 3000 && b->vertices[0].pos[3] > 1.5f) {   /* 0x1 = single-texture prop with vertex lighting (the white pillar on DM-WorstCaseScenario); w > 1.5 = a perspective 3D draw, not a menu / HUD quad */
                const Nv2aXfVertex *sv = b->vertices;
                uint32_t vi, allw = 1;
                for (vi = 0; vi < b->count; vi++)
                    if (sv[vi].d0[0] < 0.995f || sv[vi].d0[1] < 0.995f || sv[vi].d0[2] < 0.995f) { allw = 0; break; }
                if (allw) {
                    Nv2aXfVertex *dv = (Nv2aXfVertex *)((uint8_t *)m.pData + g_vb_pos);
                    static unsigned nwf;
                    float lvl = (float)g_sg1_whitepct / 100.0f;
                    for (vi = 0; vi < b->count; vi++) { dv[vi].d0[0] *= lvl; dv[vi].d0[1] *= lvl; dv[vi].d0[2] *= lvl; }
                    if (nwf < 6) { nwf++; fprintf(stderr, "[WHITEFIX] f%llu n=%u tex0=%08X %ux%u: all-white vertex colours scaled down\n", (unsigned long long)g_frame, b->count, b->tex[0].offset, b->tex[0].width, b->tex[0].height); }
                }
            }
        }
        {   /* SG1: crosshair size. The crosshair is four small alpha-blended 2D quads (HUD_reticule_aim_*) around the screen centre;
             * scale each about its own centre by the menu percentage. Off at 100%. [XHAIR] logs what matched (debug logs on). */
            settings_init();
            if (g_sg1_xhair != 100 && (b->count == 4 || b->count == 6) && rs->blend_enable && b->tex[0].width <= 128 && b->tex[0].height <= 128 && s->width >= 480) {
                const Nv2aXfVertex *sv = b->vertices;
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, cx, cy, w, h, sw = (float)s->width, sh = (float)s->height;
                uint32_t vi;
                for (vi = 0; vi < b->count; vi++) {
                    if (sv[vi].pos[0] < x0) x0 = sv[vi].pos[0]; if (sv[vi].pos[0] > x1) x1 = sv[vi].pos[0];
                    if (sv[vi].pos[1] < y0) y0 = sv[vi].pos[1]; if (sv[vi].pos[1] > y1) y1 = sv[vi].pos[1];
                }
                cx = (x0 + x1) * 0.5f; cy = (y0 + y1) * 0.5f; w = x1 - x0; h = y1 - y0;
                if (w > 2.0f && h > 2.0f && w < 0.2f * sw && h < 0.2f * sh && fabsf(cx - sw * 0.5f) < 0.2f * sw && fabsf(cy - sh * 0.5f) < 0.2f * sh) {
                    Nv2aXfVertex *dv = (Nv2aXfVertex *)((uint8_t *)m.pData + g_vb_pos);
                    float f = (float)g_sg1_xhair / 100.0f;
                    static unsigned nxh;
                    for (vi = 0; vi < b->count; vi++) { dv[vi].pos[0] = sw * 0.5f + (sv[vi].pos[0] - sw * 0.5f) * f; dv[vi].pos[1] = sh * 0.5f + (sv[vi].pos[1] - sh * 0.5f) * f; }   /* about the screen centre: arrows shrink AND move closer */
                    if (g_dbg_log && nxh < 40) { nxh++; fprintf(stderr, "[XHAIR] n=%u quad %.0fx%.0f at (%.0f,%.0f) surf %ux%u tex %08X %ux%u fmt%02X x%.2f\n", b->count, w, h, cx, cy, s->width, s->height, b->tex[0].offset, b->tex[0].width, b->tex[0].height, b->tex[0].color, f); }
                }
            }
        }
        {   /* SG1: HUD text and icons are 2D quads the game draws right after each HUD widget, at the widget's screen position. When the HUD size
             * is not 100% scale them about that widget (the 3D parts are scaled by the DrawActor hook). The quad is matched to the nearest widget
             * from the table the hook keeps; crosshair-sized centre quads and the like are left alone. */
            extern struct sg1_anch { uint32_t id; float x, y, ax; } g_sg1_anch[96]; extern volatile int g_sg1_nanch;
            settings_init();
            if (g_sg1_hudscale != 100 && s->width >= 480 && b->count >= 4 && b->count <= 1200) {
                const Nv2aXfVertex *sv = b->vertices;
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, sw = (float)s->width, sh = (float)s->height, wmax = 0;
                uint32_t vi;
                for (vi = 0; vi < b->count; vi++) {
                    if (sv[vi].pos[0] < x0) x0 = sv[vi].pos[0]; if (sv[vi].pos[0] > x1) x1 = sv[vi].pos[0];
                    if (sv[vi].pos[1] < y0) y0 = sv[vi].pos[1]; if (sv[vi].pos[1] > y1) y1 = sv[vi].pos[1];
                    if (sv[vi].pos[3] > wmax) wmax = sv[vi].pos[3];
                }
                if (wmax <= 1.5f && (x1 - x0) < 0.4f * sw && (y1 - y0) < 0.25f * sh) {
                    float fx = (x0 + x1) * 0.5f / sw, fy = (y0 + y1) * 0.5f / sh, best = 1e9f, px = 0, py = 0;
                    int i, n = g_sg1_nanch;
                    for (i = 0; i < n; i++) {
                        float dx = fx - g_sg1_anch[i].x, dy = (fy - g_sg1_anch[i].y) * 1.78f;
                        float d = dx * dx + dy * dy;
                        if (fabsf(g_sg1_anch[i].x - 0.5f) < 0.1f && fabsf(g_sg1_anch[i].y - 0.5f) < 0.15f) continue;   /* centre widgets: crosshair etc. */
                        if (d < best && fabsf(dx) < 0.16f && fabsf(dy) < 0.12f) { best = d; px = g_sg1_anch[i].ax; py = g_sg1_anch[i].y; }
                    }
                    if (best < 1e8f) {
                        Nv2aXfVertex *dv = (Nv2aXfVertex *)((uint8_t *)m.pData + g_vb_pos);
                        float f = (float)g_sg1_hudscale / 100.0f, cx = px * sw, cy = py * sh;
                                                for (vi = 0; vi < b->count; vi++) { dv[vi].pos[0] = cx + (sv[vi].pos[0] - cx) * f; dv[vi].pos[1] = cy + (sv[vi].pos[1] - cy) * f; }
                    }
                }
            }
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_vb, 0);
        off = g_vb_pos;
        g_vb_pos += bytes;
    }
    t_b = qpc();
    g_t_vb += t_b - t_a;

    t0 = b->tex[0];
    g_lastrthit = 0;
    g_smp_nomip = (rs->blend_enable && rs->blend_src == 0x0306 && rs->blend_dst == 0x0300 && (b->rc.stage_program & 0x7FFFu) != 0x0C21u) ? 1 : 0;   /* not the player-shadow receiver (0C21): it looked fine with mips */   /* lightmaps are atlases: mips would blend neighbouring charts (flickering black/white walls) */
    depth_max = rs->depth_max > 0.0f ? rs->depth_max : 16777215.0f;
    memset(&cb, 0, sizeof cb);
    cb.surf[0] = (float)s->width; cb.surf[1] = (float)s->height;
    cb.surf[2] = 1.0f / depth_max;
    cb.misc[0] = b->use_rc ? 1u : 0u;
    cb.misc[1] = rs->alpha_test_enable ? 1u : 0u;
    cb.misc[2] = rs->alpha_func;
    cb.misc[3] = rs->alpha_ref;
    memcpy(cb.icw, b->rc.color_icw, 32); memcpy(cb.aicw, b->rc.alpha_icw, 32);
    memcpy(cb.ocw, b->rc.color_ocw, 32); memcpy(cb.aocw, b->rc.alpha_ocw, 32);
    memcpy(cb.f0, b->rc.factor0, 32);    memcpy(cb.f1, b->rc.factor1, 32);
    cb.fin[0] = b->rc.final0; cb.fin[1] = b->rc.final1;
    cb.fin[2] = b->rc.final_c0; cb.fin[3] = b->rc.final_c1;
    cb.ctl[0] = b->rc.control; cb.ctl[1] = b->rc.stage_program; cb.ctl[2] = b->rc.dot_mapping;
    cb.ctl[3] = (b->state->blend_enable && b->state->blend_src == 0x0306 && b->state->blend_dst == 0x0300 && g_dbg_hide >= 7) ? (uint32_t)(g_dbg_hide - 6) : 0u;
    memcpy(cb.fogc, b->fog_color, 16);

    for (st = 0; st < 4; st++) {
        int alpha1 = 0, rthit = 0;
        const Nv2aTexture *t = &b->tex[st];
        int need;

        /* Skip the work for a stage the program does not sample. */
        if (b->use_rc)
            need = ((int)((b->rc.stage_program >> (st * 5)) & 0x1Fu) >= 1
                 && (int)((b->rc.stage_program >> (st * 5)) & 0x1Fu) <= 3)
                || ((b->rc.stage_program >> (st * 5)) & 0x1Fu) == 9u;
        else
            need = (st == 0);
        srv[st] = g_white;
        smp[st] = NULL;
        if (b->use_rc && t->cube && b->tex_valid[st] && ((b->rc.stage_program >> (st * 5)) & 0x1Fu) == 3u
                && !((b->rc.stage_program & 0x7FFFu) == 0x0C21u && g_lastrthit)) {   /* SG1: only the projected-shadow receiver (prog 0C21 whose stage 0 is the shadow-map render target) keeps the neutral cube lookup; characters and weapons with the same program keep cube lighting, or the shadow turns into a black square */
            csrv[st] = cube_srv(t);
            if (csrv[st]) cb.cube[st] = 1.0f;
        }
        cb.tex[st][0] = cb.tex[st][1] = 1.0f;
        if (need && b->tex_valid[st]) {
            srv[st] = tex_srv(t, b->tex_texel_uv[st], r, &alpha1, &rthit);
            if (st == 0) g_lastrthit = rthit;
            if (g_dbg_log && rthit) {   /* [RTSAMP]: a render target used as a texture (projected shadows, glow, ...) */
                static unsigned nrs;
                if (nrs < 400) {
                    nrs++;
                    fprintf(stderr, "[RTSAMP] f%llu st%u rt=%08X %ux%u into=%08X %ux%u n=%u blend=%u %04X->%04X eq=%04X zt=%u zw=%u cull=%u cmask=%08X rc=%u icw0=%08X ocw0=%08X aicw0=%08X aocw0=%08X fin=%08X/%08X d0=(%.2f %.2f %.2f %.2f) tc0=(%.2f %.2f %.2f %.2f) prog=%08X\n",
                            (unsigned long long)g_frame, st, t->offset, t->width, t->height, s->color_va, s->width, s->height, b->count,
                            rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq, rs->depth_test_enable, rs->depth_write, rs->cull_enable, rs->color_mask,
                            b->use_rc, b->rc.color_icw[0], b->rc.color_ocw[0], b->rc.alpha_icw[0], b->rc.alpha_ocw[0], b->rc.final0, b->rc.final1,
                            b->vertices[0].d0[0], b->vertices[0].d0[1], b->vertices[0].d0[2], b->vertices[0].d0[3],
                            b->vertices[0].tex[st][0], b->vertices[0].tex[st][1], b->vertices[0].tex[st][2], b->vertices[0].tex[st][3], b->rc.stage_program);
                }
            }
            cb.tex[st][2] = srv[st] == g_white ? 0.0f : 1.0f;
            cb.tex[st][3] = alpha1 ? 1.0f : 0.0f;
            if (b->tex_texel_uv[st] && t->width && t->height) {
                cb.tex[st][0] = 1.0f / (float)t->width;
                cb.tex[st][1] = 1.0f / (float)t->height;
            }
            {   /* SG1: in the base + lightmap world pass (program 0x21) stage 1 is the lightmap atlas: no mips for it either */
                int keep = g_smp_nomip;
                if (b->use_rc && st == 1 && (b->rc.stage_program & 0x3FFu) == 0x21u) g_smp_nomip = 1;
                smp[st] = get_sampler(t, b->tex_border_on[st], b->tex_border[st]);
                g_smp_nomip = keep;
            }
        }
        if (b->use_rc && g_haze && ((b->rc.stage_program >> (st * 5)) & 0x1Fu) == 9u) {
            ID3D11ShaderResourceView *hz = haze_scene(r);   /* refract the live scene, not the guest's copy */
            if (hz) {
                srv[st] = hz;
                cb.tex[st][0] = 1.0f / (float)s->width; cb.tex[st][1] = 1.0f / (float)s->height;
                cb.tex[st][2] = 1.0f; cb.tex[st][3] = 1.0f;
            }
        }
        if (!smp[st]) {
            Nv2aTexture dflt;
            memset(&dflt, 0, sizeof dflt);
            dflt.addr_u = dflt.addr_v = 3;
            smp[st] = get_sampler(&dflt, 0, 0);
        }
    }

    t_c = qpc();
    g_t_tex += t_c - t_b;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof cb);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb, 0);
    }

    if (g_dbg_hide >= 7 && rs->blend_enable && rs->blend_src == 0x0306 && rs->blend_dst == 0x0300) {   /* debug: paint the 2x-modulate draws magenta, opaque */
        static Nv2aRenderState mrs;
        mrs = *rs; if (g_dbg_hide == 7 || g_dbg_hide == 9 || g_dbg_hide == 10) { mrs.blend_src = 0x0001; mrs.blend_dst = 0x0000; mrs.depth_write = 0; }
        rs = &mrs;
    }
    bs = get_blend(rs);
    ds = get_ds(rs, d != NULL);
    argb_to_float(rs->blend_color, bf);

    rtv = r->rtv;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, d ? d->dsv : NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    if (sg1_letterbox_on() && r->h > s->height && s->width == 640 && s->height >= 200) { vp.TopLeftX = (float)pw_of(s->clip_x); vp.TopLeftY = (float)pw_of(s->clip_y); }   /* letterbox: draw inside the clip rectangle */
    vp.Width = (float)pw_of(s->width); vp.Height = (float)pw_of(s->height);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, rs_for(rs));
    if (bs) ID3D11DeviceContext_OMSetBlendState(g_ctx, bs, bf, 0xFFFFFFFFu);
    if (ds) ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, ds, rs->stencil_ref & 0xFF);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 4, 4, csrv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 4, smp);
    {   /* SG1 debug: F6/F7/F8 hide one blend class of effect draws */
        int skip = 0;
        if (b->use_rc) {   /* SG1: dependent/bump/dot-product texture shader modes are not emulated and
                              come out black (heat-haze / distortion quads) -- skip them */
            static int en = -1; static unsigned nlog;
            uint32_t q;
            if (en < 0) { const char *e = getenv("RECOMP_SKIP_UNSUP_TEXMODE"); en = !(e && e[0] == '0'); }
            for (q = 0; en && q < 4; q++) {
                uint32_t m = (b->rc.stage_program >> (q * 5)) & 31u;
                uint32_t pm = q ? (b->rc.stage_program >> ((q - 1) * 5)) & 31u : 0u;
                uint32_t nm = q < 3 ? (b->rc.stage_program >> ((q + 1) * 5)) & 31u : 0u;
                settings_init();
                if (m > 4u) {
                    /* heat haze: DOT_PRODUCT (0x11) followed by DOT_ST (9) = texm3x2 refraction lookup, emulated in the pixel shader */
                    int ok = g_haze && q >= 1 && ((m == 0x11u && nm == 9u) || (m == 9u && pm == 0x11u));
                    if (!ok) { skip = 1; break; }
                }
            }
            if (g_dbg_log && skip && nlog < 5) { nlog++; fprintf(stderr, "[SKIPTEX] unsupported texture shader mode, prog=%08X n=%u\n", b->rc.stage_program, b->count); }
            if (g_dbg_log) {   /* [FXPOS]: do the fire's own flame/smoke sprites move from frame to frame? */
                static unsigned fn2;
                if (fn2 < 900 && ((b->rc.stage_program >> 5) & 31u) != 0x11u && rs->blend_enable && b->tex_valid[0] && b->tex[0].color == 0x0C
                        && b->tex[0].width == 32 && b->tex[0].height == 32 && b->count >= 6) {
                    fprintf(stderr, "[FXPOS] f%llu n=%u tex=%08X pos0=(%.1f %.1f) v1=(%.1f %.1f) w=%.1f d0=(%.2f %.2f %.2f %.2f)\n",
                            (unsigned long long)g_frame, b->count, b->tex[0].offset, b->vertices[0].pos[0], b->vertices[0].pos[1],
                            b->vertices[1].pos[0], b->vertices[1].pos[1], b->vertices[0].pos[3],
                            b->vertices[0].d0[0], b->vertices[0].d0[1], b->vertices[0].d0[2], b->vertices[0].d0[3]);
                    fn2++;
                }
            }
            if (g_dbg_log && !skip && ((b->rc.stage_program >> 5) & 31u) == 0x11u) {   /* [HAZE2]: is the refraction map animated? */
                static unsigned hn; static uint64_t lastf;
                {   /* one-off: what is in the refraction map? (channel ranges, centre and corner texels) */
                    static uint32_t seen_off; static uint32_t seen_hash_lo;
                    const uint8_t *mm2 = (const uint8_t *)xbox_GetMemoryOffset();
                    size_t nb1 = tex_bytes(&b->tex[0], b->tex_texel_uv[0]);
                    uint32_t hl = (uint32_t)hash_bytes(mm2 + b->tex[0].offset, nb1 > 8192 ? 8192 : nb1);
                    if ((seen_off != b->tex[0].offset || seen_hash_lo != hl) && b->tex[0].width * b->tex[0].height <= 262144) {
                        uint32_t *tp = (uint32_t *)malloc((size_t)b->tex[0].width * b->tex[0].height * 4);
                        seen_off = b->tex[0].offset; seen_hash_lo = hl;
                        if (tp && nv2a_backend_decode_texture(&b->tex[0], tp)) {
                            size_t nn = (size_t)b->tex[0].width * b->tex[0].height, ii;
                            unsigned mn[4] = {255,255,255,255}, mxx[4] = {0,0,0,0}; unsigned long long sm[4] = {0,0,0,0};
                            for (ii = 0; ii < nn; ii++) { int c; for (c = 0; c < 4; c++) { unsigned v = (tp[ii] >> (c * 8)) & 255; if (v < mn[c]) mn[c] = v; if (v > mxx[c]) mxx[c] = v; sm[c] += v; } }
                            fprintf(stderr, "[HAZEMAP] off=%08X %ux%u fmt%02X  R min/avg/max %u/%llu/%u  G %u/%llu/%u  B %u/%llu/%u  A %u/%llu/%u  centre=%08X corner=%08X\n",
                                    b->tex[0].offset, b->tex[0].width, b->tex[0].height, b->tex[0].color,
                                    mn[2], sm[2] / nn, mxx[2], mn[1], sm[1] / nn, mxx[1], mn[0], sm[0] / nn, mxx[0], mn[3], sm[3] / nn, mxx[3],
                                    tp[(b->tex[0].height / 2) * b->tex[0].width + b->tex[0].width / 2], tp[0]);
                        }
                        free(tp);
                    }
                }
                if (hn < 700 && (lastf != g_frame || 1)) {
                    const uint8_t *mm = (const uint8_t *)xbox_GetMemoryOffset();
                    size_t nb0 = tex_bytes(&b->tex[0], b->tex_texel_uv[0]);
                    fprintf(stderr, "[HAZE2] f%llu n=%u dotmap=%08X stin=%08X tex0 %08X %ux%u fmt%02X hash=%016llX tc0 v0=(%.3f %.3f %.3f %.3f) v1=(%.3f %.3f) v2=(%.3f %.3f) tc1 v0=(%.2f %.2f %.2f) tc2 v0=(%.2f %.2f %.2f) pos0=(%.1f %.1f) size=(%.1f %.1f) d0=(%.2f %.2f %.2f %.2f) tc3 v0=(%.2f %.2f) w=%.1f\n",
                            (unsigned long long)g_frame, b->count, b->rc.dot_mapping, b->rc.stage_input, b->tex[0].offset, b->tex[0].width, b->tex[0].height, b->tex[0].color,
                            (unsigned long long)hash_bytes(mm + b->tex[0].offset, nb0 > 8192 ? 8192 : nb0),
                            b->vertices[0].tex[0][0], b->vertices[0].tex[0][1], b->vertices[0].tex[0][2], b->vertices[0].tex[0][3],
                            b->vertices[1].tex[0][0], b->vertices[1].tex[0][1], b->vertices[2].tex[0][0], b->vertices[2].tex[0][1],
                            b->vertices[0].tex[1][0], b->vertices[0].tex[1][1], b->vertices[0].tex[1][2],
                            b->vertices[0].tex[2][0], b->vertices[0].tex[2][1], b->vertices[0].tex[2][2], b->vertices[0].pos[0], b->vertices[0].pos[1],
                            b->vertices[1].pos[0] - b->vertices[0].pos[0], b->vertices[2].pos[1] - b->vertices[0].pos[1],
                            b->vertices[0].d0[0], b->vertices[0].d0[1], b->vertices[0].d0[2], b->vertices[0].d0[3],
                            b->vertices[0].tex[3][0], b->vertices[0].tex[3][1], b->vertices[0].pos[3]);
                    hn++; lastf = g_frame;
                }
            }
        }
        if (g_dbg_log && rs->blend_enable && rs->blend_src == 0x0306 && rs->blend_dst == 0x0300) {   /* [MOD2]: 2x-modulate draws */
            static unsigned nm2;
            if (nm2 < 60) {
                uint32_t q, n = b->rc.control & 255u;
                nm2++;
                fprintf(stderr, "[MOD2] f%llu n=%u rc=%u tex0=%08X %ux%u fmt=%02X v=%d tex1=%d tex2=%d pos0=(%.1f %.1f %.1f %.3f) d0=(%.2f %.2f %.2f %.2f) t0=(%.2f %.2f) t1=(%.2f %.2f) prog=%08X z=%u fin=%08X/%08X\n",
                        (unsigned long long)g_frame, b->count, b->use_rc, t0.offset, t0.width, t0.height, t0.color, b->tex_valid[0], b->tex_valid[1], b->tex_valid[2],
                        b->vertices[0].pos[0], b->vertices[0].pos[1], b->vertices[0].pos[2], b->vertices[0].pos[3],
                        b->vertices[0].d0[0], b->vertices[0].d0[1], b->vertices[0].d0[2], b->vertices[0].d0[3],
                        b->vertices[0].tex[0][0], b->vertices[0].tex[0][1], b->vertices[0].tex[1][0], b->vertices[0].tex[1][1],
                        b->rc.stage_program, rs->depth_test_enable, b->rc.final0, b->rc.final1);
                fprintf(stderr, "[MOD2]   zfunc=%04X zwrite=%u cmask=%08X cull=%u/%04X/%04X atest=%u zeta=%08X ws=%.4f %.4f\n", rs->depth_func, rs->depth_write, rs->color_mask, rs->cull_enable, rs->cull_face, rs->front_face, rs->alpha_test_enable, rs->zeta_va, rs->depth_min, rs->depth_max);
                { uint32_t vv; for (vv = 0; vv < 3 && vv < b->count; vv++)
                    fprintf(stderr, "[MOD2]   v%u pos=(%.1f %.1f %.0f %.2f) t0=(%.3f %.3f %.3f %.3f) t1=(%.3f %.3f %.3f %.3f)\n", vv, b->vertices[vv].pos[0], b->vertices[vv].pos[1], b->vertices[vv].pos[2], b->vertices[vv].pos[3],
                        b->vertices[vv].tex[0][0], b->vertices[vv].tex[0][1], b->vertices[vv].tex[0][2], b->vertices[vv].tex[0][3],
                        b->vertices[vv].tex[1][0], b->vertices[vv].tex[1][1], b->vertices[vv].tex[1][2], b->vertices[vv].tex[1][3]); }
                for (q = 0; q < n && q < 4; q++)
                    fprintf(stderr, "[MOD2]   st%u icw %08X ocw %08X | aicw %08X aocw %08X\n", q, b->rc.color_icw[q], b->rc.color_ocw[q], b->rc.alpha_icw[q], b->rc.alpha_ocw[q]);
            }
        }
        /* Projected shadow pass: the game never renders its 128x128 shadow map (no draws reach that
         * render target), so the 2x-modulate pass would multiply the floor by an empty texture = black square. */
        if (rs->blend_enable && rs->blend_src == 0x0306 && rs->blend_dst == 0x0300 && t0.color == 0x13 && !rt_find(t0.offset))
            skip = 1;
        if (g_dbg_hide) {   /* debug menu: hide one blend class to find which draw is a bad one */
            static const uint32_t hs[7] = { 0, 0x0001, 0x0000, 0x0306, 0x0302, 0x0302, 0 };
            static const uint32_t hd[7] = { 0, 0x0301, 0x0301, 0x0300, 0x0303, 0x0001, 0 };
            int k = g_dbg_hide;
            if (k == 6) { if (!rs->blend_enable) skip = 1; }
            else if (k >= 1 && k <= 5 && rs->blend_enable && rs->blend_src == hs[k] && rs->blend_dst == hd[k]) skip = 1;
        }
        if (!skip) ID3D11DeviceContext_Draw(g_ctx, b->count, 0);
    }
    t_d = qpc();
    g_t_call += t_d - t_c;
    g_t_total += t_d - t_a;

    r->last_draw = g_frame;
    g_st_draws++;
    g_st_tris += b->count / 3;
    if (g_dbg_log) {   /* SG1: [TEXAREA] which textures cover the most screen */
        uint32_t i, tcount = b->count / 3;
        double area = 0.0;
        int k, f = -1;
        for (i = 0; i < tcount; i++) {
            const float *pa = b->vertices[i * 3].pos, *pb = b->vertices[i * 3 + 1].pos, *pc = b->vertices[i * 3 + 2].pos;
            float cr = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]);
            if (pa[3] > 0.0f && pb[3] > 0.0f && pc[3] > 0.0f && cr > -4e6f && cr < 4e6f)
                area += (double)(cr < 0 ? -cr : cr) * 0.5;
        }
        for (k = 0; k < g_ta_n; k++) if (g_ta[k].off == t0.offset && g_ta[k].fmt == t0.color && g_ta[k].w == t0.width) { f = k; break; }
        if (f < 0 && g_ta_n < 96) { f = g_ta_n++; g_ta[f].off = t0.offset; g_ta[f].fmt = t0.color; g_ta[f].w = t0.width; g_ta[f].h = t0.height; g_ta[f].lv = t0.levels; g_ta[f].flt = t0.filter; g_ta[f].area = 0; g_ta[f].draws = 0; g_ta[f].uvspan = 0; g_ta[f].tris = 0;
            g_ta[f].t1f = b->tex[1].color; g_ta[f].t1w = b->tex[1].width; g_ta[f].t1h = b->tex[1].height; g_ta[f].t2f = b->tex[2].color; g_ta[f].t2w = b->tex[2].width;
            g_ta[f].prog = b->rc.stage_program; g_ta[f].nst = b->rc.control & 255u; g_ta[f].fin0 = b->rc.final0; g_ta[f].fin1 = b->rc.final1; g_ta[f].ocw0 = b->rc.color_ocw[0]; g_ta[f].icw0 = b->rc.color_icw[0];
            g_ta[f].tv = (b->tex_valid[0] ? 1u : 0u) | (b->tex_valid[1] ? 2u : 0u) | (b->tex_valid[2] ? 4u : 0u) | (b->tex_valid[3] ? 8u : 0u); g_ta[f].uvs1 = (uint32_t)(b->vertices[0].tex[1][0] * 1000.0f); }
        if (f >= 0) {
            float umin = 1e30f, umax = -1e30f;
            for (i = 0; i < b->count && i < 3000; i++) { float u = b->vertices[i].tex[0][0]; if (u < umin) umin = u; if (u > umax) umax = u; }
            g_ta[f].area += area; g_ta[f].draws++; g_ta[f].tris += tcount;
            if (umax - umin > g_ta[f].uvspan) g_ta[f].uvspan = umax - umin;
        }
    }
    if (g_dbg_log && s && !rs->blend_enable && b->count > 6 && t0.color != 0x1E) {   /* SG1: [PICK] which draw covers the crosshair / lower screen points, and how magnified its texture is */
        float px[3], py[3];
        uint32_t i, tcount = b->count / 3;
        int k;
        px[0] = s->width * 0.5f; py[0] = s->height * 0.5f;
        px[1] = s->width * 0.5f; py[1] = s->height * 0.75f;
        px[2] = s->width * 0.25f; py[2] = s->height * 0.85f;
        for (i = 0; i < tcount; i++) {
            const Nv2aXfVertex *va = &b->vertices[i * 3], *vb = &b->vertices[i * 3 + 1], *vc = &b->vertices[i * 3 + 2];
            float area = (vb->pos[0] - va->pos[0]) * (vc->pos[1] - va->pos[1]) - (vb->pos[1] - va->pos[1]) * (vc->pos[0] - va->pos[0]);
            if (va->pos[3] <= 0.0f || vb->pos[3] <= 0.0f || vc->pos[3] <= 0.0f || area == 0.0f) continue;
            for (k = 0; k < 3; k++) {
                float w0 = ((vb->pos[0] - px[k]) * (vc->pos[1] - py[k]) - (vb->pos[1] - py[k]) * (vc->pos[0] - px[k])) / area;
                float w1 = ((vc->pos[0] - px[k]) * (va->pos[1] - py[k]) - (vc->pos[1] - py[k]) * (va->pos[0] - px[k])) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                    float z = w0 * va->pos[2] + w1 * vb->pos[2] + w2 * vc->pos[2];
                    if (!g_pk[k].have || z < g_pk[k].z) {
                        float ua = va->tex[0][0], ub = vb->tex[0][0], uc = vc->tex[0][0], vva = va->tex[0][1], vvb = vb->tex[0][1], vvc = vc->tex[0][1];
                        float uvc = (ub - ua) * (vvc - vva) - (vvb - vva) * (uc - ua);
                        float tw = (float)t0.width, th = (float)t0.height;
                        g_pk[k].have = 1; g_pk[k].z = z; g_pk[k].off = t0.offset; g_pk[k].fmt = t0.color; g_pk[k].w = t0.width; g_pk[k].h = t0.height; g_pk[k].lv = t0.levels;
                        g_pk[k].t1f = b->tex[1].color; g_pk[k].t1w = b->tex[1].width; g_pk[k].t1h = b->tex[1].height; g_pk[k].prog = b->rc.stage_program;
                        g_pk[k].u = w0 * ua + w1 * ub + w2 * uc; g_pk[k].v = w0 * vva + w1 * vvb + w2 * vvc;
                        g_pk[k].density = sqrtf(fabsf(uvc) * tw * th / fabsf(area));
                        g_pk[k].uvmax = fmaxf(fmaxf(fabsf(ua), fabsf(ub)), fabsf(uc));
                        g_pk[k].blend = (int)rs->blend_enable; g_pk[k].n = b->count;
                    }
                }
            }
        }
    }
    {   /* SG1: [BLKDRAW] opaque draws whose vertex colours are all black: what feeds them instead? */
        static unsigned nb, ncheck;
        if (g_dbg_log && !rs->blend_enable && b->count >= 30 && nb < 60 && (++ncheck % 40) == 0) {
            uint32_t i, q, n = b->rc.control & 255u; int blk = 1;
            for (i = 0; i < b->count && i < 300; i++)
                if (b->vertices[i].d0[0] + b->vertices[i].d0[1] + b->vertices[i].d0[2] > 0.0f) { blk = 0; break; }
            if (blk) {
                nb++;
                fprintf(stderr, "[BLKDRAW] n=%u rc=%u stages=%u prog=%08X fin=%08X/%08X ctl=%08X d1v0=(%.2f %.2f %.2f) z %u\n", b->count, b->use_rc, n, b->rc.stage_program,
                        b->rc.final0, b->rc.final1, b->rc.control, b->vertices[0].d1[0], b->vertices[0].d1[1], b->vertices[0].d1[2], rs->depth_test_enable);
                for (q = 0; q < 4; q++)
                    fprintf(stderr, "[BLKDRAW]   tex%u valid=%d off=%08X %ux%u fmt=%02X  t(v0)=(%.2f %.2f)\n", q, b->tex_valid[q], b->tex[q].offset, b->tex[q].width, b->tex[q].height, b->tex[q].color, b->vertices[0].tex[q][0], b->vertices[0].tex[q][1]);
                for (q = 0; q < n && q < 4; q++)
                    fprintf(stderr, "[BLKDRAW]   st%u icw %08X ocw %08X | aicw %08X aocw %08X | f0 %08X\n", q, b->rc.color_icw[q], b->rc.color_ocw[q], b->rc.alpha_icw[q], b->rc.alpha_ocw[q], b->rc.factor0[q]);
                fflush(stderr);
            }
        }
    }
    {   /* SG1: particle / effect draws (additive or alpha-blended, small textured quads) */
        static unsigned nf; static uint32_t seenoff[200]; static int nseenf;
        if (g_dbg_log && (nf < 40 || (nf < 120 && rs->blend_src == 0x0302 && rs->blend_dst == 0x0303 && t0.color == 0x0C)) && rs->blend_enable && t0.width >= 16 && b->count >= 6 && b->count <= 1500) {
            int dup = 0, k;
            for (k = 0; k < nseenf; k++) if (seenoff[k] == t0.offset + b->count) dup = 1;
            if (!dup && nseenf < 200) {
                uint32_t q, n = b->rc.control & 255u;
                seenoff[nseenf++] = t0.offset + b->count; nf++;
                fprintf(stderr, "[FXDRAW] n=%u tex=%08X fmt=%02X %ux%u rc=%u blend %04X->%04X eq %04X atest %u f%03X ref%u z %u w%u d0=(%.2f %.2f %.2f %.2f) d1=(%.2f %.2f %.2f %.2f) t0=(%.2f %.2f) prog=%08X ctl=%X fin=%08X/%08X\n",
                        b->count, t0.offset, t0.color, t0.width, t0.height, b->use_rc, rs->blend_src, rs->blend_dst, rs->blend_eq,
                        rs->alpha_test_enable, rs->alpha_func, rs->alpha_ref, rs->depth_test_enable, rs->depth_write,
                        b->vertices[0].d0[0], b->vertices[0].d0[1], b->vertices[0].d0[2], b->vertices[0].d0[3],
                        b->vertices[0].d1[0], b->vertices[0].d1[1], b->vertices[0].d1[2], b->vertices[0].d1[3],
                        b->vertices[0].tex[0][0], b->vertices[0].tex[0][1], b->rc.stage_program, b->rc.control, b->rc.final0, b->rc.final1);
                fprintf(stderr, "[FXDRAW]   tex1 valid=%d off=%08X %ux%u fmt=%02X tex2 valid=%d\n", b->tex_valid[1], b->tex[1].offset, b->tex[1].width, b->tex[1].height, b->tex[1].color, b->tex_valid[2]);
                if (b->rc.stage_program > 0x3FFu) {   /* dependent-lookup program: dump everything */
                    uint32_t st2, vv;
                    for (st2 = 0; st2 < 4; st2++)
                        fprintf(stderr, "[HAZE] st%u mode=%02X valid=%d off=%08X %ux%u fmt=%02X pitch=%u tc(v0)=(%.3f %.3f %.3f %.3f) tc(v1)=(%.3f %.3f %.3f %.3f)\n", st2, (b->rc.stage_program >> (st2*5)) & 31u,
                            b->tex_valid[st2], b->tex[st2].offset, b->tex[st2].width, b->tex[st2].height, b->tex[st2].color, b->tex[st2].pitch,
                            b->vertices[0].tex[st2][0], b->vertices[0].tex[st2][1], b->vertices[0].tex[st2][2], b->vertices[0].tex[st2][3],
                            b->vertices[1].tex[st2][0], b->vertices[1].tex[st2][1], b->vertices[1].tex[st2][2], b->vertices[1].tex[st2][3]);
                    for (vv = 0; vv < b->count && vv < 6; vv++)
                        fprintf(stderr, "[HAZE] v%u pos=(%.1f %.1f %.2f %.2f)\n", vv, b->vertices[vv].pos[0], b->vertices[vv].pos[1], b->vertices[vv].pos[2], b->vertices[vv].pos[3]);
                    fprintf(stderr, "[HAZE] consts f0=%08X f1=%08X rt=%ux%u dotmap/ctl=%08X\n", b->rc.final0, b->rc.final1, s->width, s->height, b->rc.control);
                }
                for (q = 0; q < n && q < 4; q++)
                    fprintf(stderr, "[FXDRAW]   st%u icw %08X ocw %08X | aicw %08X aocw %08X | f0 %08X\n", q,
                            b->rc.color_icw[q], b->rc.color_ocw[q], b->rc.alpha_icw[q], b->rc.alpha_ocw[q], b->rc.factor0[q]);
                fflush(stderr);
            }
        }
    }
    {   /* SG1: the HUD portrait / weapon icon draws */
        static unsigned nh;
        if (g_dbg_log && nh < 24 && ((t0.color == 0x0E && t0.width <= 64 && t0.height <= 64 && t0.width >= 32 && b->count <= 12) || t0.offset == 0x80939000u || t0.offset == 0x80D6D000u || t0.offset == 0x80D6F000u)) {
            uint32_t q, n = b->rc.control & 255u;
            nh++;
            fprintf(stderr, "[HUDDRAW] n=%u tex=%08X %ux%u rc=%u blend %u %04X->%04X atest %u f%03X ref%u d0=(%.2f %.2f %.2f %.2f) d1=(%.2f %.2f %.2f) t0=(%.2f %.2f) prog=%08X fin=%08X/%08X fc=%08X/%08X ctl=%X\n",
                    b->count, t0.offset, t0.width, t0.height, b->use_rc, rs->blend_enable, rs->blend_src, rs->blend_dst,
                    rs->alpha_test_enable, rs->alpha_func, rs->alpha_ref,
                    b->vertices[0].d0[0], b->vertices[0].d0[1], b->vertices[0].d0[2], b->vertices[0].d0[3],
                    b->vertices[0].d1[0], b->vertices[0].d1[1], b->vertices[0].d1[2],
                    b->vertices[0].tex[0][0], b->vertices[0].tex[0][1], b->rc.stage_program,
                    b->rc.final0, b->rc.final1, b->rc.final_c0, b->rc.final_c1, b->rc.control);
            for (q = 0; q < n && q < 8; q++)
                fprintf(stderr, "[HUDDRAW]   st%u icw %08X ocw %08X | aicw %08X aocw %08X | f0 %08X f1 %08X\n", q,
                        b->rc.color_icw[q], b->rc.color_ocw[q], b->rc.alpha_icw[q], b->rc.alpha_ocw[q],
                        b->rc.factor0[q], b->rc.factor1[q]);
            fflush(stderr);
        }
    }
    if (g_logframe && g_logframe == g_frame) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e30f, z1 = -1e30f, w0 = 1e30f, w1 = -1e30f;
        uint32_t k;
        for (k = 0; k < b->count; k++) {
            float x = b->vertices[k].pos[0], y = b->vertices[k].pos[1];
            if (x < x0) x0 = x; if (x > x1) x1 = x;
            if (y < y0) y0 = y; if (y > y1) y1 = y;
            { float z = b->vertices[k].pos[2], w = b->vertices[k].pos[3];
              if (z < z0) z0 = z; if (z > z1) z1 = z; if (w < w0) w0 = w; if (w > w1) w1 = w; }
        }
        logf_("D#%u rt=%08X %ux%u n=%u bb=(%.0f,%.0f)-(%.0f,%.0f) rc=%u tex0 v=%u off=%08X fmt=%02X %ux%u rt_hit=%d"
              " | blend %u %04X->%04X eq %04X | atest %u f%03X ref%u | z %u f%03X w%u | st %u f%03X ref%02X op %04X/%04X/%04X"
              " | cmask %08X cull %u | prog %08X | z %.1f..%.1f w %.2f..%.2f dmax %.0f",
              g_logn++, s->color_va, s->width, s->height, b->count, x0, y0, x1, y1, b->use_rc,
              b->tex_valid[0], t0.offset, t0.color, t0.width, t0.height, g_lastrthit,
              rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq,
              rs->alpha_test_enable, rs->alpha_func, rs->alpha_ref,
              rs->depth_test_enable, rs->depth_func, rs->depth_write,
              rs->stencil_enable, rs->stencil_func, rs->stencil_ref,
              rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass,
              rs->color_mask, rs->cull_enable, b->rc.stage_program, z0, z1, w0, w1, rs->depth_max);
        if (b->use_rc) {
            uint32_t q, n = b->rc.control & 255u;
            char line[600]; int p = 0;
            p += sprintf(line + p, "    RC n=%u flags=%X fin=%08X/%08X fc=%08X/%08X d0=(%.2f %.2f %.2f %.2f) d1=(%.2f %.2f %.2f) t0=(%.2f %.2f %.2f %.2f)", n, b->rc.control >> 8,
                b->rc.final0, b->rc.final1, b->rc.final_c0, b->rc.final_c1,
                b->vertices[0].d0[0], b->vertices[0].d0[1], b->vertices[0].d0[2], b->vertices[0].d0[3],
                b->vertices[0].d1[0], b->vertices[0].d1[1], b->vertices[0].d1[2],
                b->vertices[0].tex[0][0], b->vertices[0].tex[0][1], b->vertices[0].tex[0][2], b->vertices[0].tex[0][3]);
            logf_("%s", line);
            for (q = 0; q < n && q < 8; q++)
                logf_("      st%u icw %08X ocw %08X | aicw %08X aocw %08X | f0 %08X f1 %08X", q,
                      b->rc.color_icw[q], b->rc.color_ocw[q], b->rc.alpha_icw[q], b->rc.alpha_ocw[q],
                      b->rc.factor0[q], b->rc.factor1[q]);
        }
        if (b->count >= 300 && x1 > 230 && x0 < 420 && y1 > 60 && y0 < 250 && g_dumpn < 60) {
            char nm[64];
            sprintf(nm, "gd_%03u.bmp", g_logn - 1);
            dump_rt_bmp(r, nm);
            g_dumpn++;
        }
    }
}

static int rt_map_staging(Rt *r, D3D11_MAPPED_SUBRESOURCE *m)
{
    if (!r->staging) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = r->pw; td.Height = r->ph; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &r->staging)))
            return 0;
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)r->staging, (ID3D11Resource *)r->tex);
    return SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)r->staging, 0, D3D11_MAP_READ, 0, m));
}

/* SG1: pipelined readback. The copy for this frame is queued and the PREVIOUS frame's copy is mapped,
 * so the CPU does not wait for the GPU to finish the frame it just submitted. The data is one flip
 * old (RECOMP_ASYNC_RESOLVE=0 goes back to the blocking readback). Returns the mapped texture, or
 * NULL when there is nothing mapped yet (first call). */
static ID3D11Texture2D *rt_map_async(Rt *r, int slot, D3D11_MAPPED_SUBRESOURCE *m)
{
    static int en = -1;
    uint32_t i;
    ID3D11Texture2D *prev;
    if (en < 0) { const char *e = getenv("RECOMP_ASYNC_RESOLVE"); en = !(e && e[0] == '0'); }
    if (!en) return rt_map_staging(r, m) ? r->staging : NULL;
    for (i = 0; i < 2; i++) if (!r->stg[slot][i]) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = r->pw; td.Height = r->ph; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &r->stg[slot][i]))) return NULL;
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)r->stg[slot][r->stg_idx[slot]], (ID3D11Resource *)r->tex);
    prev = r->stg[slot][r->stg_idx[slot] ^ 1];
    r->stg_idx[slot] ^= 1;
    if (!r->stg_have[slot]) { r->stg_have[slot] = 1; return NULL; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)prev, 0, D3D11_MAP_READ, 0, m))) return NULL;
    return prev;
}

static void be_resolve(uint32_t color_va, uint32_t pitch, uint32_t bpp)
{
    Rt *r;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *mt;
    uint8_t *dst = (uint8_t *)xbox_GetMemoryOffset() + color_va;
    uint32_t y, x;
    if (!g_dev || g_init_failed) return;
    r = rt_find(color_va);
    if (!r) {   /* SG1 diag: a resolve into memory that is not a known render target (a cutscene / other surface?) */
        static int miss_n; if (miss_n < 40 || (miss_n % 200) == 0) fprintf(stderr, "[RESOLVE-MISS] frame %u va %08X pitch %u bpp %u (n=%d)\n", (unsigned)g_frame, color_va, pitch, bpp, miss_n);
        miss_n++;
        return;
    }
    mt = rt_map_async(r, 0, &m);
    if (!mt)
        return;
    for (y = 0; y < r->h; y++) {
        /* at a resolution scale the guest copy is a point-sampled reduction */
        const uint8_t *src = (const uint8_t *)m.pData + (size_t)(r->pw == r->w ? y : (uint64_t)y * r->ph / r->h) * m.RowPitch;
        uint8_t *row = dst + (size_t)y * pitch;
        if (bpp == 4) {
            if (r->pw == r->w) memcpy(row, src, (size_t)r->w * 4);
            else { uint32_t *o = (uint32_t *)row; const uint32_t *sp = (const uint32_t *)src;
                   for (x = 0; x < r->w; x++) o[x] = sp[(uint64_t)x * r->pw / r->w]; }
        } else {
            uint16_t *o = (uint16_t *)row;
            for (x = 0; x < r->w; x++) {
                uint32_t sx = r->pw == r->w ? x : (uint32_t)((uint64_t)x * r->pw / r->w);
                uint32_t b = src[sx * 4 + 0], g = src[sx * 4 + 1], rr = src[sx * 4 + 2];
                o[x] = (uint16_t)(((rr >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }
    if (g_logframe && g_logframe == g_frame && r->w <= 1280 && r->pw == r->w) {
        FILE *bf = fopen("gpu_frame.bmp", "wb");
        if (bf) {
            uint32_t sz = 54 + r->w * r->h * 4;
            uint8_t hd[54];
            memset(hd, 0, sizeof hd);
            hd[0] = 'B'; hd[1] = 'M';
            memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
            { uint32_t w = r->w; int32_t h = -(int32_t)r->h;
              memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4); }
            hd[26] = 1; hd[28] = 32;
            fwrite(hd, 1, 54, bf);
            for (y = 0; y < r->h; y++)
                fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 1, (size_t)r->w * 4, bf);
            fclose(bf);
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)mt, 0);
    g_st_resolve++;
}

/* The finished frame at full render resolution, for the window (fb_present.c). Called on the thread
 * that draws. Returns 0 at scale 1 or when the frame is not a known render target. The pointer
 * stays valid until the next call. */
int sg1_hires_grab(uint32_t va, const uint32_t **pix, uint32_t *w, uint32_t *h)
{
    static uint32_t *buf; static size_t cap;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *mt;
    Rt *r;
    uint32_t y;
    if (!g_dev || g_init_failed) return 0;
    r = rt_find(va);
    if (!r) { static int hm; if (hm < 40 || (hm % 100) == 0) fprintf(stderr, "[HIRES-MISS] frame %u flip va %08X not a render target (n=%d)\n", (unsigned)g_frame, va, hm); hm++; }
    if (!r || (r->pw == r->w && r->w == 640)) return 0;   /* 640-wide frames use the normal path; wider modes (SETRES) always need this one */
    if (cap < (size_t)r->pw * r->ph) {
        free(buf); cap = (size_t)r->pw * r->ph;
        buf = (uint32_t *)malloc(cap * 4);
        if (!buf) { cap = 0; return 0; }
    }
    mt = rt_map_async(r, 1, &m);
    if (!mt) return 0;
    for (y = 0; y < r->ph; y++)
        memcpy(buf + (size_t)y * r->pw, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)r->pw * 4);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)mt, 0);
    *pix = buf; *w = r->pw; *h = r->ph;
    return 1;
}

extern int g_sg1_mode_w, g_sg1_mode_h;
/* Hor+: the menu FOV is the 4:3 value; on a wider mode raise it so the vertical view stays the same. */
static int sg1_hor_plus(int fov)
{
    double a = g_sg1_mode_h > 0 ? (double)g_sg1_mode_w / (double)g_sg1_mode_h : 4.0 / 3.0;
    if (a > 1.4 && fov > 0 && fov < 170) {
        double h = tan(fov * 0.5 * 3.14159265358979 / 180.0) * (a * 0.75);
        fov = (int)(2.0 * atan(h) * 180.0 / 3.14159265358979 + 0.5);
        if (fov > 150) fov = 150;
    }
    return fov;
}
extern void xbox_FramebufferWindowSetNote(const char *text);
static void res_note(void)
{
    char t[64];
    snprintf(t, sizeof t, "Res: %s %dx%d", g_res_presets[g_res_idx].name,
             (int)(640.0f * g_rscale + 0.5f), (int)(480.0f * g_rscale + 0.5f));
    xbox_FramebufferWindowSetNote(t);
}

/* In-game settings menu callbacks (the menu is in fb_present.c on the window thread). Items: 0 resolution,
 * 1 anisotropic filtering, 2 mipmaps. The render thread applies what they request in be_flip. */
extern int g_sg1_bm_count;
extern void (*g_sg1_bm_text)(int item, char *buf, int n);
extern void (*g_sg1_bm_change)(int item, int dir);
static void bm_text(int item, char *b, int n)
{
    settings_init();
    if (item == 0)
        snprintf(b, n, "Resolution          < %s %dx%d >", g_res_presets[g_res_idx].name,
                 (int)(640.0f * g_rscale + 0.5f), (int)(480.0f * g_rscale + 0.5f));
    else if (item == 1) {
        if (g_aniso <= 1) snprintf(b, n, "Anisotropic filter  < off >");
        else snprintf(b, n, "Anisotropic filter  < %dx >", g_aniso);
    } else if (item == 2)
        snprintf(b, n, "Mipmaps             < %s >", g_mips ? "on" : "off");
    else if (item == 15)
        snprintf(b, n, "White prop level    < %d%% >", g_sg1_whitepct);
    else if (item == 14)
        snprintf(b, n, "Field of view       < %d >", g_sg1_fov);
    else if (item == 17)
        snprintf(b, n, "HUD size            < %d%% >", g_sg1_hudscale);
    else if (item == 18)
        snprintf(b, n, "Crosshair size      < %d%% >", g_sg1_xhair);
    else if (item == 16)
        snprintf(b, n, "Hide PLACE-HOLDER   < %s >", g_sg1_hideph ? "on" : "off");
    else if (item == 13)
        snprintf(b, n, "HD texture pack     < %s >", g_sg1_hdpack ? "on" : "off");
    else if (item == 12) {
        snprintf(b, n, "Cube map lighting   < %s >", g_sg1_cube ? "on" : "off");
    } else if (item == 11) {
        snprintf(b, n, "Floor texgen fix    < %s >", g_sg1_texmat ? "on" : "off");
    } else if (item == 10) {
        static const char *ln[5] = { "off", "low", "medium", "high", "max" };
        snprintf(b, n, "Dark area lift      < %s >", ln[g_sg1_lightfloor]);
    } else if (item == 9) {
        snprintf(b, n, "Game gamma/bright.  < %s >", g_sg1_gamma_on ? "on" : "off");
    } else if (item == 8) {
        snprintf(b, n, "Player shadows      < %s >", g_sg1_tg0 ? "on" : "off");
    } else if (item == 7) {
        static const char *nm[11] = { "off", "ONE / INV_SRC_COLOR", "ZERO / INV_SRC_COLOR", "DST_COLOR / SRC_COLOR", "SRC_ALPHA / INV_SRC_ALPHA", "SRC_ALPHA / ONE", "all opaque draws", "shadow draws painted magenta", "shadow: ignore fade stages", "shadow: show raw map", "shadow: show UV as colour" };
        snprintf(b, n, "Debug: hide draws   < %s >", nm[g_dbg_hide]);
    } else if (item == 5)
        snprintf(b, n, "Debug: texture dump < %s >", g_dbg_tex ? "on" : "off");
    else if (item == 6)
        snprintf(b, n, "Debug: draw logs    < %s >", g_dbg_log ? "on" : "off");
    else if (item == 4)
        snprintf(b, n, "Heat haze           < %s >", g_haze ? "on" : "off");
    else if (item == 3)
        snprintf(b, n, "Custom textures    < %s >", g_sg1_custom ? "on" : "off");
    else b[0] = 0;
}
static void bm_change(int item, int dir)
{
    char v[16];
    settings_init();
    if (item == 0) g_req_res = dir > 0 ? 1 : -1;
    else if (item == 1) {
        static const int lv[5] = { 1, 2, 4, 8, 16 };
        int i, cur = 0;
        for (i = 0; i < 5; i++) if (lv[i] <= g_aniso) cur = i;
        cur += dir > 0 ? 1 : -1;
        if (cur < 0) cur = 0;
        if (cur > 4) cur = 4;
        g_aniso = lv[cur];
        snprintf(v, sizeof v, "%d", g_aniso);
        sg1_cfg_set("aniso", v);
    } else if (item == 2) {
        g_mips = !g_mips;
        sg1_cfg_set("mips", g_mips ? "1" : "0");
        g_req_flush = 1;
    } else if (item == 15) {
        g_sg1_whitepct += dir > 0 ? 5 : -5;
        if (g_sg1_whitepct < 0) g_sg1_whitepct = 0;
        if (g_sg1_whitepct > 100) g_sg1_whitepct = 100;
        snprintf(v, sizeof v, "%d", g_sg1_whitepct);
        sg1_cfg_set("whitepct", v);
    } else if (item == 14) {
        extern void mkb_type_command(const char *cmd, int delay_ms);
        char cmd[32];
        g_sg1_fov += dir > 0 ? 5 : -5;
        if (g_sg1_fov < 60) g_sg1_fov = 60;
        if (g_sg1_fov > 120) g_sg1_fov = 120;
        snprintf(v, sizeof v, "%d", g_sg1_fov);
        sg1_cfg_set("fov", v);
        snprintf(cmd, sizeof cmd, "FOV %d", sg1_hor_plus(g_sg1_fov));
        mkb_type_command(cmd, 700);   /* typed into the game console once you stop pressing */
    } else if (item == 18) {
        g_sg1_xhair += dir > 0 ? 5 : -5;
        if (g_sg1_xhair < 25) g_sg1_xhair = 25;
        if (g_sg1_xhair > 200) g_sg1_xhair = 200;
        snprintf(v, sizeof v, "%d", g_sg1_xhair);
        sg1_cfg_set("xhair", v);
    } else if (item == 17) {
        g_sg1_hudscale += dir > 0 ? 5 : -5;
        if (g_sg1_hudscale < 50) g_sg1_hudscale = 50;
        if (g_sg1_hudscale > 150) g_sg1_hudscale = 150;
        snprintf(v, sizeof v, "%d", g_sg1_hudscale);
        sg1_cfg_set("hudscale", v);
    } else if (item == 16) {
        g_sg1_hideph = !g_sg1_hideph;
        sg1_cfg_set("hideph", g_sg1_hideph ? "1" : "0");
    } else if (item == 13) {
        g_sg1_hdpack = !g_sg1_hdpack;
        sg1_cfg_set("hdpack", g_sg1_hdpack ? "1" : "0");
        g_req_flush = 1;
    } else if (item == 12) {
        g_sg1_cube = !g_sg1_cube;
        sg1_cfg_set("cubemaps", g_sg1_cube ? "1" : "0");
    } else if (item == 11) {
        g_sg1_texmat = !g_sg1_texmat;
        sg1_cfg_set("texmat", g_sg1_texmat ? "1" : "0");
    } else if (item == 10) {
        g_sg1_lightfloor += dir > 0 ? 1 : -1;
        if (g_sg1_lightfloor < 0) g_sg1_lightfloor = 4;
        if (g_sg1_lightfloor > 4) g_sg1_lightfloor = 0;
        { char v[8]; snprintf(v, sizeof v, "%d", g_sg1_lightfloor); sg1_cfg_set("light_floor", v); }
    } else if (item == 9) {
        g_sg1_gamma_on = !g_sg1_gamma_on;
        sg1_cfg_set("gamma_ramp", g_sg1_gamma_on ? "1" : "0");
    } else if (item == 8) {
        g_sg1_tg0 = !g_sg1_tg0;
        sg1_cfg_set("shadows", g_sg1_tg0 ? "1" : "0");
    } else if (item == 7) {
        g_dbg_hide += dir > 0 ? 1 : -1;
        if (g_dbg_hide < 0) g_dbg_hide = 10;
        if (g_dbg_hide > 10) g_dbg_hide = 0;
    } else if (item == 5) {
        g_dbg_tex = !g_dbg_tex;
        sg1_cfg_set("dbg_texdump", g_dbg_tex ? "1" : "0");
    } else if (item == 6) {
        g_dbg_log = !g_dbg_log;
        sg1_cfg_set("dbg_logs", g_dbg_log ? "1" : "0");
    } else if (item == 4) {
        g_haze = !g_haze;
        sg1_cfg_set("haze", g_haze ? "1" : "0");
    } else if (item == 3) {
        g_sg1_custom = !g_sg1_custom;
        sg1_cfg_set("customtex", g_sg1_custom ? "1" : "0");
        g_req_flush = 1;
    }
}

static void dbg_dump_small_rts(void)
{   /* debug draw logs: save the 128x128 shadow maps as BMP a few times */
    static unsigned cnt, lastf;
    int i, did = 0;
    if (!getenv("RECOMP_RT_DUMP") || cnt >= 8 || g_frame < lastf + 90) return;   /* only with RECOMP_RT_DUMP=1, not with the draw logs */
    for (i = 0; i < MAX_RT; i++) {
        Rt *r = &g_rt[i];
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D *st = NULL;
        D3D11_MAPPED_SUBRESOURCE m;
        if (!r->tex || r->w != 128 || r->h != 128) continue;
        memset(&td, 0, sizeof td);
        td.Width = r->pw; td.Height = r->ph; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &st))) continue;
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)r->tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            char fn[64]; FILE *f; uint8_t hdr[54]; unsigned x, y; unsigned long long sr = 0, sg = 0, sb = 0, sa = 0;
            snprintf(fn, sizeof fn, "rt128_%08X_%u.bmp", r->va, cnt);
            for (y = 0; y < 128; y++) for (x = 0; x < 128; x++) {
                const uint8_t *p = (const uint8_t *)m.pData + y * m.RowPitch + x * 4;
                sb += p[0]; sg += p[1]; sr += p[2]; sa += p[3];
            }
            fprintf(stderr, "[RT128] f%llu va=%08X avg B%llu G%llu R%llu A%llu\n", (unsigned long long)g_frame, r->va, sb / 16384, sg / 16384, sr / 16384, sa / 16384);
            f = fopen(fn, "wb");
            if (f) {
                uint32_t fs = 54 + 128 * 128 * 3, z = 0;
                memset(hdr, 0, 54); hdr[0] = 'B'; hdr[1] = 'M'; memcpy(hdr + 2, &fs, 4); hdr[10] = 54; hdr[14] = 40;
                { uint32_t w = 128, h = 128; memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); } hdr[26] = 1; hdr[28] = 24;
                (void)z; fwrite(hdr, 1, 54, f);
                for (y = 0; y < 128; y++) for (x = 0; x < 128; x++) {
                    const uint8_t *p = (const uint8_t *)m.pData + (127 - y) * m.RowPitch + x * 4;
                    fputc(p[0], f); fputc(p[1], f); fputc(p[2], f);
                }
                fclose(f);
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st); did = 1;
    }
    if (did) { cnt++; lastf = (unsigned)g_frame; }
}

static void dbg_dump_texarea(void)
{
    static unsigned fl;
    int k, j, top[8], nt = 0;
    if (g_dbg_log && ++fl % 90 == 0) {
        for (k = 0; k < 3; k++)
            if (g_pk[k].have)
                fprintf(stderr, "[PICK] pt%d tex0 %08X fmt=%02X %ux%u lv=%u | uv=(%.3f %.3f) uvmax=%.2f texels/pixel=%.3f blend=%d verts=%u | tex1 fmt=%02X %ux%u prog=%08X\n", k, g_pk[k].off, g_pk[k].fmt, g_pk[k].w, g_pk[k].h, g_pk[k].lv,
                        (double)g_pk[k].u, (double)g_pk[k].v, (double)g_pk[k].uvmax, (double)g_pk[k].density, g_pk[k].blend, g_pk[k].n, g_pk[k].t1f, g_pk[k].t1w, g_pk[k].t1h, g_pk[k].prog);
    }
    if (g_dbg_log) memset(g_pk, 0, sizeof g_pk);
    if (!g_dbg_log || fl % 300 != 0) { return; }
    for (j = 0; j < 8; j++) {
        int best = -1;
        for (k = 0; k < g_ta_n; k++) {
            int used = 0, q;
            for (q = 0; q < nt; q++) if (top[q] == k) used = 1;
            if (!used && (best < 0 || g_ta[k].area > g_ta[best].area)) best = k;
        }
        if (best < 0) break;
        top[nt++] = best;
        fprintf(stderr, "[TEXAREA] off=%08X fmt=%02X %ux%u levels=%u filter=%08X area=%.0f px draws=%u tris=%u uvspan0=%.2f\n", g_ta[best].off, g_ta[best].fmt,
                g_ta[best].w, g_ta[best].h, g_ta[best].lv, g_ta[best].flt, g_ta[best].area, g_ta[best].draws, g_ta[best].tris, (double)g_ta[best].uvspan);
        fprintf(stderr, "[TEXAREA]    tex1 fmt=%02X %ux%u tex2 fmt=%02X w=%u valid=%X stages=%u prog=%08X fin=%08X/%08X st0 icw %08X ocw %08X\n", g_ta[best].t1f, g_ta[best].t1w, g_ta[best].t1h, g_ta[best].t2f, g_ta[best].t2w,
                g_ta[best].tv, g_ta[best].nst, g_ta[best].prog, g_ta[best].fin0, g_ta[best].fin1, g_ta[best].icw0, g_ta[best].ocw0);
    }
    fprintf(stderr, "[TEXAREA] ---\n");
    g_ta_n = 0;
}

static void be_flip(void)
{
    dbg_dump_texarea();
    dbg_dump_small_rts();
    static int note_done;
    if (!note_done) { note_done = 1; res_note(); }
    {   /* SG1 ultrawide: when SETRES changes the aspect, re-send the FOV (Hor+) so the vertical view stays as at 4:3 */
        static int lw = 640, lh = 480, settle;
        if (g_sg1_mode_w != lw || g_sg1_mode_h != lh) { lw = g_sg1_mode_w; lh = g_sg1_mode_h; settle = 120; }
        if (settle && --settle == 0) {
            extern void mkb_type_command(const char *cmd, int delay_ms);
            char fc[32];
            snprintf(fc, sizeof fc, "FOV %d", sg1_hor_plus(g_sg1_fov));
            fprintf(stderr, "[SG1-FOV] mode %dx%d -> %s\n", lw, lh, fc);
            mkb_type_command(fc, 300);
        }
    }
    static DWORD last_ms;
    DWORD now = GetTickCount();
    g_frame++;
    {   /* SG1: F12 cycles the render resolution */
        static int f12;   /* Home = next preset, End = previous (F12 is reserved by Windows for debuggers and breaks the run) */
        int up = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0, dwn = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        int dn = up || dwn;
        if (dn && !f12) {
            FILE *rf;
            res_set_idx(g_res_idx + (up ? 1 : -1));
            res_note();
            fprintf(stderr, "[RES] resolution %s (scale %.2f)\n", g_res_presets[g_res_idx].name, (double)g_rscale);
            rf = fopen("resolution.txt", "w");
            if (rf) { fprintf(rf, "%d\n", (int)(480.0f * g_rscale + 0.5f)); fclose(rf); }
        }
        f12 = dn;
    }
    {   /* SG1: changes requested by the in-game settings menu (they arrive from the window thread) */
        int rq = g_req_res;
        g_req_res = 0;
        if (rq) {
            FILE *rf;
            res_set_idx(g_res_idx + (rq > 0 ? 1 : -1));
            res_note();
            fprintf(stderr, "[RES] resolution %s (scale %.2f)\n", g_res_presets[g_res_idx].name, (double)g_rscale);
            rf = fopen("resolution.txt", "w");
            if (rf) { fprintf(rf, "%d\n", (int)(480.0f * g_rscale + 0.5f)); fclose(rf); }
        }
        if (g_req_flush) {      /* mipmaps toggled: drop every uploaded texture so they are rebuilt */
            int ti;
            g_req_flush = 0;
            for (ti = 0; ti < MAX_TEX; ti++) {
                if (g_tex[ti].srv) ID3D11ShaderResourceView_Release(g_tex[ti].srv);
                if (g_tex[ti].tex) ID3D11Texture2D_Release(g_tex[ti].tex);
                memset(&g_tex[ti], 0, sizeof g_tex[ti]);
            }
        }
    }
    if (g_logframe && g_frame > g_logframe) g_logframe = 0;
    if (!g_logframe) {
        FILE *ff = fopen("gpu_frame.flag", "rb");
        if (ff) { fclose(ff); remove("gpu_frame.flag"); g_logframe = g_frame; g_logn = 0; g_dumpn = 0;
                  logf_("---- logging draws of frame %u", g_frame); }
    }
    if (g_debug && now - last_ms > 5000) {
        { LARGE_INTEGER fq; double ms; QueryPerformanceFrequency(&fq); ms = 1000.0 / (double)fq.QuadPart;
          logf_("5s: draws %u tris %u texup %u rt-as-tex %u clears %u resolves %u | be_draw ms: total %.0f vb %.0f tex %.0f state+draw %.0f",
              g_st_draws, g_st_tris, g_st_texup, g_st_rtuse, g_st_clears, g_st_resolve,
              g_t_total * ms, g_t_vb * ms, g_t_tex * ms, g_t_call * ms);
          g_t_total = g_t_vb = g_t_tex = g_t_call = 0; }
        g_st_draws = g_st_tris = g_st_texup = g_st_rtuse = g_st_clears = g_st_resolve = 0;
        last_ms = now;
    }
}

static const Nv2aBackendEx g_backend = { be_clear, be_draw, be_resolve, be_flip };

void sg1_gpu_backend_install(void)
{
    extern int (*xbox_fb_hires_hook)(uint32_t, const uint32_t **, uint32_t *, uint32_t *);
    nv2a_backend_register_ex(&g_backend);
    xbox_fb_hires_hook = sg1_hires_grab;
    g_sg1_bm_count = 19; g_sg1_bm_text = bm_text; g_sg1_bm_change = bm_change;
    fprintf(stderr, "[GPU11] D3D11 back end registered\n");
}
