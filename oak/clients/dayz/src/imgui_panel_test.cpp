// Standalone DX11 host to verify the ImGui Oak panel shell (no DayZ, no injection).
#include "imgui_menu.h"
#include "imgui.h"

#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")

static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static IDXGISwapChain* g_Swap = nullptr;
static ID3D11RenderTargetView* g_Rtv = nullptr;
static bool g_Running = true;
static int g_Frames = 0;
static bool g_TestPass = false;

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DESTROY)
    {
        g_Running = false;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_SIZE && g_Device && wParam != SIZE_MINIMIZED)
    {
        if (g_Rtv) { g_Rtv->Release(); g_Rtv = nullptr; }
        g_Swap->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* bb = nullptr;
        g_Swap->GetBuffer(0, IID_PPV_ARGS(&bb));
        if (bb)
        {
            g_Device->CreateRenderTargetView(bb, nullptr, &g_Rtv);
            bb->Release();
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool CreateDevice(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &g_Swap, &g_Device, &fl, &g_Context);
    if (FAILED(hr))
        return false;

    ID3D11Texture2D* bb = nullptr;
    g_Swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (!bb) return false;
    g_Device->CreateRenderTargetView(bb, nullptr, &g_Rtv);
    bb->Release();
    return g_Rtv != nullptr;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int)
{
    // Optional: auto-close after N frames for CI-style check
    bool headlessBrief = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 0; i < argc; i++)
        if (argv && lstrcmpiW(argv[i], L"--auto-test") == 0)
            headlessBrief = true;
    if (argv) LocalFree(argv);

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hi, nullptr, nullptr, nullptr, nullptr, L"OakImGuiTest", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Oak ImGui Panel Test",
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, hi, nullptr);

    if (!CreateDevice(hwnd) || !ImGuiMenu_Init(hwnd, g_Device, g_Context))
    {
        MessageBoxW(nullptr, L"Failed to init DX11/ImGui", L"OakImGuiTest", MB_ICONERROR);
        return 1;
    }

    // Present-path frames are skipped until the backend objects exist.
    ImGuiMenu_Warmup();

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    FILE* log = nullptr;
    freopen_s(&log, "oak_imgui_test.log", "w", stdout);

    MSG msg = {};
    while (g_Running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                g_Running = false;
        }
        if (!g_Running) break;

        const float clear[4] = { 0.06f, 0.07f, 0.06f, 1.0f };
        g_Context->OMSetRenderTargets(1, &g_Rtv, nullptr);
        g_Context->ClearRenderTargetView(g_Rtv, clear);

        ImGuiMenu_SetOpen(true);
        ImGuiMenu_OnPresent(g_Rtv);

        // Validate draw data after Present path builds it
        ImDrawData* dd = ImGui::GetDrawData();
        if (dd && dd->TotalVtxCount > 100)
        {
            g_TestPass = true;
            if (g_Frames == 30)
                printf("PASS: ImGui draw data vtx=%d idx=%d cmdlists=%d menu_open=%d\n",
                    dd->TotalVtxCount, dd->TotalIdxCount, dd->CmdListsCount, ImGuiMenu_IsOpen() ? 1 : 0);
        }

        g_Swap->Present(1, 0);
        g_Frames++;

        if (headlessBrief && g_Frames >= 60)
        {
            printf(g_TestPass ? "RESULT: PASS\n" : "RESULT: FAIL\n");
            if (log) fflush(log);
            g_Running = false;
        }
    }

    printf("frames=%d pass=%d\n", g_Frames, g_TestPass ? 1 : 0);
    if (log) fflush(log);

    ImGuiMenu_Shutdown();
    if (g_Rtv) g_Rtv->Release();
    if (g_Swap) g_Swap->Release();
    if (g_Context) g_Context->Release();
    if (g_Device) g_Device->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hi);

    return g_TestPass ? 0 : 2;
}
