// ---------------------------------------------------------------------------
// imgui_oak_panel.inl ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â OakPanel UI.
//
// Monochrome tactical layout ported from the WPF mockup (Theme.xaml,
// MainWindow.xaml, Data.cs): a chamfered nav spine plus free-floating
// chamfered module panels, indexed module rows with fold-out setting drawers.
//
// Included from imgui_menu.cpp AFTER MenuKeyName / BindPreference /
// PollBindCapture. Defines ApplyStyle() and DrawMenu().
// ---------------------------------------------------------------------------

// ---- palette (Theme.xaml) -------------------------------------------------

static const ImU32 kOakPanel  = IM_COL32(0x12, 0x12, 0x12, 0xF5);
static const ImU32 kOakBar    = IM_COL32(0x1B, 0x1B, 0x1B, 0xFF);
static const ImU32 kOakHi     = IM_COL32(255, 255, 255, 20);
static const ImU32 kOakWash   = IM_COL32(255, 255, 255, 19);
static const ImU32 kOakLine   = IM_COL32(0x28, 0x28, 0x28, 0xFF);
static const ImU32 kOakLineHi = IM_COL32(0x3A, 0x3A, 0x3A, 0xFF);
static const ImU32 kOakTxt    = IM_COL32(0xE8, 0xE8, 0xE8, 0xFF);
static const ImU32 kOakTxtMid = IM_COL32(0x90, 0x90, 0x90, 0xFF);
static const ImU32 kOakTxtDim = IM_COL32(0x5C, 0x5C, 0x5C, 0xFF);
static const ImU32 kOakAcc    = IM_COL32(255, 255, 255, 255);
static const ImU32 kOakAccDim = IM_COL32(255, 255, 255, 89);
static const ImU32 kOakWarn   = IM_COL32(0xD8, 0xBE, 0x7C, 0xFF);
static const ImU32 kOakBad    = IM_COL32(0xD6, 0x91, 0x85, 0xFF);
static const ImU32 kOakSunken = IM_COL32(0, 0, 0, 89);
static const ImU32 kOakTick   = IM_COL32(0x22, 0x26, 0x1C, 0xFF);
static const ImU32 kOakDead   = IM_COL32(0x38, 0x38, 0x38, 0xFF);

// ---- geometry -------------------------------------------------------------

static const float kOakCut     = 9.0f;  // chamfer corner cut
static const float kOakBarH    = 28.0f; // panel / nav header
static const float kOakRowH    = 23.0f; // module row
static const float kOakBindH   = 30.0f; // keybind row
static const float kOakFontUi  = 12.0f;
static const float kOakFontLbl = 9.5f;  // MicroLabel
static const float kOakFontMon = 8.5f;  // Mono readout
static const float kOakFontTtl = 10.0f; // panel title

enum OakPanelId
{
    kOakVisuals = 0,
    kOakCombat,
    kOakWorld,
    kOakBinds,
    kOakSession,
    kOakPanelCount
};

// Capture ids must match the target table order inside PollBindCapture().
enum OakBindId
{
    kBindEsp = 0, kBindAimbot, kBindMagic, kBindFullbright, kBindDespawn,
    kBindLootMagnet, kBindContainerMagnet, kBindDaytime, kBindOverlays,
    kBindPanic, kBindWaypoint, kBindCoords, kBindSteamNames, kBindPullPart,
    kBindFreecam, kBindSilentAim, kBindWarp
};

static bool  s_PanelShow[kOakPanelCount] = { true, true, true, true, true };
static bool  s_PanelFold[kOakPanelCount] = { false, false, false, false, false };
static int   s_PanelOn[kOakPanelCount]   = { 0, 0, 0, 0, 0 };
static int   s_PanelTotal[kOakPanelCount]= { 0, 0, 0, 0, 0 };
static float s_PanelPosX[kOakPanelCount] = {};
static float s_PanelPosY[kOakPanelCount] = {};
static bool  s_PanelPosSet[kOakPanelCount] = {};
static float s_NavPosX = 0.0f;
static float s_NavPosY = 0.0f;
static bool  s_NavPosSet = false;
static int   s_CurPanel = 0;
static int   s_CountOn = 0;
static int   s_CountTotal = 0;
static char  s_Search[48] = {};
static float s_UiScale = 1.0f;
static int   s_UiScalePct = 100;

static inline float OakS(float v) { return v * s_UiScale; }
static inline float OakMax(float a, float b) { return a > b ? a : b; }
static inline float OakMin(float a, float b) { return a < b ? a : b; }
static inline float OakClamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Default unscaled panel anchors (match OakDraw* call sites).
static void OakUiLayoutDefaults(float* px, float* py, float* nx, float* ny)
{
    static const float kDefX[kOakPanelCount] = { 214.0f, 430.0f, 646.0f, 884.0f, 1108.0f };
    static const float kDefY[kOakPanelCount] = { 26.0f, 26.0f, 26.0f, 26.0f, 26.0f };
    for (int i = 0; i < kOakPanelCount; i++)
    {
        px[i] = kDefX[i];
        py[i] = kDefY[i];
    }
    *nx = 26.0f;
    *ny = 26.0f;
}

static void OakUiLayoutSave(const char* path)
{
    if (!path || !path[0]) return;
    char key[32], buf[48];
    wsprintfA(buf, "%d", s_UiScalePct);
    WritePrivateProfileStringA("ui", "scalePct", buf, path);
    for (int i = 0; i < kOakPanelCount; i++)
    {
        wsprintfA(key, "show%d", i);
        WritePrivateProfileStringA("ui", key, s_PanelShow[i] ? "1" : "0", path);
        wsprintfA(key, "fold%d", i);
        WritePrivateProfileStringA("ui", key, s_PanelFold[i] ? "1" : "0", path);
        float x = s_PanelPosSet[i] ? s_PanelPosX[i] : 0.0f;
        float y = s_PanelPosSet[i] ? s_PanelPosY[i] : 0.0f;
        // Store screen coords as hundredths
        wsprintfA(key, "px%d", i);
        wsprintfA(buf, "%d", (int)(x * 100.0f));
        WritePrivateProfileStringA("ui", key, buf, path);
        wsprintfA(key, "py%d", i);
        wsprintfA(buf, "%d", (int)(y * 100.0f));
        WritePrivateProfileStringA("ui", key, buf, path);
    }
    wsprintfA(buf, "%d", (int)((s_NavPosSet ? s_NavPosX : 0.0f) * 100.0f));
    WritePrivateProfileStringA("ui", "navX", buf, path);
    wsprintfA(buf, "%d", (int)((s_NavPosSet ? s_NavPosY : 0.0f) * 100.0f));
    WritePrivateProfileStringA("ui", "navY", buf, path);
}

static void OakUiLayoutLoad(const char* path)
{
    if (!path || !path[0]) return;
    int pct = GetPrivateProfileIntA("ui", "scalePct", s_UiScalePct, path);
    if (pct < 60) pct = 60;
    if (pct > 200) pct = 200;
    s_UiScalePct = pct;
    s_UiScale = (float)pct / 100.0f;

    float defX[kOakPanelCount], defY[kOakPanelCount], defNx, defNy;
    OakUiLayoutDefaults(defX, defY, &defNx, &defNy);

    for (int i = 0; i < kOakPanelCount; i++)
    {
        char key[32];
        wsprintfA(key, "show%d", i);
        s_PanelShow[i] = GetPrivateProfileIntA("ui", key, s_PanelShow[i] ? 1 : 0, path) != 0;
        wsprintfA(key, "fold%d", i);
        s_PanelFold[i] = GetPrivateProfileIntA("ui", key, s_PanelFold[i] ? 1 : 0, path) != 0;

        wsprintfA(key, "px%d", i);
        int xi = GetPrivateProfileIntA("ui", key, -1, path);
        wsprintfA(key, "py%d", i);
        int yi = GetPrivateProfileIntA("ui", key, -1, path);
        if (xi >= 0 && yi >= 0)
        {
            s_PanelPosX[i] = (float)xi / 100.0f;
            s_PanelPosY[i] = (float)yi / 100.0f;
            s_PanelPosSet[i] = true;
        }
        else
        {
            s_PanelPosX[i] = OakS(defX[i]);
            s_PanelPosY[i] = OakS(defY[i]);
            s_PanelPosSet[i] = true;
        }
    }
    int nx = GetPrivateProfileIntA("ui", "navX", -1, path);
    int ny = GetPrivateProfileIntA("ui", "navY", -1, path);
    if (nx >= 0 && ny >= 0)
    {
        s_NavPosX = (float)nx / 100.0f;
        s_NavPosY = (float)ny / 100.0f;
        s_NavPosSet = true;
    }
    else
    {
        s_NavPosX = OakS(defNx);
        s_NavPosY = OakS(defNy);
        s_NavPosSet = true;
    }
}

static inline ImFont* OakFont(ImFont* f) { return f ? f : ImGui::GetFont(); }

static inline void OakTxt(ImDrawList* dl, ImFont* f, float sz, ImVec2 p, ImU32 col, const char* s)
{
    dl->AddText(OakFont(f), OakS(sz), p, col, s);
}

static inline float OakTxtW(ImFont* f, float sz, const char* s)
{
    return OakFont(f)->CalcTextSizeA(OakS(sz), FLT_MAX, 0.0f, s).x;
}

// Vertically centred baseline for a line of `sz` inside a box of height `h`.
static inline float OakTxtY(float top, float h, float sz)
{
    return top + (h - OakS(sz)) * 0.5f;
}

static inline ImU32 OakCol(const ImVec4& c) { return ImGui::GetColorU32(c); }

// ---- primitive shapes -----------------------------------------------------

// Signature silhouette: top-left and bottom-right corners cut at 45 degrees.
static void OakChamfer(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut, ImU32 fill, ImU32 stroke)
{
    const ImVec2 pts[6] = {
        ImVec2(a.x + cut, a.y), ImVec2(b.x, a.y),
        ImVec2(b.x, b.y - cut), ImVec2(b.x - cut, b.y),
        ImVec2(a.x, b.y),       ImVec2(a.x, a.y + cut)
    };
    if (fill)
        dl->AddConvexPolyFilled(pts, 6, fill);
    if (stroke)
    {
        const ImVec2 in[6] = {
            ImVec2(a.x + cut + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, a.y + 0.5f),
            ImVec2(b.x - 0.5f, b.y - cut - 0.5f), ImVec2(b.x - cut - 0.5f, b.y - 0.5f),
            ImVec2(a.x + 0.5f, b.y - 0.5f),       ImVec2(a.x + 0.5f, a.y + cut + 0.5f)
        };
        dl->AddPolyline(in, 6, stroke, ImDrawFlags_Closed, 1.0f);
    }
}

// Header bar shares the panel's top-left cut so no square corner pokes out.
static void OakBarShape(ImDrawList* dl, ImVec2 a, float w, float h, float cut, ImU32 fill)
{
    const ImVec2 pts[5] = {
        ImVec2(a.x + cut, a.y), ImVec2(a.x + w, a.y),
        ImVec2(a.x + w, a.y + h), ImVec2(a.x, a.y + h), ImVec2(a.x, a.y + cut)
    };
    dl->AddConvexPolyFilled(pts, 5, fill);
}

static void OakChevron(ImDrawList* dl, ImVec2 c, float sz, bool down, ImU32 col)
{
    const float th = OakMax(1.0f, OakS(1.5f));
    const float r = OakS(sz) * 0.5f;
    if (down)
    {
        dl->AddLine(ImVec2(c.x - r, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.6f), col, th);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.6f), ImVec2(c.x + r, c.y - r * 0.5f), col, th);
    }
    else
    {
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y - r), ImVec2(c.x + r * 0.6f, c.y), col, th);
        dl->AddLine(ImVec2(c.x + r * 0.6f, c.y), ImVec2(c.x - r * 0.5f, c.y + r), col, th);
    }
}

static void OakTickMark(ImDrawList* dl, ImVec2 a, float box, ImU32 col)
{
    const float th = OakMax(1.0f, OakS(2.0f));
    const ImVec2 p0(a.x + box * 0.24f, a.y + box * 0.52f);
    const ImVec2 p1(a.x + box * 0.44f, a.y + box * 0.73f);
    const ImVec2 p2(a.x + box * 0.78f, a.y + box * 0.28f);
    dl->AddLine(p0, p1, col, th);
    dl->AddLine(p1, p2, col, th);
}

// 13x13 nav glyphs ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â eye / crosshair / globe / key / sliders.
static void OakNavIcon(ImDrawList* dl, ImVec2 c, float sz, int kind, ImU32 col)
{
    const float r = OakS(sz) * 0.5f;
    const float th = OakMax(1.0f, OakS(1.1f));
    switch (kind)
    {
    case kOakVisuals:
        dl->AddBezierQuadratic(ImVec2(c.x - r, c.y), ImVec2(c.x, c.y - r * 1.4f), ImVec2(c.x + r, c.y), col, th, 12);
        dl->AddBezierQuadratic(ImVec2(c.x - r, c.y), ImVec2(c.x, c.y + r * 1.4f), ImVec2(c.x + r, c.y), col, th, 12);
        dl->AddCircle(c, r * 0.34f, col, 10, th);
        break;
    case kOakCombat:
        dl->AddCircle(c, r * 0.72f, col, 14, th);
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.4f, c.y), col, th);
        dl->AddLine(ImVec2(c.x + r * 0.4f, c.y), ImVec2(c.x + r, c.y), col, th);
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y - r * 0.4f), col, th);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.4f), ImVec2(c.x, c.y + r), col, th);
        break;
    case kOakWorld:
        dl->AddCircle(c, r * 0.9f, col, 16, th);
        dl->AddLine(ImVec2(c.x - r * 0.9f, c.y), ImVec2(c.x + r * 0.9f, c.y), col, th);
        dl->AddBezierQuadratic(ImVec2(c.x, c.y - r * 0.9f), ImVec2(c.x - r * 0.85f, c.y),
                               ImVec2(c.x, c.y + r * 0.9f), col, th, 12);
        break;
    case kOakBinds:
        dl->AddCircle(ImVec2(c.x - r * 0.42f, c.y - r * 0.3f), r * 0.42f, col, 12, th);
        dl->AddLine(ImVec2(c.x - r * 0.14f, c.y), ImVec2(c.x + r * 0.85f, c.y + r * 0.85f), col, th);
        dl->AddLine(ImVec2(c.x + r * 0.4f, c.y + r * 0.4f), ImVec2(c.x + r * 0.72f, c.y + r * 0.1f), col, th);
        break;
    default:
        for (int i = 0; i < 3; i++)
        {
            const float y = c.y + (float)(i - 1) * r * 0.72f;
            dl->AddLine(ImVec2(c.x - r, y), ImVec2(c.x + r, y), col, th);
            dl->AddCircleFilled(ImVec2(c.x - r + r * (0.5f + 0.6f * (float)i), y), OakS(1.6f), col, 8);
        }
        break;
    }
}

// ---- search filter --------------------------------------------------------

static bool OakSearchPass(const char* name)
{
    if (!s_Search[0])
        return true;
    for (const char* p = name; *p; ++p)
    {
        const char* a = p;
        const char* b = s_Search;
        while (*a && *b)
        {
            const char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            const char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) break;
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}

static const char* OakKeyLabel(int vk)
{
    return vk ? MenuKeyName(vk) : "-";
}

// ---- setting rows (label left, control right) -----------------------------

static void OakSectionLabel(const char* text, float topPad)
{
    if (topPad > 0.0f) ImGui::Dummy(ImVec2(0.0f, OakS(topPad)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = OakS(13.0f);
    ImGui::Dummy(ImVec2(w, h));
    OakTxt(ImGui::GetWindowDrawList(), g_FontSmall, kOakFontLbl,
           ImVec2(p.x, OakTxtY(p.y, h, kOakFontLbl)), kOakTxtDim, text);
}

// LabelSetting ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â hairline divider then a dim micro caption.
static void OakGroupLabel(const char* text)
{
    ImGui::Dummy(ImVec2(0.0f, OakS(7.0f)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, OakS(1.0f)));
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + w, p.y), kOakLine, 1.0f);
    OakSectionLabel(text, 5.0f);
}

static bool OakSliderRow(const char* label, int* v, int lo, int hi, const char* unit)
{
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;

    ImGui::Dummy(ImVec2(0.0f, OakS(5.0f)));
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float lh = OakS(13.0f);
    ImGui::Dummy(ImVec2(w, lh));
    OakTxt(dl, g_FontSmall, kOakFontLbl, ImVec2(p.x, OakTxtY(p.y, lh, kOakFontLbl)), kOakTxtMid, label);

    char val[40];
    snprintf(val, sizeof(val), "%d%s", *v, unit ? unit : "");
    OakTxt(dl, g_FontSmall, kOakFontMon,
           ImVec2(p.x + w - OakTxtW(g_FontSmall, kOakFontMon, val), OakTxtY(p.y, lh, kOakFontMon)),
           kOakAcc, val);

    p = ImGui::GetCursorScreenPos();
    const float th = OakS(14.0f);
    ImGui::InvisibleButton("##rng", ImVec2(w, th));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    float t = (hi > lo) ? (float)(*v - lo) / (float)(hi - lo) : 0.0f;
    if (active && w > 1.0f)
    {
        float nt = (ImGui::GetIO().MousePos.x - p.x) / w;
        nt = OakClamp(nt, 0.0f, 1.0f);
        const int nv = lo + (int)(nt * (float)(hi - lo) + 0.5f);
        if (nv != *v) { *v = nv; changed = true; }
        t = nt;
    }

    const float cy = p.y + th * 0.5f;
    const float bar = OakMax(1.0f, OakS(3.0f));
    dl->AddRectFilled(ImVec2(p.x, cy - bar * 0.5f), ImVec2(p.x + w, cy + bar * 0.5f), kOakLineHi, bar * 0.5f);
    dl->AddRectFilled(ImVec2(p.x, cy - bar * 0.5f), ImVec2(p.x + w * t, cy + bar * 0.5f), kOakAcc, bar * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + w * t, cy), OakS((hovered || active) ? 5.5f : 4.5f), kOakAcc, 16);

    ImGui::PopID();
    return changed;
}

static bool OakCheckBox(ImDrawList* dl, ImVec2 p, float box, bool on, bool hovered)
{
    dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box), on ? kOakAcc : kOakSunken, OakS(3.0f));
    dl->AddRect(p, ImVec2(p.x + box, p.y + box), on ? kOakAcc : (hovered ? kOakAcc : kOakLineHi),
                OakS(3.0f), 0, 1.0f);
    if (on)
        OakTickMark(dl, p, box, kOakTick);
    return on;
}

static bool OakCheckRow(const char* label, bool* v)
{
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = OakS(19.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##chk", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }

    const float box = OakS(15.0f);
    OakCheckBox(dl, ImVec2(p.x, p.y + (h - box) * 0.5f), box, *v, hovered);
    OakTxt(dl, g_FontRegular, 11.0f, ImVec2(p.x + box + OakS(9.0f), OakTxtY(p.y, h, 11.0f)),
           *v ? kOakTxt : (hovered ? kOakTxt : kOakTxtMid), label);

    ImGui::PopID();
    return changed;
}

// 30x15 swatch that opens an inline picker popup.
static void OakSwatch(const char* id, ImVec2 p, ImVec4* c)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = OakS(30.0f);
    const float h = OakS(15.0f);
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(id);
    ImGui::InvisibleButton("##sw", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
        ImGui::OpenPopup("##pick");

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(0x2A, 0x2A, 0x2E, 0xFF), OakS(4.0f));
    dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + w - 1, p.y + h - 1), OakCol(*c), OakS(3.0f));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), hovered ? kOakTxt : kOakLineHi, OakS(4.0f), 0, 1.0f);

    if (ImGui::BeginPopup("##pick"))
    {
        ImGui::SetNextItemWidth(OakS(170.0f));
        ImGui::ColorPicker4("##p", (float*)c,
            ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview |
            ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_DisplayHex);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

static void OakColorRow(const char* label, ImVec4* c)
{
    ImGui::PushID(label);
    ImGui::Dummy(ImVec2(0.0f, OakS(5.0f)));
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = OakS(17.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    OakTxt(ImGui::GetWindowDrawList(), g_FontSmall, kOakFontLbl,
           ImVec2(p.x, OakTxtY(p.y, h, kOakFontLbl)), kOakTxtMid, label);
    OakSwatch("c", ImVec2(p.x + w - OakS(30.0f), p.y + (h - OakS(15.0f)) * 0.5f), c);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::PopID();
}

static void OakCheckColorRow(const char* label, bool* v, ImVec4* c)
{
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = OakS(19.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();

    // The checkbox owns the row minus the swatch column so both stay clickable.
    ImGui::PushID("k");
    ImGui::InvisibleButton("##chk", ImVec2(OakMax(1.0f, w - OakS(36.0f)), h));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) *v = !*v;
    ImGui::PopID();

    const float box = OakS(15.0f);
    OakCheckBox(dl, ImVec2(p.x, p.y + (h - box) * 0.5f), box, *v, hovered);
    OakTxt(dl, g_FontRegular, 11.0f, ImVec2(p.x + box + OakS(9.0f), OakTxtY(p.y, h, 11.0f)),
           (*v || hovered) ? kOakTxt : kOakTxtMid, label);
    OakSwatch("c", ImVec2(p.x + w - OakS(30.0f), p.y + (h - OakS(15.0f)) * 0.5f), c);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::PopID();
}

// Boxed mono key chip. Returns true when clicked (left), clears on right-click.
static bool OakKeyChip(const char* id, const char* text, bool capturing, float minW, ImVec2* size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float padX = OakS(8.0f);
    const float padY = OakS(4.0f);
    const float tw = OakTxtW(g_FontSmall, 9.0f, text);
    const float w = OakMax(OakS(minW), tw + padX * 2.0f);
    const float h = OakS(9.0f) + padY * 2.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::PushID(id);
    ImGui::InvisibleButton("##chip", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = ImGui::IsItemClicked();
    ImGui::PopID();

    const ImU32 edge = capturing ? kOakAcc : (hovered ? kOakAccDim : kOakLine);
    const ImU32 txt = capturing ? kOakAcc : (hovered ? kOakAcc : kOakTxt);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kOakSunken, OakS(4.0f));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), edge, OakS(4.0f), 0, 1.0f);
    OakTxt(dl, g_FontSmall, 9.0f, ImVec2(p.x + (w - tw) * 0.5f, OakTxtY(p.y, h, 9.0f)), txt, text);

    if (size) *size = ImVec2(w, h);
    return pressed;
}

static void OakKeyRow(const char* label, int* bind, int captureId, float chipW)
{
    ImGui::PushID(label);
    ImGui::Dummy(ImVec2(0.0f, OakS(5.0f)));
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = OakS(19.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    OakTxt(ImGui::GetWindowDrawList(), g_FontSmall, kOakFontLbl,
           ImVec2(p.x, OakTxtY(p.y, h, kOakFontLbl)), kOakTxtMid, label);

    const bool capturing = (g_BindCaptureTarget == captureId);
    const char* text = capturing ? "..." : OakKeyLabel(*bind);
    const float est = OakMax(OakS(chipW), OakTxtW(g_FontSmall, 9.0f, text) + OakS(16.0f));
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - est, p.y + (h - OakS(17.0f)) * 0.5f));
    if (OakKeyChip("k", text, capturing, chipW, nullptr))
        g_BindCaptureTarget = captureId;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        *bind = 0;
        if (capturing) g_BindCaptureTarget = -1;
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::PopID();
}

// "Label - Value" with a chevron, opening a selection popup.
static bool OakModeRow(const char* label, int* idx, const char* const* opts, int n)
{
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0.0f, OakS(5.0f)));
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = OakS(20.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##mode", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
        ImGui::OpenPopup("##opts");

    const int sel = (*idx >= 0 && *idx < n) ? *idx : 0;
    char line[96];
    snprintf(line, sizeof(line), "%s - %s", label, opts[sel]);
    OakTxt(dl, g_FontRegular, 11.5f, ImVec2(p.x, OakTxtY(p.y, h, 11.5f)),
           hovered ? kOakTxt : kOakTxtMid, line);
    OakChevron(dl, ImVec2(p.x + w - OakS(6.0f), p.y + h * 0.5f), 9.0f, true,
               hovered ? kOakAcc : kOakTxtDim);

    bool changed = false;
    if (ImGui::BeginPopup("##opts"))
    {
        for (int i = 0; i < n; i++)
            if (ImGui::Selectable(opts[i], i == sel))
            {
                *idx = i;
                changed = true;
            }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// TBtn ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â boxed, quiet, uppercase-ish micro label.
static bool OakButton(const char* label, float w, bool accent)
{
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = OakS(21.0f);
    const float bw = (w > 0.0f) ? w : ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##b", ImVec2(bw, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool pressed = ImGui::IsItemClicked();

    const ImU32 fill = held ? kOakSunken
                     : hovered ? IM_COL32(0x2C, 0x2C, 0x2C, 0xFF)
                               : IM_COL32(0x1F, 0x1F, 0x1F, 0xFF);
    dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + h), fill, OakS(4.0f));
    dl->AddRect(p, ImVec2(p.x + bw, p.y + h), hovered ? kOakLineHi : kOakLine, OakS(4.0f), 0, 1.0f);
    const float tw = OakTxtW(g_FontSmall, kOakFontLbl, label);
    OakTxt(dl, g_FontSmall, kOakFontLbl, ImVec2(p.x + (bw - tw) * 0.5f, OakTxtY(p.y, h, kOakFontLbl)),
           accent ? kOakAcc : (hovered ? kOakTxt : kOakTxtMid), label);
    ImGui::PopID();
    return pressed;
}

// Borderless text action, used where a boxed button would shout.
static bool OakGhost(const char* label, ImVec2 pos)
{
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float tw = OakTxtW(g_FontSmall, kOakFontLbl, label);
    const float h = OakS(15.0f);
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##g", ImVec2(tw + OakS(8.0f), h));
    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = ImGui::IsItemClicked();
    OakTxt(dl, g_FontSmall, kOakFontLbl, ImVec2(pos.x + OakS(4.0f), OakTxtY(pos.y, h, kOakFontLbl)),
           hovered ? kOakTxt : kOakTxtDim, label);
    ImGui::PopID();
    return pressed;
}

// Tone-coded advisory strip inside a drawer. tone: 0 neutral, 1 warn, 2 bad.
static void OakNote(int tone, const char* text)
{
    const ImU32 col = (tone == 2) ? kOakBad : (tone == 1) ? kOakWarn : kOakTxtMid;
    ImGui::Dummy(ImVec2(0.0f, OakS(7.0f)));
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImFont* f = OakFont(g_FontRegular);
    const float sz = OakS(9.5f);
    const float wrap = w - OakS(14.0f);
    const ImVec2 ts = f->CalcTextSizeA(sz, FLT_MAX, wrap, text);
    const float h = ts.y + OakS(11.0f);
    ImGui::Dummy(ImVec2(w, h));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 13));
    dl->AddRectFilled(p, ImVec2(p.x + OakS(2.0f), p.y + h), col);
    dl->AddText(f, sz, ImVec2(p.x + OakS(9.0f), p.y + OakS(5.0f)), col, text, nullptr, wrap);
}

static void OakDivider()
{
    ImGui::Dummy(ImVec2(0.0f, OakS(6.0f)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, OakS(1.0f)));
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + w, p.y), kOakLine, 1.0f);
}

// ---- module row + settings drawer ----------------------------------------

// Returns true when the settings drawer is open; caller must then call
// OakModuleEnd(). Rows filtered out by search return false and draw nothing.
static bool OakModule(int index, const char* name, bool* on, int keyVk, bool hasSettings, bool locked)
{
    if (!OakSearchPass(name))
        return false;

    s_CountTotal++;
    if (on && *on) s_CountOn++;

    ImGui::PushID(name);
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID openId = ImGui::GetID("##open");
    bool open = hasSettings && store->GetBool(openId, false);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = OakS(kOakRowH);
    const float chevW = hasSettings ? OakS(21.0f) : 0.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##row", ImVec2(OakMax(1.0f, w - chevW), h));
    const bool hovered = !locked && ImGui::IsItemHovered();
    if (!locked && on && ImGui::IsItemClicked())
        *on = !*on;
    if (locked && ImGui::IsItemHovered())
        ImGui::SetTooltip("Forced off in this build");

    bool chevHot = false;
    if (hasSettings)
    {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::InvisibleButton("##cfg", ImVec2(chevW, h));
        chevHot = ImGui::IsItemHovered();
        if (!locked && ImGui::IsItemClicked())
        {
            open = !open;
            store->SetBool(openId, open);
        }
    }

    const bool isOn = on && *on && !locked;
    if (isOn)
    {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kOakWash);
        dl->AddRectFilled(p, ImVec2(p.x + OakS(2.0f), p.y + h), kOakAcc);
    }
    else if (hovered || chevHot)
    {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kOakHi);
    }

    char idx[8];
    snprintf(idx, sizeof(idx), "%02d", index + 1);
    const ImU32 idxCol = locked ? kOakDead : (isOn ? kOakAccDim : kOakTxtDim);
    const ImU32 nameCol = locked ? kOakDead
                        : isOn ? kOakAcc
                        : (hovered ? kOakTxt : kOakTxtMid);
    OakTxt(dl, g_FontSmall, kOakFontMon, ImVec2(p.x + OakS(10.0f), OakTxtY(p.y, h, kOakFontMon)), idxCol, idx);
    OakTxt(dl, g_FontRegular, kOakFontUi, ImVec2(p.x + OakS(26.0f), OakTxtY(p.y, h, kOakFontUi)), nameCol, name);

    float rightEdge = p.x + w - chevW - OakS(4.0f);
    if (keyVk != 0)
    {
        const char* kt = OakKeyLabel(keyVk);
        const float kw = OakTxtW(g_FontSmall, kOakFontMon, kt) + OakS(7.0f);
        const float kh = OakS(13.0f);
        const ImVec2 kp(rightEdge - kw, p.y + (h - kh) * 0.5f);
        dl->AddRect(kp, ImVec2(kp.x + kw, kp.y + kh), kOakLine, OakS(2.0f), 0, 1.0f);
        OakTxt(dl, g_FontSmall, kOakFontMon,
               ImVec2(kp.x + OakS(3.5f), OakTxtY(kp.y, kh, kOakFontMon)), kOakTxtDim, kt);
    }

    if (hasSettings)
        OakChevron(dl, ImVec2(p.x + w - chevW * 0.5f, p.y + h * 0.5f), 9.0f, open,
                   (chevHot || open) ? kOakAcc : kOakTxtDim);

    if (!open)
    {
        ImGui::PopID();
        return false;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(OakS(11.0f), OakS(5.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, OakS(1.0f)));
    ImGui::BeginChild("##drawer", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    return true;
}

static void OakModuleEnd()
{
    ImGui::Dummy(ImVec2(0.0f, OakS(4.0f)));
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopID();
}

// Momentary module row: reads like a toggle but fires once and springs back.
static bool OakActionModule(int index, const char* name, int keyVk, bool hasSettings, bool* fired)
{
    static bool s_Latch[16] = {};
    const int slot = index & 15;
    const bool open = OakModule(index, name, &s_Latch[slot], keyVk, hasSettings, false);
    if (s_Latch[slot])
    {
        s_Latch[slot] = false;
        if (fired) *fired = true;
    }
    return open;
}

static void OakBindRow(const char* name, int* bind, int captureId, bool locked)
{
    if (!OakSearchPass(name))
        return;
    s_CountTotal++;
    if (*bind) s_CountOn++;

    ImGui::PushID(name);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = OakS(kOakBindH);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));

    OakTxt(dl, g_FontRegular, 11.0f, ImVec2(p.x + OakS(12.0f), OakTxtY(p.y, h, 11.0f)),
           locked ? kOakDead : kOakTxtMid, name);

    const bool capturing = (g_BindCaptureTarget == captureId);
    const char* text = capturing ? "..." : OakKeyLabel(*bind);
    const float est = OakMax(OakS(64.0f), OakTxtW(g_FontSmall, 9.0f, text) + OakS(18.0f));
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - est - OakS(12.0f), p.y + (h - OakS(18.0f)) * 0.5f));
    if (!locked)
    {
        if (OakKeyChip("k", text, capturing, 64.0f, nullptr))
            g_BindCaptureTarget = captureId;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            *bind = 0;
            if (capturing) g_BindCaptureTarget = -1;
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::PopID();
}

// ---- panel window ---------------------------------------------------------

static void OakPanelHeader(int idx, const char* title)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = OakS(kOakBarH);
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##drag", ImVec2(OakMax(1.0f, w - OakS(22.0f)), h));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        s_PanelPosX[idx] += d.x;
        s_PanelPosY[idx] += d.y;
        s_PanelPosSet[idx] = true;
        ImGui::SetWindowPos(ImVec2(s_PanelPosX[idx], s_PanelPosY[idx]));
    }
    else
    {
        const ImVec2 wp = ImGui::GetWindowPos();
        s_PanelPosX[idx] = wp.x;
        s_PanelPosY[idx] = wp.y;
        s_PanelPosSet[idx] = true;
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("##fold", ImVec2(OakS(22.0f), h));
    const bool foldHot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
        s_PanelFold[idx] = !s_PanelFold[idx];

    char seq[8];
    snprintf(seq, sizeof(seq), "%02d", idx + 1);
    OakTxt(dl, g_FontSmall, kOakFontMon, ImVec2(p.x + OakS(10.0f), OakTxtY(p.y, h, kOakFontMon)),
           IM_COL32(255, 255, 255, 115), seq);
    const float tx = p.x + OakS(10.0f) + OakTxtW(g_FontSmall, kOakFontMon, seq) + OakS(7.0f);
    OakTxt(dl, g_FontMedium, kOakFontTtl, ImVec2(tx, OakTxtY(p.y, h, kOakFontTtl)), kOakTxt, title);

    char count[24];
    snprintf(count, sizeof(count), "%d/%d", s_PanelOn[idx], s_PanelTotal[idx]);
    OakTxt(dl, g_FontSmall, kOakFontMon,
           ImVec2(p.x + w - OakS(26.0f) - OakTxtW(g_FontSmall, kOakFontMon, count),
                  OakTxtY(p.y, h, kOakFontMon)), kOakTxtDim, count);

    OakChevron(dl, ImVec2(p.x + w - OakS(11.0f), p.y + h * 0.5f), 9.0f, !s_PanelFold[idx],
               foldHot ? kOakAcc : kOakTxtDim);
}

static bool OakPanelBegin(int idx, const char* id, const char* title, float x, float y, float w)
{
    if (!s_PanelShow[idx])
        return false;

    const float width = OakS(w);
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (!s_PanelPosSet[idx])
    {
        float px = OakS(x);
        if (px + width > disp.x - OakS(8.0f))
            px = OakMax(OakS(8.0f), disp.x - width - OakS(8.0f));
        s_PanelPosX[idx] = px;
        s_PanelPosY[idx] = OakS(y);
        s_PanelPosSet[idx] = true;
    }
    // Keep on-screen if resolution changed since last save.
    s_PanelPosX[idx] = OakClamp(s_PanelPosX[idx], OakS(0.0f), OakMax(OakS(0.0f), disp.x - width));
    s_PanelPosY[idx] = OakClamp(s_PanelPosY[idx], OakS(0.0f), OakMax(OakS(0.0f), disp.y - OakS(kOakBarH)));

    ImGui::SetNextWindowPos(ImVec2(s_PanelPosX[idx], s_PanelPosY[idx]), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, OakS(kOakBarH)),
                                        ImVec2(width, OakMax(OakS(120.0f), disp.y - OakS(48.0f))));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin(id, nullptr, flags))
    {
        ImGui::End();
        ImGui::PopStyleVar(4);
        return false;
    }

    ImGui::SetWindowFontScale(s_UiScale);
    ImGui::GetWindowDrawList()->ChannelsSplit(2);
    ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);

    s_CurPanel = idx;
    s_CountOn = 0;
    s_CountTotal = 0;
    OakPanelHeader(idx, title);
    return true;
}

static bool OakPanelBody()
{
    if (s_PanelFold[s_CurPanel])
        return false;
    ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
    return true;
}

static void OakPanelEnd()
{
    const int idx = s_CurPanel;
    if (!s_PanelFold[idx])
        ImGui::Dummy(ImVec2(0.0f, OakS(4.0f)));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    const float cut = OakS(kOakCut);
    const float bar = OakS(kOakBarH);

    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(ImVec2(p0.x + OakS(3.0f), p0.y + OakS(5.0f)),
                      ImVec2(p1.x + OakS(3.0f), p1.y + OakS(6.0f)), IM_COL32(0, 0, 0, 90));
    OakChamfer(dl, p0, p1, cut, kOakPanel, 0);
    OakBarShape(dl, p0, sz.x, OakMin(bar, sz.y), cut, kOakBar);
    if (!s_PanelFold[idx] && sz.y > bar)
        dl->AddLine(ImVec2(p0.x, p0.y + bar), ImVec2(p1.x, p0.y + bar), kOakLine, 1.0f);
    OakChamfer(dl, p0, p1, cut, 0, kOakLine);
    dl->ChannelsMerge();

    s_PanelOn[idx] = s_CountOn;
    s_PanelTotal[idx] = s_CountTotal;

    ImGui::End();
    ImGui::PopStyleVar(4);
}

// Padded body block for panels that are not module lists (Session, hints).
static void OakBlockBegin(const char* id)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(OakS(12.0f), OakS(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, OakS(3.0f)));
    ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}

static void OakBlockEnd()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// ---- panels ---------------------------------------------------------------

static void OakWaypointSlots()
{
    ImGui::Dummy(ImVec2(0.0f, OakS(5.0f)));
    const float w = ImGui::GetContentRegionAvail().x;
    const float lh = OakS(13.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, lh));
    OakTxt(ImGui::GetWindowDrawList(), g_FontSmall, kOakFontLbl,
           ImVec2(p.x, OakTxtY(p.y, lh, kOakFontLbl)), kOakTxtMid, "Markers");
    char tally[16];
    snprintf(tally, sizeof(tally), "%d/%d", g_Ui.waypointCount, OAK_WAYPOINT_MAX);
    OakTxt(ImGui::GetWindowDrawList(), g_FontSmall, kOakFontMon,
           ImVec2(p.x + w - OakTxtW(g_FontSmall, kOakFontMon, tally), OakTxtY(p.y, lh, kOakFontMon)),
           kOakAcc, tally);

    ImGui::PushFont(OakFont(g_FontSmall));
    for (int i = 0; i < g_Ui.waypointCount; i++)
    {
        ImGui::PushID(i);
        ImGui::Dummy(ImVec2(0.0f, OakS(4.0f)));
        p = ImGui::GetCursorScreenPos();
        char slot[8];
        snprintf(slot, sizeof(slot), "%02d", i + 1);
        OakTxt(ImGui::GetWindowDrawList(), g_FontSmall, kOakFontMon,
               ImVec2(p.x, p.y + OakS(5.0f)), kOakTxtDim, slot);

        const float nameX = OakS(18.0f);
        const float btnW = OakS(34.0f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + nameX, p.y));
        ImGui::SetNextItemWidth(w - nameX - btnW - OakS(5.0f));
        ImGui::InputText("##n", g_Ui.waypointName[i], 32);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - btnW, p.y));
        bool erase = OakButton("CLR", btnW, false);

        const float cellW = (w - OakS(6.0f)) / 3.0f;
        ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetCursorScreenPos().y + OakS(3.0f)));
        float* axis[3] = { &g_Ui.waypointX[i], &g_Ui.waypointY[i], &g_Ui.waypointZ[i] };
        for (int a = 0; a < 3; a++)
        {
            ImGui::PushID(a);
            if (a) ImGui::SameLine(0.0f, OakS(3.0f));
            ImGui::SetNextItemWidth(cellW);
            ImGui::InputFloat("##v", axis[a], 0.0f, 0.0f, "%.1f");
            ImGui::PopID();
        }
        ImVec4 wc(g_Ui.waypointColor[i][0], g_Ui.waypointColor[i][1], g_Ui.waypointColor[i][2], g_Ui.waypointColor[i][3]);
        OakColorRow("Colour", &wc);
        g_Ui.waypointColor[i][0]=wc.x; g_Ui.waypointColor[i][1]=wc.y; g_Ui.waypointColor[i][2]=wc.z; g_Ui.waypointColor[i][3]=wc.w;

        if (erase)
        {
            for (int j = i; j < g_Ui.waypointCount - 1; j++)
            {
                g_Ui.waypointX[j] = g_Ui.waypointX[j + 1];
                g_Ui.waypointY[j] = g_Ui.waypointY[j + 1];
                g_Ui.waypointZ[j] = g_Ui.waypointZ[j + 1];
                lstrcpynA(g_Ui.waypointName[j], g_Ui.waypointName[j + 1], 32);
            }
            g_Ui.waypointCount--;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0.0f, OakS(6.0f)));
    if (OakButton("STAMP MY POSITION", 0.0f, true) && g_Ui.waypointCount < OAK_WAYPOINT_MAX)
    {
        const int i = g_Ui.waypointCount++;
        g_Ui.waypointX[i] = g_Ui.posX;
        g_Ui.waypointY[i] = g_Ui.posY;
        g_Ui.waypointZ[i] = g_Ui.posZ;
        snprintf(g_Ui.waypointName[i], 32, "WP%d", i + 1);
    }
    ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
    if (OakButton("CLEAR ALL", 0.0f, false))
    {
        g_Ui.waypointCount = 0;
        ZeroMemory(g_Ui.waypointName, sizeof(g_Ui.waypointName));
    }
}

static void OakDrawVisuals()
{
    if (!OakPanelBegin(kOakVisuals, "##oak_visuals", "Visuals", 214.0f, 26.0f, 202.0f))
        return;

    if (OakPanelBody())
    {
        int i = 0;
        OakSectionLabel("ESP", 0.0f);

        OakModule(i++, "Enable ESP", &g_Ui.espEnabled, g_Ui.bindEsp, false, false);

        if (OakModule(i++, "Players", &g_Ui.espPlayers, 0, true, false))
        {
            OakSliderRow("Max Distance", &g_Ui.playerMaxDistance, 0, OAK_CAP_DIST_M, "m");

            OakGroupLabel("Draw");
            OakCheckColorRow("Box", &g_Ui.playerBox, &g_Ui.colorPlayerBox);
            OakCheckColorRow("Skeleton", &g_Ui.espSkeleton, &g_Ui.colorPlayerSkeleton);
            OakCheckColorRow("Name", &g_Ui.playerName, &g_Ui.colorPlayerName);
            OakCheckRow("Distance", &g_Ui.playerDistance);
            OakCheckRow("Draw Local Player", &g_Ui.drawLocalPlayer);

            OakGroupLabel("Identity");
            OakCheckRow("Steam Avatars", &g_Ui.miscSteamAvatars);
            if (g_Ui.miscSteamAvatars)
                OakNote(1, "Small Steam PFPs beside ESP names (crash-safe; auto-disables on fault).");
            OakCheckRow("Weapon ESP", &g_Ui.weaponEsp);

            OakGroupLabel("Vitals");
            OakCheckRow("Health Bars", &g_Ui.healthBars);
            if (g_Ui.healthBars)
            {
                OakCheckRow("Health", &g_Ui.barHealth);
                OakCheckRow("Blood", &g_Ui.barBlood);
                OakCheckRow("Shock", &g_Ui.barShock);
                OakCheckRow("Stamina", &g_Ui.barStamina);
            }

            OakGroupLabel("Style");
            {
                static const char* boxStyles[] = { "Full", "Corner", "3D" };
                OakModeRow("Box Style", &g_Ui.playerStyle.boxStyle, boxStyles, 3);
            }
            {
                int thick = (int)(g_Ui.playerStyle.boxThickness * 10.f);
                OakSliderRow("Box Thickness", &thick, 5, 50, "");
                g_Ui.playerStyle.boxThickness = thick / 10.f;
            }
            OakCheckRow("Visibility Colors", &g_Ui.playerStyle.useVisibilityColors);
            if (g_Ui.playerStyle.useVisibilityColors)
            {
                OakColorRow("Visible Color", (ImVec4*)&g_Ui.playerStyle.colorVisible);
                OakColorRow("Occluded Color", (ImVec4*)&g_Ui.playerStyle.colorOccluded);
            }

            OakGroupLabel("Indicators");
            OakCheckRow("Stance Icon (S/C/P)", &g_Ui.stanceIcon.enabled);
            if (g_Ui.stanceIcon.enabled)
                OakSliderRow("Stance Max Dist", &g_Ui.stanceIcon.maxDistanceM, 50, OAK_CAP_DIST_M, "m");
            OakCheckRow("Look Direction", &g_Ui.lookDirection.enabled);
            if (g_Ui.lookDirection.enabled)
            {
                OakCheckRow("Look: Players", &g_Ui.lookDirection.players);
                OakSliderRow("Look Line Max", &g_Ui.lookDirection.maxDistanceM, 50, OAK_CAP_DIST_M, "m");
                {
                    int ll = (int)(g_Ui.lookDirection.lineLengthM * 10.f);
                    OakSliderRow("Look Line Len", &ll, 10, 80, "m");
                    g_Ui.lookDirection.lineLengthM = ll / 10.f;
                }
                OakColorRow("Look Color", (ImVec4*)&g_Ui.lookDirection.color);
            }
            OakCheckRow("Looking At Me", &g_Ui.batch4Esp.lookingAtMe.enabled);
            if (g_Ui.batch4Esp.lookingAtMe.enabled)
            {
                OakCheckRow("Looking: Players", &g_Ui.batch4Esp.lookingAtMe.players);
                OakSliderRow("Looking Angle", &g_Ui.batch4Esp.lookingAtMe.angleDeg, 10, 70, "deg");
                OakSliderRow("Looking Max Dist", &g_Ui.batch4Esp.lookingAtMe.maxDistanceM, 20, OAK_CAP_DIST_M, "m");
                OakColorRow("Looking Color", (ImVec4*)&g_Ui.batch4Esp.lookingAtMe.color);
            }

            OakGroupLabel("Inventory");
            OakCheckRow("Crosshair Inv View", &g_Ui.batch4Esp.playerInvViewer.enabled);
            if (g_Ui.batch4Esp.playerInvViewer.enabled)
                OakNote(0, "Shows YOU when alone; other players when under crosshair.");

            OakGroupLabel("Trail");
            OakCheckRow("Player Trail", &g_Ui.playerTrail.enabled);
            if (g_Ui.playerTrail.enabled)
            {
                OakSliderRow("Max Players", &g_Ui.playerTrail.maxPlayers, 1, 32, "");
                OakSliderRow("Trail Points", &g_Ui.playerTrail.maxPoints, 4, 48, "");
                { int ds = (int)(g_Ui.playerTrail.durationSec * 10.f); OakSliderRow("Duration", &ds, 20, 120, "s"); g_Ui.playerTrail.durationSec = ds / 10.f; }
            }
            OakModuleEnd();
        }

        if (OakModule(i++, "Zombies", &g_Ui.espZombies, 0, true, false))
        {
            OakSliderRow("Max Distance", &g_Ui.zombieMaxDistance, 0, OAK_CAP_DIST_M, "m");

            OakGroupLabel("Draw");
            OakCheckColorRow("Box", &g_Ui.zombieBox, &g_Ui.colorZombieBox);
            OakCheckColorRow("Skeleton", &g_Ui.espSkeleton, &g_Ui.colorZombieSkeleton);
            OakCheckColorRow("Name", &g_Ui.zombieName, &g_Ui.colorZombieName);
            OakCheckRow("Distance", &g_Ui.zombieDistance);

            OakGroupLabel("Vitals");
            OakCheckRow("Health Bars", &g_Ui.healthBars);
            if (g_Ui.healthBars)
                OakNote(1, "Infected bars are HP only.");

            OakGroupLabel("Style");
            {
                static const char* boxStyles[] = { "Full", "Corner", "3D" };
                OakModeRow("Box Style", &g_Ui.zombieStyle.boxStyle, boxStyles, 3);
            }
            {
                int thick = (int)(g_Ui.zombieStyle.boxThickness * 10.f);
                OakSliderRow("Box Thickness", &thick, 5, 50, "");
                g_Ui.zombieStyle.boxThickness = thick / 10.f;
            }
            OakCheckRow("Visibility Colors", &g_Ui.zombieStyle.useVisibilityColors);
            if (g_Ui.zombieStyle.useVisibilityColors)
            {
                OakColorRow("Visible Color", (ImVec4*)&g_Ui.zombieStyle.colorVisible);
                OakColorRow("Occluded Color", (ImVec4*)&g_Ui.zombieStyle.colorOccluded);
            }

            OakGroupLabel("Indicators");
            OakCheckRow("Look Direction", &g_Ui.lookDirection.enabled);
            if (g_Ui.lookDirection.enabled)
            {
                OakCheckRow("Look: Zombies", &g_Ui.lookDirection.zombies);
                {
                    ImVec4 zc(g_Ui.lookDirection.colorZombie[0], g_Ui.lookDirection.colorZombie[1],
                        g_Ui.lookDirection.colorZombie[2], g_Ui.lookDirection.colorZombie[3]);
                    OakColorRow("Zombie Look Color", &zc);
                    g_Ui.lookDirection.colorZombie[0]=zc.x; g_Ui.lookDirection.colorZombie[1]=zc.y;
                    g_Ui.lookDirection.colorZombie[2]=zc.z; g_Ui.lookDirection.colorZombie[3]=zc.w;
                }
            }
            OakCheckRow("Looking At Me", &g_Ui.batch4Esp.lookingAtMe.enabled);
            if (g_Ui.batch4Esp.lookingAtMe.enabled)
            {
                OakCheckRow("Looking: Zombies", &g_Ui.batch4Esp.lookingAtMe.zombies);
                {
                    ImVec4 zc(g_Ui.batch4Esp.lookingAtMe.colorZombie[0], g_Ui.batch4Esp.lookingAtMe.colorZombie[1],
                        g_Ui.batch4Esp.lookingAtMe.colorZombie[2], g_Ui.batch4Esp.lookingAtMe.colorZombie[3]);
                    OakColorRow("Zombie Looking Color", &zc);
                    g_Ui.batch4Esp.lookingAtMe.colorZombie[0]=zc.x; g_Ui.batch4Esp.lookingAtMe.colorZombie[1]=zc.y;
                    g_Ui.batch4Esp.lookingAtMe.colorZombie[2]=zc.z; g_Ui.batch4Esp.lookingAtMe.colorZombie[3]=zc.w;
                }
            }
            OakModuleEnd();
        }

        if (OakModule(i++, "Animals", &g_Ui.espAnimals, 0, true, false))
        {
            OakSliderRow("Max Distance", &g_Ui.animalMaxDistance, 0, OAK_CAP_DIST_M, "m");
            OakGroupLabel("Draw");
            OakCheckColorRow("Box", &g_Ui.animalBox, &g_Ui.colorAnimalBox);
            OakCheckColorRow("Name", &g_Ui.animalName, &g_Ui.colorAnimalName);
            OakCheckRow("Distance", &g_Ui.animalDistance);
            OakModuleEnd();
        }

        if (OakModule(i++, "Items", &g_Ui.espItems, 0, true, false))
        {
            OakSliderRow("Max Distance", &g_Ui.itemMaxDistance, 0, OAK_CAP_DIST_M, "m");
            OakGroupLabel("Draw");
            OakCheckColorRow("Box", &g_Ui.itemBox, &g_Ui.colorItemBox);
            OakCheckColorRow("Name", &g_Ui.itemName, &g_Ui.colorItemName);
            OakCheckRow("Distance", &g_Ui.itemDistance);
            OakGroupLabel("Categories");
            OakCheckRow("Weapons", &g_Ui.lootShowWeapons);
            OakCheckRow("Ammo", &g_Ui.lootShowAmmo);
            OakCheckRow("Medical", &g_Ui.lootShowMedical);
            OakCheckRow("Food", &g_Ui.lootShowFood);
            OakCheckRow("Clothing", &g_Ui.lootShowClothing);
            OakCheckRow("Tools", &g_Ui.lootShowTools);
            OakCheckRow("Other", &g_Ui.lootShowOther);
            OakSyncLootCatsFromShow();
            OakGroupLabel("Per-Category Limits");
            {
                static const char* catNames[] = { "Weapons", "Ammo", "Medical", "Food", "Clothing", "Tools", "Other" };
                for (int ci = 0; ci < OAK_LOOT_CAT_COUNT; ci++)
                {
                    ImGui::PushID(ci);
                    OakSliderRow(catNames[ci], &g_Ui.lootCats[ci].maxDistance, 0, OAK_CAP_DIST_M, "m");
                    OakSliderRow("Max Count", &g_Ui.lootCats[ci].maxCount, 1, OAK_CAP_MAX_LOOT, "");
                    OakCheckRow("Custom Color", &g_Ui.lootCats[ci].useCustomColor);
                    if (g_Ui.lootCats[ci].useCustomColor)
                    {
                        ImVec4 c(g_Ui.lootCats[ci].color[0], g_Ui.lootCats[ci].color[1],
                                 g_Ui.lootCats[ci].color[2], g_Ui.lootCats[ci].color[3]);
                        OakColorRow("Colour", &c);
                        g_Ui.lootCats[ci].color[0] = c.x; g_Ui.lootCats[ci].color[1] = c.y;
                        g_Ui.lootCats[ci].color[2] = c.z; g_Ui.lootCats[ci].color[3] = c.w;
                    }
                    ImGui::PopID();
                }
            }
            {
                static const char* sorts[] = { "Distance", "Name", "Category" };
                OakModeRow("Sort Over Cap", &g_Ui.lootSortMode, sorts, 3);
            }
            OakGroupLabel("Filters");
            {
                static char s_BlIn[48] = {};
                ImGui::PushFont(OakFont(g_FontSmall));
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##bl", "Blacklist pattern", s_BlIn, sizeof(s_BlIn));
                ImGui::PopFont();
                if (OakButton("ADD BLACKLIST", 0.0f, false) && s_BlIn[0] &&
                    g_Ui.lootFilterBlacklistCount < OAK_LOOT_FILTER_MAX)
                {
                    lstrcpynA(g_Ui.lootFilterBlacklist[g_Ui.lootFilterBlacklistCount++], s_BlIn, 48);
                    s_BlIn[0] = 0;
                }
                ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
                static char s_WlIn[48] = {};
                ImGui::PushFont(OakFont(g_FontSmall));
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##wl", "Whitelist pattern", s_WlIn, sizeof(s_WlIn));
                ImGui::PopFont();
                if (OakButton("ADD WHITELIST", 0.0f, false) && s_WlIn[0] &&
                    g_Ui.lootFilterWhitelistCount < OAK_LOOT_FILTER_MAX)
                {
                    lstrcpynA(g_Ui.lootFilterWhitelist[g_Ui.lootFilterWhitelistCount++], s_WlIn, 48);
                    s_WlIn[0] = 0;
                }
            }
            OakModuleEnd();
        }

        if (OakModule(i++, "Loot Extras", &g_Ui.batch4Esp.lootExtrasEnabled, 0, true, false))
        {
            OakNote(0, "Master toggle. Off hides quality, quantity, container loot, and contamination.");
            OakGroupLabel("Quality");
            OakCheckRow("Quality Badge", &g_Ui.batch4Esp.quality.enabled);
            if (g_Ui.batch4Esp.quality.enabled)
            {
                OakCheckRow("Show Badge Chip", &g_Ui.batch4Esp.quality.showBadge);
                OakSliderRow("Quality Probes/Frame", &g_Ui.batch4Esp.quality.maxProbesPerFrame, 8, OAK_BATCH4_MAX_QUALITY_PROBES, "");
            }
            OakGroupLabel("Quantity");
            OakCheckRow("Stack / Quantity", &g_Ui.batch4Esp.quantity.enabled);
            if (g_Ui.batch4Esp.quantity.enabled)
                OakSliderRow("Qty Probes/Frame", &g_Ui.batch4Esp.quantity.maxProbesPerFrame, 4, OAK_BATCH4_MAX_QTY_PROBES, "");
            OakGroupLabel("Containers");
            OakCheckRow("Container Contents", &g_Ui.batch4Esp.containerContents.enabled);
            if (g_Ui.batch4Esp.containerContents.enabled)
                OakSliderRow("Container Lines", &g_Ui.batch4Esp.containerContents.maxLines, 1, OAK_BATCH4_MAX_CONTAINER_LINES, "");
            OakGroupLabel("Contamination");
            OakCheckRow("Contamination ESP", &g_Ui.batch4Esp.contamination.enabled);
            if (g_Ui.batch4Esp.contamination.enabled)
            {
                int yCm = (int)(g_Ui.batch4Esp.contamination.yOffsetM * 100.f);
                if (OakSliderRow("Gas Ring Y Fine Tune", &yCm, -3000, 1500, "cm"))
                    g_Ui.batch4Esp.contamination.yOffsetM = yCm / 100.f;
                OakCheckRow("Ground Ring + Spokes", &g_Ui.batch4Esp.contamination.drawGroundRing);
                OakNote(0, "Main ring tracks your camera height + Y offset (stays visible under/over gas).");
            }
            OakModuleEnd();
        }

        if (OakModule(i++, "Vehicles", &g_Ui.espVehicles, 0, true, false))
        {
            OakSliderRow("Max Distance", &g_Ui.vehicleMaxDistance, 0, OAK_CAP_DIST_M, "m");
            OakGroupLabel("Draw");
            OakCheckColorRow("Box", &g_Ui.vehicleBox, &g_Ui.colorVehicleBox);
            OakCheckColorRow("Name", &g_Ui.vehicleName, &g_Ui.colorVehicleName);
            OakCheckRow("Distance", &g_Ui.vehicleDistance);
            OakModuleEnd();
        }

        if (OakModule(i++, "Containers", &g_Ui.espContainers, 0, true, false))
        {
            OakSliderRow("Max Distance", &g_Ui.containerMaxDistance, 0, OAK_CAP_DIST_M, "m");
            OakColorRow("Colour", &g_Ui.colorContainer);
            OakModuleEnd();
        }

        if (OakModule(i++, "Corpses", &g_Ui.espCorpses, 0, true, false))
        {
            OakSliderRow("Max Distance", &g_Ui.corpseMaxDistance, 0, OAK_CAP_DIST_M, "m");
            OakGroupLabel("Filters");
            OakCheckRow("Player Corpses", &g_Ui.corpseEsp.playerCorpses);
            OakCheckRow("Infected Corpses", &g_Ui.corpseEsp.infectedCorpses);
            OakGroupLabel("Draw");
            OakCheckRow("Name", &g_Ui.corpseEsp.name);
            OakCheckRow("Distance", &g_Ui.corpseEsp.distance);
            OakColorRow("Colour", &g_Ui.colorCorpse);
            OakModuleEnd();
        }

        if (OakModule(i++, "Traps", &g_Ui.espTraps, 0, true, false))
        {
            OakSliderRow("Max Distance", &g_Ui.trapMaxDistance, 0, OAK_CAP_DIST_M, "m");
            OakGroupLabel("Draw");
            OakCheckRow("Name", &g_Ui.trapEsp.name);
            OakCheckRow("Distance", &g_Ui.trapEsp.distance);
            OakColorRow("Colour", &g_Ui.colorTrap);
            OakModuleEnd();
        }

        if (OakModule(i++, "Heli / Crash ESP", &g_Ui.heliCrashEsp.enabled, 0, true, false))
        {
            OakSliderRow("Max Distance", &g_Ui.heliCrashEsp.maxDistanceM, 200, OAK_CAP_DIST_M, "m");
            ImVec4 hc(g_Ui.heliCrashEsp.color[0], g_Ui.heliCrashEsp.color[1], g_Ui.heliCrashEsp.color[2], g_Ui.heliCrashEsp.color[3]);
            OakColorRow("Colour", &hc);
            g_Ui.heliCrashEsp.color[0]=hc.x; g_Ui.heliCrashEsp.color[1]=hc.y; g_Ui.heliCrashEsp.color[2]=hc.z; g_Ui.heliCrashEsp.color[3]=hc.w;
            OakModuleEnd();
        }

        OakSectionLabel("HUD", 10.0f);

        if (OakModule(i++, "Threat Ring", &g_Ui.threatRing.enabled, 0, true, false))
        {
            OakSliderRow("Max Distance", &g_Ui.threatRing.maxDistanceM, 20, 500, "m");
            { int rr = (int)g_Ui.threatRing.ringRadiusPx; OakSliderRow("Ring Radius", &rr, 60, 300, "px"); g_Ui.threatRing.ringRadiusPx = (float)rr; }
            OakGroupLabel("Targets");
            OakCheckRow("Show Players", &g_Ui.threatRing.showPlayers);
            OakCheckRow("Show Zombies", &g_Ui.threatRing.showZombies);
            OakCheckRow("Zombies Only", &g_Ui.threatRing.zombiesOnly);
            OakGroupLabel("Display");
            OakCheckRow("Distance Labels", &g_Ui.threatRing.showDistLabels);
            OakCheckRow("Hide In Menu", &g_Ui.threatRing.hideInMenu);
            ImVec4 cp(g_Ui.threatRing.colorPlayer[0], g_Ui.threatRing.colorPlayer[1], g_Ui.threatRing.colorPlayer[2], g_Ui.threatRing.colorPlayer[3]);
            OakColorRow("Player Colour", &cp);
            g_Ui.threatRing.colorPlayer[0]=cp.x; g_Ui.threatRing.colorPlayer[1]=cp.y; g_Ui.threatRing.colorPlayer[2]=cp.z; g_Ui.threatRing.colorPlayer[3]=cp.w;
            ImVec4 cz(g_Ui.threatRing.colorZombie[0], g_Ui.threatRing.colorZombie[1], g_Ui.threatRing.colorZombie[2], g_Ui.threatRing.colorZombie[3]);
            OakColorRow("Zombie Colour", &cz);
            g_Ui.threatRing.colorZombie[0]=cz.x; g_Ui.threatRing.colorZombie[1]=cz.y; g_Ui.threatRing.colorZombie[2]=cz.z; g_Ui.threatRing.colorZombie[3]=cz.w;
            OakModuleEnd();
        }

        if (OakModule(i++, "Threat Counter", &g_Ui.threatCounter.enabled, 0, true, false))
        {
            OakSliderRow("Max Distance", &g_Ui.threatCounter.maxDistanceM, 20, 500, "m");
            OakCheckRow("Hide When Zero", &g_Ui.threatCounter.hideWhenZero);
            OakModuleEnd();
        }

        if (OakModule(i++, "Compass Strip", &g_Ui.compass.enabled, 0, true, false))
        {
            static const char* cpos[] = { "Top", "Bottom" };
            OakModeRow("Position", &g_Ui.compass.position, cpos, 2);
            OakCheckRow("Show Degrees", &g_Ui.compass.showDegrees);
            OakCheckRow("Waypoint Bearing", &g_Ui.compass.showWaypointBearing);
            OakModuleEnd();
        }

        if (OakModule(i++, "Waypoints", &g_Ui.miscDrawWaypoints, g_Ui.bindAddWaypoint, true, false))
        {
            OakKeyRow("Add Waypoint", &g_Ui.bindAddWaypoint, kBindWaypoint, 54.0f);
            OakWaypointSlots();
            OakModuleEnd();
        }

        if (OakModule(i++, "Death Marker", &g_Ui.deathMarker.enabled, 0, true, false))
        {
            OakCheckRow("Auto-Clear On Respawn", &g_Ui.deathMarker.autoClearOnRespawn);
            OakNote(0, "Places a waypoint at death location with timestamp.");
            OakModuleEnd();
        }

        if (OakModule(i++, "Grid Coordinates", &g_Ui.gridCoordsHud.enabled, 0, true, false))
        {
            static const char* corners[] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
            OakModeRow("Corner", &g_Ui.gridCoordsHud.corner, corners, 4);
            OakModuleEnd();
        }

        if (OakModule(i++, "Waypoint Distance HUD", &g_Ui.waypointHud.enabled, 0, true, false))
        {
            OakSliderRow("Active Waypoint", &g_Ui.waypointHud.activeIndex, 0, OAK_WAYPOINT_MAX - 1, "");
            static const char* corners[] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
            OakModeRow("Corner", &g_Ui.waypointHud.corner, corners, 4);
            OakCheckRow("Show Bearing", &g_Ui.waypointHud.showBearing);
            OakModuleEnd();
        }

        if (OakModule(i++, "Local Vitals HUD", &g_Ui.localVitalsHud, 0, true, false))
        {
            OakGroupLabel("Channels");
            OakCheckRow("Health", &g_Ui.barHealth);
            OakCheckRow("Blood", &g_Ui.barBlood);
            OakCheckRow("Shock", &g_Ui.barShock);
            OakCheckRow("Stamina", &g_Ui.barStamina);
            OakCheckRow("Hunger", &g_Ui.barHunger);
            OakCheckRow("Thirst", &g_Ui.barThirst);
            OakNote(0, "Channels also drive remote Health Bars under Players / Zombies.");
            OakModuleEnd();
        }

        OakModule(i++, "Local Weapon Ammo", &g_Ui.localWeaponAmmo, 0, false, false);

        if (OakModule(i++, "Reload Bar", &g_Ui.reloadBar.enabled, 0, true, false))
        {
            { int w = (int)g_Ui.reloadBar.width; OakSliderRow("Width", &w, 60, 240, "px"); g_Ui.reloadBar.width = (float)w; }
            { int h = (int)g_Ui.reloadBar.height; OakSliderRow("Height", &h, 2, 16, "px"); g_Ui.reloadBar.height = (float)h; }
            { int oy = (int)g_Ui.reloadBar.offsetY; OakSliderRow("Offset Y", &oy, 8, 60, "px"); g_Ui.reloadBar.offsetY = (float)oy; }
            OakModuleEnd();
        }

        if (OakModule(i++, "Crosshair", &g_Ui.crosshair, 0, true, false))
        {
            static const char* styles[] = { "Cross", "Gap-Cross", "Dot", "Circle", "T-Cross" };
            OakModeRow("Style", &g_Ui.crosshairStyle, styles, 5);
            OakSliderRow("Size", &g_Ui.crosshairSize, 2, 24, "px");
            OakSliderRow("Gap", &g_Ui.crosshairGap, 0, 16, "px");
            OakSliderRow("Thickness", &g_Ui.crosshairThickness, 1, 5, "px");
            OakColorRow("Colour", &g_Ui.colorCrosshair);
            OakGroupLabel("Enemy Highlight");
            OakCheckRow("Enemy Highlight", &g_Ui.crosshairHighlight.enabled);
            if (g_Ui.crosshairHighlight.enabled)
            {
                ImVec4 cd(g_Ui.crosshairHighlight.colorDefault[0], g_Ui.crosshairHighlight.colorDefault[1],
                    g_Ui.crosshairHighlight.colorDefault[2], g_Ui.crosshairHighlight.colorDefault[3]);
                OakColorRow("Default Colour", &cd);
                g_Ui.crosshairHighlight.colorDefault[0]=cd.x; g_Ui.crosshairHighlight.colorDefault[1]=cd.y;
                g_Ui.crosshairHighlight.colorDefault[2]=cd.z; g_Ui.crosshairHighlight.colorDefault[3]=cd.w;
                ImVec4 ch(g_Ui.crosshairHighlight.colorHighlight[0], g_Ui.crosshairHighlight.colorHighlight[1],
                    g_Ui.crosshairHighlight.colorHighlight[2], g_Ui.crosshairHighlight.colorHighlight[3]);
                OakColorRow("Enemy Colour", &ch);
                g_Ui.crosshairHighlight.colorHighlight[0]=ch.x; g_Ui.crosshairHighlight.colorHighlight[1]=ch.y;
                g_Ui.crosshairHighlight.colorHighlight[2]=ch.z; g_Ui.crosshairHighlight.colorHighlight[3]=ch.w;
            }
            OakModuleEnd();
        }

        if (OakModule(i++, "Bullet Tracers", &g_Ui.bulletTracers, 0, true, false))
        {
            OakSliderRow("Lifetime", &g_Ui.tracerLifetimeMs, 500, 6000, "ms");
            OakColorRow("Tracer", &g_Ui.colorTracer);
            OakModuleEnd();
        }

        if (OakModule(i++, "Impact Markers", &g_Ui.impactMarkers, 0, true, false))
        {
            OakSliderRow("Lifetime", &g_Ui.impactLifetimeMs, 300, 5000, "ms");
            OakColorRow("Marker", &g_Ui.colorImpact);
            OakModuleEnd();
        }

        if (OakModule(i++, "Shot Indicators", &g_Ui.shotIndicators, 0, true, false))
        {
            OakColorRow("Flash", &g_Ui.colorShotInd);
            OakModuleEnd();
        }

        if (OakModule(i++, "Hit Markers", &g_Ui.hitMarkers, 0, true, false))
        {
            OakColorRow("Marker", &g_Ui.colorHitMarker);
            OakModuleEnd();
        }

        if (OakModule(i++, "Grenade Trajectory", &g_Ui.grenadeTrajectory, 0, true, false))
        {
            OakColorRow("Arc", &g_Ui.colorGrenade);
            OakModuleEnd();
        }

        OakSectionLabel("Render", 10.0f);

        if (OakModule(i++, "Fullbright", &g_Ui.fullbright, g_Ui.bindFullbright, true, false))
        {
            OakKeyRow("Toggle Key", &g_Ui.bindFullbright, kBindFullbright, 54.0f);
            OakSliderRow("Brightness", &g_Ui.fullbrightBrightness, 5, 100, "%");
            OakModuleEnd();
        }

        if (OakModule(i++, "Night Boost", &g_Ui.nightBoost.enabled, 0, true, false))
        {
            OakSliderRow("Night Brightness", &g_Ui.nightBoost.brightness, 5, 100, "%");
            OakNote(0, "Auto fullbright during night only. Manual fullbright takes precedence.");
            OakModuleEnd();
        }

        if (OakModule(i++, "Shadow Chams", &g_Ui.shadowChams.enabled, 0, true, false))
        {
            OakGroupLabel("Targets");
            OakCheckRow("Players", &g_Ui.shadowChams.players);
            OakCheckRow("Zombies", &g_Ui.shadowChams.zombies);
            OakCheckRow("Items", &g_Ui.shadowChams.items);
            OakCheckRow("Hands / Viewmodel", &g_Ui.shadowChams.hands);
            OakGroupLabel("Fill");
            {
                static const char* patterns[] = { "Solid", "Pulse", "Scroll", "Rainbow", "Stripes" };
                OakModeRow("Pattern", &g_Ui.shadowChams.pattern, patterns, 5);
            }
            { int fa = (int)(g_Ui.shadowChams.fillAlpha * 100.f); if (OakSliderRow("Fill Alpha", &fa, 5, 95, "%")) g_Ui.shadowChams.fillAlpha = fa / 100.f; }
            { int ot = (int)(g_Ui.shadowChams.outlineThick * 10.f); if (OakSliderRow("Outline", &ot, 10, 60, "px")) g_Ui.shadowChams.outlineThick = ot / 10.f; }
            { int sp = (int)(g_Ui.shadowChams.animSpeed * 10.f); if (OakSliderRow("Anim Speed", &sp, 1, 80, "x")) g_Ui.shadowChams.animSpeed = sp / 10.f; }
            OakColorRow("Fill A", (ImVec4*)&g_Ui.shadowChams.color);
            OakColorRow("Fill B", (ImVec4*)&g_Ui.shadowChams.color2);
            OakColorRow("Outline", (ImVec4*)&g_Ui.shadowChams.outlineColor);
            OakNote(0, "Only players, zombies, items, and your hands — not terrain/fences.");
            OakModuleEnd();
        }

        if (OakModule(i++, "Wireframe", &g_Ui.worldMisc.wireframe, 0, true, false))
        {
            OakNote(0, "Renders the whole scene as wireframes.");
            OakModuleEnd();
        }

        if (OakModule(i++, "FOV Changer", &g_Ui.worldMisc.fovChanger, 0, true, false))
        {
            int fovDeg = (int)g_Ui.worldMisc.horizontalFov;
            if (OakSliderRow("Target FOV", &fovDeg, 70, 110, "deg"))
                g_Ui.worldMisc.horizontalFov = (float)fovDeg;
            OakCheckRow("Keep FOV while ADS", &g_Ui.worldMisc.fovKeepWhileAds);
            if (g_Ui.worldMisc.fovKeepWhileAds)
                OakNote(0, "ADS/optics keep hipfire FOV (no zoom-in).");
            OakNote(0, "Clamped 70-110 - wider breaks terrain/near-plane.");
            OakModuleEnd();
        }

        if (OakModule(i++, "Third Person", &g_Ui.worldMisc.thirdPerson, 0, true, false))
        {
            OakNote(0, "Unlocks engine 3PP (stock V). Camera distance is vanilla.");
            OakModuleEnd();
        }

        OakModule(i++, "No Grass", &g_Ui.miscNoGrass, 0, false, false);

        if (OakModule(i++, "Clear Weather", &g_Ui.worldMisc.clearWeather, 0, true, false))
        {
            OakNote(0, "Client visual - zeros rain/fog/overcast under WeatherController.");
            OakModuleEnd();
        }

        if (OakModule(i++, "Time Lock", &g_Ui.worldMisc.timeLock, 0, true, false))
        {
            int hour = (int)g_Ui.worldMisc.lockHour;
            if (OakSliderRow("Lock Hour", &hour, 0, 24, "h"))
                g_Ui.worldMisc.lockHour = (float)hour;
            OakModuleEnd();
        }
    }
    OakPanelEnd();
}

static void OakDrawCombat()
{
    if (!OakPanelBegin(kOakCombat, "##oak_combat", "Combat", 430.0f, 26.0f, 202.0f))
        return;

    if (OakPanelBody())
    {
        int i = 0;
        if (OakModule(i++, "Aimbot", &g_Ui.aimbotEnabled, g_Ui.aimbotKey, true, false))
        {
            {
                static const int keys[] = {
                    VK_XBUTTON1, VK_XBUTTON2, VK_MBUTTON, VK_MENU, VK_SHIFT, VK_CONTROL,
                    VK_RBUTTON, VK_LBUTTON
                };
                ImGui::Dummy(ImVec2(0.0f, OakS(5.0f)));
                const float w = ImGui::GetContentRegionAvail().x;
                const float h = OakS(19.0f);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(w, h));
                OakTxt(ImGui::GetWindowDrawList(), g_FontSmall, kOakFontLbl,
                       ImVec2(p.x, OakTxtY(p.y, h, kOakFontLbl)), kOakTxtMid, "Activation Key");
                const char* kt = OakKeyLabel(g_Ui.aimbotKey);
                const float est = OakMax(OakS(54.0f), OakTxtW(g_FontSmall, 9.0f, kt) + OakS(16.0f));
                ImGui::SetCursorScreenPos(ImVec2(p.x + w - est, p.y + (h - OakS(17.0f)) * 0.5f));
                if (OakKeyChip("ak", kt, false, 54.0f, nullptr))
                {
                    int idx = 0;
                    for (int k = 0; k < 8; k++)
                        if (keys[k] == g_Ui.aimbotKey) { idx = (k + 1) % 8; break; }
                    g_Ui.aimbotKey = keys[idx];
                }
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
            }
            OakKeyRow("Aim Assist Key", &g_Ui.bindAimAssist, kBindAimbot, 54.0f);
            OakGroupLabel("Targets");
            {
                static const char* filters[] = { "Players", "Zombies", "Both" };
                if (OakModeRow("Target Filter", &g_Ui.aimTargetFilter, filters, 3))
                    OakSyncAimbotBoolsFromFilter(g_Ui.aimTargetFilter, &g_Ui.aimbotPlayers, &g_Ui.aimbotZombies);
            }
            OakGroupLabel("Behaviour");
            OakSliderRow("FOV", &g_Ui.aimbotFov, 20, 500, "px");
            OakSliderRow("Smoothing", &g_Ui.aimbotSmooth, 1, 20, "");
            {
                static const char* bones[] = { "Head", "Chest" };
                OakModeRow("Bone", &g_Ui.aimbotBone, bones, 2);
            }
            OakSliderRow("Max Distance", &g_Ui.aimbotMaxDistance, 50, OAK_CAP_DIST_M, "m");
            OakCheckRow("Auto-Fire When Locked", &g_Ui.aimbotB4.autoFireWhenLocked);
            OakGroupLabel("Display");
            OakCheckRow("Draw FOV Circle", &g_Ui.aimbotDrawFov);
            OakCheckRow("FOV Target Highlight", &g_Ui.fovHighlight.enabled);
            if (g_Ui.fovHighlight.enabled || g_Ui.aimbotDrawFov)
            {
                ImVec4 fi(g_Ui.fovHighlight.colorIdle[0], g_Ui.fovHighlight.colorIdle[1],
                    g_Ui.fovHighlight.colorIdle[2], g_Ui.fovHighlight.colorIdle[3]);
                OakColorRow("FOV Idle", &fi);
                g_Ui.fovHighlight.colorIdle[0]=fi.x; g_Ui.fovHighlight.colorIdle[1]=fi.y;
                g_Ui.fovHighlight.colorIdle[2]=fi.z; g_Ui.fovHighlight.colorIdle[3]=fi.w;
                ImVec4 fa(g_Ui.fovHighlight.colorActive[0], g_Ui.fovHighlight.colorActive[1],
                    g_Ui.fovHighlight.colorActive[2], g_Ui.fovHighlight.colorActive[3]);
                OakColorRow("FOV Active", &fa);
                g_Ui.fovHighlight.colorActive[0]=fa.x; g_Ui.fovHighlight.colorActive[1]=fa.y;
                g_Ui.fovHighlight.colorActive[2]=fa.z; g_Ui.fovHighlight.colorActive[3]=fa.w;
            }
            OakGroupLabel("Advanced");
            {
                static const char* prios[] = { "Crosshair", "Distance", "Low HP" };
                OakModeRow("Priority", &g_Ui.aimExtras.priority, prios, 3);
            }
            OakSliderRow("Smooth Variance", &g_Ui.aimExtras.smoothVariancePct, 0, 50, "%");
            OakSliderRow("Reaction Delay", &g_Ui.aimExtras.reactionDelayMs, 0, 500, "ms");
            OakCheckRow("Require LOS (props)", &g_Ui.aimExtras.requireLos);
            OakNote(0, "Variance adds jitter without slowing base track speed. Reaction delay applies only on new target acquire.");
            if (g_Ui.aimbotFov > 320 && g_Ui.aimbotSmooth < 3)
                OakNote(2, "Wide and instant - unmistakable.");
            else if (g_Ui.aimbotFov > 240 && g_Ui.aimbotSmooth < 5)
                OakNote(1, "Large cone, low smoothing. Very visible.");
            else if (g_Ui.aimbotSmooth > 14)
                OakNote(1, "Heavy smoothing - may lag behind fast targets.");
            else
                OakNote(0, "Balanced - tracks without snapping.");
            OakModuleEnd();
        }

        if (OakModule(i++, "Silent Aim", &g_Ui.silentAim.enabled, g_Ui.bindSilentAim, true, false))
        {
            OakNote(0, "Expand chevron for FOV. Off while Magic Bullet is on. Bullets snap while you shoot.");
            OakKeyRow("Hold Key (0=always)", &g_Ui.bindSilentAim, kBindSilentAim, 54.0f);
            OakGroupLabel("Targets");
            OakCheckRow("Target Players", &g_Ui.silentAim.players);
            OakCheckRow("Target Zombies", &g_Ui.silentAim.zombies);
            OakGroupLabel("Behaviour");
            OakSliderRow("FOV", &g_Ui.silentAim.fovPx, 20, 800, "px");
            OakSliderRow("Max Distance", &g_Ui.silentAim.maxDistanceM, 50, OAK_CAP_DIST_M, "m");
            {
                static const char* bones[] = { "Head", "Chest" };
                OakModeRow("Bone", &g_Ui.silentAim.bone, bones, 2);
            }
            OakCheckRow("Bypass 25m Rule", &g_Ui.silentAim.bypass25m);
            OakGroupLabel("Display");
            OakCheckRow("Draw FOV", &g_Ui.silentAim.drawFov);
            OakCheckRow("Draw Lock", &g_Ui.silentAim.drawLock);
            OakModuleEnd();
        }

        if (OakModule(i++, "Magic Bullet", &g_Ui.magicBullet, g_Ui.bindMagicBullet, true, false))
        {
            OakKeyRow("Toggle Key", &g_Ui.bindMagicBullet, kBindMagic, 54.0f);
            OakGroupLabel("Behaviour");
            OakSliderRow("FOV", &g_Ui.magicBulletFov, 20, 800, "px");
            OakSliderRow("Max Distance", &g_Ui.magicBulletMaxDistance, 100, OAK_CAP_DIST_M, "m");
            OakCheckRow("Auto Fire", &g_Ui.magicBulletAutoFire);
            OakNote(0, "Holds LMB while any player is in Max Distance.");
            OakCheckRow("Bullet Chain", &g_Ui.magicBulletChain);
            OakNote(0, "One shot snaps through every targeted player in range.");
            OakGroupLabel("Display");
            OakCheckRow("Draw FOV Circle", &g_Ui.magicBulletDrawFov);
            OakModuleEnd();
        }

        if (OakModule(i++, "Triggerbot", &g_Ui.triggerbot.enabled, 0, true, false))
        {
            OakNote(0, "Clicks LMB while the crosshair is on a body. Delay = reaction before first shot.");
            OakGroupLabel("Targets");
            OakCheckRow("Target Players", &g_Ui.triggerbot.players);
            OakCheckRow("Target Zombies", &g_Ui.triggerbot.zombies);
            OakGroupLabel("Behaviour");
            OakSliderRow("Delay", &g_Ui.triggerbot.delayMs, 0, 500, "ms");
            OakSliderRow("Max Distance", &g_Ui.triggerbot.maxDistanceM, 20, OAK_CAP_DIST_M, "m");
            OakSliderRow("Deadzone", &g_Ui.triggerbot.deadzonePx, 4, 120, "px");
            OakCheckRow("Require ADS", &g_Ui.triggerbot.requireAds);
            OakCheckRow("Require LOS (props)", &g_Ui.triggerbot.requireLos);
            OakNote(0, "Prop-sphere LOS heuristic — skips obvious cover (cars/wrecks).");
            OakModuleEnd();
        }

        if (OakModule(i++, "Grenade Teleporter", &g_Ui.grenadeTeleport.enabled, 0, true, false))
        {
            OakNote(0, "Hold grenade: FOV + blue lock. Throw snaps mesh/VFX to feet (client-only).");
            OakGroupLabel("Targets");
            OakCheckRow("Target Players", &g_Ui.grenadeTeleport.players);
            OakCheckRow("Target Zombies", &g_Ui.grenadeTeleport.zombies);
            OakGroupLabel("Behaviour");
            OakSliderRow("FOV", &g_Ui.grenadeTeleport.fovPx, 10, 800, "px");
            OakSliderRow("Max Distance", &g_Ui.grenadeTeleport.maxDistanceM, 10, OAK_CAP_DIST_M, "m");
            OakGroupLabel("Display");
            OakCheckRow("Draw FOV Circle", &g_Ui.grenadeTeleport.drawFov);
            OakModuleEnd();
        }

        OakModule(i++, "Fast Bullets", &g_Ui.fastBullets, 0, false, false);
        OakModule(i++, "No Dispersion", &g_Ui.noDispersion, 0, false, false);
        OakModule(i++, "Perfect Ballistics", &g_Ui.perfectBallistics, 0, false, false);

        if (OakModule(i++, "No Recoil / Sway", &g_Ui.recoil.noRecoil, 0, true, false))
        {
            static bool s_WasRecoil = false;
            if (g_Ui.recoil.noRecoil && !s_WasRecoil)
                g_Ui.recoil.noSway = true; // enabling master also kills sway
            s_WasRecoil = g_Ui.recoil.noRecoil;
            OakSliderRow("Recoil Remaining", &g_Ui.recoil.recoilPct, 0, 100, "%");
            OakCheckRow("No Sway", &g_Ui.recoil.noSway);
            if (g_Ui.recoil.noSway)
                OakSliderRow("Sway Remaining", &g_Ui.recoil.swayPct, 0, 100, "%");
            OakNote(0, ImGuiMenu_RecoilNote());
            OakModuleEnd();
        }

    }
    OakPanelEnd();
}

static void OakDrawWorld()
{
    if (!OakPanelBegin(kOakWorld, "##oak_world", "Misc", 646.0f, 26.0f, 222.0f))
        return;

    if (OakPanelBody())
    {
        int i = 0;

        if (OakModule(i++, "Freecam", &g_Ui.miscFreecam, g_Ui.bindFreecam, true, false))
        {
            OakKeyRow("Toggle Key", &g_Ui.bindFreecam, kBindFreecam, 54.0f);
            OakSliderRow("Speed", &g_Ui.miscFreecamSpeed, 1, 60, "x");
            {
                ImGui::Dummy(ImVec2(0.0f, OakS(4.0f)));
                const float w = ImGui::GetContentRegionAvail().x;
                const float h = OakS(19.0f);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(w, h));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float box = OakS(15.0f);
                const ImVec2 bp(p.x, p.y + (h - box) * 0.5f);
                dl->AddRectFilled(bp, ImVec2(bp.x + box, bp.y + box), kOakSunken, OakS(3.0f));
                dl->AddRect(bp, ImVec2(bp.x + box, bp.y + box), kOakDead, OakS(3.0f), 0, 1.0f);
                OakTxt(dl, g_FontRegular, 11.0f, ImVec2(p.x + box + OakS(9.0f), OakTxtY(p.y, h, 11.0f)),
                       kOakDead, "Move Body");
            }
            OakNote(0, "F10 toggles (or bind). ESC exits. WASD fly, Space/E up, Q down. Combat/ADS forced off.");
            OakModuleEnd();
        }

        if (OakModule(i++, "Loot Magnet", &g_Ui.miscLootMagnet, g_Ui.bindLootMagnet, true, false))
        {
            OakKeyRow("Toggle Key", &g_Ui.bindLootMagnet, kBindLootMagnet, 54.0f);
            OakSliderRow("Range", &g_Ui.miscLootMagnetRange, 1, 40, "m");
            OakModuleEnd();
        }

        if (OakModule(i++, "Container Magnet", &g_Ui.miscContainerMagnet, g_Ui.bindContainerMagnet, true, false))
        {
            OakKeyRow("Toggle Key", &g_Ui.bindContainerMagnet, kBindContainerMagnet, 54.0f);
            OakSliderRow("Range", &g_Ui.miscContainerMagnetRange, 5, 200, "m");
            OakModuleEnd();
        }

        if (OakModule(i++, "Middle Click Despawn", &g_Ui.miscMiddleClickDespawn, g_Ui.bindMiddleClickDespawn, true, false))
        {
            OakKeyRow("Key", &g_Ui.bindMiddleClickDespawn, kBindDespawn, 54.0f);
            OakSliderRow("Range", &g_Ui.miscDespawnRange, 1, 100, "m");
            OakNote(0, "Deletes the entity under the crosshair: infected, players, fences, gates, walls.");
            OakModuleEnd();
        }

        bool pull = false;
        if (OakActionModule(i++, "Pull Base Part", g_Ui.bindPullBasePart, true, &pull))
        {
            OakKeyRow("Trigger Key", &g_Ui.bindPullBasePart, kBindPullPart, 54.0f);
            if (OakButton("PULL NEAREST FENCE / GATE", 0.0f, true))
                pull = true;
            OakNote(0, "Teleports an existing fence or gate to your feet.");
            OakModuleEnd();
        }
        if (pull)
            g_PullBasePartLatch = true;

        if (OakModule(i++, "Lag Switch", &g_Ui.exploits.warp, g_Ui.exploits.warpKey, true, false))
        {
            static const char* kLagModes[] = { "Safe (4s max)", "Balanced (7s)", "Aggressive (12s)" };
            OakModeRow("Max hold", &g_Ui.exploits.warpMode, kLagModes, 3);
            int cd = g_Ui.exploits.warpCooldownMs;
            if (OakSliderRow("Cooldown after release", &cd, 200, 8000, "ms"))
                g_Ui.exploits.warpCooldownMs = cd;
            OakKeyRow("Hold Key", &g_Ui.exploits.warpKey, kBindWarp, 54.0f);
            OakNote(1, "HOLD key DROPS outbound packets (works on localhost). Firewall helps remote only.");
            OakNote(1, g_Batch4StatusWarp);
            OakModuleEnd();
        }

        OakSectionLabel("Exploits", 10.0f);

        if (OakModule(i++, "Door Unlock", &g_Ui.exploits.doorUnlock, 0, true, false))
        {
            OakNote(0, "Enable near a locked door. Local NoBE: server opens when flag is on.");
            if (g_Batch4StatusDoor[0])
                OakNote(1, g_Batch4StatusDoor);
            OakModuleEnd();
        }

        if (OakModule(i++, "Grenade Through Walls", &g_Ui.exploits.grenadeThroughWalls, 0, true, false))
        {
            OakNote(1, "Client VS teleport to look+40m - not PhysX wall pen; server may reject damage.");
            OakModuleEnd();
        }
    }
    OakPanelEnd();
}

static void OakDrawBinds()
{
    if (!OakPanelBegin(kOakBinds, "##oak_binds", "Keybinds", 884.0f, 26.0f, 216.0f))
        return;

    if (OakPanelBody())
    {
        OakBindRow("Aimbot Toggle", &g_Ui.bindAimbot, kBindAimbot, false);
        OakBindRow("Magic Bullet", &g_Ui.bindMagicBullet, kBindMagic, false);
        OakBindRow("Silent Aim", &g_Ui.bindSilentAim, kBindSilentAim, false);
        OakBindRow("Fullbright", &g_Ui.bindFullbright, kBindFullbright, false);
        OakBindRow("ESP Toggle", &g_Ui.bindEsp, kBindEsp, false);
        OakBindRow("Loot Magnet", &g_Ui.bindLootMagnet, kBindLootMagnet, false);
        OakBindRow("Container Magnet", &g_Ui.bindContainerMagnet, kBindContainerMagnet, false);
        OakBindRow("Disable Overlays", &g_Ui.bindDisableOverlays, kBindOverlays, false);
        OakBindRow("Panic Mode", &g_Ui.bindPanic, kBindPanic, false);
        OakBindRow("Add Waypoint", &g_Ui.bindAddWaypoint, kBindWaypoint, false);
        OakBindRow("Copy Coordinates", &g_Ui.bindCopyCoords, kBindCoords, false);
        OakBindRow("Pull Base Part", &g_Ui.bindPullBasePart, kBindPullPart, false);
        OakBindRow("Freecam", &g_Ui.bindFreecam, kBindFreecam, false);
        OakBindRow("Middle Click", &g_Ui.bindMiddleClickDespawn, kBindDespawn, false);

        OakGroupLabel("Hold Mode");
        OakBlockBegin("##holdbits");
        {
            auto holdRow = [&](const char* label, unsigned bit) {
                bool on = (g_Ui.uiExtras.bindHoldMask & bit) != 0;
                if (OakCheckRow(label, &on))
                {
                    if (on) g_Ui.uiExtras.bindHoldMask |= bit;
                    else g_Ui.uiExtras.bindHoldMask &= ~bit;
                }
            };
            holdRow("ESP", 1u);
            holdRow("Aimbot", 2u);
            holdRow("Magic Bullet", 4u);
            holdRow("Fullbright", 8u);
            holdRow("Overlays", 16u);
            holdRow("Container Magnet", 64u);
            holdRow("Freecam", 128u);
        }
        OakBlockEnd();

        OakBlockBegin("##bindhint");
        OakNote(0, "Click to rebind, right-click to clear. Esc cancels.");
        OakBlockEnd();
    }
    OakPanelEnd();
}

static void OakCopyText(const char* text)
{
    if (!OpenClipboard(nullptr))
        return;
    EmptyClipboard();
    const int len = lstrlenA(text) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)len);
    if (h)
    {
        void* dst = GlobalLock(h);
        if (dst)
        {
            memcpy(dst, text, (size_t)len);
            GlobalUnlock(h);
            SetClipboardData(CF_TEXT, h);
        }
    }
    CloseClipboard();
}

static void OakDrawSession()
{
    if (!OakPanelBegin(kOakSession, "##oak_session", "Session", 1108.0f, 26.0f, 216.0f))
        return;

    if (OakPanelBody())
    {
        OakBlockBegin("##sess");
        const float w = ImGui::GetContentRegionAvail().x;
        const float half = (w - OakS(4.0f)) * 0.5f;

        OakSectionLabel("CONFIG", 0.0f);
        ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
        if (OakButton("SAVE", half, true)) ImGuiMenu_SaveConfig();
        ImGui::SameLine(0.0f, OakS(4.0f));
        if (OakButton("RELOAD", half, false)) ImGuiMenu_LoadConfig();

        OakSectionLabel("PROFILES", 9.0f);
        ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
        static const char* kProfiles[3] = { "LEGIT", "RAGE", "PVE" };
        const float third = (w - OakS(8.0f)) / 3.0f;
        for (int i = 0; i < 3; i++)
        {
            if (i) ImGui::SameLine(0.0f, OakS(4.0f));
            if (OakButton(kProfiles[i], third, false))
                ImGuiMenu_ApplyProfile(i);
        }
        ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
        static int s_Profile = 0;
        OakModeRow("Slot", &s_Profile, kProfiles, 3);
        ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
        if (OakButton("SAVE TO SLOT", half, false)) ImGuiMenu_SaveProfile(s_Profile);
        ImGui::SameLine(0.0f, OakS(4.0f));
        if (OakButton("LOAD SLOT", half, false)) ImGuiMenu_LoadProfile(s_Profile);

        ImGui::Dummy(ImVec2(0.0f, OakS(5.0f)));
        static char s_NamedProfile[48] = "custom";
        ImGui::PushFont(OakFont(g_FontSmall));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##profname", "Profile name", s_NamedProfile, sizeof(s_NamedProfile));
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
        if (OakButton("SAVE NAMED", half, false) && s_NamedProfile[0])
            ImGuiMenu_SaveNamedProfile(s_NamedProfile);
        ImGui::SameLine(0.0f, OakS(4.0f));
        if (OakButton("LOAD NAMED", half, false) && s_NamedProfile[0])
            ImGuiMenu_LoadNamedProfile(s_NamedProfile);
        ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
        if (OakButton("EXPORT", half, false)) ImGuiMenu_ExportConfig();
        ImGui::SameLine(0.0f, OakS(4.0f));
        if (OakButton("IMPORT", half, false)) ImGuiMenu_ImportConfig();

        // Own module — not buried under ESP Performance budgets.
        if (OakModule(0, "Frame Boost", &g_Ui.perf.frameBoost, 0, true, false))
        {
            OakNote(0, "In-world ~140 with Present free = DayZ main-thread limit (not VSync).");
            OakCheckRow("Limit FPS", &g_Ui.perf.fpsCapEnabled);
            if (g_Ui.perf.fpsCapEnabled)
                OakSliderRow("FPS Limit", &g_Ui.perf.fpsCap, OAK_CAP_FPS_LIMIT_MIN, OAK_CAP_FPS_LIMIT, "fps");
            OakCheckRow("Uncap FPS (no VSync)", &g_Ui.perf.unlockPresent);
            if (g_Ui.perf.unlockPresent)
            {
                OakCheckRow("Allow Tearing (beat monitor Hz)", &g_Ui.perf.allowTearing);
                OakCheckRow("Break Refresh Lock (exclusive FS)", &g_Ui.perf.breakRefreshLock);
                if (g_Ui.perf.breakRefreshLock)
                    ImGui::TextWrapped("Off recommended with NVIDIA filters — exclusive FS toggles can crash nvppex.");
            }
            OakCheckRow("High CPU/GPU Priority", &g_Ui.perf.highPerfMode);
            {
                const char* st = ImGuiMenu_GetPerfBoostStatus();
                if (st && st[0])
                    OakNote(0, st);
            }
            OakClampPerfLimits(&g_Ui.perf);
            OakModuleEnd();
        }

        static bool s_PerfOpen = false;
        if (OakModule(1, "ESP Performance", &s_PerfOpen, 0, true, false))
        {
            OakPerfStats ps = {};
            ImGuiMenu_GetPerfStats(&ps);
            char live[160];
            snprintf(live, sizeof(live), "Drawn %d/%d  |  Scanned %d/%d  |  %.1fms",
                ps.drawn, ps.effectiveDrawCap > 0 ? ps.effectiveDrawCap : g_Ui.perf.maxEntitiesDrawn,
                ps.scanned, ps.effectiveScanCap > 0 ? ps.effectiveScanCap : g_Ui.perf.maxEntitiesScanned,
                ps.frameMs);
            OakNote(0, live);
            OakSliderRow("Scan Rate", &g_Ui.perf.espUpdateHz, OAK_CAP_ESP_UPDATE_HZ_MIN, OAK_CAP_ESP_UPDATE_HZ, "Hz");
            OakSliderRow("Max Scanned", &g_Ui.perf.maxEntitiesScanned, 50, OAK_CAP_MAX_ENTITIES_SCANNED, "");
            OakSliderRow("Max Drawn", &g_Ui.perf.maxEntitiesDrawn, 20, OAK_CAP_MAX_ENTITIES_DRAWN, "");
            OakSliderRow("Max Labels", &g_Ui.perf.maxLabelsPerFrame, 10, OAK_CAP_MAX_LABELS, "");
            OakSliderRow("Max Skeletons", &g_Ui.perf.maxSkeletonsPerFrame, 5, OAK_CAP_MAX_SKELETONS, "");
            OakSliderRow("Max Loot", &g_Ui.perf.maxLootPerFrame, 20, OAK_CAP_MAX_LOOT, "");
            OakSliderRow("Max Corpses", &g_Ui.perf.maxCorpsesPerFrame, 10, OAK_CAP_MAX_CORPSES, "");
            OakSliderRow("Max Traps", &g_Ui.perf.maxTrapsPerFrame, 10, OAK_CAP_MAX_TRAPS, "");
            OakSliderRow("Max Players", &g_Ui.perf.maxPlayersDrawn, 5, OAK_CAP_MAX_PLAYERS_DRAWN, "");
            OakSliderRow("Max Zombies", &g_Ui.perf.maxZombiesDrawn, 5, OAK_CAP_MAX_ZOMBIES_DRAWN, "");
            OakSliderRow("Max Animals", &g_Ui.perf.maxAnimalsDrawn, 5, OAK_CAP_MAX_ANIMALS_DRAWN, "");
            OakSliderRow("Frame Budget", &g_Ui.perf.frameBudgetMs, OAK_CAP_FRAME_BUDGET_MS_MIN, OAK_CAP_FRAME_BUDGET_MS, "ms");
            OakGroupLabel("LOD Bands");
            OakSliderRow("Near", &g_Ui.perf.lodNearM, 25, OAK_CAP_DIST_M, "m");
            OakSliderRow("Mid", &g_Ui.perf.lodMidM, 50, OAK_CAP_DIST_M, "m");
            OakSliderRow("Far", &g_Ui.perf.lodFarM, 100, OAK_CAP_DIST_M, "m");
            OakCheckRow("Cull Offscreen", &g_Ui.perf.cullOffscreen);
            if (g_Ui.perf.maxEntitiesDrawn > OAK_WARN_MAX_ENTITIES_DRAWN)
                OakNote(1, "Draw cap above recommended — may impact FPS.");
            if (g_Ui.perf.maxLootPerFrame > OAK_WARN_MAX_LOOT)
                OakNote(1, "Loot cap above recommended.");
            if (g_Ui.perf.espUpdateHz > OAK_WARN_ESP_UPDATE_HZ)
                OakNote(1, "Scan rate above recommended.");
            OakClampPerfLimits(&g_Ui.perf);
            OakModuleEnd();
        }

        OakSectionLabel("UI", 9.0f);
        {
            static const char* panicScopes[] = { "All Features", "ESP Only", "Combat Only" };
            OakModeRow("Panic Scope", &g_Ui.uiExtras.panicScope, panicScopes, 3);
        }
        {
            ImVec4 acc(g_Ui.uiExtras.menuAccent[0], g_Ui.uiExtras.menuAccent[1],
                       g_Ui.uiExtras.menuAccent[2], g_Ui.uiExtras.menuAccent[3]);
            OakColorRow("Menu Accent", &acc);
            g_Ui.uiExtras.menuAccent[0] = acc.x;
            g_Ui.uiExtras.menuAccent[1] = acc.y;
            g_Ui.uiExtras.menuAccent[2] = acc.z;
            g_Ui.uiExtras.menuAccent[3] = acc.w;
        }
        if (OakModule(2, "Stream Proof", &g_Ui.worldMisc.streamProof, 0, true, false))
        {
            OakNote(0, "Not available on internal BE — Windows blanks the DayZ window. Leave OFF.");
            if (g_Ui.worldMisc.streamProof)
                g_Ui.worldMisc.streamProof = false;
            if (g_Ui.streamProof)
                g_Ui.streamProof = false;
            OakModuleEnd();
        }

        if (OakModule(3, "Disable Overlays", &g_Ui.miscDisableOverlays, g_Ui.bindDisableOverlays, true, false))
        {
            OakKeyRow("Toggle Key", &g_Ui.bindDisableOverlays, kBindOverlays, 54.0f);
            OakModuleEnd();
        }

        OakSliderRow("Profile Key Legit", &g_Ui.uiExtras.profileHotkey[0], 0, 255, "vk");
        OakSliderRow("Profile Key Rage", &g_Ui.uiExtras.profileHotkey[1], 0, 255, "vk");
        OakSliderRow("Profile Key PvE", &g_Ui.uiExtras.profileHotkey[2], 0, 255, "vk");

        OakSectionLabel("RESET SECTIONS", 9.0f);
        if (OakButton("PERF", third, false)) ImGuiMenu_ResetSection("performance");
        ImGui::SameLine(0.0f, OakS(4.0f));
        if (OakButton("LOOT", third, false)) ImGuiMenu_ResetSection("loot");
        ImGui::SameLine(0.0f, OakS(4.0f));
        if (OakButton("ESP STYLE", third, false)) ImGuiMenu_ResetSection("esp");
        ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
        if (OakButton("COMBAT", half, false)) ImGuiMenu_ResetSection("combat");
        ImGui::SameLine(0.0f, OakS(4.0f));
        if (OakButton("CORPSES", half, false)) ImGuiMenu_ResetSection("corpses");
        ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
        if (OakButton("TRAPS", half, false)) ImGuiMenu_ResetSection("traps");

        OakSectionLabel("FRIENDS", 9.0f);
        OakNote(0, "Friends are skipped by aimbot and magic bullet.");
        ImGui::Dummy(ImVec2(0.0f, OakS(4.0f)));
        static char s_FriendIn[64] = {};
        ImGui::PushFont(OakFont(g_FontSmall));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##friend", "Steam name or SteamID64", s_FriendIn, sizeof(s_FriendIn));
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
        if (OakButton("ADD FRIEND", 0.0f, false) && s_FriendIn[0] && g_Ui.friendCount < 16)
        {
            unsigned long long sid = 0;
            bool allDigit = true;
            for (int i = 0; s_FriendIn[i]; i++)
                if (s_FriendIn[i] < '0' || s_FriendIn[i] > '9') { allDigit = false; break; }
            if (allDigit && lstrlenA(s_FriendIn) >= 15)
                sid = _strtoui64(s_FriendIn, nullptr, 10);
            bool dup = false;
            for (int i = 0; i < g_Ui.friendCount; i++)
                if ((sid && g_Ui.friendSteamId[i] == sid) ||
                    (!sid && _stricmp(g_Ui.friendName[i], s_FriendIn) == 0))
                { dup = true; break; }
            if (!dup)
            {
                const int i = g_Ui.friendCount++;
                g_Ui.friendSteamId[i] = sid;
                lstrcpynA(g_Ui.friendName[i], s_FriendIn, 64);
                s_FriendIn[0] = 0;
            }
        }
        for (int i = 0; i < g_Ui.friendCount; i++)
        {
            ImGui::PushID(i);
            ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rh = OakS(17.0f);
            ImGui::Dummy(ImVec2(w, rh));
            OakTxt(ImGui::GetWindowDrawList(), g_FontRegular, 11.0f,
                   ImVec2(p.x, OakTxtY(p.y, rh, 11.0f)), kOakTxt, g_Ui.friendName[i]);
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - OakS(34.0f), p.y));
            const bool drop = OakButton("DEL", OakS(34.0f), false);
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
            if (drop)
            {
                for (int j = i; j < g_Ui.friendCount - 1; j++)
                {
                    g_Ui.friendSteamId[j] = g_Ui.friendSteamId[j + 1];
                    lstrcpynA(g_Ui.friendName[j], g_Ui.friendName[j + 1], 64);
                }
                g_Ui.friendCount--;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }

        OakSectionLabel("ACTIONS", 9.0f);
        ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
        if (OakButton("PANIC NOW", 0.0f, true))
            g_PanicLatch = true;
        ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));
        if (OakButton("RESET ALL SETTINGS", 0.0f, false))
            ImGuiMenu_ResetAll();

        OakDivider();
        ImGui::Dummy(ImVec2(0.0f, OakS(5.0f)));
        {
            char build[96];
            snprintf(build, sizeof(build), "BUILD %s", ImGuiMenu_Version());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(w, OakS(12.0f)));
            OakTxt(ImGui::GetWindowDrawList(), g_FontSmall, kOakFontMon, p, kOakTxtDim, build);
        }
        OakBlockEnd();
    }
    OakPanelEnd();
}

// ---- nav spine ------------------------------------------------------------

static bool OakNavRow(int idx, const char* title)
{
    ImGui::PushID(idx);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = OakS(26.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##nav", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = ImGui::IsItemClicked();
    const bool on = s_PanelShow[idx];

    if (on)
    {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kOakWash);
        dl->AddRectFilled(p, ImVec2(p.x + OakS(2.0f), p.y + h), kOakAcc);
    }
    else if (hovered)
    {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kOakHi);
    }

    const ImU32 fg = on ? kOakAcc : (hovered ? kOakTxt : kOakTxtMid);
    OakNavIcon(dl, ImVec2(p.x + OakS(18.0f), p.y + h * 0.5f), 13.0f, idx,
               on ? kOakAcc : (hovered ? IM_COL32(0xC8, 0xC8, 0xC8, 0xFF) : IM_COL32(0x6E, 0x6E, 0x6E, 0xFF)));
    OakTxt(dl, g_FontRegular, kOakFontUi, ImVec2(p.x + OakS(32.0f), OakTxtY(p.y, h, kOakFontUi)), fg, title);

    char count[16];
    snprintf(count, sizeof(count), "%d", s_PanelOn[idx]);
    OakTxt(dl, g_FontSmall, kOakFontMon,
           ImVec2(p.x + w - OakS(20.0f) - OakTxtW(g_FontSmall, kOakFontMon, count),
                  OakTxtY(p.y, h, kOakFontMon)), on ? kOakTxtMid : kOakTxtDim, count);

    const ImVec2 nc(p.x + w - OakS(11.0f), p.y + h * 0.5f);
    const float r = OakS(3.2f);
    const ImVec2 diamond[4] = {
        ImVec2(nc.x, nc.y - r), ImVec2(nc.x + r, nc.y), ImVec2(nc.x, nc.y + r), ImVec2(nc.x - r, nc.y)
    };
    dl->AddConvexPolyFilled(diamond, 4, on ? kOakAcc : kOakLineHi);

    ImGui::PopID();
    if (pressed)
        s_PanelShow[idx] = !s_PanelShow[idx];
    return pressed;
}

static void OakDrawNav()
{
    const float width = OakS(168.0f);
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (!s_NavPosSet)
    {
        s_NavPosX = OakS(26.0f);
        s_NavPosY = OakS(26.0f);
        s_NavPosSet = true;
    }
    s_NavPosX = OakClamp(s_NavPosX, OakS(0.0f), OakMax(OakS(0.0f), disp.x - width));
    s_NavPosY = OakClamp(s_NavPosY, OakS(0.0f), OakMax(OakS(0.0f), disp.y - OakS(kOakBarH)));
    ImGui::SetNextWindowPos(ImVec2(s_NavPosX, s_NavPosY), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, OakS(kOakBarH)),
                                        ImVec2(width, OakMax(OakS(160.0f), disp.y - OakS(48.0f))));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("##oak_nav", nullptr, flags))
    {
        ImGui::End();
        ImGui::PopStyleVar(4);
        return;
    }
    ImGui::SetWindowFontScale(s_UiScale);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);

    const float w = ImGui::GetContentRegionAvail().x;

    // Brand bar ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â hamburger glyph, name, build. Doubles as the drag handle.
    {
        const float h = OakS(kOakBarH);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##navdrag", ImVec2(w, h));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            s_NavPosX += d.x;
            s_NavPosY += d.y;
            s_NavPosSet = true;
            ImGui::SetWindowPos(ImVec2(s_NavPosX, s_NavPosY));
        }
        else
        {
            const ImVec2 wp = ImGui::GetWindowPos();
            s_NavPosX = wp.x;
            s_NavPosY = wp.y;
            s_NavPosSet = true;
        }
        const float gx = p.x + OakS(10.0f);
        const float gy = p.y + h * 0.5f;
        for (int i = 0; i < 3; i++)
            dl->AddLine(ImVec2(gx, gy + (float)(i - 1) * OakS(4.0f)),
                        ImVec2(gx + OakS(12.0f), gy + (float)(i - 1) * OakS(4.0f)),
                        IM_COL32(255, 255, 255, 166), OakMax(1.0f, OakS(1.2f)));
        OakTxt(dl, g_FontMedium, kOakFontTtl, ImVec2(gx + OakS(21.0f), OakTxtY(p.y, h, kOakFontTtl)),
               kOakTxt, "Main Menu");
        char ver[64];
        snprintf(ver, sizeof(ver), "V%s", ImGuiMenu_Version());
        OakTxt(dl, g_FontSmall, kOakFontMon,
               ImVec2(p.x + w - OakS(9.0f) - OakTxtW(g_FontSmall, kOakFontMon, ver),
                      OakTxtY(p.y, h, kOakFontMon)), kOakTxtDim, ver);
    }

    // Live filter across every panel.
    {
        ImGui::Dummy(ImVec2(0.0f, OakS(9.0f)));
        ImGui::Indent(OakS(10.0f));
        ImGui::PushFont(OakFont(g_FontSmall));
        ImGui::SetNextItemWidth(w - OakS(20.0f));
        ImGui::InputTextWithHint("##search", "Search", s_Search, sizeof(s_Search));
        ImGui::PopFont();
        ImGui::Unindent(OakS(10.0f));
    }

    ImGui::Dummy(ImVec2(0.0f, OakS(8.0f)));
    ImGui::Indent(OakS(10.0f));
    OakSectionLabel("MODULES", 0.0f);
    ImGui::Unindent(OakS(10.0f));
    ImGui::Dummy(ImVec2(0.0f, OakS(3.0f)));

    OakNavRow(kOakVisuals, "Visuals");
    OakNavRow(kOakCombat, "Combat");
    OakNavRow(kOakWorld, "Misc");
    OakNavRow(kOakBinds, "Keybinds");
    OakNavRow(kOakSession, "Session");

    ImGui::Dummy(ImVec2(0.0f, OakS(10.0f)));
    ImGui::Indent(OakS(12.0f));
    OakSectionLabel("SETTINGS", 0.0f);

    const float inner = w - OakS(24.0f);

    // UI scale ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â label, ghost reset, readout, then the track.
    {
        ImGui::Dummy(ImVec2(0.0f, OakS(4.0f)));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = OakS(15.0f);
        ImGui::Dummy(ImVec2(inner, h));
        OakTxt(dl, g_FontSmall, kOakFontLbl, ImVec2(p.x, OakTxtY(p.y, h, kOakFontLbl)), kOakTxtMid, "UI Scale");
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", s_UiScalePct);
        OakTxt(dl, g_FontSmall, kOakFontLbl,
               ImVec2(p.x + inner - OakTxtW(g_FontSmall, kOakFontLbl, pct), OakTxtY(p.y, h, kOakFontLbl)),
               kOakTxtMid, pct);
        if (OakGhost("Reset", ImVec2(p.x + OakS(52.0f), p.y)))
            s_UiScalePct = 100;
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
        ImGui::PushID("scale");
        ImGui::PushItemWidth(inner);
        int pctv = s_UiScalePct;
        {
            // Inline track: same visual language as the drawer sliders.
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            const float th = OakS(14.0f);
            ImGui::InvisibleButton("##t", ImVec2(inner, th));
            float t = (float)(pctv - 60) / 140.0f;
            if (ImGui::IsItemActive() && inner > 1.0f)
            {
                t = OakClamp((ImGui::GetIO().MousePos.x - sp.x) / inner, 0.0f, 1.0f);
                pctv = 60 + (int)(t * 140.0f / 5.0f + 0.5f) * 5;
            }
            t = OakClamp((float)(pctv - 60) / 140.0f, 0.0f, 1.0f);
            const float cy = sp.y + th * 0.5f;
            const float bh = OakMax(1.0f, OakS(3.0f));
            dl->AddRectFilled(ImVec2(sp.x, cy - bh * 0.5f), ImVec2(sp.x + inner, cy + bh * 0.5f), kOakLineHi, bh * 0.5f);
            dl->AddRectFilled(ImVec2(sp.x, cy - bh * 0.5f), ImVec2(sp.x + inner * t, cy + bh * 0.5f), kOakAcc, bh * 0.5f);
            dl->AddCircleFilled(ImVec2(sp.x + inner * t, cy), OakS(4.5f), kOakAcc, 16);
        }
        ImGui::PopItemWidth();
        ImGui::PopID();
        s_UiScalePct = pctv;
    }

    // Menu key ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â cycles a short, safe list.
    {
        ImGui::Dummy(ImVec2(0.0f, OakS(9.0f)));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = OakS(19.0f);
        ImGui::Dummy(ImVec2(inner, h));
        OakTxt(dl, g_FontSmall, kOakFontLbl, ImVec2(p.x, OakTxtY(p.y, h, kOakFontLbl)), kOakTxtMid, "Menu Key");
        ImGui::SetCursorScreenPos(ImVec2(p.x + inner - OakS(54.0f), p.y + (h - OakS(17.0f)) * 0.5f));
        if (OakKeyChip("mk", OakKeyLabel(g_Ui.menuKey), false, 54.0f, nullptr))
        {
            static const int keys[] = { 'K', VK_INSERT, VK_HOME, VK_DELETE, VK_F1, VK_F2, VK_END };
            int idx = 0;
            for (int i = 0; i < 7; i++)
                if (keys[i] == g_Ui.menuKey) { idx = (i + 1) % 7; break; }
            g_Ui.menuKey = keys[idx];
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    }

    // Stream proof — internal BE blanks the DayZ window; force off.
    {
        ImGui::Dummy(ImVec2(0.0f, OakS(7.0f)));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = OakS(17.0f);
        g_Ui.streamProof = false;
        g_Ui.worldMisc.streamProof = false;
        ImGui::Dummy(ImVec2(inner, h));
        OakTxt(dl, g_FontSmall, kOakFontLbl, ImVec2(p.x, OakTxtY(p.y, h, kOakFontLbl)), kOakTxtDim, "Stream Proof (N/A internal)");
        const float box = OakS(15.0f);
        OakCheckBox(dl, ImVec2(p.x + inner - box, p.y + (h - box) * 0.5f), box, false, false);
    }

    OakDivider();

    // Position readout + copy.
    {
        ImGui::Dummy(ImVec2(0.0f, OakS(7.0f)));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = OakS(15.0f);
        ImGui::Dummy(ImVec2(inner, h));
        OakTxt(dl, g_FontSmall, kOakFontLbl, ImVec2(p.x, OakTxtY(p.y, h, kOakFontLbl)), kOakTxtMid, "Position");
        if (OakGhost("Copy", ImVec2(p.x + inner - OakS(36.0f), p.y)))
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", g_Ui.posX, g_Ui.posY, g_Ui.posZ);
            OakCopyText(buf);
            g_CopyCoordsLatch = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));

        ImGui::Dummy(ImVec2(0.0f, OakS(5.0f)));
        const ImVec2 q = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(inner, OakS(24.0f)));
        const char* axis[3] = { "X", "Y", "Z" };
        const float vals[3] = { g_Ui.posX, g_Ui.posY, g_Ui.posZ };
        for (int i = 0; i < 3; i++)
        {
            const float cx = q.x + inner / 3.0f * (float)i;
            char v[24];
            snprintf(v, sizeof(v), "%.1f", vals[i]);
            OakTxt(dl, g_FontSmall, 8.0f, ImVec2(cx, q.y), kOakTxtDim, axis[i]);
            OakTxt(dl, g_FontSmall, kOakFontLbl, ImVec2(cx, q.y + OakS(11.0f)), kOakTxtMid, v);
        }
    }

    ImGui::Unindent(OakS(12.0f));
    ImGui::Dummy(ImVec2(0.0f, OakS(9.0f)));

    // Chamfered nav backdrop, behind everything drawn above.
    {
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
        const float cut = OakS(kOakCut);
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(ImVec2(p0.x + OakS(3.0f), p0.y + OakS(5.0f)),
                          ImVec2(p1.x + OakS(3.0f), p1.y + OakS(6.0f)), IM_COL32(0, 0, 0, 90));
        OakChamfer(dl, p0, p1, cut, kOakPanel, 0);
        OakBarShape(dl, p0, sz.x, OakS(kOakBarH), cut, kOakBar);
        dl->AddLine(ImVec2(p0.x, p0.y + OakS(kOakBarH)), ImVec2(p1.x, p0.y + OakS(kOakBarH)), kOakLine, 1.0f);
        OakChamfer(dl, p0, p1, cut, 0, kOakLine);
        dl->ChannelsMerge();
    }

    ImGui::End();
    ImGui::PopStyleVar(4);
}

// ---- HUD ------------------------------------------------------------------

static void OakDrawHud()
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImFont* mono = OakFont(g_FontSmall);

    struct Entry { const char* name; bool on; };
    const Entry entries[] = {
        { "ESP",              g_Ui.espEnabled },
        { "AIMBOT",           g_Ui.aimbotEnabled },
        { "MAGIC BULLET",     g_Ui.magicBullet },
        { "FULLBRIGHT",       g_Ui.fullbright },
        { "FREECAM",          g_Ui.miscFreecam },
        { "LAG SWITCH",       g_Ui.exploits.warp },
        { "LOOT MAGNET",      g_Ui.miscLootMagnet },
        { "CONTAINER MAGNET", g_Ui.miscContainerMagnet },
        { "DESPAWN",          g_Ui.miscMiddleClickDespawn },
        { "NO GRASS",         g_Ui.miscNoGrass },
        { "OVERLAYS OFF",     g_Ui.miscDisableOverlays },
    };

    float y = OakS(12.0f);
    for (int i = 0; i < (int)(sizeof(entries) / sizeof(entries[0])); i++)
    {
        if (!entries[i].on)
            continue;
        const float sz = OakS(11.0f);
        const float tw = mono->CalcTextSizeA(sz, FLT_MAX, 0.0f, entries[i].name).x;
        const float right = disp.x - OakS(16.0f);
        dl->AddText(mono, sz, ImVec2(right - tw - OakS(8.0f), y), IM_COL32(255, 255, 255, 217), entries[i].name);
        dl->AddRectFilled(ImVec2(right - OakS(2.0f), y + OakS(1.0f)), ImVec2(right, y + sz), kOakAcc);
        y += sz + OakS(3.0f);
    }

    // POS / SYS telemetry, bottom-left.
    char pos[96];
    char sys[96];
    snprintf(pos, sizeof(pos), "%.1f  %.1f  %.1f", g_Ui.posX, g_Ui.posY, g_Ui.posZ);
    snprintf(sys, sizeof(sys), "%.0f FPS   %u F", ImGui::GetIO().Framerate, ImGuiMenu_FrameCount());
    const char* tag[2] = { "POS", "SYS" };
    const char* val[2] = { pos, sys };
    float ty = disp.y - OakS(38.0f);
    for (int i = 0; i < 2; i++)
    {
        const float x = OakS(16.0f);
        dl->AddRectFilled(ImVec2(x, ty + OakS(1.0f)), ImVec2(x + OakS(2.0f), ty + OakS(11.0f)), kOakAccDim);
        dl->AddText(mono, OakS(kOakFontMon), ImVec2(x + OakS(7.0f), ty + OakS(1.5f)), kOakTxtDim, tag[i]);
        dl->AddText(mono, OakS(10.0f), ImVec2(x + OakS(34.0f), ty), IM_COL32(255, 255, 255, 235), val[i]);
        ty += OakS(15.0f);
    }
}

// ---- style + entry point --------------------------------------------------

static void ApplyStyle()
{
    // Monochrome tactical: flat near-black surfaces separated by tone alone,
    // white as the only accent. Geometry is drawn by hand; the ImGui style just
    // has to keep the stock widgets (inputs, popups) from clashing.
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 3.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.WindowPadding = ImVec2(0.0f, 0.0f);
    s.FramePadding = ImVec2(7.0f, 4.0f);
    s.ItemSpacing = ImVec2(6.0f, 4.0f);
    s.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
    s.CellPadding = ImVec2(4.0f, 2.0f);
    s.ScrollbarSize = 7.0f;
    s.GrabMinSize = 9.0f;
    s.IndentSpacing = 12.0f;

    ImVec4* c = s.Colors;
    const ImVec4 panel  = ImVec4(0.071f, 0.071f, 0.071f, 0.96f);
    const ImVec4 sunken = ImVec4(0.000f, 0.000f, 0.000f, 0.35f);
    const ImVec4 line   = ImVec4(0.157f, 0.157f, 0.157f, 1.00f);
    const ImVec4 lineHi = ImVec4(0.227f, 0.227f, 0.227f, 1.00f);
    const ImVec4 txt    = ImVec4(0.910f, 0.910f, 0.910f, 1.00f);
    const ImVec4 txtMid = ImVec4(0.565f, 0.565f, 0.565f, 1.00f);
    const ImVec4 acc    = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);

    c[ImGuiCol_Text]                  = txt;
    c[ImGuiCol_TextDisabled]          = ImVec4(0.361f, 0.361f, 0.361f, 1.00f);
    c[ImGuiCol_WindowBg]              = panel;
    c[ImGuiCol_ChildBg]               = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]               = ImVec4(0.067f, 0.067f, 0.067f, 0.98f);
    c[ImGuiCol_Border]                = line;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = sunken;
    c[ImGuiCol_FrameBgHovered]        = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_TitleBg]               = panel;
    c[ImGuiCol_TitleBgActive]         = panel;
    c[ImGuiCol_TitleBgCollapsed]      = panel;
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.106f, 0.106f, 0.106f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]         = lineHi;
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = acc;
    c[ImGuiCol_CheckMark]             = acc;
    c[ImGuiCol_SliderGrab]            = acc;
    c[ImGuiCol_SliderGrabActive]      = acc;
    c[ImGuiCol_Button]                = ImVec4(0.122f, 0.122f, 0.122f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.173f, 0.173f, 0.173f, 1.00f);
    c[ImGuiCol_ButtonActive]          = sunken;
    c[ImGuiCol_Header]                = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_HeaderActive]          = ImVec4(1, 1, 1, 0.16f);
    c[ImGuiCol_Separator]             = line;
    c[ImGuiCol_SeparatorHovered]      = lineHi;
    c[ImGuiCol_SeparatorActive]       = acc;
    c[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(1, 1, 1, 0.18f);
    c[ImGuiCol_Tab]                   = ImVec4(0.098f, 0.098f, 0.098f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_TabActive]             = ImVec4(0.145f, 0.145f, 0.145f, 1.00f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.098f, 0.098f, 0.098f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.129f, 0.129f, 0.129f, 1.00f);
    c[ImGuiCol_PlotLines]             = txtMid;
    c[ImGuiCol_PlotHistogram]         = acc;
    c[ImGuiCol_TextSelectedBg]        = ImVec4(1, 1, 1, 0.20f);
    c[ImGuiCol_NavHighlight]          = ImVec4(1, 1, 1, 0.35f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.15f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.55f);
}

static void DrawMenu()
{
    s_UiScalePct = (s_UiScalePct < 60) ? 60 : (s_UiScalePct > 200 ? 200 : s_UiScalePct);
    s_UiScale = (float)s_UiScalePct / 100.0f;

    OakDrawVisuals();
    OakDrawCombat();
    OakDrawWorld();
    OakDrawBinds();
    OakDrawSession();
    OakDrawNav();
    OakDrawHud();
}

