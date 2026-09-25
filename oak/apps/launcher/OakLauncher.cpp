// OakLauncher — one elevated click: kill → stage → map driver → DayZ → PASS/FAIL
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <stdio.h>
#include <string.h>

static void LogLine(const char* s)
{
    puts(s);
    fflush(stdout);
}

static BOOL IsAdmin()
{
    BOOL ok = FALSE;
    PSID g = NULL;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &g))
    {
        CheckTokenMembership(NULL, g, &ok);
        FreeSid(g);
    }
    return ok;
}

static void RelaunchElevated()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.hwnd = NULL;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei))
        ExitProcess(0);
}

static void KillImage(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, name) == 0)
            {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) { TerminateProcess(h, 1); CloseHandle(h); }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static BOOL FileExistsA(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL CopyForce(const char* src, const char* dst)
{
    if (!FileExistsA(src)) return FALSE;
    return CopyFileA(src, dst, FALSE) != 0;
}

static BOOL FindFirstExisting(char* out, size_t outLen, const char** cands, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (FileExistsA(cands[i]))
        {
            strncpy(out, cands[i], outLen - 1);
            out[outLen - 1] = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static DWORD FileSizeA(const char* p)
{
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(h, NULL);
    CloseHandle(h);
    return sz == INVALID_FILE_SIZE ? 0 : sz;
}

static BOOL ReadFileAll(const char* path, char* buf, DWORD cap, DWORD* outLen)
{
    *outLen = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD n = 0;
    if (!ReadFile(h, buf, cap - 1, &n, NULL)) { CloseHandle(h); return FALSE; }
    buf[n] = 0;
    *outLen = n;
    CloseHandle(h);
    return TRUE;
}

static BOOL ContainsI(const char* hay, const char* needle)
{
    if (!hay || !needle) return FALSE;
    size_t n = strlen(needle);
    for (const char* p = hay; *p; p++)
    {
        size_t i = 0;
        for (; i < n && p[i]; i++)
        {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (i == n) return TRUE;
    }
    return FALSE;
}

int main()
{
    if (!IsAdmin())
    {
        LogLine("OakLauncher: elevating...");
        RelaunchElevated();
        LogLine("OakLauncher: elevation failed");
        return 2;
    }

    LogLine("=== OakLauncher ===");
    CreateDirectoryA("C:\\oak", NULL);

    // Resolve staging sources (exe dir, then common build outputs)
    char exeDir[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exeDir, MAX_PATH);
    char* slash = strrchr(exeDir, '\\');
    if (slash) *slash = 0;

    char dllSrc[MAX_PATH] = {}, sysSrc[MAX_PATH] = {}, loaderSrc[MAX_PATH] = {};
    char tryDll[8][MAX_PATH];
    char trySys[6][MAX_PATH];
    char tryLoader[4][MAX_PATH];
    sprintf(tryDll[0], "%s\\dayz_internal.dll", exeDir);
    sprintf(tryDll[1], "C:\\oak\\dayz_internal.dll");
    sprintf(tryDll[2], "%s\\..\\build\\Release\\dayz_internal.dll", exeDir);
    sprintf(tryDll[3], "%s\\..\\bin\\dayz_internal.dll", exeDir);
    sprintf(tryDll[4], "%s\\build\\Release\\dayz_internal.dll", exeDir);
    sprintf(tryDll[5], "%s\\bin\\dayz_internal.dll", exeDir);
    // Absolute fallback for this machine's repo
    strcpy(tryDll[6], "..\\..\\..\\build\\Release\\dayz_internal.dll");
    strcpy(tryDll[7], "..\\..\\..\\bin\\dayz_internal.dll");

    sprintf(trySys[0], "%s\\oak.sys", exeDir);
    sprintf(trySys[1], "C:\\oak\\oak.sys");
    sprintf(trySys[2], "%s\\..\\driver\\bin\\oak.sys", exeDir);
    sprintf(trySys[3], "%s\\..\\bin\\oak.sys", exeDir);
    strcpy(trySys[4], "..\\..\\..\\driver\\bin\\oak.sys");
    strcpy(trySys[5], "..\\..\\..\\bin\\oak.sys");

    sprintf(tryLoader[0], "%s\\oak_loader.exe", exeDir);
    sprintf(tryLoader[1], "C:\\oak\\oak_loader.exe");
    sprintf(tryLoader[2], "%s\\..\\bin\\oak_loader.exe", exeDir);
    strcpy(tryLoader[3], "..\\..\\..\\bin\\oak_loader.exe");

    const char* dllCands[8]; for (int i = 0; i < 8; i++) dllCands[i] = tryDll[i];
    const char* sysCands[6]; for (int i = 0; i < 6; i++) sysCands[i] = trySys[i];
    const char* loaderCands[4]; for (int i = 0; i < 4; i++) loaderCands[i] = tryLoader[i];

    if (!FindFirstExisting(dllSrc, MAX_PATH, dllCands, 8) ||
        !FindFirstExisting(sysSrc, MAX_PATH, sysCands, 6) ||
        !FindFirstExisting(loaderSrc, MAX_PATH, loaderCands, 4))
    {
        LogLine("RESULT=FAIL missing dayz_internal.dll / oak.sys / oak_loader.exe");
        return 3;
    }

    LogLine("Killing DayZ + oak_loader...");
    KillImage(L"DayZ_x64.exe");
    KillImage(L"DayZ_BE.exe");
    KillImage(L"DayZLauncher.exe");
    KillImage(L"oak_loader.exe");
    Sleep(3000);

    LogLine("Staging C:\\oak ...");
    DeleteFileA("C:\\oak\\inject.log");
    DeleteFileA("C:\\oak\\dll_attach.flag");
    DeleteFileA("C:\\oak\\imgui_init.flag");
    DeleteFileA("C:\\oak\\mainthread.flag");
    char imguiLog[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", imguiLog, MAX_PATH))
    {
        char p[MAX_PATH];
        sprintf(p, "%s\\DayZ\\oak_imgui.log", imguiLog);
        DeleteFileA(p);
    }

    if (!CopyForce(dllSrc, "C:\\oak\\dayz_internal.dll") ||
        !CopyForce(sysSrc, "C:\\oak\\oak.sys"))
    {
        LogLine("RESULT=FAIL stage copy failed");
        return 4;
    }
    // Loader may be locked — try copy, else use existing
    if (!CopyForce(loaderSrc, "C:\\oak\\oak_loader.exe") && !FileExistsA("C:\\oak\\oak_loader.exe"))
    {
        LogLine("RESULT=FAIL oak_loader.exe missing");
        return 4;
    }

    DWORD dllSz = FileSizeA("C:\\oak\\dayz_internal.dll");
    char msg[128];
    sprintf(msg, "Staged DLL bytes=%u", dllSz);
    LogLine(msg);

    LogLine("Starting oak_loader...");
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    char cmd[MAX_PATH];
    sprintf(cmd, "\"C:\\oak\\oak_loader.exe\" \"C:\\oak\\oak.sys\"");
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, "C:\\oak", &si, &pi))
    {
        LogLine("RESULT=FAIL oak_loader start failed");
        return 5;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Sleep(4000);

    char inj[65536];
    DWORD injLen = 0;
    if (ReadFileAll("C:\\oak\\inject.log", inj, sizeof(inj), &injLen) && ContainsI(inj, "refusing duplicate"))
    {
        LogLine("Inject lock was held — driver auto-bumps on rebuild; retrying loader once...");
        KillImage(L"oak_loader.exe");
        Sleep(1000);
        DeleteFileA("C:\\oak\\inject.log");
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, "C:\\oak", &si, &pi))
        {
            LogLine("RESULT=FAIL oak_loader retry failed");
            return 5;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Sleep(4000);
    }

    // Launch DayZ only through DayZ_BE.exe. Steam's generic app URI can honor a
    // saved NoBattlEye launch choice, so it is not a fallback.
    CreateDirectoryA("C:\\oak\\dayz", NULL);
    HANDLE beFlag = CreateFileA("C:\\oak\\dayz\\be_launch.flag", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (beFlag != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(beFlag, "1", 1, &w, NULL); CloseHandle(beFlag); }
    beFlag = CreateFileA("C:\\oak\\be_launch.flag", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (beFlag != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(beFlag, "1", 1, &w, NULL); CloseHandle(beFlag); }

    const char* be = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\DayZ\\DayZ_BE.exe";
    const char* dzDir = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\DayZ";
    auto startDayZ = [&]() -> BOOL {
        if (!FileExistsA(be))
        {
            LogLine("DayZ_BE.exe missing; direct/Steam fallback disabled");
            return FALSE;
        }
        LogLine("Launching DayZ_BE...");
        STARTUPINFOA dsi{};
        PROCESS_INFORMATION dpi{};
        dsi.cb = sizeof(dsi);
        char dcmd[MAX_PATH * 2];
        sprintf(dcmd, "\"%s\"", be);
        if (CreateProcessA(be, dcmd, NULL, NULL, FALSE, 0, NULL, dzDir, &dsi, &dpi))
        {
            CloseHandle(dpi.hThread);
            CloseHandle(dpi.hProcess);
            return TRUE;
        }
        return ((INT_PTR)ShellExecuteA(NULL, "open", be, NULL, dzDir, SW_SHOWNORMAL) > 32) ? TRUE : FALSE;
    };
    if (!startDayZ())
    {
        LogLine("RESULT=FAIL DayZ_BE.exe missing or would not start");
        return 8;
    }

    // Poll success
    LogLine("Waiting for inject...");
    BOOL ok = FALSE;
    for (int i = 0; i < 48; i++)
    {
        Sleep(5000);
        BOOL attach = FileExistsA("C:\\oak\\dll_attach.flag");
        injLen = 0;
        ReadFileAll("C:\\oak\\inject.log", inj, sizeof(inj), &injLen);
        BOOL success = ContainsI(inj, "SUCCESS via spoofed") || ContainsI(inj, "INJECTION COMPLETE");
        BOOL refuse = ContainsI(inj, "refusing duplicate");
        BOOL dayz = FALSE;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe))
            {
                do { if (_wcsicmp(pe.szExeFile, L"DayZ_x64.exe") == 0) dayz = TRUE; } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        // Mid-poll: if game never came up, launch DayZ again once
        if (!dayz && i == 4)
        {
            LogLine("DayZ not running yet — launching again...");
            startDayZ();
        }
        sprintf(msg, "  poll attach=%d success=%d refuse=%d dayz=%d", attach, success, refuse, dayz);
        LogLine(msg);
        if (refuse) { LogLine("RESULT=FAIL inject lock held"); return 6; }
        if (attach && success && dayz) { ok = TRUE; break; }
    }

    if (!ok)
    {
        LogLine("RESULT=FAIL inject timeout");
        return 7;
    }

    // Optional Present wait (up to ~90s)
    char local[MAX_PATH];
    GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
    char logPath[MAX_PATH];
    sprintf(logPath, "%s\\DayZ\\oak_imgui.log", local);
    BOOL present = FALSE;
    for (int i = 0; i < 24; i++)
    {
        Sleep(5000);
        char lb[65536]; DWORD ln = 0;
        if (ReadFileAll(logPath, lb, sizeof(lb), &ln) && ContainsI(lb, "present#1"))
        {
            present = TRUE;
            break;
        }
        if (!FileExistsA("C:\\oak\\dll_attach.flag")) break;
    }

    sprintf(msg, "RESULT=PASS dll=%u attach=1 inject=1 present=%d", dllSz, present ? 1 : 0);
    LogLine(msg);
    LogLine("Menu key: K");
    return 0;
}
