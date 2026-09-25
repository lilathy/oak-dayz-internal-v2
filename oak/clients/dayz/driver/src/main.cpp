#include "driver.h"
#include <stdarg.h>
#include <stdio.h>
#include <ntstrsafe.h>


DRIVER_STATE g_State = { 0 };
HANDLE g_InjectLockHandle = NULL;

VOID Log(const char* Format, ...);

static VOID ReleaseInjectLock()
{
    if (g_InjectLockHandle)
    {
        ZwClose(g_InjectLockHandle);
        g_InjectLockHandle = NULL;
        Log("Inject lock released (retry allowed)");
    }
}

static VOID EndInjectThread(NTSTATUS st)
{
    ReleaseInjectLock();
    PsTerminateSystemThread(st);
}


#define CHEAT_DLL_PATH_PRIMARY   L"\\??\\C:\\oak\\dayz\\oak_payload.dll"
#define CHEAT_DLL_PATH_LEGACY    L"\\??\\C:\\oak\\dayz\\oak_payload.dll"
#define CHEAT_DLL_PATH CHEAT_DLL_PATH_PRIMARY


VOID LogWrite(const char* msg)
{
    DbgPrintEx(0, 0, "%s", msg);

    // Also append to C:\oak\inject.log so we can diagnose without DbgView
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;

    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\??\\C:\\oak\\inject.log");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE h = NULL;
    IO_STATUS_BLOCK ios = { 0 };
    NTSTATUS st = ZwCreateFile(
        &h, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &ios, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st) || !h)
        return;

    SIZE_T len = 0;
    while (msg[len] && len < 700) len++;
    ZwWriteFile(h, NULL, NULL, NULL, &ios, (PVOID)msg, (ULONG)len, NULL, NULL);
    ZwClose(h);
}

VOID LogInit()
{
}

VOID LogClose()
{
}

VOID Log(const char* Format, ...)
{
    va_list args;
    va_start(args, Format);
    
    char buffer[512];
    RtlStringCbVPrintfA(buffer, sizeof(buffer) - 16, Format, args);
    va_end(args);
    
    
    char finalBuffer[600];
    RtlStringCbPrintfA(finalBuffer, sizeof(finalBuffer), "[oak] %s\r\n", buffer);
    
    
    DbgPrintEx(0, 0, "%s", finalBuffer);
    
    
    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        LogWrite(finalBuffer);
    }
}


NTSTATUS ReadFileToPool(PUNICODE_STRING FilePath, PVOID* Buffer, PSIZE_T Size)
{
    OBJECT_ATTRIBUTES objAttr;
    InitializeObjectAttributes(&objAttr, FilePath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    
    HANDLE fileHandle;
    IO_STATUS_BLOCK ioStatus;
    
    NTSTATUS status = ZwOpenFile(
        &fileHandle,
        GENERIC_READ,
        &objAttr,
        &ioStatus,
        FILE_SHARE_READ,
        FILE_SYNCHRONOUS_IO_NONALERT
    );
    
    if (!NT_SUCCESS(status))
    {
        Log("Failed to open file: 0x%X", status);
        return status;
    }
    
    
    FILE_STANDARD_INFORMATION fileInfo;
    status = ZwQueryInformationFile(fileHandle, &ioStatus, &fileInfo, sizeof(fileInfo), FileStandardInformation);
    if (!NT_SUCCESS(status))
    {
        Log("Failed to query file info: 0x%X", status);
        ZwClose(fileHandle);
        return status;
    }
    
    SIZE_T fileSize = (SIZE_T)fileInfo.EndOfFile.QuadPart;
    Log("File size: %llu bytes", fileSize);
    
    
    PVOID buffer = ExAllocatePoolWithTag(NonPagedPool, fileSize, DRIVER_TAG);
    if (!buffer)
    {
        Log("Failed to allocate %llu bytes", fileSize);
        ZwClose(fileHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    
    LARGE_INTEGER byteOffset = { 0 };
    status = ZwReadFile(fileHandle, NULL, NULL, NULL, &ioStatus, buffer, (ULONG)fileSize, &byteOffset, NULL);
    
    ZwClose(fileHandle);
    
    if (!NT_SUCCESS(status))
    {
        Log("Failed to read file: 0x%X", status);
        ExFreePoolWithTag(buffer, DRIVER_TAG);
        return status;
    }
    
    *Buffer = buffer;
    *Size = fileSize;
    
    Log("File read successfully");
    return STATUS_SUCCESS;
}


VOID DoInjection(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    PEPROCESS process = NULL;
    HANDLE processId = NULL;
    LARGE_INTEGER delay;

restart:
    process = NULL;
    processId = NULL;
    Log("=== DEBUG: Injection thread watching for DayZ ===");
    
    for (int i = 0; i < 600; i++)  // ~10 minutes — enough to start Steam/BE after loader
    {
        
        // PIDs on modern Windows routinely exceed 65536 (observed DayZ at 67660+).
        for (ULONG pid = 4; pid < 0x400000UL; pid += 4)
        {
            PEPROCESS testProc = NULL;
            if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &testProc)))
            {
                PCHAR imageName = (PCHAR)PsGetProcessImageFileName(testProc);
                if (imageName && _stricmp(imageName, TARGET_PROCESS_NAME) == 0
                    && PsGetProcessExitStatus(testProc) == STATUS_PENDING)
                {
                    process = testProc;
                    processId = (HANDLE)(ULONG_PTR)pid;
                    break;
                }
                ObDereferenceObject(testProc);
            }
        }
        
        if (process) break;
        
        if (i == 0 || (i % 30) == 0)
        {
            Log("Searching... (%d/600)", i);
        }
        
        delay.QuadPart = -10000000LL; 
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    
    if (!process)
    {
        Log("ERROR: DayZ not found — watching again");
        delay.QuadPart = -30000000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        goto restart;
    }
    
    g_State.TargetProcess = process;
    g_State.TargetProcessId = processId;
    
    Log("Step 3: Found DayZ PID=%d", (ULONG)(ULONG_PTR)processId);
    
wait_for_stable:
    while (PsGetProcessExitStatus(process) != STATUS_PENDING)
    {
        Log("Process is terminating, waiting for fresh instance...");
        ObDereferenceObject(process);
        process = NULL;
        processId = NULL;
        
        
        for (int retry = 0; retry < 600; retry++)  // ~10 min for Steam/BE relaunch
        {
            delay.QuadPart = -10000000LL; 
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
            
            
            for (ULONG pid = 4; pid < 0x400000UL; pid += 4)
            {
                PEPROCESS testProc = NULL;
                if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &testProc)))
                {
                    PCHAR imageName = (PCHAR)PsGetProcessImageFileName(testProc);
                    if (imageName && _stricmp(imageName, TARGET_PROCESS_NAME) == 0)
                    {
                        if (PsGetProcessExitStatus(testProc) == STATUS_PENDING)
                        {
                            process = testProc;
                            processId = (HANDLE)(ULONG_PTR)pid;
                            break;
                        }
                    }
                    ObDereferenceObject(testProc);
                }
            }
            
            if (process)
            {
                Log("Found fresh process PID=%d", (ULONG)(ULONG_PTR)processId);
                break;
            }
            
            if (retry % 30 == 0)
                Log("Still waiting for DayZ... (%d/600)", retry);
        }
        
        if (!process)
        {
            Log("ERROR: Timeout waiting for DayZ process — watching again");
            goto restart;
        }
    }
    
    // DayZ/BE often relaunch once; inject after the PID stays alive for a short
    // stretch instead of one long blind sleep (old builds waited 90s and the game
    // often exited before inject).
    Log("Step 4: Waiting for stable DayZ (BE init)...");
    {
        const int kPolls = 45;          // up to ~90s wall clock
        const int kStableNeeded = 9;    // 9 * 2s = 18s continuous uptime
        int stableTicks = 0;
        for (int poll = 0; poll < kPolls; poll++)
        {
            if (PsGetProcessExitStatus(process) != STATUS_PENDING)
            {
                Log("Process terminated during wait — hunting next instance...");
                ObDereferenceObject(process);
                process = NULL;
                processId = NULL;
                g_State.TargetProcess = NULL;
                for (int retry = 0; retry < 600 && !process; retry++)
                {
                    delay.QuadPart = -10000000LL;
                    KeDelayExecutionThread(KernelMode, FALSE, &delay);
                    for (ULONG pid = 4; pid < 0x400000UL; pid += 4)
                    {
                        PEPROCESS testProc = NULL;
                        if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &testProc)))
                        {
                            PCHAR imageName = (PCHAR)PsGetProcessImageFileName(testProc);
                            if (imageName && _stricmp(imageName, TARGET_PROCESS_NAME) == 0
                                && PsGetProcessExitStatus(testProc) == STATUS_PENDING)
                            {
                                process = testProc;
                                processId = (HANDLE)(ULONG_PTR)pid;
                                break;
                            }
                            ObDereferenceObject(testProc);
                        }
                    }
                    if (process)
                    {
                        Log("Found replacement PID=%d", (ULONG)(ULONG_PTR)processId);
                        break;
                    }
                    if (retry % 30 == 0)
                        Log("Still waiting after early exit... (%d/600)", retry);
                }
                if (!process)
                {
                    Log("ERROR: No replacement DayZ — watching again");
                    goto restart;
                }
                g_State.TargetProcess = process;
                g_State.TargetProcessId = processId;
                stableTicks = 0;
                poll = -1;
                continue;
            }

            stableTicks++;
            if (stableTicks >= kStableNeeded)
            {
                Log("Step 4: stable for %ds — injecting", stableTicks * 2);
                break;
            }
            if (poll == 0 || (poll % 10) == 0)
                Log("Step 4: stable %d/%d (%ds elapsed)", stableTicks, kStableNeeded, poll * 2);

            delay.QuadPart = -20000000LL; // 2s
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }

        if (stableTicks < kStableNeeded)
        {
            Log("ERROR: DayZ never stabilized before inject timeout — watching again");
            if (process)
            {
                ObDereferenceObject(process);
                process = NULL;
            }
            RtlZeroMemory(&g_State, sizeof(g_State));
            goto restart;
        }
    }
    
    
    g_State.TargetProcess = process;
    g_State.TargetProcessId = processId;
    g_State.Attached = TRUE;
    Log("Process verified and attached");
    
    
    Log("Step 5: Reading DLL...");
    UNICODE_STRING dllPathPrimary = RTL_CONSTANT_STRING(CHEAT_DLL_PATH_PRIMARY);
    UNICODE_STRING dllPathLegacy = RTL_CONSTANT_STRING(CHEAT_DLL_PATH_LEGACY);
    PVOID dllBuffer = NULL;
    SIZE_T dllSize = 0;

    NTSTATUS status = ReadFileToPool(&dllPathPrimary, &dllBuffer, &dllSize);
    if (!NT_SUCCESS(status))
    {
        Log("Primary DLL missing — trying legacy C:\\oak\\dayz_internal.dll (0x%X)", status);
        status = ReadFileToPool(&dllPathLegacy, &dllBuffer, &dllSize);
    }
    if (!NT_SUCCESS(status))
    {
        Log("ERROR: DLL read failed: 0x%X — watching again", status);
        if (process)
        {
            ObDereferenceObject(process);
            process = NULL;
        }
        RtlZeroMemory(&g_State, sizeof(g_State));
        goto restart;
    }
    
    Log("Step 6: DLL loaded (%llu bytes)", dllSize);
    Log("Step 7: Mapping DLL...");
    
    ULONG64 mappedBase = 0;
    status = MapDllToProcess(dllBuffer, dllSize, &mappedBase);
    
    ExFreePoolWithTag(dllBuffer, DRIVER_TAG);
    
    if (!NT_SUCCESS(status))
    {
        Log("ERROR: Mapping failed: 0x%X — watching again", status);
        if (process)
        {
            ObDereferenceObject(process);
            process = NULL;
        }
        RtlZeroMemory(&g_State, sizeof(g_State));
        goto restart;
    }
    
    Log("Step 8: DLL mapped at 0x%llX", mappedBase);
    
    // Health-check: confirm DayZ still alive after DllMain (BE often kills within seconds)
    for (int hi = 0; hi < 10; hi++)
    {
        delay.QuadPart = -20000000LL; // 2s
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        if (PsGetProcessExitStatus(process) != STATUS_PENDING)
        {
            Log("ERROR: DayZ died %d seconds after inject (BE kill?) — watching again", (hi + 1) * 2);
            ObDereferenceObject(process);
            RtlZeroMemory(&g_State, sizeof(g_State));
            goto restart;
        }
        Log("Post-inject alive +%ds", (hi + 1) * 2);
    }
    
    Log("=======================================");
    Log("INJECTION COMPLETE (BE path)!");
    Log("Cheat should now be running.");
    Log("Press INSERT to open menu.");
    Log("Check LocalAppData\\DayZ\\oak_imgui.log");
    Log("=======================================");
    
    
    while (TRUE)
    {
        delay.QuadPart = -100000000LL; 
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        
        if (!g_State.TargetProcess)
        {
            Log("Target process cleared, cleaning up...");
            break;
        }
        if (PsGetProcessExitStatus(g_State.TargetProcess) != STATUS_PENDING)
        {
            Log("Target process exited, releasing inject lock...");
            break;
        }
    }
    
    
    Log("Target process gone — watching for next DayZ");
    if (process)
        ObDereferenceObject(process);
    RtlZeroMemory(&g_State, sizeof(g_State));
    goto restart;
}


#pragma pack(push, 1)
typedef struct _HOOK_REGION_INFO {
    ULONG64 Address;
    ULONG64 Size;
    UCHAR OriginalBytes[32];
    UCHAR Valid;
} HOOK_REGION_INFO, *PHOOK_REGION_INFO;
#pragma pack(pop)


NTSTATUS RegisterHookRegions()
{
    Log("Reading hook regions from file...");
    
    
    UNICODE_STRING filePath = RTL_CONSTANT_STRING(L"\\??\\C:\\oak\\hooks.dat");
    OBJECT_ATTRIBUTES objAttr;
    InitializeObjectAttributes(&objAttr, &filePath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    
    HANDLE fileHandle;
    IO_STATUS_BLOCK ioStatus;
    
    NTSTATUS status = ZwOpenFile(
        &fileHandle,
        GENERIC_READ,
        &objAttr,
        &ioStatus,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_SYNCHRONOUS_IO_NONALERT
    );
    
    if (!NT_SUCCESS(status))
    {
        Log("hooks.dat not found (0x%X) - this is normal if cheat hasn't registered hooks", status);
        return status;
    }
    
    
    FILE_STANDARD_INFORMATION fileInfo;
    status = ZwQueryInformationFile(fileHandle, &ioStatus, &fileInfo, sizeof(fileInfo), FileStandardInformation);
    if (!NT_SUCCESS(status))
    {
        ZwClose(fileHandle);
        return status;
    }
    
    SIZE_T fileSize = (SIZE_T)fileInfo.EndOfFile.QuadPart;
    ULONG numEntries = (ULONG)(fileSize / sizeof(HOOK_REGION_INFO));
    
    Log("Found %lu hook entries", numEntries);
    
    if (numEntries == 0)
    {
        ZwClose(fileHandle);
        return STATUS_SUCCESS;
    }
    
    
    PHOOK_REGION_INFO entries = (PHOOK_REGION_INFO)ExAllocatePoolWithTag(
        NonPagedPool, 
        numEntries * sizeof(HOOK_REGION_INFO), 
        DRIVER_TAG
    );
    
    if (!entries)
    {
        ZwClose(fileHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    
    LARGE_INTEGER byteOffset = { 0 };
    status = ZwReadFile(fileHandle, NULL, NULL, NULL, &ioStatus, 
        entries, (ULONG)(numEntries * sizeof(HOOK_REGION_INFO)), &byteOffset, NULL);
    
    ZwClose(fileHandle);
    
    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(entries, DRIVER_TAG);
        return status;
    }
    
    
    for (ULONG i = 0; i < numEntries && i < 32; i++)
    {
        if (!entries[i].Valid) continue;
        
        
        PVOID origBytes = ExAllocatePoolWithTag(NonPagedPool, 32, DRIVER_TAG);
        if (!origBytes) continue;
        
        RtlCopyMemory(origBytes, entries[i].OriginalBytes, 32);
        
        
        AddCloakRegion(entries[i].Address, entries[i].Size, origBytes);
        
        Log("Registered hook region: 0x%llX (size: %llu)", entries[i].Address, entries[i].Size);
    }
    
    ExFreePoolWithTag(entries, DRIVER_TAG);
    
    Log("Hook regions registered successfully");
    return STATUS_SUCCESS;
}


extern "C" NTSTATUS DriverEntry(
    _In_ ULONG64 Param1,
    _In_ ULONG64 Param2
)
{
    UNREFERENCED_PARAMETER(Param1);
    UNREFERENCED_PARAMETER(Param2);
    
    LogInit();
    
    // One global inject lock — do not spawn parallel injection threads when the
    // launcher maps oak.sys again while a prior thread is still waiting.
    {
        // V2: prior lock name could outlive a dead inject thread after remap, blocking
        // all future maps. Bump when the wait/scan contract changes.
        UNICODE_STRING lockName = RTL_CONSTANT_STRING(L"\\BaseNamedObjects\\OakDayZInjectLockV3");
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, &lockName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        HANDLE existing = NULL;
        if (NT_SUCCESS(ZwOpenEvent(&existing, EVENT_ALL_ACCESS, &oa)))
        {
            ZwClose(existing);
            Log("Oak Driver: inject already active — skipping duplicate map");
            return STATUS_SUCCESS;
        }
        NTSTATUS est = ZwCreateEvent(&g_InjectLockHandle, EVENT_ALL_ACCESS, &oa, NotificationEvent, TRUE);
        if (!NT_SUCCESS(est) || !g_InjectLockHandle)
        {
            Log("Oak Driver: inject lock create failed (0x%X)", est);
            return STATUS_SUCCESS;
        }
        Log("Oak Driver: inject lock OK");
    }

    Log("Oak Driver v1.0 - Starting");
    
    
    RtlZeroMemory(&g_State, sizeof(g_State));
    
    Log("State initialized, creating thread...");
    
    
    HANDLE threadHandle = NULL;
    NTSTATUS status = PsCreateSystemThread(
        &threadHandle,
        THREAD_ALL_ACCESS,
        NULL,
        NULL,
        NULL,
        DoInjection,
        NULL
    );
    
    if (!NT_SUCCESS(status))
    {
        Log("ERROR: Failed to create thread: 0x%X", status);
        LogClose();
        return status;
    }
    
    ZwClose(threadHandle);
    
    Log("Thread created - waiting for DayZ process...");
    return STATUS_SUCCESS;
}
