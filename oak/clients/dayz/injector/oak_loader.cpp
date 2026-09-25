
#include <Windows.h>
#include <iostream>
#include <fstream>
#include <vector>


#include "nt.hpp"
#include "utils.hpp"
#include "intel_driver.hpp"
#include "kdmapper.hpp"

std::vector<BYTE> ReadFileToMemory(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<BYTE> buffer((size_t)size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return {};
    }

    return buffer;
}

bool EnsureDriverLoaded(NTSTATUS* outStatus = nullptr) {
    
    if (intel_driver::IsRunning()) {
        std::wcout << L"intel driver already loaded" << std::endl;
        if (outStatus) *outStatus = STATUS_SUCCESS;
        return true;
    }
    
    
    std::wcout << L"loading intel driver..." << std::endl;
    NTSTATUS status = intel_driver::Load();
    if (outStatus) *outStatus = status;
    if (!NT_SUCCESS(status)) {
        std::wcout << L"failed to load intel driver, status 0x" << std::hex << status << std::endl;
        return false;
    }
    
    std::wcout << L"intel driver loaded" << std::endl;
    return true;
}

static void WriteLoaderLog(const char* result, ULONG64 mapped, NTSTATUS intelStatus = 0)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\oak\\loader.log", "w") == 0 && f) {
        fprintf(f, "result=%s\nmapped=0x%llx\n", result, (unsigned long long)mapped);
        if (intelStatus)
            fprintf(f, "intel_status=0x%08X\n", (unsigned)intelStatus);
        fclose(f);
    }
}

static void WaitForConsoleIfInteractive()
{
    if (GetConsoleWindow() != NULL) {
        std::wcout << std::endl;
        std::wcout << L"press enter to exit..." << std::endl;
        std::wcin.get();
    }
}

int wmain(int argc, wchar_t* argv[]) {
    std::wcout << L"========================================" << std::endl;
    std::wcout << L"      Oak Loader (Intel Driver)" << std::endl;
    std::wcout << L"========================================" << std::endl;
    std::wcout << std::endl;

    
    std::wstring driverPath;
    if (argc > 1) {
        driverPath = argv[1];
    } else {
        wchar_t currentDir[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, currentDir);
        driverPath = std::wstring(currentDir) + L"\\oak.sys";
    }

    
    if (GetFileAttributesW(driverPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcout << L"driver not found: " << driverPath << std::endl;
        std::wcout << L"put oak.sys here or give path" << std::endl;
        WriteLoaderLog("fail", 0);
        WaitForConsoleIfInteractive();
        return 1;
    }

    std::wcout << L"driver: " << driverPath << std::endl;

    
    std::wcout << L"checking intel driver..." << std::endl;
    NTSTATUS intelStatus = 0;
    if (!EnsureDriverLoaded(&intelStatus)) {
        std::wcout << L"failed to load intel driver" << std::endl;
        WriteLoaderLog("fail", 0, intelStatus);
        WaitForConsoleIfInteractive();
        return 1;
    }

    std::wcout << L"intel driver ready" << std::endl;

    
    std::wcout << L"reading driver file..." << std::endl;
    auto driverData = ReadFileToMemory(driverPath);
    if (driverData.empty()) {
        std::wcout << L"failed to read driver" << std::endl;
        intel_driver::Unload();
        WriteLoaderLog("fail", 0);
        WaitForConsoleIfInteractive();
        return 1;
    }

    std::wcout << L"read " << driverData.size() << L" bytes" << std::endl;

    
    std::wcout << L"mapping driver..." << std::endl;
    
    NTSTATUS exitCode = 0;
    ULONG64 result = kdmapper::MapDriver(driverData.data(), 0, 0, false, true);

    if (result) {
        std::wcout << L"driver mapped at 0x" << std::hex << result << std::endl;
        std::wcout << std::endl;
        std::wcout << L"========================================" << std::endl;
        std::wcout << L"           DRIVER LOADED!" << std::endl;
        std::wcout << L"========================================" << std::endl;
        std::wcout << std::endl;
        std::wcout << L"Driver will auto-inject when DayZ_x64.exe starts." << std::endl;
        std::wcout << L"Required: C:\\oak\\dayz\\dayz_internal.dll (or legacy C:\\oak\\dayz_internal.dll)" << std::endl;
        std::wcout << L"Launch DayZ WITH BattlEye (normal Steam start)." << std::endl;
        std::wcout << L"Wait ~30s after join for inject (BE init delay)." << std::endl;
    } else {
        std::wcout << L"failed to map driver" << std::endl;
        std::wcout << L"If status was blocklist-related: set VulnerableDriverBlocklistEnable=0 and reboot." << std::endl;
    }

    
    intel_driver::Unload();

    WriteLoaderLog(result ? "ok" : "fail", result);
    return result ? 0 : 1;
}
