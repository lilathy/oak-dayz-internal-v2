// UI-only DayZ overlay: DX11 Present hook + ImGui panel shell.
// No ESP, aimbot, shared-memory cheat config, or feature logic.
#include "imgui_menu.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

typedef HRESULT(WINAPI* tPresent)(IDXGISwapChain*, UINT, UINT);

static HMODULE g_Module = nullptr;
static bool g_Running = true;
static bool g_Initialized = false;
static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static ID3D11RenderTargetView* g_Rtv = nullptr;
static tPresent oPresent = nullptr;
static void** g_VTable = nullptr;

static HRESULT WINAPI hkPresent(IDXGISwapChain* swap, UINT sync, UINT flags)
{
    if (!g_Initialized)
    {
        ImGuiMenu_Log("overlay: first Present — init D3D/ImGui");
        if (SUCCEEDED(swap->GetDevice(__uuidof(ID3D11Device), (void**)&g_Device)) && g_Device)
        {
            g_Device->GetImmediateContext(&g_Context);

            DXGI_SWAP_CHAIN_DESC desc = {};
            swap->GetDesc(&desc);

            char buf[160];
            wsprintfA(buf, "overlay: screen %ux%u hwnd=0x%p",
                desc.BufferDesc.Width, desc.BufferDesc.Height, desc.OutputWindow);
            ImGuiMenu_Log(buf);

            ID3D11Texture2D* bb = nullptr;
            if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb)
            {
                g_Device->CreateRenderTargetView(bb, nullptr, &g_Rtv);
                bb->Release();
            }

            if (g_Rtv && ImGuiMenu_Init(desc.OutputWindow, g_Device, g_Context))
                ImGuiMenu_Log("overlay: imgui ok");
            else
                ImGuiMenu_Log("overlay: imgui init FAILED");

            g_Initialized = true;
        }
        else
        {
            ImGuiMenu_Log("overlay: GetDevice failed");
        }
    }

    if (g_Initialized && g_Rtv)
        ImGuiMenu_OnPresent(g_Rtv);

    if (GetAsyncKeyState(VK_END) & 1)
    {
        ImGuiMenu_Log("overlay: END pressed — unload requested");
        g_Running = false;
    }

    return oPresent(swap, sync, flags);
}

static HWND FindDayZWindow()
{
    HWND hwnd = FindWindowA("DayZ", nullptr);
    if (hwnd) return hwnd;
    hwnd = FindWindowA(nullptr, "DayZ");
    if (hwnd) return hwnd;
    return GetForegroundWindow();
}

static DWORD WINAPI MainThread(LPVOID)
{
    ImGuiMenu_Log("overlay: MainThread start");
    Sleep(3000);

    HWND hwnd = nullptr;
    for (int i = 0; i < 60 && !(hwnd = FindDayZWindow()); i++)
        Sleep(500);

    if (!hwnd)
    {
        ImGuiMenu_Log("overlay: DayZ window not found");
        return 1;
    }

    char buf[128];
    wsprintfA(buf, "overlay: window=0x%p", hwnd);
    ImGuiMenu_Log(buf);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &swap, &dev, &fl, &ctx);

    if (FAILED(hr) || !swap)
    {
        wsprintfA(buf, "overlay: dummy swapchain failed hr=0x%X", hr);
        ImGuiMenu_Log(buf);
        return 1;
    }

    void** vt = *reinterpret_cast<void***>(swap);
    g_VTable = vt;
    oPresent = (tPresent)vt[8];

    DWORD oldProt = 0;
    if (!VirtualProtect(&vt[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
    {
        ImGuiMenu_Log("overlay: VirtualProtect failed");
        swap->Release();
        ctx->Release();
        dev->Release();
        return 1;
    }
    vt[8] = (void*)hkPresent;
    VirtualProtect(&vt[8], sizeof(void*), oldProt, &oldProt);
    ImGuiMenu_Log("overlay: Present hooked");

    // Release dummy objects; vtable slot stays patched for the real game swapchain
    // (same pattern as the original project — game uses same DXGI factory path).
    // NOTE: DayZ may use a different swapchain instance; hooking via dummy vtable
    // only works if the game shares the same vtable pointer (typical for D3D11).
    swap->Release();
    ctx->Release();
    dev->Release();

    while (g_Running)
        Sleep(200);

    if (g_VTable && oPresent)
    {
        DWORD op = 0;
        if (VirtualProtect(&g_VTable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &op))
        {
            g_VTable[8] = (void*)oPresent;
            VirtualProtect(&g_VTable[8], sizeof(void*), op, &op);
            ImGuiMenu_Log("overlay: Present unhooked");
        }
    }

    ImGuiMenu_Shutdown();
    if (g_Rtv) { g_Rtv->Release(); g_Rtv = nullptr; }
    if (g_Context) { g_Context->Release(); g_Context = nullptr; }
    if (g_Device) { g_Device->Release(); g_Device = nullptr; }

    ImGuiMenu_Log("overlay: shutdown complete");
    FreeLibraryAndExitThread(g_Module, 0);
    return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_Module = hModule;
        ImGuiMenu_Log("overlay: DLL_PROCESS_ATTACH");
        HANDLE t = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        else ImGuiMenu_Log("overlay: CreateThread failed");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_Running = false;
        ImGuiMenu_Log("overlay: DLL_PROCESS_DETACH");
    }
    return TRUE;
}
