// Loads the UI-only OakImGuiOverlay.dll into DayZ_x64 for ImGui verification.
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cstring>

static DWORD FindPid(const char* name)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe))
    {
        do
        {
            if (_stricmp(pe.szExeFile, name) == 0)
            {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool Inject(DWORD pid, const char* dllPath)
{
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc)
    {
        printf("[!] OpenProcess failed (%lu) — try running as Administrator\n", GetLastError());
        return false;
    }

    size_t len = strlen(dllPath) + 1;
    void* remote = VirtualAllocEx(proc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        printf("[!] VirtualAllocEx failed (%lu)\n", GetLastError());
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote, dllPath, len, nullptr))
    {
        printf("[!] WriteProcessMemory failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th)
    {
        printf("[!] CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(th, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(th, &exitCode);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (exitCode == 0)
    {
        printf("[!] LoadLibrary returned NULL inside target\n");
        return false;
    }
    printf("[+] LoadLibrary OK (module=0x%lX)\n", exitCode);
    return true;
}

int main(int argc, char** argv)
{
    char dllPath[MAX_PATH] = {};
    if (argc > 1)
        strncpy_s(dllPath, argv[1], _TRUNCATE);
    else
    {
        GetModuleFileNameA(nullptr, dllPath, MAX_PATH);
        char* slash = strrchr(dllPath, '\\');
        if (slash) *(slash + 1) = 0;
        strcat_s(dllPath, "OakImGuiOverlay.dll");
    }

    printf("Oak ImGui overlay loader (UI only)\n");
    printf("DLL: %s\n", dllPath);

    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES)
    {
        printf("[!] DLL not found\n");
        return 1;
    }

    // Absolute path required for remote LoadLibrary
    char full[MAX_PATH];
    if (!GetFullPathNameA(dllPath, MAX_PATH, full, nullptr))
    {
        printf("[!] GetFullPathName failed\n");
        return 1;
    }

    printf("[*] Waiting for DayZ_x64.exe...\n");
    DWORD pid = 0;
    if (argc > 2)
    {
        pid = (DWORD)strtoul(argv[2], nullptr, 10);
        if (pid == 0)
        {
            printf("[!] Invalid PID argument\n");
            return 2;
        }
        printf("[+] Using PID %lu from command line\n", pid);
    }
    else
    {
        for (int i = 0; i < 120 && !pid; i++)
        {
            pid = FindPid("DayZ_x64.exe");
            if (!pid) Sleep(1000);
        }
        if (!pid)
        {
            printf("[!] DayZ_x64.exe not found\n");
            return 2;
        }
    }

    printf("[+] PID %lu — waiting 8s for D3D...\n", pid);
    Sleep(8000);

    if (!Inject(pid, full))
        return 3;

    printf("[*] Check log: %%LOCALAPPDATA%%\\DayZ\\oak_imgui.log\n");
    return 0;
}
