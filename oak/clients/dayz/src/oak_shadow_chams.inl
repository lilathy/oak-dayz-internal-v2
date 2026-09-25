// Mesh chams: recolor the game's own character draws (DrawIndexed), depth off.
// That is the actual silhouette — helmet, backpack, gun, pose — not boxes or bones.

#include "oak_chams_ps.h"

static OakShadowChamsSettings g_ShadowChams = {};
static int g_ShadowChamsDrew = 0;
static int g_ShadowChamsConsidered = 0;

static void OakShadowChamsApplySettings(const OakShadowChamsSettings& s)
{
    g_ShadowChams = s;
    // Hard filter: never cham world props even if an old config toggled them on.
    g_ShadowChams.vehicles = false;
    g_ShadowChams.containers = false;
    g_ShadowChams.corpses = false;
}

static bool OakShadowChamsIsEnabled()
{
    return g_ShadowChams.enabled && !g_PanicHidden;
}

static bool OakShadowChamsKindEnabled(OakShadowEntityKind kind)
{
    if (!OakShadowChamsIsEnabled()) return false;
    switch (kind)
    {
    case OakShadowKindPlayer:    return g_ShadowChams.players;
    case OakShadowKindZombie:    return g_ShadowChams.zombies;
    case OakShadowKindItem:      return g_ShadowChams.items;
    case OakShadowKindVehicle:   return g_ShadowChams.vehicles;
    case OakShadowKindContainer: return g_ShadowChams.containers;
    case OakShadowKindCorpse:    return g_ShadowChams.corpses;
    default: return false;
    }
}

static float OakShadowChamsAnimT()
{
    float spd = g_ShadowChams.animSpeed;
    if (spd < 0.05f) spd = 0.05f;
    if (spd > 8.f) spd = 8.f;
    return (float)(GetTickCount() % 120000) * 0.001f * spd;
}

static void OakShadowChamsRgbFromHue(float hue, float* r, float* g, float* b)
{
    float h = hue - floorf(hue);
    float x = 1.f - fabsf(fmodf(h * 6.f, 2.f) - 1.f);
    if (h < 1.f / 6.f)      { *r = 1.f; *g = x;   *b = 0.f; }
    else if (h < 2.f / 6.f) { *r = x;   *g = 1.f; *b = 0.f; }
    else if (h < 3.f / 6.f) { *r = 0.f; *g = 1.f; *b = x;   }
    else if (h < 4.f / 6.f) { *r = 0.f; *g = x;   *b = 1.f; }
    else if (h < 5.f / 6.f) { *r = x;   *g = 0.f; *b = 1.f; }
    else                    { *r = 1.f; *g = 0.f; *b = x;   }
}

static void OakShadowChamsFillRgb(float* r, float* g, float* b, float* a)
{
    const float* c0 = g_ShadowChams.color;
    const float* c1 = g_ShadowChams.color2;
    float t = OakShadowChamsAnimT();
    *r = c0[0]; *g = c0[1]; *b = c0[2];
    *a = g_ShadowChams.fillAlpha;
    if (*a < 0.20f) *a = 0.20f;
    if (*a > 1.f) *a = 1.f;

    switch (g_ShadowChams.pattern)
    {
    case OakShadowPatternPulse:
    {
        float p = 0.55f + 0.45f * sinf(t * 3.2f);
        *r = c0[0] * p + c1[0] * (1.f - p);
        *g = c0[1] * p + c1[1] * (1.f - p);
        *b = c0[2] * p + c1[2] * (1.f - p);
        break;
    }
    case OakShadowPatternRainbow:
        OakShadowChamsRgbFromHue(t * 0.18f, r, g, b);
        break;
    case OakShadowPatternScroll:
    case OakShadowPatternStripes:
    {
        float k = 0.5f + 0.5f * sinf(t * 2.5f);
        *r = c0[0] * (1.f - k) + c1[0] * k;
        *g = c0[1] * (1.f - k) + c1[1] * k;
        *b = c0[2] * (1.f - k) + c1[2] * k;
        break;
    }
    default:
        break;
    }
}

enum { kOakChamMax = 64, kOakChamCbFloats = 384, kOakChamCbSlots = 128, kOakChamSticky = 96 };

struct OakChamTarget
{
    float x, y, z;
    float hx, hy, hz;
    float rXZ;
    bool hands;
    bool item;
};

static OakChamTarget g_ChamTgt[kOakChamMax];
static int g_ChamN = 0;
static int g_ChamsHits = 0;
static int g_ChamsLooked = 0;
static int g_ChamsStickyHits = 0;

static ID3D11PixelShader* g_ChamsPS = nullptr;
static ID3D11Buffer* g_ChamsCB = nullptr;
static ID3D11DepthStencilState* g_ChamsDSS = nullptr;
static ID3D11RasterizerState* g_ChamsRS = nullptr;
static ID3D11RasterizerState* g_WireframeRS = nullptr;
static ID3D11BlendState* g_ChamsBlend = nullptr;
static bool g_ChamsHooked = false;
static volatile LONG g_ChamsCalls = 0;
static ID3D11DeviceContext* g_ChamsCtxObj = nullptr;
static void** g_ChamsOrigVt = nullptr;
static void* g_ChamsVtCopy[256];
static ID3D11Device* g_ChamsDevObj = nullptr;
static void** g_ChamsDevOrigVt = nullptr;
static void* g_ChamsDevVtCopy[256];

typedef void (STDMETHODCALLTYPE* OakDrawIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE* OakDrawFn)(ID3D11DeviceContext*, UINT, UINT);
typedef void (STDMETHODCALLTYPE* OakDrawIndexedInstancedFn)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* OakMapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void (STDMETHODCALLTYPE* OakUnmapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void (STDMETHODCALLTYPE* OakUpdateSubresourceFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* OakCreateDeferredFn)(ID3D11Device*, UINT, ID3D11DeviceContext**);

static OakDrawIndexedFn oChamsDrawIndexed = nullptr;
static OakDrawFn oChamsDraw = nullptr;
static OakDrawIndexedInstancedFn oChamsDrawIndexedInstanced = nullptr;
static OakMapFn oChamsMap = nullptr;
static OakUnmapFn oChamsUnmap = nullptr;
static OakUpdateSubresourceFn oChamsUpdateSubresource = nullptr;
static OakCreateDeferredFn oChamsCreateDeferred = nullptr;

static ID3D11Resource* g_ChamMapRes = nullptr;
static void* g_ChamMapPtr = nullptr;
static UINT g_ChamMapBytes = 0;

struct OakChamCbSnap
{
    ID3D11Resource* res;
    UINT bytes;
    DWORD tick;
    float data[kOakChamCbFloats];
};
static OakChamCbSnap g_ChamCb[kOakChamCbSlots];
static int g_ChamCbW = 0;

struct OakChamSticky
{
    ID3D11Buffer* vb;
    float x, y, z;
    float rXZ;
    bool hands;
    bool item;
    DWORD lastHit;
};
static OakChamSticky g_ChamSticky[kOakChamSticky];
static int g_ChamStickyN = 0;

static __declspec(thread) int t_ChamsBusy = 0;

static bool OakChamsWireframeOn()
{
    return g_WorldMisc.wireframe && !g_PanicHidden;
}

static bool OakChamsSwapVptr(void* obj, void** neu, void*** savedOrig)
{
    if (!obj || !neu) return false;
    void** cur = *(void***)obj;
    if (cur == neu) return true;
    DWORD prot = 0;
    if (!VirtualProtect(obj, sizeof(void*), PAGE_READWRITE, &prot))
        return false;
    if (savedOrig && !*savedOrig)
        *savedOrig = cur;
    *(void***)obj = neu;
    DWORD tmp = 0;
    VirtualProtect(obj, sizeof(void*), prot, &tmp);
    return true;
}

static void OakChamsStoreCb(ID3D11Resource* res, const void* src, UINT bytes)
{
    if (!res || !src || bytes < 48) return;
    UINT n = bytes;
    if (n > (UINT)(kOakChamCbFloats * 4)) n = (UINT)(kOakChamCbFloats * 4);
    int slot = -1;
    for (int i = 0; i < kOakChamCbSlots; i++)
    {
        if (g_ChamCb[i].res == res) { slot = i; break; }
    }
    if (slot < 0)
    {
        slot = g_ChamCbW % kOakChamCbSlots;
        g_ChamCbW++;
        g_ChamCb[slot].res = res;
    }
    memcpy(g_ChamCb[slot].data, src, n);
    g_ChamCb[slot].bytes = n;
    g_ChamCb[slot].tick = GetTickCount();
}

static bool OakChamsNear3(float px, float py, float pz, float ex, float ey, float ez, float rXZ, float yLo, float yHi)
{
    float dx = px - ex, dy = py - ey, dz = pz - ez;
    if (dx * dx + dz * dz > rXZ * rXZ) return false;
    return dy >= yLo && dy <= yHi;
}

static int OakChamsMatchTargetIndex(float px, float py, float pz, bool viewSpace)
{
    if (!(px == px && py == py && pz == pz)) return -1;
    if (viewSpace)
    {
        if (pz < 0.08f || pz > 420.f) return -1;
        if (fabsf(px) > 90.f || fabsf(py) > 90.f) return -1;
    }
    for (int e = 0; e < g_ChamN; e++)
    {
        const OakChamTarget& t = g_ChamTgt[e];
        float r = t.rXZ > 0.1f ? t.rXZ : 1.15f;
        if (t.hands)
        {
            // Viewmodel only — refuse world-space random matches.
            if (!viewSpace) continue;
            if (pz < 0.08f || pz > 2.2f) continue;
            if (OakChamsNear3(px, py, pz, t.x, t.y, t.z, r, -0.85f, 0.85f))
                return e;
            continue;
        }
        if (viewSpace)
        {
            if (!g_W2S.valid) continue;
            const Vec3 cam = g_W2S.translation;
            const Vec3 right = g_W2S.right;
            const Vec3 up = g_W2S.up;
            const Vec3 fwd = g_W2S.forward;
            auto toView = [&](float wx, float wy, float wz, float& ox, float& oy, float& oz) {
                float dx = wx - cam.x, dy = wy - cam.y, dz = wz - cam.z;
                ox = dx * right.x + dy * right.y + dz * right.z;
                oy = dx * up.x + dy * up.y + dz * up.z;
                oz = dx * fwd.x + dy * fwd.y + dz * fwd.z;
            };
            float vx, vy, vz;
            toView(t.x, t.y, t.z, vx, vy, vz);
            // y floor above feet kills terrain/fence bases sharing the same XZ.
            if (OakChamsNear3(px, py, pz, vx, vy, vz, r + 0.15f, t.item ? -0.35f : 0.20f, t.item ? 1.2f : 2.40f))
                return e;
            toView(t.hx, t.hy, t.hz, vx, vy, vz);
            if (OakChamsNear3(px, py, pz, vx, vy, vz, r, -0.55f, 1.00f))
                return e;
        }
        else
        {
            if (OakChamsNear3(px, py, pz, t.x, t.y, t.z, r, t.item ? -0.35f : 0.25f, t.item ? 1.4f : 2.25f))
                return e;
            if (OakChamsNear3(px, py, pz, t.hx, t.hy, t.hz, r * 0.9f, -0.55f, 0.90f))
                return e;
        }
    }
    return -1;
}

static int OakChamsCbMatchTarget(const float* f, UINT bytes)
{
    if (g_ChamN <= 0 || !f || bytes < 48) return -1;
    int nFloats = (int)(bytes / 4);
    if (nFloats > kOakChamCbFloats) nFloats = kOakChamCbFloats;

    auto tryAt = [&](int ix, int iy, int iz) -> int {
        if (iz >= nFloats) return -1;
        float x = f[ix], y = f[iy], z = f[iz];
        int w = OakChamsMatchTargetIndex(x, y, z, false);
        if (w >= 0) return w;
        return OakChamsMatchTargetIndex(x, y, z, true);
    };

    // Strict matrix translation slots only — scanning every float3 paints dirt/fences.
    int hit = -1;
    if (nFloats >= 16) { hit = tryAt(12, 13, 14); if (hit >= 0) return hit; }
    if (nFloats >= 12) { hit = tryAt(3, 7, 11); if (hit >= 0) return hit; }
    if (nFloats >= 12) { hit = tryAt(9, 10, 11); if (hit >= 0) return hit; }
    if (nFloats >= 32) { hit = tryAt(28, 29, 30); if (hit >= 0) return hit; }
    for (int i = 0; i + 14 < nFloats; i += 16)
    {
        hit = tryAt(i + 12, i + 13, i + 14);
        if (hit >= 0) return hit;
        hit = tryAt(i + 3, i + 7, i + 11);
        if (hit >= 0) return hit;
    }
    return -1;
}

static int OakChamsBoundCbHits(ID3D11DeviceContext* ctx)
{
    if (!ctx) return -1;
    ID3D11Buffer* vsb[8] = {};
    ctx->VSGetConstantBuffers(0, 8, vsb);
    int hit = -1;
    const DWORD now = GetTickCount();
    for (int i = 0; i < 8; i++)
    {
        if (!vsb[i]) continue;
        if (hit < 0)
        {
            for (int j = 0; j < kOakChamCbSlots; j++)
            {
                if (g_ChamCb[j].res != (ID3D11Resource*)vsb[i]) continue;
                if (g_ChamCb[j].bytes < 48) continue;
                if (now - g_ChamCb[j].tick > 250) continue;
                hit = OakChamsCbMatchTarget(g_ChamCb[j].data, g_ChamCb[j].bytes);
                if (hit >= 0) break;
            }
        }
        vsb[i]->Release();
    }
    return hit;
}

static void OakChamsStickyRemember(ID3D11Buffer* vb, int targetIdx)
{
    if (!vb || targetIdx < 0 || targetIdx >= g_ChamN) return;
    const OakChamTarget& t = g_ChamTgt[targetIdx];
    DWORD now = GetTickCount();
    for (int i = 0; i < g_ChamStickyN; i++)
    {
        if (g_ChamSticky[i].vb == vb)
        {
            g_ChamSticky[i].x = t.x;
            g_ChamSticky[i].y = t.y;
            g_ChamSticky[i].z = t.z;
            g_ChamSticky[i].rXZ = t.rXZ;
            g_ChamSticky[i].hands = t.hands;
            g_ChamSticky[i].item = t.item;
            g_ChamSticky[i].lastHit = now;
            return;
        }
    }
    if (g_ChamStickyN >= kOakChamSticky)
    {
        int oldest = 0;
        for (int i = 1; i < g_ChamStickyN; i++)
            if (g_ChamSticky[i].lastHit < g_ChamSticky[oldest].lastHit) oldest = i;
        g_ChamSticky[oldest] = { vb, t.x, t.y, t.z, t.rXZ, t.hands, t.item, now };
        return;
    }
    g_ChamSticky[g_ChamStickyN++] = { vb, t.x, t.y, t.z, t.rXZ, t.hands, t.item, now };
}

static bool OakChamsStickyStillValid(const OakChamSticky& s)
{
    DWORD now = GetTickCount();
    if (now - s.lastHit > 12000) return false;
    // Sticky mesh must still belong to a live tracked target near the learned origin.
    for (int e = 0; e < g_ChamN; e++)
    {
        const OakChamTarget& t = g_ChamTgt[e];
        if (t.hands != s.hands || t.item != s.item) continue;
        float r = (s.rXZ > 0.1f ? s.rXZ : 1.15f) + (s.hands ? 0.35f : (s.item ? 0.90f : 2.50f));
        if (OakChamsNear3(t.x, t.y, t.z, s.x, s.y, s.z, r, -1.8f, 3.2f))
            return true;
        if (OakChamsNear3(t.hx, t.hy, t.hz, s.x, s.y, s.z, r, -1.8f, 3.2f))
            return true;
    }
    return false;
}

static bool OakChamsStickyHit(ID3D11Buffer* vb)
{
    if (!vb || g_ChamStickyN <= 0 || g_ChamN <= 0) return false;
    for (int i = 0; i < g_ChamStickyN; i++)
    {
        if (g_ChamSticky[i].vb != vb) continue;
        if (!OakChamsStickyStillValid(g_ChamSticky[i]))
            continue;
        g_ChamSticky[i].lastHit = GetTickCount();
        return true;
    }
    return false;
}

static void OakChamsPushColor(ID3D11DeviceContext* ctx)
{
    if (!ctx || !g_ChamsCB) return;
    float col[4];
    OakShadowChamsFillRgb(&col[0], &col[1], &col[2], &col[3]);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(g_ChamsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
        return;
    memcpy(mapped.pData, col, 16);
    ctx->Unmap(g_ChamsCB, 0);
}

static void OakChamsEmit(ID3D11DeviceContext* ctx, void (*drawFn)(void*), void* drawArg)
{
    if (!ctx || !g_ChamsPS || !drawFn) return;

    ID3D11PixelShader* oldPS = nullptr;
    ID3D11ClassInstance* inst[8] = {};
    UINT nInst = 8;
    ctx->PSGetShader(&oldPS, inst, &nInst);

    ID3D11Buffer* oldCb = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &oldCb);

    ID3D11DepthStencilState* oldDss = nullptr;
    UINT stencil = 0;
    ctx->OMGetDepthStencilState(&oldDss, &stencil);

    ID3D11RasterizerState* oldRs = nullptr;
    ctx->RSGetState(&oldRs);

    ID3D11BlendState* oldBlend = nullptr;
    FLOAT blendFactor[4] = { 1, 1, 1, 1 };
    UINT sampleMask = 0xffffffff;
    ctx->OMGetBlendState(&oldBlend, blendFactor, &sampleMask);

    OakChamsPushColor(ctx);
    ctx->PSSetShader(g_ChamsPS, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_ChamsCB);
    ctx->OMSetDepthStencilState(g_ChamsDSS, 0);
    ctx->RSSetState(OakChamsWireframeOn() && g_WireframeRS ? g_WireframeRS : g_ChamsRS);
    ctx->OMSetBlendState(g_ChamsBlend, blendFactor, sampleMask);

    drawFn(drawArg);

    ctx->PSSetShader(oldPS, inst, nInst);
    ctx->PSSetConstantBuffers(0, 1, &oldCb);
    ctx->OMSetDepthStencilState(oldDss, stencil);
    ctx->RSSetState(oldRs);
    ctx->OMSetBlendState(oldBlend, blendFactor, sampleMask);

    if (oldPS) oldPS->Release();
    for (UINT i = 0; i < nInst; i++)
        if (inst[i]) inst[i]->Release();
    if (oldCb) oldCb->Release();
    if (oldDss) oldDss->Release();
    if (oldRs) oldRs->Release();
    if (oldBlend) oldBlend->Release();
}

struct OakChamsDrawIndexedArgs
{
    ID3D11DeviceContext* ctx;
    UINT indexCount;
    UINT startIndex;
    INT baseVertex;
};

static void OakChamsCallDrawIndexed(void* p)
{
    auto* a = (OakChamsDrawIndexedArgs*)p;
    oChamsDrawIndexed(a->ctx, a->indexCount, a->startIndex, a->baseVertex);
}

struct OakChamsDrawIndexedInstancedArgs
{
    ID3D11DeviceContext* ctx;
    UINT indexCountPerInstance;
    UINT instanceCount;
    UINT startIndex;
    INT baseVertex;
    UINT startInstance;
};

static void OakChamsCallDrawIndexedInstanced(void* p)
{
    auto* a = (OakChamsDrawIndexedInstancedArgs*)p;
    oChamsDrawIndexedInstanced(a->ctx, a->indexCountPerInstance, a->instanceCount,
        a->startIndex, a->baseVertex, a->startInstance);
}

static void OakChamsWithWireframe(ID3D11DeviceContext* ctx, void (*drawFn)(void*), void* drawArg)
{
    if (!ctx || !drawFn) return;
    if (!OakChamsWireframeOn() || !g_WireframeRS)
    {
        drawFn(drawArg);
        return;
    }
    ID3D11RasterizerState* oldRs = nullptr;
    ctx->RSGetState(&oldRs);
    ctx->RSSetState(g_WireframeRS);
    drawFn(drawArg);
    ctx->RSSetState(oldRs);
    if (oldRs) oldRs->Release();
}

static bool OakChamsShouldRetint(ID3D11DeviceContext* ctx, UINT indexCount)
{
    if (!ctx || t_ChamsBusy || g_ShuttingDown) return false;
    if (!OakShadowChamsIsEnabled() || !g_ChamsPS) return false;
    if (g_ChamN <= 0) return false;
    // Characters / gear / small items — never terrain slabs or huge world chunks.
    if (indexCount < 120 || indexCount > 22000) return false;

    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topo);
    if (topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
        topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
        return false;

    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);

    bool ok = false;
    if (vb && stride >= 32 && stride <= 72)
    {
        g_ChamsLooked++;
        int tgt = OakChamsBoundCbHits(ctx);
        if (tgt >= 0)
        {
            OakChamsStickyRemember(vb, tgt);
            ok = true;
        }
        else if (OakChamsStickyHit(vb))
        {
            // Only if a live player/zombie/item/hands target is still near the learned origin.
            g_ChamsStickyHits++;
            ok = true;
        }
    }
    if (vb) vb->Release();
    return ok;
}

static void STDMETHODCALLTYPE hkChamsDrawIndexed(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex)
{
    InterlockedIncrement(&g_ChamsCalls);
    if (!oChamsDrawIndexed) return;
    OakChamsDrawIndexedArgs args{ ctx, indexCount, startIndex, baseVertex };
    if (OakChamsShouldRetint(ctx, indexCount))
    {
        t_ChamsBusy++;
        OakChamsEmit(ctx, OakChamsCallDrawIndexed, &args);
        g_ChamsHits++;
        t_ChamsBusy--;
        return;
    }
    OakChamsWithWireframe(ctx, OakChamsCallDrawIndexed, &args);
}

static void STDMETHODCALLTYPE hkChamsDraw(ID3D11DeviceContext* ctx, UINT vertexCount, UINT startVertex)
{
    if (!oChamsDraw) return;
    struct Args { ID3D11DeviceContext* ctx; UINT vc; UINT sv; } args{ ctx, vertexCount, startVertex };
    auto call = [](void* p) {
        auto* a = (Args*)p;
        oChamsDraw(a->ctx, a->vc, a->sv);
    };
    OakChamsWithWireframe(ctx, call, &args);
}

static void STDMETHODCALLTYPE hkChamsDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT indexCountPerInstance,
    UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance)
{
    if (!oChamsDrawIndexedInstanced) return;
    OakChamsDrawIndexedInstancedArgs args{ ctx, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance };
    if (OakChamsShouldRetint(ctx, indexCountPerInstance))
    {
        t_ChamsBusy++;
        OakChamsEmit(ctx, OakChamsCallDrawIndexedInstanced, &args);
        g_ChamsHits++;
        t_ChamsBusy--;
        return;
    }
    OakChamsWithWireframe(ctx, OakChamsCallDrawIndexedInstanced, &args);
}

static HRESULT STDMETHODCALLTYPE hkChamsMap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub,
    D3D11_MAP mapType, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    HRESULT hr = oChamsMap ? oChamsMap(ctx, res, sub, mapType, flags, mapped) : E_FAIL;
    if (FAILED(hr) || t_ChamsBusy || !OakShadowChamsIsEnabled() || !res || !mapped || !mapped->pData || sub != 0)
        return hr;
    if (mapType == D3D11_MAP_READ)
        return hr;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER)
        return hr;
    D3D11_BUFFER_DESC desc{};
    ((ID3D11Buffer*)res)->GetDesc(&desc);
    // Character bone CBs are often large; also accept non-CB typed uploads that VS still binds.
    const bool looksCb = (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
    const bool looksDyn = (desc.Usage == D3D11_USAGE_DYNAMIC) || (desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE);
    if (!looksCb && !looksDyn)
        return hr;
    if (desc.ByteWidth < 64 || desc.ByteWidth > 65536)
        return hr;
    g_ChamMapRes = res;
    g_ChamMapPtr = mapped->pData;
    g_ChamMapBytes = desc.ByteWidth;
    return hr;
}

static void STDMETHODCALLTYPE hkChamsUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub)
{
    if (!t_ChamsBusy && res == g_ChamMapRes && g_ChamMapPtr)
        OakChamsStoreCb(res, g_ChamMapPtr, g_ChamMapBytes);
    g_ChamMapRes = nullptr;
    g_ChamMapPtr = nullptr;
    g_ChamMapBytes = 0;
    if (oChamsUnmap)
        oChamsUnmap(ctx, res, sub);
}

static void STDMETHODCALLTYPE hkChamsUpdateSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dstSub,
    const D3D11_BOX* box, const void* src, UINT rowPitch, UINT depthPitch)
{
    if (!t_ChamsBusy && OakShadowChamsIsEnabled() && dst && src && dstSub == 0 && !box)
    {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        dst->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER)
        {
            D3D11_BUFFER_DESC desc{};
            ((ID3D11Buffer*)dst)->GetDesc(&desc);
            const bool looksCb = (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
            if (looksCb && desc.ByteWidth >= 64 && desc.ByteWidth <= 65536)
                OakChamsStoreCb(dst, src, desc.ByteWidth);
        }
    }
    if (oChamsUpdateSubresource)
        oChamsUpdateSubresource(ctx, dst, dstSub, box, src, rowPitch, depthPitch);
}

static bool OakShadowChams_CreateDeviceObjects(ID3D11Device* dev)
{
    if (!dev) return false;
    if (g_ChamsPS) return true;

    if (FAILED(dev->CreatePixelShader(g_OakChamsPS, sizeof(g_OakChamsPS), nullptr, &g_ChamsPS)) || !g_ChamsPS)
    {
        Log("chams: CreatePixelShader failed");
        return false;
    }

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cb, nullptr, &g_ChamsCB)))
    {
        Log("chams: CreateBuffer failed");
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ds.StencilEnable = FALSE;
    if (FAILED(dev->CreateDepthStencilState(&ds, &g_ChamsDSS)))
        return false;

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    rs.ScissorEnable = FALSE;
    rs.MultisampleEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rs, &g_ChamsRS)))
        return false;

    D3D11_RASTERIZER_DESC wr = rs;
    wr.FillMode = D3D11_FILL_WIREFRAME;
    wr.CullMode = D3D11_CULL_NONE;
    if (FAILED(dev->CreateRasterizerState(&wr, &g_WireframeRS)))
        return false;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &g_ChamsBlend)))
        return false;
    return true;
}

static void OakChamsAttachContext(ID3D11DeviceContext* ctx)
{
    if (!ctx || !g_ChamsOrigVt) return;
    OakChamsSwapVptr(ctx, g_ChamsVtCopy, nullptr);
}

static HRESULT STDMETHODCALLTYPE hkChamsCreateDeferred(ID3D11Device* dev, UINT flags, ID3D11DeviceContext** out)
{
    HRESULT hr = oChamsCreateDeferred ? oChamsCreateDeferred(dev, flags, out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out)
        OakChamsAttachContext(*out);
    return hr;
}

static void OakShadowChams_Install(ID3D11DeviceContext* ctx)
{
    if (g_ChamsHooked || !ctx || g_ShuttingDown) return;
    ID3D11Device* dev = g_Device;
    if (!dev) ctx->GetDevice(&dev);
    if (!OakShadowChams_CreateDeviceObjects(dev))
        return;

    void** vt = *(void***)ctx;
    if (!vt) return;
    memcpy(g_ChamsVtCopy, vt, sizeof(g_ChamsVtCopy));
    oChamsDrawIndexed = (OakDrawIndexedFn)vt[12];
    oChamsDraw = (OakDrawFn)vt[13];
    oChamsMap = (OakMapFn)vt[14];
    oChamsUnmap = (OakUnmapFn)vt[15];
    oChamsDrawIndexedInstanced = (OakDrawIndexedInstancedFn)vt[20];
    oChamsUpdateSubresource = (OakUpdateSubresourceFn)vt[48];
    if (!oChamsDrawIndexed || !oChamsMap || !oChamsUnmap)
        return;
    g_ChamsVtCopy[12] = (void*)hkChamsDrawIndexed;
    if (oChamsDraw)
        g_ChamsVtCopy[13] = (void*)hkChamsDraw;
    g_ChamsVtCopy[14] = (void*)hkChamsMap;
    g_ChamsVtCopy[15] = (void*)hkChamsUnmap;
    if (oChamsDrawIndexedInstanced)
        g_ChamsVtCopy[20] = (void*)hkChamsDrawIndexedInstanced;
    if (oChamsUpdateSubresource)
        g_ChamsVtCopy[48] = (void*)hkChamsUpdateSubresource;
    g_ChamsOrigVt = vt;
    if (!OakChamsSwapVptr(ctx, g_ChamsVtCopy, &g_ChamsOrigVt))
        return;
    g_ChamsCtxObj = ctx;

    if (dev)
    {
        void** dvt = *(void***)dev;
        if (dvt && dvt[27])
        {
            memcpy(g_ChamsDevVtCopy, dvt, sizeof(g_ChamsDevVtCopy));
            oChamsCreateDeferred = (OakCreateDeferredFn)dvt[27];
            g_ChamsDevVtCopy[27] = (void*)hkChamsCreateDeferred;
            if (OakChamsSwapVptr(dev, g_ChamsDevVtCopy, &g_ChamsDevOrigVt))
                g_ChamsDevObj = dev;
        }
    }

    g_ChamsHooked = true;
    Log("chams: mesh context vptr swap on");
}

static void OakShadowChams_Shutdown()
{
    g_ChamN = 0;
    if (g_ChamsCtxObj && g_ChamsOrigVt)
        OakChamsSwapVptr(g_ChamsCtxObj, g_ChamsOrigVt, nullptr);
    if (g_ChamsDevObj && g_ChamsDevOrigVt)
        OakChamsSwapVptr(g_ChamsDevObj, g_ChamsDevOrigVt, nullptr);
    g_ChamsCtxObj = nullptr;
    g_ChamsDevObj = nullptr;
    g_ChamsOrigVt = nullptr;
    g_ChamsDevOrigVt = nullptr;
    g_ChamsHooked = false;
}

static void OakShadowChamsPushTargetEx(const Vec3& pos, float rXZ, bool hands, bool item)
{
    if (g_ChamN >= kOakChamMax) return;
    if (!(pos.x == pos.x && pos.y == pos.y && pos.z == pos.z)) return;
    if (!hands && g_LocalPlayerValid)
    {
        float dx = pos.x - g_LocalPlayerPos.x;
        float dy = pos.y - g_LocalPlayerPos.y;
        float dz = pos.z - g_LocalPlayerPos.z;
        if (dx * dx + dy * dy + dz * dz > (420.f * 420.f)) return;
    }
    OakChamTarget& t = g_ChamTgt[g_ChamN++];
    t.x = pos.x;
    t.y = pos.y;
    t.z = pos.z;
    t.hx = pos.x;
    t.hy = pos.y + (item ? 0.35f : (hands ? 0.05f : 1.62f));
    t.hz = pos.z;
    t.rXZ = rXZ;
    t.hands = hands;
    t.item = item;
}

static void OakShadowChamsPushTarget(const Vec3& pos, float radiusM)
{
    OakShadowChamsPushTargetEx(pos, radiusM, false, false);
}

static void OakShadowChamsPushEntity(uintptr_t entity, bool isPlayer)
{
    Vec3 pos{};
    if (!GetEntityPosition(entity, pos)) return;
    OakShadowChamsPushTargetEx(pos, 1.35f, false, false);
    if (g_ChamN <= 0) return;
    Vec3 head = GetBonePosition(entity, BONE_HEAD, isPlayer);
    if (head.x == head.x && head.y == head.y && head.z == head.z &&
        !(head.x == 0.f && head.y == 0.f && head.z == 0.f))
    {
        OakChamTarget& t = g_ChamTgt[g_ChamN - 1];
        t.hx = head.x;
        t.hy = head.y;
        t.hz = head.z;
    }
    Vec3 pelvis = GetBonePosition(entity, BONE_PELVIS, isPlayer);
    if (pelvis.x == pelvis.x && pelvis.y == pelvis.y && pelvis.z == pelvis.z &&
        !(pelvis.x == 0.f && pelvis.y == 0.f && pelvis.z == 0.f))
    {
        OakChamTarget& t = g_ChamTgt[g_ChamN - 1];
        t.x = pelvis.x;
        t.y = pelvis.y;
        t.z = pelvis.z;
    }
}

static void OakShadowChamsPushLocalHands(uintptr_t localPlayer)
{
    if (!g_ShadowChams.hands || !g_W2S.valid) return;
    // Viewmodel pocket in camera space — hands matcher only accepts view-space CB hits.
    OakShadowChamsPushTargetEx(Vec3{ 0.12f, -0.15f, 0.45f }, 0.65f, true, false);
    OakShadowChamsPushTargetEx(Vec3{ -0.10f, -0.18f, 0.40f }, 0.65f, true, false);
    OakShadowChamsPushTargetEx(Vec3{ 0.00f, -0.20f, 0.55f }, 0.75f, true, false);

    if (IsValidPtr(localPlayer) && localPlayer > 0x100000000)
    {
        // World-space hand/weapon anchors — tight radius so dirt/fences near feet never stick.
        Vec3 rh = GetBonePosition(localPlayer, BONE_R_HAND, true);
        if (rh.x == rh.x && !(rh.x == 0.f && rh.y == 0.f && rh.z == 0.f))
            OakShadowChamsPushTargetEx(rh, 0.40f, false, true);
        Vec3 lh = GetBonePosition(localPlayer, BONE_L_HAND, true);
        if (lh.x == lh.x && !(lh.x == 0.f && lh.y == 0.f && lh.z == 0.f))
            OakShadowChamsPushTargetEx(lh, 0.40f, false, true);
        uintptr_t weapon = GetLocalHandsWeapon(localPlayer);
        Vec3 wp{};
        if (IsValidPtr(weapon) && GetEntityPosition(weapon, wp))
            OakShadowChamsPushTargetEx(wp, 0.50f, false, true);
    }
}

static bool OakShadowChamsLooksVehicle(const char* cfg, const char* tn)
{
    if (StrCmpI(cfg, "car") == 0 || StrCmpI(cfg, "boat") == 0) return true;
    if (StrContainsI(cfg, "truck") || StrContainsI(cfg, "vehicle")) return true;
    if (StrContainsI(tn, "CivilianSedan") || StrContainsI(tn, "Offroad") ||
        StrContainsI(tn, "Hatchback") || StrContainsI(tn, "Truck_") ||
        StrContainsI(tn, "Boat_"))
        return true;
    if (StrContainsI(tn, "wreck") || StrContainsI(tn, "Wreck") || StrContainsI(tn, "Land_"))
        return false;
    return false;
}

static bool OakShadowChamsLooksContainer(const char* cfg, const char* tn)
{
    const char* keys[] = { "barrel", "crate", "chest", "sea_chest", "seachest",
        "woodencrate", "wooden_crate", "ammobox", "firstaidkit", "protectorcase" };
    for (int i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        if (StrContainsI(cfg, keys[i]) || StrContainsI(tn, keys[i]))
            return true;
    return false;
}

static void OakShadowChamsConsider(uintptr_t entity)
{
    if (!IsValidPtr(entity) || entity < 0x100000000) return;
    if (g_ResolvedLocalPlayer && entity == g_ResolvedLocalPlayer) return;

    bool isP = false, isZ = false;
    if (!CombatEntityIsPlayerOrZombie(entity, isP, isZ))
        return;

    g_ShadowChamsConsidered++;
    // Only living players/zombies — no corpses, vehicles, world props.
    if (EntityIsDead(entity))
        return;
    if (isP && OakShadowChamsKindEnabled(OakShadowKindPlayer))
        OakShadowChamsPushEntity(entity, true);
    else if (isZ && OakShadowChamsKindEnabled(OakShadowKindZombie))
        OakShadowChamsPushEntity(entity, false);
}

static void OakShadowChamsConsiderItem(uintptr_t entity)
{
    if (!OakShadowChamsKindEnabled(OakShadowKindItem)) return;
    if (!IsValidPtr(entity) || entity < 0x100000000) return;
    if (g_ResolvedLocalPlayer && entity == g_ResolvedLocalPlayer) return;

    char cfg[48] = {};
    char tn[64] = {};
    ReadEntityConfigName(entity, cfg, 48);
    ReadEntityTypeName(entity, tn, 64);
    if (StrCmpI(cfg, "dayzplayer") == 0 || StrCmpI(cfg, "dayzinfected") == 0)
        return;
    if (OakShadowChamsLooksVehicle(cfg, tn) || OakShadowChamsLooksContainer(cfg, tn))
        return;
    if (StrContainsI(tn, "Land_") || StrContainsI(tn, "Fence") || StrContainsI(tn, "Wall") ||
        StrContainsI(tn, "House") || StrContainsI(tn, "Wreck") || StrContainsI(tn, "Static"))
        return;

    Vec3 pos{};
    if (!GetEntityPosition(entity, pos)) return;
    OakShadowChamsPushTargetEx(pos, 0.55f, false, true);
}

static void OakShadowChamsDrawWorld(uintptr_t worldPtr)
{
    int hits = g_ChamsHits;
    int looked = g_ChamsLooked;
    int stickyHits = g_ChamsStickyHits;
    g_ChamsHits = 0;
    g_ChamsLooked = 0;
    g_ChamsStickyHits = 0;
    g_ShadowChamsDrew = hits;
    g_ShadowChamsConsidered = 0;
    g_ChamN = 0;

    if (!OakShadowChamsIsEnabled())
    {
        g_ChamStickyN = 0;
        return;
    }
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000)
        return;

    const uintptr_t lists[] = {
        oak_offsets::world::NearEntList,
        oak_offsets::world::FarEntList
    };
    for (int li = 0; li < 2; li++)
    {
        uintptr_t data = 0;
        int count = 0;
        if (!ResolveEntityList(worldPtr, lists[li], 2000, data, count, nullptr))
        {
            data = Read<uintptr_t>(worldPtr + lists[li]);
            count = Read<int>(worldPtr + lists[li] + 8);
            if (!IsValidPtr(data) || count <= 0 || count > 2000)
                continue;
        }
        int maxI = count > 180 ? 180 : count;
        for (int i = 0; i < maxI && g_ChamN < kOakChamMax; i++)
            OakShadowChamsConsider(Read<uintptr_t>(data + (uintptr_t)i * 8));
    }

    // Infected/players often sit in SlowEntList ({flag,pad,ent*} @ 0x18), not only Near/Far.
    if (g_ChamN < kOakChamMax)
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::SlowEntList);
        int count = Read<int>(worldPtr + oak_offsets::world::SlowTableSize);
        int valid = Read<int>(worldPtr + oak_offsets::world::SlowEntValidCount);
        if (valid > count && valid < 30000)
            count = valid;
        if (IsValidPtr(data) && data > 0x100000000 && count > 0 && count < 20000)
        {
            int maxI = count > 400 ? 400 : count;
            for (int i = 0; i < maxI && g_ChamN < kOakChamMax; i++)
            {
                uintptr_t entry = data + (uintptr_t)i * 0x18;
                WORD flag = Read<WORD>(entry);
                if (flag == 0) continue;
                OakShadowChamsConsider(Read<uintptr_t>(entry + 8));
            }
        }
    }

    if (g_ChamN < kOakChamMax && OakShadowChamsKindEnabled(OakShadowKindItem))
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::ItemList);
        int count = Read<int>(worldPtr + oak_offsets::world::ItemListSize);
        if (IsValidPtr(data) && data > 0x100000000 && count > 0 && count < 200000)
        {
            // Keep headroom for players/zombies/hands — items must not fill the whole table.
            const int itemBudget = 18;
            int itemAdded = 0;
            int maxI = count > 260 ? 260 : count;
            for (int i = 0; i < maxI && g_ChamN < kOakChamMax && itemAdded < itemBudget; i++)
            {
                uintptr_t entry = data + (uintptr_t)i * 0x18;
                if (Read<WORD>(entry) == 0) continue;
                int before = g_ChamN;
                OakShadowChamsConsiderItem(Read<uintptr_t>(entry + 8));
                if (g_ChamN > before) itemAdded++;
            }
        }
    }

    OakShadowChamsPushLocalHands(g_ResolvedLocalPlayer);

    static DWORD s_Log = 0;
    DWORD now = GetTickCount();
    if (!s_Log || (now - s_Log) > 3000)
    {
        s_Log = now;
        int calls = (int)InterlockedExchange(&g_ChamsCalls, 0);
        char b[192];
        wsprintfA(b, "chams: mesh hooked=%d targets=%d hits=%d sticky=%d looked=%d calls=%d stickyN=%d wf=%d",
            g_ChamsHooked ? 1 : 0, g_ChamN, hits, stickyHits, looked, calls, g_ChamStickyN,
            OakChamsWireframeOn() ? 1 : 0);
        Log(b);
    }
}
