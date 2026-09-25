// Oak awareness / HUD features — included from main.cpp after misc_impl.inl

static bool CombatIsFriendEntity(uintptr_t entity);

// Threat ring scan buffer (filled during ESP entity pass)
enum { kMaxThreatEntries = 64 };
struct OakThreatEntry {
    float dirX, dirZ;
    int distM;
    bool isPlayer;
    bool isFriend;
};
static OakThreatEntry g_ThreatEntries[kMaxThreatEntries];
static int g_ThreatEntryCount = 0;
static int g_ThreatCountPlayers = 0;
static int g_ThreatCountZombies = 0;

// Player trails
struct OakTrailPoint { Vec3 pos; DWORD tick; };
struct OakPlayerTrail {
    uintptr_t entity;
    OakTrailPoint pts[64];
    int count;
    DWORD lastSeen;
};
static OakPlayerTrail g_PlayerTrails[32];
static int g_PlayerTrailSlots = 32;

// Death marker latch
static bool g_DeathLatched = false;
static bool g_WasAlive = true;
static int g_DeathWpSlot = -1;

// Reload tracking
static uintptr_t g_ReloadWeapon = 0;
static DWORD g_ReloadStart = 0;
static float g_ReloadDurationMs = 2800.f;
static bool g_ReloadActive = false;

static void OakFeaturesResetThreatScan()
{
    g_ThreatEntryCount = 0;
    g_ThreatCountPlayers = 0;
    g_ThreatCountZombies = 0;
}

static bool OakGameMenuLikelyOpen()
{
    if (ImGuiMenu_IsOpen()) return true;
    // Tab / inventory / map often show cursor — coarse heuristic
    if (GetAsyncKeyState(VK_TAB) & 0x8000) return true;
    return false;
}

static void OakGetCameraBasisXZ(float& rx, float& rz, float& fx, float& fz)
{
    rx = 1.f; rz = 0.f; fx = 0.f; fz = 1.f;
    uintptr_t world = g_CachedWorldPtr;
    if (!IsValidPtr(world)) return;
    uintptr_t cam = Read<uintptr_t>(world + offsets::world::Camera);
    if (!IsValidPtr(cam)) return;
    Vec3 right = Read<Vec3>(cam + offsets::camera::InvertedViewRight);
    Vec3 fwd = Read<Vec3>(cam + offsets::camera::InvertedViewForward);
    rx = right.x; rz = right.z;
    fx = fwd.x; fz = fwd.z;
    float rLen = sqrtf(rx * rx + rz * rz);
    float fLen = sqrtf(fx * fx + fz * fz);
    if (rLen > 0.01f) { rx /= rLen; rz /= rLen; }
    else { rx = 1.f; rz = 0.f; }
    if (fLen > 0.01f) { fx /= fLen; fz /= fLen; }
    else { fx = 0.f; fz = 1.f; }
}

static float OakCameraYawDeg()
{
    float rx, rz, fx, fz;
    OakGetCameraBasisXZ(rx, rz, fx, fz);
    float yaw = atan2f(fx, fz) * (180.f / 3.14159265f);
    if (yaw < 0.f) yaw += 360.f;
    return yaw;
}

static const char* OakHeadingLabel(float yawDeg)
{
    static const char* dirs[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    int idx = (int)((yawDeg + 22.5f) / 45.f) & 7;
    return dirs[idx];
}

static void OakPosToGridLabel(float x, float z, char* out, int outMax)
{
    if (!out || outMax < 8) return;
    int gx = (int)(x / 100.f);
    int gz = (int)(z / 100.f);
    if (gx < 0) gx = 0;
    if (gz < 0) gz = 0;
    wsprintfA(out, "%03d %03d", gx, gz);
}

// Full look vector from VisualState rotation (includes pitch) — used for head gaze lines.
static bool OakGetEntityLookDir(uintptr_t entity, Vec3& outFwd)
{
    outFwd = {};
    if (!IsValidPtr(entity))
        return false;
    uintptr_t vs = Read<uintptr_t>(entity + offsets::entity::VisualState);
    if (!IsValidPtr(vs) || vs < 0x100000000)
        vs = Read<uintptr_t>(entity + offsets::entity::FutureVisualState);
    if (!IsValidPtr(vs) || vs < 0x100000000)
        return false;

    auto tryFwd = [&](uintptr_t base) -> bool {
        Vec3 f;
        f.x = Read<float>(base + 0);
        f.y = Read<float>(base + 4);
        f.z = Read<float>(base + 8);
        float len = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
        if (len < 0.15f || len > 1.75f)
            return false;
        outFwd.x = f.x / len;
        outFwd.y = f.y / len;
        outFwd.z = f.z / len;
        return true;
    };

    // VS 3x3 at +0x8 — row 2 (forward) is floats [6..8] → +0x20.
    if (tryFwd(vs + 0x20))
        return true;
    // Fallback: some builds expose forward as column / alternate row.
    if (tryFwd(vs + 0x8 + 2 * 4))
        return true;

    // Last resort: flatten from old XZ-only reads.
    float fx = Read<float>(vs + 0x8 + 2 * 4);
    float fz = Read<float>(vs + 0x8 + 2 * 4 + 8);
    float len = sqrtf(fx * fx + fz * fz);
    if (len < 0.01f)
        return false;
    outFwd.x = fx / len;
    outFwd.y = 0.f;
    outFwd.z = fz / len;
    return true;
}

static bool OakGetEntityForwardXZ(uintptr_t entity, float& fx, float& fz)
{
    Vec3 f = {};
    if (!OakGetEntityLookDir(entity, f))
        return false;
    float len = sqrtf(f.x * f.x + f.z * f.z);
    if (len < 0.01f)
        return false;
    fx = f.x / len;
    fz = f.z / len;
    return true;
}

// 0=stand 1=crouch 2=prone — bone height heuristic
static int OakInferStance(uintptr_t entity)
{
    Vec3 pelvis = GetBonePosition(entity, BONE_PELVIS, true);
    Vec3 head = GetBonePosition(entity, BONE_HEAD, true);
    if (head.y == 0.f && pelvis.y == 0.f) return 0;
    float h = head.y - pelvis.y;
    if (h < 0.35f) return 2;
    if (h < 0.55f) return 1;
    return 0;
}

static const char* OakStanceLetter(int stance)
{
    switch (stance)
    {
    case 1: return "C";
    case 2: return "P";
    default: return "S";
    }
}

static bool OakIsHeliCrashName(const char* tn, const char* cfg)
{
    const char* s = (tn && tn[0]) ? tn : cfg;
    if (!s || !s[0]) return false;
    if (StrContainsI(s, "HelicopterWreck") || StrContainsI(s, "Wreck_Mi8") ||
        StrContainsI(s, "Wreck_UH1Y") || StrContainsI(s, "CrashSite") ||
        StrContainsI(s, "StaticObj_Wreck") || StrContainsI(s, "Land_Wreck"))
        return true;
    if (StrContainsI(s, "wreck") && (StrContainsI(s, "Heli") || StrContainsI(s, "Mi8") ||
        StrContainsI(s, "UH1") || StrContainsI(s, "Crash")))
        return true;
    return false;
}

static bool OakIsNightTime(uintptr_t worldPtr)
{
    if (!IsValidPtr(worldPtr)) return false;
    float hour = 0.f;
    __try { hour = Read<float>(worldPtr + oak_offsets::world::Hour); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    // Night window ~19:30 — 05:30
    return (hour >= 19.5f || hour <= 5.5f);
}

static OakPlayerTrail* OakFindTrail(uintptr_t ent)
{
    for (int i = 0; i < g_PlayerTrailSlots; i++)
        if (g_PlayerTrails[i].entity == ent) return &g_PlayerTrails[i];
    return nullptr;
}

static OakPlayerTrail* OakAllocTrail(uintptr_t ent)
{
    DWORD now = GetTickCount();
    OakPlayerTrail* oldest = &g_PlayerTrails[0];
    for (int i = 0; i < g_PlayerTrailSlots; i++)
    {
        if (g_PlayerTrails[i].entity == 0) return &g_PlayerTrails[i];
        if (g_PlayerTrails[i].lastSeen < oldest->lastSeen) oldest = &g_PlayerTrails[i];
    }
    oldest->entity = ent;
    oldest->count = 0;
    oldest->lastSeen = now;
    return oldest;
}

static void OakTrailPush(OakPlayerTrail* t, const Vec3& pos)
{
    if (!t) return;
    DWORD now = GetTickCount();
    DWORD interval = 250;
    if (t->count > 0)
    {
        OakTrailPoint& last = t->pts[t->count - 1];
        if ((now - last.tick) < interval) return;
        float dx = pos.x - last.pos.x, dz = pos.z - last.pos.z;
        if (dx * dx + dz * dz < 0.25f) return;
    }
    int maxPts = g_PlayerTrail.maxPoints;
    if (maxPts < 4) maxPts = 4;
    if (maxPts > 64) maxPts = 64;
    if (t->count >= maxPts)
    {
        for (int i = 1; i < t->count; i++)
            t->pts[i - 1] = t->pts[i];
        t->count--;
    }
    t->pts[t->count].pos = pos;
    t->pts[t->count].tick = now;
    t->count++;
    t->lastSeen = now;
}

static void OakFeaturesOnEntitySeen(uintptr_t entity, const Vec3& pos, bool isPlayer, bool isZombie, int distM)
{
    if (g_PanicHidden) return;
    if (!g_ThreatRing.enabled && !g_ThreatCounter.enabled && !g_PlayerTrail.enabled)
        return;

    // Threat ring + counter
    int threatMax = g_ThreatRing.maxDistanceM > 0 ? g_ThreatRing.maxDistanceM : 80;
    int counterMax = g_ThreatCounter.maxDistanceM > 0 ? g_ThreatCounter.maxDistanceM : threatMax;
    bool friendEnt = isPlayer && CombatIsFriendEntity(entity);

    if (g_ThreatRing.enabled && distM >= 0 && distM <= threatMax)
    {
        bool show = false;
        if (isPlayer && g_ThreatRing.showPlayers && !g_ThreatRing.zombiesOnly) show = true;
        if (isZombie && g_ThreatRing.showZombies) show = true;
        if (show && g_ThreatEntryCount < kMaxThreatEntries)
        {
            Vec3 ref = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
            float dx = pos.x - ref.x;
            float dz = pos.z - ref.z;
            float len = sqrtf(dx * dx + dz * dz);
            if (len > 0.05f)
            {
                OakThreatEntry& e = g_ThreatEntries[g_ThreatEntryCount++];
                e.dirX = dx / len;
                e.dirZ = dz / len;
                e.distM = distM;
                e.isPlayer = isPlayer;
                e.isFriend = friendEnt;
            }
        }
    }

    if (g_ThreatCounter.enabled && distM >= 0 && distM <= counterMax)
    {
        if (isPlayer && g_ThreatRing.showPlayers && !g_ThreatRing.zombiesOnly && !friendEnt)
            g_ThreatCountPlayers++;
        if (isZombie && g_ThreatRing.showZombies)
            g_ThreatCountZombies++;
    }

    // Player trail
    if (g_PlayerTrail.enabled && isPlayer && !friendEnt && distM >= 0 && distM <= g_PlayerDistance)
    {
        int active = 0;
        for (int i = 0; i < g_PlayerTrailSlots; i++)
            if (g_PlayerTrails[i].entity) active++;
        OakPlayerTrail* t = OakFindTrail(entity);
        if (!t && active < g_PlayerTrail.maxPlayers)
            t = OakAllocTrail(entity);
        if (t) OakTrailPush(t, pos);
    }
}

static void OakDrawThreatRing()
{
    if (!g_ThreatRing.enabled || g_PanicHidden) return;
    if (g_ThreatRing.hideInMenu && OakGameMenuLikelyOpen()) return;

    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl || g_ThreatEntryCount <= 0) return;

    float cx = g_ScreenWidth * 0.5f;
    float cy = g_ScreenHeight * 0.5f;
    float radius = g_ThreatRing.ringRadiusPx > 20.f ? g_ThreatRing.ringRadiusPx : 120.f;

    float rx, rz, fx, fz;
    OakGetCameraBasisXZ(rx, rz, fx, fz);

    ImU32 ringCol = IM_COL32(180, 190, 200, 50);
    dl->AddCircle(ImVec2(cx, cy), radius, ringCol, 64, 1.2f);

    for (int i = 0; i < g_ThreatEntryCount; i++)
    {
        const OakThreatEntry& te = g_ThreatEntries[i];
        float sx = te.dirX * rx + te.dirZ * rz;
        float sy = -(te.dirX * fx + te.dirZ * fz);
        float len = sqrtf(sx * sx + sy * sy);
        if (len < 0.01f) continue;
        sx /= len; sy /= len;

        const float* col = te.isPlayer ? g_ThreatRing.colorPlayer : g_ThreatRing.colorZombie;
        if (te.isFriend) col = g_ThreatRing.colorFriend;
        float alpha = col[3];
        if (te.isFriend) alpha *= 0.65f;

        float px = cx + sx * radius;
        float py = cy + sy * radius;
        float ax = -sy, ay = sx;

        float crx, crz, cfx, cfz;
        OakGetCameraBasisXZ(crx, crz, cfx, cfz);
        float worldDot = te.dirX * cfx + te.dirZ * cfz;
        bool behind = worldDot < -0.05f;

        float tipX, tipY, tailX, tailY;
        if (behind)
        {
            tipX = px + sx * 14.f; tipY = py + sy * 14.f;
            tailX = px - sx * 5.f; tailY = py - sy * 5.f;
        }
        else
        {
            tipX = px + sx * 10.f; tipY = py + sy * 10.f;
            tailX = px - sx * 4.f; tailY = py - sy * 4.f;
        }

        ImU32 c = ToCol(col[0], col[1], col[2], alpha);
        dl->AddTriangleFilled(
            ImVec2(tipX, tipY),
            ImVec2(tailX + ax * 7.f, tailY + ay * 7.f),
            ImVec2(tailX - ax * 7.f, tailY - ay * 7.f),
            c);

        if (g_ThreatRing.showDistLabels && te.distM > 0)
        {
            char dbuf[16];
            wsprintfA(dbuf, "%dm", te.distM);
            dl->AddText(ImVec2(px + 8.f, py - 6.f), c, dbuf);
        }
    }
    g_EspDrawCount++;
}

static void OakDrawThreatCounter()
{
    if (!g_ThreatCounter.enabled || g_PanicHidden) return;
    if (g_ThreatRing.hideInMenu && OakGameMenuLikelyOpen()) return;
    if (g_ThreatCounter.hideWhenZero && g_ThreatCountPlayers == 0 && g_ThreatCountZombies == 0)
        return;

    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    char buf[64];
    if (g_ThreatCountPlayers > 0 && g_ThreatCountZombies > 0)
        wsprintfA(buf, "%d players · %d infected", g_ThreatCountPlayers, g_ThreatCountZombies);
    else if (g_ThreatCountPlayers > 0)
        wsprintfA(buf, "%d players", g_ThreatCountPlayers);
    else
        wsprintfA(buf, "%d infected", g_ThreatCountZombies);

    float cx = g_ScreenWidth * 0.5f + g_ThreatCounter.offsetX;
    float cy = g_ScreenHeight * 0.5f + g_ThreatCounter.offsetY;
    ImVec2 sz = ImGui::CalcTextSize(buf);
    float x0 = cx - sz.x * 0.5f - 6.f;
    float y0 = cy - sz.y * 0.5f - 3.f;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + sz.x + 12.f, y0 + sz.y + 6.f), IM_COL32(8, 10, 14, 180), 4.f);
    dl->AddText(ImVec2(x0 + 6.f, y0 + 3.f), IM_COL32(235, 240, 255, 240), buf);
    g_EspDrawCount++;
}

static void OakDrawCompassStrip()
{
    if (!g_Compass.enabled || g_PanicHidden) return;
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    float yaw = OakCameraYawDeg();
    char line[96];
    if (g_Compass.showDegrees)
        wsprintfA(line, "%s  %.0f°", OakHeadingLabel(yaw), yaw);
    else
        wsprintfA(line, "%s", OakHeadingLabel(yaw));

    if (g_Compass.showWaypointBearing && g_WaypointCount > 0)
    {
        int wi = g_WaypointHud.activeIndex;
        if (wi < 0) wi = 0;
        if (wi >= g_WaypointCount) wi = g_WaypointCount - 1;
        Vec3 ref = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
        Vec3 wp{ g_WaypointX[wi], g_WaypointY[wi], g_WaypointZ[wi] };
        float dx = wp.x - ref.x, dz = wp.z - ref.z;
        float bearing = atan2f(dx, dz) * (180.f / 3.14159265f);
        if (bearing < 0.f) bearing += 360.f;
        float rel = bearing - yaw;
        while (rel > 180.f) rel -= 360.f;
        while (rel < -180.f) rel += 360.f;
        char extra[48];
        wsprintfA(extra, "  · WP %+.0f°", rel);
        lstrcatA(line, extra);
    }

    float y = (g_Compass.position == OakCompassBottom) ? (g_ScreenHeight - 32.f) : 36.f;
    float cx = g_ScreenWidth * 0.5f;
    ImVec2 sz = ImGui::CalcTextSize(line);
    dl->AddRectFilled(ImVec2(cx - sz.x * 0.5f - 10.f, y - 4.f),
        ImVec2(cx + sz.x * 0.5f + 10.f, y + sz.y + 4.f), IM_COL32(8, 10, 14, 160), 4.f);
    dl->AddText(ImVec2(cx - sz.x * 0.5f, y), IM_COL32(200, 220, 255, 235), line);
    g_EspDrawCount++;
}

static void OakDrawHudCornerText(int corner, const char* line, float padX, float padY)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl || !line || !line[0]) return;
    ImVec2 sz = ImGui::CalcTextSize(line);
    float x = padX, y = padY;
    switch (corner)
    {
    case OakHudTopRight:
        x = g_ScreenWidth - sz.x - padX - 12.f;
        y = padY;
        break;
    case OakHudBottomLeft:
        x = padX;
        y = g_ScreenHeight - sz.y - padY - 8.f;
        break;
    case OakHudBottomRight:
        x = g_ScreenWidth - sz.x - padX - 12.f;
        y = g_ScreenHeight - sz.y - padY - 8.f;
        break;
    default:
        x = padX;
        y = padY;
        break;
    }
    dl->AddRectFilled(ImVec2(x - 6.f, y - 3.f), ImVec2(x + sz.x + 6.f, y + sz.y + 3.f), IM_COL32(8, 10, 14, 150), 3.f);
    dl->AddText(ImVec2(x, y), IM_COL32(210, 225, 245, 240), line);
    g_EspDrawCount++;
}

static void OakDrawGridCoordsHud()
{
    if (!g_GridCoordsHud.enabled || g_PanicHidden) return;
    Vec3 ref = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
    char grid[16];
    OakPosToGridLabel(ref.x, ref.z, grid, 16);
    char line[48];
    wsprintfA(line, "Grid %s", grid);
    float padY = 36.f;
    if (g_GridCoordsHud.corner == OakHudBottomLeft || g_GridCoordsHud.corner == OakHudBottomRight)
        padY = 28.f;
    OakDrawHudCornerText(g_GridCoordsHud.corner, line, 14.f, padY);
}

static void OakDrawWaypointDistanceHud()
{
    if (!g_WaypointHud.enabled || g_PanicHidden || g_WaypointCount <= 0) return;
    int wi = g_WaypointHud.activeIndex;
    if (wi < 0) wi = 0;
    if (wi >= g_WaypointCount) wi = g_WaypointCount - 1;
    Vec3 ref = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
    Vec3 wp{ g_WaypointX[wi], g_WaypointY[wi], g_WaypointZ[wi] };
    int dist = (int)Distance3D(ref, wp);
    char line[80];
    if (g_WaypointHud.showBearing)
    {
        float yaw = OakCameraYawDeg();
        float dx = wp.x - ref.x, dz = wp.z - ref.z;
        float bearing = atan2f(dx, dz) * (180.f / 3.14159265f);
        if (bearing < 0.f) bearing += 360.f;
        float rel = bearing - yaw;
        while (rel > 180.f) rel -= 360.f;
        while (rel < -180.f) rel += 360.f;
        const char* nm = g_WaypointName[wi][0] ? g_WaypointName[wi] : "WP";
        wsprintfA(line, "%s  %dm  %+.0f°", nm, dist, rel);
    }
    else
    {
        const char* nm = g_WaypointName[wi][0] ? g_WaypointName[wi] : "WP";
        wsprintfA(line, "%s  %dm", nm, dist);
    }
    float padY = 56.f;
    if (g_WaypointHud.corner == OakHudBottomLeft || g_WaypointHud.corner == OakHudBottomRight)
        padY = 52.f;
    OakDrawHudCornerText(g_WaypointHud.corner, line, 14.f, padY);
}

static void OakDrawPlayerTrails()
{
    if (!g_PlayerTrail.enabled || g_PanicHidden) return;
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    DWORD now = GetTickCount();
    DWORD maxAge = (DWORD)(g_PlayerTrail.durationSec * 1000.f);
    if (maxAge < 1000) maxAge = 1000;

    for (int ti = 0; ti < g_PlayerTrailSlots; ti++)
    {
        OakPlayerTrail& t = g_PlayerTrails[ti];
        if (!t.entity || t.count < 2) continue;
        if ((now - t.lastSeen) > maxAge + 2000)
        {
            t.entity = 0;
            t.count = 0;
            continue;
        }
        for (int pi = 1; pi < t.count; pi++)
        {
            const OakTrailPoint& a = t.pts[pi - 1];
            const OakTrailPoint& b = t.pts[pi];
            if ((now - b.tick) > maxAge) continue;
            Vec2 sa, sb;
            if (!WorldToScreen(a.pos, sa) || !WorldToScreen(b.pos, sb)) continue;
            float life = 1.f - (float)(now - b.tick) / (float)maxAge;
            if (life < 0.f) life = 0.f;
            ImU32 col = ToCol(g_PlayerTrail.color[0], g_PlayerTrail.color[1],
                g_PlayerTrail.color[2], g_PlayerTrail.color[3] * life);
            dl->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), col, 2.f);
        }
    }
    g_EspDrawCount++;
}

static void OakFeaturesDrawLookDirection(uintptr_t entity, int distM, bool isPlayer)
{
    if (!g_LookDir.enabled || g_PanicHidden || distM < 0 || distM > g_LookDir.maxDistanceM)
        return;
    if (isPlayer && !g_LookDir.players)
        return;
    if (!isPlayer && !g_LookDir.zombies)
        return;

    Vec3 fwd = {};
    if (!OakGetEntityLookDir(entity, fwd))
        return;

    Vec3 head = GetBonePosition(entity, BONE_HEAD, isPlayer);
    auto headOk = [&](const Vec3& h) -> bool {
        if (!(h.x == h.x && h.y == h.y && h.z == h.z)) return false;
        if (h.x == 0.f && h.y == 0.f && h.z == 0.f) return false;
        return true;
    };
    if (!headOk(head))
    {
        if (!GetEntityPosition(entity, head))
            return;
        head.y += isPlayer ? 1.65f : 1.55f;
    }

    float len = g_LookDir.lineLengthM;
    if (len < 0.5f) len = 0.5f;
    if (len > 12.f) len = 12.f;
    // Slight LOD: shorter lines when far so the overlay stays readable.
    if (distM > 80)
        len *= 0.75f;
    if (distM > 160)
        len *= 0.75f;

    Vec3 end = head;
    end.x += fwd.x * len;
    end.y += fwd.y * len;
    end.z += fwd.z * len;

    Vec2 s0, s1;
    if (!WorldToScreen(head, s0) || !WorldToScreen(end, s1))
        return;

    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    const float* c = isPlayer ? g_LookDir.color : g_LookDir.colorZombie;
    ImU32 col = ToCol(c[0], c[1], c[2], c[3]);
    ImU32 outline = IM_COL32(0, 0, 0, 200);

    // Gaze line from head → look point.
    dl->AddLine(ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y), outline, 3.2f);
    dl->AddLine(ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y), col, 2.0f);

    // Small tip so direction is obvious at a glance.
    float dx = s1.x - s0.x, dy = s1.y - s0.y;
    float dlen = sqrtf(dx * dx + dy * dy);
    if (dlen > 6.f)
    {
        float ux = dx / dlen, uy = dy / dlen;
        float px = -uy, py = ux;
        float tip = 5.f;
        float wing = 3.5f;
        ImVec2 a(s1.x - ux * tip + px * wing, s1.y - uy * tip + py * wing);
        ImVec2 b(s1.x - ux * tip - px * wing, s1.y - uy * tip - py * wing);
        dl->AddTriangleFilled(ImVec2(s1.x, s1.y), a, b, col);
        dl->AddCircleFilled(ImVec2(s0.x, s0.y), 2.2f, col, 8);
    }
    g_EspDrawCount++;
}

static void OakDrawStanceIcon(uintptr_t entity, float headCx, float textY, int distM)
{
    if (!g_StanceIcon.enabled || g_PanicHidden || distM < 0 || distM > g_StanceIcon.maxDistanceM) return;
    int stance = OakInferStance(entity);
    const char* letter = OakStanceLetter(stance);
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;
    float x = headCx - 18.f;
    float y = textY - 2.f;
    dl->AddCircleFilled(ImVec2(x, y + 6.f), 7.f, IM_COL32(12, 14, 18, 200), 12);
    dl->AddText(ImVec2(x - 3.f, y), IM_COL32(220, 230, 255, 240), letter);
    g_EspDrawCount++;
}

static bool OakProbeReloadState(uintptr_t hands, bool* outReloading, float* outProgress)
{
    if (!outReloading || !outProgress) return false;
    *outReloading = false;
    *outProgress = 0.f;
    if (!IsValidPtr(hands)) return false;
    __try
    {
        const uintptr_t offs[] = { 0x6C8, 0x6D0, 0x6D8, 0x5F8, 0x600, 0x608 };
        for (int i = 0; i < 6; i++)
        {
            int v = Read<int>(hands + offs[i]);
            if (v == 1 || v == 2)
            {
                *outReloading = true;
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static void OakUpdateReloadBar(uintptr_t localPlayer)
{
    g_ReloadActive = false;
    if (!g_ReloadBar.enabled || g_PanicHidden || !IsValidPtr(localPlayer)) return;

    uintptr_t hands = GetLocalHandsWeapon(localPlayer);
    DWORD now = GetTickCount();
    bool reloading = false;
    float prog = 0.f;
    OakProbeReloadState(hands, &reloading, &prog);

    int ammo = -1;
    char wpn[64] = {};
    GetHandWeaponInfo(localPlayer, wpn, 64, &ammo);

    static int s_LastAmmo = -1;
    static uintptr_t s_LastHands = 0;

    if (hands != s_LastHands)
    {
        s_LastHands = hands;
        s_LastAmmo = ammo;
        g_ReloadWeapon = 0;
        g_ReloadActive = false;
    }

    if (reloading && hands)
    {
        if (!g_ReloadActive || g_ReloadWeapon != hands)
        {
            g_ReloadWeapon = hands;
            g_ReloadStart = now;
            g_ReloadDurationMs = 2800.f;
            g_ReloadActive = true;
        }
    }
    else if (g_ReloadActive && ammo >= 0 && s_LastAmmo >= 0 && ammo > s_LastAmmo)
    {
        g_ReloadActive = false;
    }
    else if (g_ReloadActive && (now - g_ReloadStart) > (DWORD)(g_ReloadDurationMs + 500.f))
    {
        g_ReloadActive = false;
    }

    if (ammo >= 0) s_LastAmmo = ammo;

    if (!g_ReloadActive) return;

    float elapsed = (float)(now - g_ReloadStart);
    float pct = elapsed / g_ReloadDurationMs;
    if (pct < 0.f) pct = 0.f;
    if (pct > 1.f) pct = 1.f;
    float remain = (g_ReloadDurationMs - elapsed) / 1000.f;
    if (remain < 0.f) remain = 0.f;

    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    float cx = g_ScreenWidth * 0.5f;
    float cy = g_ScreenHeight * 0.5f + g_ReloadBar.offsetY;
    float w = g_ReloadBar.width > 20.f ? g_ReloadBar.width : 120.f;
    float h = g_ReloadBar.height > 1.f ? g_ReloadBar.height : 6.f;
    float x0 = cx - w * 0.5f;

    dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x0 + w, cy + h),
        ToCol(g_ReloadBar.colorBg[0], g_ReloadBar.colorBg[1], g_ReloadBar.colorBg[2], g_ReloadBar.colorBg[3]), 2.f);
    dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x0 + w * pct, cy + h),
        ToCol(g_ReloadBar.colorFill[0], g_ReloadBar.colorFill[1], g_ReloadBar.colorFill[2], g_ReloadBar.colorFill[3]), 2.f);

    char tbuf[24];
    wsprintfA(tbuf, "%.1fs", remain);
    dl->AddText(ImVec2(cx - 12.f, cy + h + 2.f), IM_COL32(220, 235, 245, 230), tbuf);
    g_EspDrawCount++;
}

static void OakTickDeathMarker(uintptr_t localPlayer)
{
    if (!g_DeathMarker.enabled || !IsValidPtr(localPlayer)) return;

    float hp = 100.f;
    bool vitOk = false;
    __try { vitOk = ReadVitals(localPlayer, &hp, nullptr, nullptr, nullptr, nullptr, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { vitOk = false; }

    bool alive = !vitOk || hp > 0.5f;
    if (g_WasAlive && !alive && !g_DeathLatched && g_LocalPlayerValid)
    {
        g_DeathLatched = true;
        SYSTEMTIME st;
        GetLocalTime(&st);
        char label[32];
        wsprintfA(label, "Death %02d:%02d", st.wHour, st.wMinute);
        ImGuiMenu_AddWaypointHere(g_LocalPlayerPos.x, g_LocalPlayerPos.y, g_LocalPlayerPos.z, label);
        g_DeathWpSlot = g_WaypointCount - 1;
        if (g_DeathWpSlot >= 0 && g_DeathWpSlot < OAK_WAYPOINT_MAX)
        {
            g_WaypointColor[g_DeathWpSlot][0] = 1.f;
            g_WaypointColor[g_DeathWpSlot][1] = 0.25f;
            g_WaypointColor[g_DeathWpSlot][2] = 0.25f;
            g_WaypointColor[g_DeathWpSlot][3] = 0.95f;
        }
        Log("features: death marker placed");
    }

    if (!g_WasAlive && alive)
    {
        g_DeathLatched = false;
        if (g_DeathMarker.autoClearOnRespawn && g_DeathWpSlot >= 0)
            g_DeathWpSlot = -1;
    }
    g_WasAlive = alive;
}

static void OakApplyNightBoost(uintptr_t worldPtr, bool fullbrightManual)
{
    if (!IsValidPtr(worldPtr) || fullbrightManual) return;
    if (!g_NightBoost.enabled) return;
    if (!OakIsNightTime(worldPtr)) return;
    int b = g_NightBoost.brightness;
    if (b < 5) b = 5;
    if (b > 100) b = 100;
    WriteEyeAccom(worldPtr, EyeAccomFromBrightness(b));
}

static void OakUpdateCrosshairEnemyFlag(uintptr_t worldPtr, uintptr_t localPlayer)
{
    g_CrosshairOnEnemy = false;
    if (!g_CrosshairHighlight.enabled || g_PanicHidden) return;
    if (!IsValidPtr(worldPtr) || !IsValidPtr(localPlayer)) return;

    float fovPx = (float)(g_AimbotFov > 10 ? g_AimbotFov : 80);
    CombatTarget t = {};
    if (CombatFindBestTarget(worldPtr, localPlayer, t, false, false))
    {
        if (t.valid)
        {
            Vec2 scr;
            if (WorldToScreen(t.bonePos, scr))
            {
                float cx = g_ScreenWidth * 0.5f;
                float cy = g_ScreenHeight * 0.5f;
                float dx = scr.x - cx, dy = scr.y - cy;
                if (sqrtf(dx * dx + dy * dy) <= fovPx * 0.35f)
                    g_CrosshairOnEnemy = true;
            }
        }
    }
}

static void OakFeaturesPreCombatFrame(uintptr_t worldPtr, uintptr_t localPlayer)
{
    OakUpdateCrosshairEnemyFlag(worldPtr, localPlayer);
}

static void OakFeaturesDrawOverlays(uintptr_t worldPtr, uintptr_t localPlayer)
{
    if (g_PanicHidden) return;
    OakDrawThreatRing();
    OakDrawThreatCounter();
    OakDrawCompassStrip();
    OakDrawGridCoordsHud();
    OakDrawWaypointDistanceHud();
    OakDrawPlayerTrails();
    OakUpdateReloadBar(localPlayer);
}

static void OakFeaturesSessionTick(uintptr_t worldPtr, uintptr_t localPlayer)
{
    OakTickDeathMarker(localPlayer);
    OakApplyNightBoost(worldPtr, g_Fullbright);
}

static void OakFeaturesDrawHeliCrash(uintptr_t entity, const char* label, int distance)
{
    if (!g_HeliCrashEsp.enabled || g_PanicHidden) return;
    if (distance < 0 || distance > g_HeliCrashEsp.maxDistanceM) return;
    DrawWorldMarker(entity, label, distance, g_HeliCrashEsp.color, 14.f);
}

static bool OakFeaturesTryHeliCrash(uintptr_t entity, const char* tn, const char* cfg)
{
    if (!g_HeliCrashEsp.enabled || g_PanicHidden) return false;
    if (!OakIsHeliCrashName(tn, cfg)) return false;
    Vec3 wpos;
    int distance = -1;
    if (GetEntityPosition(entity, wpos))
    {
        Vec3 ref = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
        if (g_LocalPlayerValid || g_CameraValid)
            distance = (int)Distance3D(ref, wpos);
    }
    if (distance < 0 || distance > g_HeliCrashEsp.maxDistanceM) return false;
    char label[64];
    ZeroMem(label, 64);
    if (tn && tn[0]) lstrcpynA(label, tn, 64);
    else if (cfg && cfg[0]) lstrcpynA(label, cfg, 64);
    else lstrcpynA(label, "Crash", 64);
    OakFeaturesDrawHeliCrash(entity, label, distance);
    return true;
}
