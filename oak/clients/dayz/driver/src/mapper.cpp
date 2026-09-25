#include "driver.h"

extern VOID Log(const char* Format, ...);

#pragma pack(push, 1)
typedef struct _SHELLCODE_BLOCK {
    UCHAR Code[128];                // 0x00
    ULONG64 DllBase;                // 0x80
    ULONG64 EntryPoint;             // 0x88
    ULONG64 ContinueRip;            // 0x90  (hook target)
    volatile ULONG Done;            // 0x98
    ULONG ExceptionCount;           // 0x9C
    ULONG64 pRtlAddFunctionTable;   // 0xA0
    ULONG64 ExceptionTable;         // 0xA8
    ULONG64 AddressOfTlsCallbacks;  // 0xB0
    UCHAR OrigBytes[16];            // 0xB8
    UCHAR Trampoline[40];           // 0xC8
} SHELLCODE_BLOCK;
#pragma pack(pop)

// Minimal entry: aligned stack → call OakMapEntry/DllMain → Done=1 → ret
// No EH/TLS here — RtlAddFunctionTable with a manual map has crashed entry before.
static const UCHAR g_EntryShell[] = {
    0x53,                                     // push rbx
    0x56,                                     // push rsi
    0x41, 0x57,                               // push r15
    0x48, 0x83, 0xEC, 0x28,                   // sub rsp, 0x28
    0x49, 0x89, 0xCF,                         // mov r15, rcx
    0x49, 0x8B, 0x4F, 0x80,                   // mov rcx, DllBase
    0x49, 0x8B, 0x47, 0x88,                   // mov rax, Entry
    0xBA, 0x01, 0x00, 0x00, 0x00,             // mov edx, DLL_PROCESS_ATTACH
    0x45, 0x31, 0xC0,                         // xor r8d, r8d
    0xFF, 0xD0,                               // call rax
    0x41, 0xC7, 0x47, 0x98, 0x01, 0x00, 0x00, 0x00, // Done=1
    0x48, 0x83, 0xC4, 0x28,                   // add rsp, 0x28
    0x41, 0x5F,                               // pop r15
    0x5E,                                     // pop rsi
    0x5B,                                     // pop rbx
    0x31, 0xC0,                               // xor eax, eax
    0xC3                                      // ret
};

#define g_ApcShell g_EntryShell

// Hijack variant: return to ContinueRip
static const UCHAR g_HijackShell[] = {
    0x53,
    0x56,
    0x41, 0x57,
    0x48, 0x83, 0xEC, 0x28,
    0x49, 0x89, 0xCF,
    0x49, 0x8B, 0x4F, 0x80,
    0x49, 0x8B, 0x47, 0x88,
    0xBA, 0x01, 0x00, 0x00, 0x00, 
    0x45, 0x31, 0xC0,             
    0xFF, 0xD0,                   
    0x41, 0xC7, 0x47, 0x98, 0x01, 0x00, 0x00, 0x00,
    0x49, 0x8B, 0x47, 0x90,                   // mov rax, ContinueRip
    0x48, 0x83, 0xC4, 0x28,
    0x41, 0x5F,
    0x5E,
    0x5B,
    0xFF, 0xE0                                // jmp rax
};

#define g_ExecShell g_HijackShell

typedef NTSTATUS(NTAPI* tPsGetContextThread)(PETHREAD, PCONTEXT, KPROCESSOR_MODE);
typedef NTSTATUS(NTAPI* tPsSetContextThread)(PETHREAD, PCONTEXT, KPROCESSOR_MODE);
typedef NTSTATUS(NTAPI* tPsLookupThreadByThreadId)(HANDLE, PETHREAD*);
typedef NTSTATUS(NTAPI* tZwSusp)(HANDLE, PULONG);
typedef NTSTATUS(NTAPI* tZwQuerySys)(ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS(NTAPI* tZwAlertThread)(HANDLE);

typedef enum _OAK_KAPC_ENVIRONMENT {
    OakOriginalApcEnvironment = 0,
    OakAttachedApcEnvironment,
    OakCurrentApcEnvironment,
    OakInsertApcEnvironment
} OAK_KAPC_ENVIRONMENT;

typedef VOID(NTAPI* OAK_PKNORMAL_ROUTINE)(PVOID, PVOID, PVOID);
typedef VOID(NTAPI* OAK_PKKERNEL_ROUTINE)(
    PRKAPC, OAK_PKNORMAL_ROUTINE*, PVOID*, PVOID*, PVOID*);
typedef VOID(NTAPI* OAK_PKRUNDOWN_ROUTINE)(PRKAPC);

typedef VOID(NTAPI* tKeInitializeApc)(
    PRKAPC, PRKTHREAD, OAK_KAPC_ENVIRONMENT, OAK_PKKERNEL_ROUTINE, OAK_PKRUNDOWN_ROUTINE,
    OAK_PKNORMAL_ROUTINE, KPROCESSOR_MODE, PVOID);
typedef BOOLEAN(NTAPI* tKeInsertQueueApc)(PRKAPC, PVOID, PVOID, KPRIORITY);
typedef BOOLEAN(NTAPI* tKeAlertThread)(PKTHREAD, KPROCESSOR_MODE);

static tPsGetContextThread g_PsGetContextThread = NULL;
static tPsSetContextThread g_PsSetContextThread = NULL;
static tPsLookupThreadByThreadId g_PsLookupThreadByThreadId = NULL;
static tZwSusp g_ZwSuspendThread = NULL;
static tZwSusp g_ZwResumeThread = NULL;
static tZwQuerySys g_ZwQuerySystemInformation = NULL;
static tZwAlertThread g_ZwAlertThread = NULL;
static tKeInitializeApc g_KeInitializeApc = NULL;
static tKeInsertQueueApc g_KeInsertQueueApc = NULL;
static tKeAlertThread g_KeAlertThread = NULL;

static PVOID OakGetRoutine(const WCHAR* name)
{
    UNICODE_STRING u;
    RtlInitUnicodeString(&u, name);
    return MmGetSystemRoutineAddress(&u);
}

#define SystemProcessInformation 5

typedef struct _OAK_SYS_THREAD {
    LARGE_INTEGER KernelTime, UserTime, CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    CLIENT_ID ClientId;
    KPRIORITY Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    ULONG WaitReason;
} OAK_SYS_THREAD;

typedef struct _OAK_SYS_PROCESS {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    ULONGLONG Reserved[3];
    LARGE_INTEGER CreateTime, UserTime, KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize, VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage, PeakPagefileUsage, PrivatePageCount;
    LARGE_INTEGER ReadOperationCount, WriteOperationCount, OtherOperationCount;
    LARGE_INTEGER ReadTransferCount, WriteTransferCount, OtherTransferCount;
    OAK_SYS_THREAD Threads[1];
} OAK_SYS_PROCESS;

static BOOLEAN ResolveHijackApis()
{
    if (g_PsLookupThreadByThreadId && g_KeInitializeApc && g_KeInsertQueueApc
        && g_PsGetContextThread && g_PsSetContextThread && g_ZwSuspendThread && g_ZwResumeThread
        && g_ZwQuerySystemInformation)
        return TRUE;
    g_PsGetContextThread = (tPsGetContextThread)OakGetRoutine(L"PsGetContextThread");
    g_PsSetContextThread = (tPsSetContextThread)OakGetRoutine(L"PsSetContextThread");
    g_PsLookupThreadByThreadId = (tPsLookupThreadByThreadId)OakGetRoutine(L"PsLookupThreadByThreadId");
    g_ZwSuspendThread = (tZwSusp)OakGetRoutine(L"ZwSuspendThread");
    g_ZwResumeThread = (tZwSusp)OakGetRoutine(L"ZwResumeThread");
    g_ZwQuerySystemInformation = (tZwQuerySys)OakGetRoutine(L"ZwQuerySystemInformation");
    g_ZwAlertThread = (tZwAlertThread)OakGetRoutine(L"ZwAlertThread");
    g_KeInitializeApc = (tKeInitializeApc)OakGetRoutine(L"KeInitializeApc");
    g_KeInsertQueueApc = (tKeInsertQueueApc)OakGetRoutine(L"KeInsertQueueApc");
    g_KeAlertThread = (tKeAlertThread)OakGetRoutine(L"KeAlertThread");
    Log("Exec APIs Lookup=%p GetCtx=%p Susp=%p APC=%p",
        g_PsLookupThreadByThreadId, g_PsGetContextThread, g_ZwSuspendThread, g_KeInitializeApc);
    return g_PsLookupThreadByThreadId && g_ZwQuerySystemInformation
        && g_PsGetContextThread && g_PsSetContextThread
        && g_ZwSuspendThread && g_ZwResumeThread;
}

static BOOLEAN TargetHasModule(const char* Name)
{
    if (!g_State.TargetProcess || !Name) return FALSE;
    BOOLEAN found = FALSE;
    KAPC_STATE apc;
    KeStackAttachProcess(g_State.TargetProcess, &apc);
    __try {
        PPEB peb = PsGetProcessPeb(g_State.TargetProcess);
        if (FindModuleBase(peb, Name))
            found = TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    KeUnstackDetachProcess(&apc);
    return found;
}

static NTSTATUS CollectProcessTids(HANDLE Pid, HANDLE* OutTids, ULONG MaxTids, PULONG OutCount)
{
    *OutCount = 0;
    ULONG len = 0x40000;
    PVOID buf = ExAllocatePoolWithTag(NonPagedPool, len, DRIVER_TAG);
    if (!buf) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS st = g_ZwQuerySystemInformation(SystemProcessInformation, buf, len, &len);
    if (st == STATUS_INFO_LENGTH_MISMATCH)
    {
        ExFreePoolWithTag(buf, DRIVER_TAG);
        buf = ExAllocatePoolWithTag(NonPagedPool, len + 0x10000, DRIVER_TAG);
        if (!buf) return STATUS_INSUFFICIENT_RESOURCES;
        st = g_ZwQuerySystemInformation(SystemProcessInformation, buf, len + 0x10000, &len);
    }
    if (!NT_SUCCESS(st))
    {
        ExFreePoolWithTag(buf, DRIVER_TAG);
        return st;
    }

    for (OAK_SYS_PROCESS* p = (OAK_SYS_PROCESS*)buf;;)
    {
        if (p->UniqueProcessId == Pid)
        {
            ULONG n = p->NumberOfThreads;
            for (ULONG pass = 0; pass < 2 && *OutCount < MaxTids; pass++)
            {
                for (ULONG i = 0; i < n && *OutCount < MaxTids; i++)
                {
                    if (pass == 0 && i == 0 && n > 1) continue;
                    if (pass == 1 && i != 0) continue;
                    OutTids[*OutCount] = p->Threads[i].ClientId.UniqueThread;
                    (*OutCount)++;
                }
            }
            break;
        }
        if (!p->NextEntryOffset) break;
        p = (OAK_SYS_PROCESS*)((PUCHAR)p + p->NextEntryOffset);
    }
    ExFreePoolWithTag(buf, DRIVER_TAG);
    return (*OutCount > 0) ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

// (entry shell is g_EntryShell / g_ApcShell above)

static VOID NTAPI OakApcKernelRoutine(
    PRKAPC Apc,
    OAK_PKNORMAL_ROUTINE* NormalRoutine,
    PVOID* NormalContext,
    PVOID* SystemArgument1,
    PVOID* SystemArgument2)
{
    UNREFERENCED_PARAMETER(NormalRoutine);
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    ExFreePoolWithTag(Apc, DRIVER_TAG);
}

static BOOLEAN BeaconFlagExists(VOID);
static LARGE_INTEGER g_BeaconEpoch = { 0 };

static VOID ResetInjectBeacon(VOID)
{
    KeQuerySystemTime(&g_BeaconEpoch);
    static const UNICODE_STRING kBeacons[] = {
        RTL_CONSTANT_STRING(L"\\??\\C:\\oak\\dayz\\dll_attach.flag"),
        RTL_CONSTANT_STRING(L"\\??\\C:\\oak\\dll_attach.flag"),
    };
    for (ULONG i = 0; i < ARRAYSIZE(kBeacons); i++)
    {
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, (PUNICODE_STRING)&kBeacons[i],
            OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        ZwDeleteFile(&oa);
    }
}

// Kernel NEVER reads usermode shell fields (SMAP BSOD). Success = beacon file only.
static BOOLEAN WaitInjectSuccess(ULONG Tenths)
{
    for (ULONG i = 0; i < Tenths; i++)
    {
        LARGE_INTEGER d;
        d.QuadPart = -1000000LL; // 100ms
        KeDelayExecutionThread(KernelMode, FALSE, &d);

        if (BeaconFlagExists())
            return TRUE;

        if (!g_State.TargetProcess)
            return FALSE;
        if (PsGetProcessExitStatus(g_State.TargetProcess) != STATUS_PENDING)
            return BeaconFlagExists();
    }
    return BeaconFlagExists();
}

// Back-compat name for APC/hijack call sites
static BOOLEAN WaitShellDone(PVOID ShellAddr, ULONG Tenths)
{
    UNREFERENCED_PARAMETER(ShellAddr);
    return WaitInjectSuccess(Tenths);
}

static NTSTATUS WriteShellBlock(PVOID ShellAddr, const UCHAR* Code, ULONG CodeLen, BOOLEAN ClearDone)
{
    // Writes use attach+copy (same as section mapping — proven stable here).
    // Reads of Done use MmCopyVirtualMemory only (direct deref BSODs under SMAP).
    if (!g_State.TargetProcess || !ShellAddr) return STATUS_INVALID_PARAMETER;

    KAPC_STATE apc;
    KeStackAttachProcess(g_State.TargetProcess, &apc);
    NTSTATUS st = STATUS_SUCCESS;
    __try {
        PVOID base = ShellAddr;
        SIZE_T sz = sizeof(SHELLCODE_BLOCK);
        ULONG oldP = 0;
        ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_READWRITE, &oldP);

        if (Code && CodeLen)
        {
            if (CodeLen > sizeof(((SHELLCODE_BLOCK*)0)->Code))
                CodeLen = (ULONG)sizeof(((SHELLCODE_BLOCK*)0)->Code);
            RtlZeroMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code,
                sizeof(((SHELLCODE_BLOCK*)ShellAddr)->Code));
            RtlCopyMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code, Code, CodeLen);
        }
        if (ClearDone)
            ((SHELLCODE_BLOCK*)ShellAddr)->Done = 0;

        base = ShellAddr; sz = sizeof(SHELLCODE_BLOCK);
        // RWX required: shell writes Done (+ TLS/EH metadata live here)
        ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = STATUS_ACCESS_VIOLATION;
    }
    KeUnstackDetachProcess(&apc);
    return st;
}

static NTSTATUS ExecuteViaUserApc(HANDLE Pid, PVOID ShellAddr)
{
    if (!ResolveHijackApis()) return STATUS_NOT_SUPPORTED;

    // Switch shell to APC-returning stub
    {
        KAPC_STATE apc;
        KeStackAttachProcess(g_State.TargetProcess, &apc);
        __try {
            PVOID base = ShellAddr;
            SIZE_T sz = sizeof(SHELLCODE_BLOCK);
            ULONG oldP = 0;
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_READWRITE, &oldP);
            RtlZeroMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code, sizeof(((SHELLCODE_BLOCK*)ShellAddr)->Code));
            RtlCopyMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code, g_ApcShell, sizeof(g_ApcShell));
            ((SHELLCODE_BLOCK*)ShellAddr)->Done = 0;
            base = ShellAddr; sz = sizeof(SHELLCODE_BLOCK);
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            KeUnstackDetachProcess(&apc);
            return STATUS_ACCESS_VIOLATION;
        }
        KeUnstackDetachProcess(&apc);
    }

    HANDLE tids[48] = {0};
    ULONG tidCount = 0;
    NTSTATUS st = CollectProcessTids(Pid, tids, 48, &tidCount);
    if (!NT_SUCCESS(st) || tidCount == 0) return !NT_SUCCESS(st) ? st : STATUS_NOT_FOUND;

    ULONG queued = 0;
    ULONG maxQ = tidCount < 6 ? tidCount : 6;
    for (ULONG t = 0; t < maxQ; t++)
    {
        PETHREAD eth = NULL;
        if (!NT_SUCCESS(g_PsLookupThreadByThreadId(tids[t], &eth)) || !eth)
            continue;

        PKAPC kapc = (PKAPC)ExAllocatePoolWithTag(NonPagedPool, sizeof(KAPC), DRIVER_TAG);
        if (!kapc)
        {
            ObDereferenceObject(eth);
            break;
        }

        g_KeInitializeApc(
            kapc,
            (PKTHREAD)eth,
            OakOriginalApcEnvironment,
            OakApcKernelRoutine,
            NULL,
            (OAK_PKNORMAL_ROUTINE)ShellAddr,
            UserMode,
            ShellAddr);

        if (g_KeInsertQueueApc(kapc, NULL, NULL, 0))
        {
            queued++;
            if (g_KeAlertThread)
                g_KeAlertThread((PKTHREAD)eth, UserMode);
            if (g_ZwAlertThread)
            {
                HANDLE thr = NULL;
                if (NT_SUCCESS(ObOpenObjectByPointer(eth, OBJ_KERNEL_HANDLE, NULL,
                    0x0004 /* THREAD_ALERT */, *PsThreadType, KernelMode, &thr)) && thr)
                {
                    g_ZwAlertThread(thr);
                    ZwClose(thr);
                }
            }
        }
        else
        {
            ExFreePoolWithTag(kapc, DRIVER_TAG);
        }
        ObDereferenceObject(eth);
    }

    Log("APC: queued=%lu / cap=%lu, waiting...", queued, maxQ);
    if (queued == 0) return STATUS_UNSUCCESSFUL;

    if (WaitShellDone(ShellAddr, 100)) // 10s
    {
        Log("APC: DllMain OK (user APC)");
        return STATUS_SUCCESS;
    }
    Log("APC: timeout (threads may not be alertable)");
    // Do NOT return STATUS_TIMEOUT — NT_SUCCESS(0x102) is TRUE!
    return STATUS_UNSUCCESSFUL;
}

// Kernel APC runs in the target thread; Get/SetContext on self often works where
// cross-thread PsGetContextThread returns STATUS_UNSUCCESSFUL.
static PVOID g_KApcShell = NULL;
static volatile LONG g_KApcRedirected = 0;

static VOID NTAPI OakKernelRedirectApc(
    PRKAPC Apc,
    OAK_PKNORMAL_ROUTINE* NormalRoutine,
    PVOID* NormalContext,
    PVOID* SystemArgument1,
    PVOID* SystemArgument2)
{
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    if (NormalRoutine) *NormalRoutine = NULL;

    if (g_PsGetContextThread && g_PsSetContextThread && g_KApcShell &&
        InterlockedCompareExchange(&g_KApcRedirected, 1, 0) == 0)
    {
        PCONTEXT ctx = (PCONTEXT)ExAllocatePoolWithTag(NonPagedPool, sizeof(CONTEXT) + 64, DRIVER_TAG);
        if (ctx)
        {
            RtlZeroMemory(ctx, sizeof(CONTEXT));
            ctx->ContextFlags = CONTEXT_FULL;
            PETHREAD self = (PETHREAD)KeGetCurrentThread();
            NTSTATUS gst = g_PsGetContextThread(self, ctx, KernelMode);
            if (NT_SUCCESS(gst) && ctx->Rip > 0x7FF000000000ULL)
            {
                __try {
                    PVOID base = g_KApcShell;
                    SIZE_T sz = sizeof(SHELLCODE_BLOCK);
                    ULONG oldP = 0;
                    ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_READWRITE, &oldP);
                    ((SHELLCODE_BLOCK*)g_KApcShell)->ContinueRip = ctx->Rip;
                    ((SHELLCODE_BLOCK*)g_KApcShell)->Done = 0;
                    base = g_KApcShell; sz = sizeof(SHELLCODE_BLOCK);
                    ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    InterlockedExchange(&g_KApcRedirected, 0);
                    ExFreePoolWithTag(ctx, DRIVER_TAG);
                    ExFreePoolWithTag(Apc, DRIVER_TAG);
                    return;
                }
                ctx->Rip = (ULONG64)g_KApcShell;
                ctx->Rcx = (ULONG64)g_KApcShell;
                if (!NT_SUCCESS(g_PsSetContextThread(self, ctx, KernelMode)))
                    InterlockedExchange(&g_KApcRedirected, 0);
            }
            else
            {
                InterlockedExchange(&g_KApcRedirected, 0);
            }
            ExFreePoolWithTag(ctx, DRIVER_TAG);
        }
        else
        {
            InterlockedExchange(&g_KApcRedirected, 0);
        }
    }
    ExFreePoolWithTag(Apc, DRIVER_TAG);
}

static NTSTATUS ExecuteViaKernelApcRedirect(HANDLE Pid, PVOID ShellAddr)
{
    if (!ResolveHijackApis() || !g_PsGetContextThread || !g_PsSetContextThread)
        return STATUS_NOT_SUPPORTED;

    // jmp-style shell for RIP redirect
    {
        KAPC_STATE apc;
        KeStackAttachProcess(g_State.TargetProcess, &apc);
        __try {
            PVOID base = ShellAddr; SIZE_T sz = sizeof(SHELLCODE_BLOCK); ULONG oldP = 0;
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_READWRITE, &oldP);
            RtlZeroMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code, sizeof(((SHELLCODE_BLOCK*)ShellAddr)->Code));
            RtlCopyMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code, g_ExecShell, sizeof(g_ExecShell));
            ((SHELLCODE_BLOCK*)ShellAddr)->Done = 0;
            base = ShellAddr; sz = sizeof(SHELLCODE_BLOCK);
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            KeUnstackDetachProcess(&apc);
            return STATUS_ACCESS_VIOLATION;
        }
        KeUnstackDetachProcess(&apc);
    }

    g_KApcShell = ShellAddr;
    InterlockedExchange(&g_KApcRedirected, 0);

    HANDLE tids[16] = {0};
    ULONG tidCount = 0;
    NTSTATUS st = CollectProcessTids(Pid, tids, 16, &tidCount);
    if (!NT_SUCCESS(st) || tidCount == 0) return STATUS_NOT_FOUND;

    ULONG queued = 0;
    ULONG maxQ = tidCount < 4 ? tidCount : 4;
    for (ULONG t = 0; t < maxQ; t++)
    {
        PETHREAD eth = NULL;
        if (!NT_SUCCESS(g_PsLookupThreadByThreadId(tids[t], &eth)) || !eth)
            continue;
        PKAPC kapc = (PKAPC)ExAllocatePoolWithTag(NonPagedPool, sizeof(KAPC), DRIVER_TAG);
        if (!kapc) { ObDereferenceObject(eth); break; }

        g_KeInitializeApc(
            kapc, (PKTHREAD)eth, OakOriginalApcEnvironment,
            OakKernelRedirectApc, NULL, NULL, KernelMode, NULL);

        if (g_KeInsertQueueApc(kapc, NULL, NULL, 0))
        {
            queued++;
            // Nudge thread into kernel so APC can deliver
            if (g_ZwAlertThread)
            {
                HANDLE thr = NULL;
                if (NT_SUCCESS(ObOpenObjectByPointer(eth, OBJ_KERNEL_HANDLE, NULL,
                    THREAD_ALL_ACCESS, *PsThreadType, KernelMode, &thr)) && thr)
                {
                    g_ZwAlertThread(thr);
                    ZwClose(thr);
                }
            }
        }
        else
        {
            ExFreePoolWithTag(kapc, DRIVER_TAG);
        }
        ObDereferenceObject(eth);
    }

    Log("KAPC: queued=%lu, waiting redirect+DllMain...", queued);
    if (queued == 0) return STATUS_UNSUCCESSFUL;

    if (WaitShellDone(ShellAddr, 200))
    {
        Log("KAPC: DllMain OK (kernel APC redirect)");
        g_KApcShell = NULL;
        return STATUS_SUCCESS;
    }
    Log("KAPC: timeout redirected=%ld", g_KApcRedirected);
    g_KApcShell = NULL;
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS ExecuteViaThreadHijack(HANDLE Pid, PVOID ShellAddr)
{
    if (!g_PsGetContextThread || !g_PsSetContextThread || !g_ZwSuspendThread)
        return STATUS_NOT_SUPPORTED;

    // Restore hijack shell (call entry, jmp ContinueRip) — keep page RWX
    {
        KAPC_STATE apc;
        KeStackAttachProcess(g_State.TargetProcess, &apc);
        __try {
            PVOID base = ShellAddr; SIZE_T sz = sizeof(SHELLCODE_BLOCK); ULONG oldP = 0;
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_READWRITE, &oldP);
            RtlZeroMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code, sizeof(((SHELLCODE_BLOCK*)ShellAddr)->Code));
            RtlCopyMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code, g_HijackShell, sizeof(g_HijackShell));
            ((SHELLCODE_BLOCK*)ShellAddr)->Done = 0;
            base = ShellAddr; sz = sizeof(SHELLCODE_BLOCK);
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        KeUnstackDetachProcess(&apc);
    }

    HANDLE tids[32] = {0};
    ULONG tidCount = 0;
    NTSTATUS st = CollectProcessTids(Pid, tids, 32, &tidCount);
    if (!NT_SUCCESS(st) || tidCount == 0)
        return !NT_SUCCESS(st) ? st : STATUS_NOT_FOUND;
    Log("Hijack: candidate threads=%lu sizeof(CONTEXT)=%lu", tidCount, (ULONG)sizeof(CONTEXT));

    NTSTATUS last = STATUS_UNSUCCESSFUL;
    for (ULONG t = 0; t < tidCount; t++)
    {
        PETHREAD eth = NULL;
        st = g_PsLookupThreadByThreadId(tids[t], &eth);
        if (!NT_SUCCESS(st) || !eth) { last = st; continue; }

        HANDLE thr = NULL;
        st = ObOpenObjectByPointer(eth, OBJ_KERNEL_HANDLE, NULL,
            THREAD_ALL_ACCESS, *PsThreadType, KernelMode, &thr);
        if (!NT_SUCCESS(st) || !thr)
        {
            last = st;
            ObDereferenceObject(eth);
            continue;
        }

        st = g_ZwSuspendThread(thr, NULL);
        if (!NT_SUCCESS(st))
        {
            last = st;
            ObDereferenceObject(eth);
            ZwClose(thr);
            continue;
        }

        PCONTEXT ctx = (PCONTEXT)ExAllocatePoolWithTag(NonPagedPool, sizeof(CONTEXT) + 64, DRIVER_TAG);
        if (!ctx)
        {
            g_ZwResumeThread(thr, NULL);
            ObDereferenceObject(eth);
            ZwClose(thr);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(ctx, sizeof(CONTEXT));
        ctx->ContextFlags = CONTEXT_FULL;
        st = g_PsGetContextThread(eth, ctx, KernelMode);
        if (!NT_SUCCESS(st))
        {
            if (t < 3) Log("Hijack: PsGetContext tid=%p 0x%X", tids[t], st);
            last = st;
            ExFreePoolWithTag(ctx, DRIVER_TAG);
            g_ZwResumeThread(thr, NULL);
            ObDereferenceObject(eth);
            ZwClose(thr);
            continue;
        }

        if (ctx->Rip < 0x7FF000000000ULL)
        {
            last = STATUS_INVALID_THREAD;
            ExFreePoolWithTag(ctx, DRIVER_TAG);
            g_ZwResumeThread(thr, NULL);
            ObDereferenceObject(eth);
            ZwClose(thr);
            continue;
        }

        Log("Hijack: tid=%p RIP=0x%llX -> shellcode", tids[t], (ULONG64)ctx->Rip);

        {
            KAPC_STATE apc;
            KeStackAttachProcess(g_State.TargetProcess, &apc);
            __try {
                PVOID base = ShellAddr; SIZE_T sz = sizeof(SHELLCODE_BLOCK); ULONG oldP = 0;
                ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_READWRITE, &oldP);
                ((SHELLCODE_BLOCK*)ShellAddr)->ContinueRip = ctx->Rip;
                ((SHELLCODE_BLOCK*)ShellAddr)->Done = 0;
                base = ShellAddr; sz = sizeof(SHELLCODE_BLOCK);
                ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            KeUnstackDetachProcess(&apc);
        }

        ctx->Rip = (ULONG64)ShellAddr;
        ctx->Rcx = (ULONG64)ShellAddr;
        st = g_PsSetContextThread(eth, ctx, KernelMode);
        ExFreePoolWithTag(ctx, DRIVER_TAG);
        if (!NT_SUCCESS(st))
        {
            Log("Hijack: PsSetContext 0x%X", st);
            last = st;
            g_ZwResumeThread(thr, NULL);
            ObDereferenceObject(eth);
            ZwClose(thr);
            continue;
        }

        g_ZwResumeThread(thr, NULL);
        Log("Hijack: resumed, waiting beacon...");
        ObDereferenceObject(eth);
        ZwClose(thr);

        if (WaitInjectSuccess(200))
        {
            Log("Hijack: DllMain OK (beacon)");
            return STATUS_SUCCESS;
        }
        last = STATUS_IO_TIMEOUT;
        break; // don't hijack more threads after one attempt
    }

    Log("Hijack: failed last=0x%X", last);
    return last;
}

// ---- Special User APC via SSDT (fires on next kernel→user, no alertable wait) ----

typedef struct _OAK_KSERVICE_TABLE {
    PULONG OffsetTable;   // Win10+ relative offsets
    PVOID CounterTable;
    ULONG Limit;
    PUCHAR ArgumentTable;
} OAK_KSERVICE_TABLE;

static OAK_KSERVICE_TABLE* g_Ssdt = NULL;

static OAK_KSERVICE_TABLE* FindSsdt()
{
    if (g_Ssdt) return g_Ssdt;
    __try {
        ULONG64 lstar = __readmsr(0xC0000082); // KiSystemCall64 / Shadow
        for (ULONG i = 0; i < 0x1000; i++)
        {
            PUCHAR p = (PUCHAR)lstar + i;
            // lea r10, [KeServiceDescriptorTable]
            if (p[0] == 0x4C && p[1] == 0x8D && p[2] == 0x15)
            {
                LONG rel = *(LONG*)(p + 3);
                g_Ssdt = (OAK_KSERVICE_TABLE*)(p + 7 + rel);
                Log("SSDT @ %p (from LSTAR+%lu)", g_Ssdt, i);
                return g_Ssdt;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("SSDT: scan exception");
    }
    Log("SSDT: not found");
    return NULL;
}

static PVOID GetSsdtRoutine(ULONG Index)
{
    OAK_KSERVICE_TABLE* s = FindSsdt();
    if (!s || !s->OffsetTable || Index >= s->Limit) return NULL;
    LONG entry = (LONG)s->OffsetTable[Index];
    return (PVOID)((PUCHAR)s->OffsetTable + (entry >> 4));
}

static ULONG RvaToFileOffset(PIMAGE_NT_HEADERS64 Nt, ULONG Rva)
{
    PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(Nt);
    for (USHORT i = 0; i < Nt->FileHeader.NumberOfSections; i++)
    {
        ULONG va = sec[i].VirtualAddress;
        ULONG sz = sec[i].Misc.VirtualSize > sec[i].SizeOfRawData
            ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        if (Rva >= va && Rva < va + sz)
            return sec[i].PointerToRawData + (Rva - va);
    }
    return Rva;
}

static ULONG GetSyscallIdFromNtdllFile(const char* ExportName)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\SystemRoot\\System32\\ntdll.dll");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE h = NULL;
    IO_STATUS_BLOCK ios = {0};
    if (!NT_SUCCESS(ZwCreateFile(&h, FILE_READ_DATA | SYNCHRONIZE, &oa, &ios, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0)))
        return (ULONG)-1;

    FILE_STANDARD_INFORMATION fsi = {0};
    if (!NT_SUCCESS(ZwQueryInformationFile(h, &ios, &fsi, sizeof(fsi), FileStandardInformation))
        || fsi.EndOfFile.QuadPart < 0x1000 || fsi.EndOfFile.QuadPart > 8 * 1024 * 1024)
    {
        ZwClose(h);
        return (ULONG)-1;
    }

    ULONG size = (ULONG)fsi.EndOfFile.QuadPart;
    PVOID buf = ExAllocatePoolWithTag(PagedPool, size, DRIVER_TAG);
    if (!buf) { ZwClose(h); return (ULONG)-1; }

    LARGE_INTEGER off = {0};
    if (!NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &ios, buf, size, &off, NULL)))
    {
        ExFreePoolWithTag(buf, DRIVER_TAG);
        ZwClose(h);
        return (ULONG)-1;
    }
    ZwClose(h);

    ULONG syscallId = (ULONG)-1;
    __try {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)buf;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) __leave;
        PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)((PUCHAR)buf + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) __leave;
        ULONG er = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        if (!er) __leave;
        PIMAGE_EXPORT_DIRECTORY exp = (PIMAGE_EXPORT_DIRECTORY)((PUCHAR)buf + RvaToFileOffset(nt, er));
        PULONG names = (PULONG)((PUCHAR)buf + RvaToFileOffset(nt, exp->AddressOfNames));
        PUSHORT ords = (PUSHORT)((PUCHAR)buf + RvaToFileOffset(nt, exp->AddressOfNameOrdinals));
        PULONG funcs = (PULONG)((PUCHAR)buf + RvaToFileOffset(nt, exp->AddressOfFunctions));
        for (ULONG i = 0; i < exp->NumberOfNames; i++)
        {
            const char* n = (const char*)((PUCHAR)buf + RvaToFileOffset(nt, names[i]));
            if (strcmp(n, ExportName) != 0) continue;
            ULONG frva = funcs[ords[i]];
            PUCHAR stub = (PUCHAR)buf + RvaToFileOffset(nt, frva);
            // 4C 8B D1 B8 xx xx xx xx
            if (stub[0] == 0x4C && stub[1] == 0x8B && stub[2] == 0xD1 && stub[3] == 0xB8)
                syscallId = *(ULONG*)(stub + 4);
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    ExFreePoolWithTag(buf, DRIVER_TAG);
    return syscallId;
}

static NTSTATUS OakCfgMarkRange(HANDLE ProcessHandle, PVOID AllocBase, SIZE_T RegionSize,
    ULONG64* Targets, ULONG TargetCount)
{
    if (!ProcessHandle || !AllocBase || !RegionSize || !Targets || !TargetCount)
        return STATUS_INVALID_PARAMETER;

    ULONG id = GetSyscallIdFromNtdllFile("NtSetInformationVirtualMemory");
    if (id == (ULONG)-1) return STATUS_NOT_SUPPORTED;
    typedef NTSTATUS(NTAPI* tNtSIVM)(HANDLE, ULONG, ULONG_PTR, PVOID, PVOID, ULONG);
    tNtSIVM pFn = (tNtSIVM)GetSsdtRoutine(id);
    if (!pFn) return STATUS_NOT_FOUND;

    typedef struct _OAK_MEM_RANGE { PVOID VirtualAddress; SIZE_T NumberOfBytes; } OAK_MEM_RANGE;
    typedef struct _OAK_CFG_INFO { ULONG_PTR Offset; ULONG_PTR Flags; } OAK_CFG_INFO;
    typedef struct _OAK_CFG_LIST {
        ULONG NumberOfEntries;
        ULONG Reserved;
        PULONG NumberOfEntriesProcessed;
        OAK_CFG_INFO* CallTargetInfo;
        PVOID Section;
        ULONGLONG FileOffset;
    } OAK_CFG_LIST;

    OAK_CFG_INFO* infos = (OAK_CFG_INFO*)ExAllocatePoolWithTag(
        NonPagedPool, sizeof(OAK_CFG_INFO) * TargetCount, DRIVER_TAG);
    if (!infos) return STATUS_INSUFFICIENT_RESOURCES;

    for (ULONG i = 0; i < TargetCount; i++)
    {
        infos[i].Offset = (ULONG_PTR)(Targets[i] - (ULONG64)AllocBase);
        infos[i].Flags = 0x1; // CFG_CALL_TARGET_VALID
    }

    OAK_MEM_RANGE range;
    range.VirtualAddress = AllocBase;
    range.NumberOfBytes = RegionSize;

    ULONG processed = 0;
    OAK_CFG_LIST list = {0};
    list.NumberOfEntries = TargetCount;
    list.NumberOfEntriesProcessed = &processed;
    list.CallTargetInfo = infos;

    NTSTATUS st = STATUS_UNSUCCESSFUL;
    __try {
        st = pFn(ProcessHandle, 2, 1, &range, &list, sizeof(list));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = STATUS_ACCESS_VIOLATION;
    }

    // Alternate layout used by some samples: VmInformation = CFG_CALL_TARGET_INFO[]
    if (!NT_SUCCESS(st))
    {
        __try {
            st = pFn(ProcessHandle, 2, 1, &range, infos, sizeof(OAK_CFG_INFO) * TargetCount);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            st = STATUS_ACCESS_VIOLATION;
        }
    }

    ExFreePoolWithTag(infos, DRIVER_TAG);
    Log("CFG mark: base=%p size=0x%llX n=%lu st=0x%X proc=%lu",
        AllocBase, (ULONG64)RegionSize, TargetCount, st, processed);
    return st;
}

static NTSTATUS MarkShellCfgValid(HANDLE ProcessHandle, PVOID ShellAddr)
{
    MEMORY_BASIC_INFORMATION mbi = {0};
    SIZE_T retLen = 0;
    KAPC_STATE apc;
    KeStackAttachProcess(g_State.TargetProcess, &apc);
    NTSTATUS qst = ZwQueryVirtualMemory(ZwCurrentProcess(), ShellAddr, MemoryBasicInformation,
        &mbi, sizeof(mbi), &retLen);
    KeUnstackDetachProcess(&apc);
    if (!NT_SUCCESS(qst) || !mbi.AllocationBase)
    {
        // Shell alloc base is usually ShellAddr itself
        ULONG64 t = (ULONG64)ShellAddr;
        return OakCfgMarkRange(ProcessHandle, ShellAddr, 0x1000, &t, 1);
    }
    ULONG64 t = (ULONG64)ShellAddr;
    SIZE_T sz = mbi.RegionSize ? mbi.RegionSize : 0x1000;
    return OakCfgMarkRange(ProcessHandle, mbi.AllocationBase, sz, &t, 1);
}

// Mark OakMapEntry using full image allocation (offsets from ImageBase).
static NTSTATUS MarkMappedImageCfg(HANDLE ProcessHandle, PVOID ImageBase, SIZE_T ImageSize, ULONG64 Entry)
{
    if (!ProcessHandle || !ImageBase || ImageSize < 0x1000)
        return STATUS_INVALID_PARAMETER;

    ULONG64 targets[64];
    ULONG n = 0;
    if (Entry)
        targets[n++] = Entry;
    // Also mark aligned slots around entry (CFG granularity)
    if (Entry)
    {
        ULONG64 aligned = Entry & ~0xFULL;
        for (int i = -2; i <= 8 && n < 64; i++)
        {
            ULONG64 a = aligned + (ULONG64)(i * 16);
            if (a >= (ULONG64)ImageBase && a < (ULONG64)ImageBase + ImageSize)
                targets[n++] = a;
        }
    }
    if (n == 0)
    {
        targets[n++] = (ULONG64)ImageBase + 0x1000;
    }
    return OakCfgMarkRange(ProcessHandle, ImageBase, ImageSize, targets, n);
}

#define QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC 0x1

static NTSTATUS ExecuteViaSpecialUserApc(HANDLE Pid, PVOID ShellAddr)
{
    if (!ResolveHijackApis()) return STATUS_NOT_SUPPORTED;

    ULONG sysIdEx2 = GetSyscallIdFromNtdllFile("NtQueueApcThreadEx2");
    ULONG sysIdEx = GetSyscallIdFromNtdllFile("NtQueueApcThreadEx");
    BOOLEAN useEx2 = (sysIdEx2 != (ULONG)-1);
    ULONG sysId = useEx2 ? sysIdEx2 : sysIdEx;
    if (sysId == (ULONG)-1)
    {
        Log("SpecialAPC: syscall id not found");
        return STATUS_NOT_SUPPORTED;
    }

    typedef NTSTATUS(NTAPI* tNtQueueApcThreadEx2)(
        HANDLE, HANDLE, ULONG, PVOID, PVOID, PVOID, PVOID);
    typedef NTSTATUS(NTAPI* tNtQueueApcThreadEx)(
        HANDLE, HANDLE, PVOID, PVOID, PVOID, PVOID);
    PVOID pQueue = GetSsdtRoutine(sysId);
    if (!pQueue)
    {
        Log("SpecialAPC: SSDT[%lu] null", sysId);
        return STATUS_NOT_FOUND;
    }
    Log("SpecialAPC: SSDT[%lu]=%p ex2=%d", sysId, pQueue, (int)useEx2);

    // APC-returning stub; RCX will be Arg1 = ShellAddr
    {
        KAPC_STATE apc;
        KeStackAttachProcess(g_State.TargetProcess, &apc);
        __try {
            PVOID base = ShellAddr; SIZE_T sz = sizeof(SHELLCODE_BLOCK); ULONG oldP = 0;
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_READWRITE, &oldP);
            RtlZeroMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code, sizeof(((SHELLCODE_BLOCK*)ShellAddr)->Code));
            RtlCopyMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code, g_ApcShell, sizeof(g_ApcShell));
            ((SHELLCODE_BLOCK*)ShellAddr)->Done = 0;
            base = ShellAddr; sz = sizeof(SHELLCODE_BLOCK);
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            KeUnstackDetachProcess(&apc);
            return STATUS_ACCESS_VIOLATION;
        }
        KeUnstackDetachProcess(&apc);
    }

    HANDLE processHandle = NULL;
    NTSTATUS st = ObOpenObjectByPointer(g_State.TargetProcess, OBJ_KERNEL_HANDLE, NULL,
        PROCESS_ALL_ACCESS, *PsProcessType, KernelMode, &processHandle);
    if (NT_SUCCESS(st) && processHandle)
    {
        MarkShellCfgValid(processHandle, ShellAddr);
        ZwClose(processHandle);
    }

    HANDLE tids[16] = {0};
    ULONG tidCount = 0;
    st = CollectProcessTids(Pid, tids, 16, &tidCount);
    if (!NT_SUCCESS(st) || tidCount == 0) return STATUS_NOT_FOUND;

    ULONG queued = 0;
    ULONG maxQ = tidCount < 3 ? tidCount : 3;
    for (ULONG t = 0; t < maxQ; t++)
    {
        PETHREAD eth = NULL;
        if (!NT_SUCCESS(g_PsLookupThreadByThreadId(tids[t], &eth)) || !eth)
            continue;
        HANDLE thr = NULL;
        st = ObOpenObjectByPointer(eth, OBJ_KERNEL_HANDLE, NULL,
            0x0010 | 0x0040 /* THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION */,
            *PsThreadType, KernelMode, &thr);
        ObDereferenceObject(eth);
        if (!NT_SUCCESS(st) || !thr) continue;

        // Special user APC: ApcRoutine(Arg1, Arg2, Arg3) in usermode on next K→U
        if (useEx2)
            st = ((tNtQueueApcThreadEx2)pQueue)(thr, NULL, QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC,
                ShellAddr, ShellAddr, NULL, NULL);
        else
            st = ((tNtQueueApcThreadEx)pQueue)(thr, (HANDLE)(ULONG_PTR)QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC,
                ShellAddr, ShellAddr, NULL, NULL);
        Log("SpecialAPC: tid=%p queue=0x%X", tids[t], st);
        if (NT_SUCCESS(st)) queued++;
        ZwClose(thr);
        if (queued > 0) break; // one successful queue is enough
    }

    if (queued == 0)
    {
        Log("SpecialAPC: no queue success");
        return STATUS_UNSUCCESSFUL;
    }

    Log("SpecialAPC: waiting Done...");
    if (WaitShellDone(ShellAddr, 150))
    {
        Log("SpecialAPC: DllMain OK");
        return STATUS_SUCCESS;
    }
    Log("SpecialAPC: timeout");
    return STATUS_UNSUCCESSFUL;
}

// Instrumentation kept disabled — previous run BSOD 0x1E (KMODE AV writing shell VA).
#if 0
// Instrumentation callback shell: r10 = return RIP (Win10/11 x64).
static const UCHAR g_IcShell[] = {
    0x90
};
#endif

// One-shot hook shell: preserve regs, DllMain once, restore 12-byte hook site,
// then jmp back to HookTarget (re-enter clean syscall stub — no stolen-byte trampoline).
// Syscall stubs are 16+ bytes; a 12-byte trampoline splits `test [SharedUserData]`.
static const UCHAR g_HookShell[] = {
    0x50, 0x51, 0x52,                         // push rax,rcx,rdx
    0x41, 0x50, 0x41, 0x51, 0x41, 0x52,       // push r8,r9,r10
    0x41, 0x53,                               // push r11
    0x9C,                                     // pushfq
    0x48, 0x83, 0xEC, 0x28,                   // sub rsp, 0x28
    // @0x10: mov r11, imm64 (imm @0x12)
    0x49, 0xBB,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x41, 0x83, 0xBB, 0x98, 0x00, 0x00, 0x00, 0x01, // cmp Done,1
    0x74, 0x38,                               // je reenter
    0x49, 0x8B, 0x4B, 0x80,                   // mov rcx,[r11+DllBase]
    0x49, 0x8B, 0x43, 0x88,                   // mov rax,[r11+Entry]
    0xBA, 0x01, 0x00, 0x00, 0x00,
    0x45, 0x31, 0xC0,
    0xFF, 0xD0,                               // call DllMain
    0x41, 0xC7, 0x83, 0x98, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    // restore 12 hook bytes to HookTarget (must be RWX)
    0x49, 0x8B, 0x83, 0xB8, 0x00, 0x00, 0x00, // mov rax,[r11+0xB8] OrigBytes
    0x49, 0x8B, 0x8B, 0x90, 0x00, 0x00, 0x00, // mov rcx,[r11+0x90] HookTarget
    0x48, 0x89, 0x01,                         // mov [rcx],rax
    0x41, 0x8B, 0x83, 0xC0, 0x00, 0x00, 0x00, // mov eax,[r11+0xC0] OrigBytes+8
    0x89, 0x41, 0x08,                         // mov [rcx+8],eax
    // reenter @0x5C:
    0x48, 0x83, 0xC4, 0x28,
    0x9D,
    0x41, 0x5B,
    0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,
    0x5A, 0x59, 0x58,
    // @0x6C: mov rax, HookTarget; jmp rax  (imm @0x6E) — patched
    0x48, 0xB8,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xE0
};

static NTSTATUS ExecuteViaInlineHook(HANDLE Pid, PVOID ShellAddr)
{
    UNREFERENCED_PARAMETER(Pid);
    if (!g_State.TargetProcess || !ShellAddr) return STATUS_INVALID_PARAMETER;
    C_ASSERT(sizeof(g_HookShell) <= 128);

    ULONG64 hookTarget = 0;
    UCHAR orig[16] = {0};
    BOOLEAN ready = FALSE;
    const char* hookedName = "?";

    KAPC_STATE apc;
    KeStackAttachProcess(g_State.TargetProcess, &apc);
    __try {
        PPEB peb = PsGetProcessPeb(g_State.TargetProcess);

        // Prefer user32 message APIs — UI thread, called every frame, not a
        // syscall stub (less BE StartAddress / ntdll-integrity noise than Nt*).
        ULONG64 user32 = FindModuleBase(peb, "user32.dll");
        if (!user32) user32 = FindModuleBase(peb, "USER32.DLL");
        if (user32)
        {
            hookTarget = GetExportByName(user32, "PeekMessageW");
            if (hookTarget) hookedName = "user32!PeekMessageW";
            if (!hookTarget)
            {
                hookTarget = GetExportByName(user32, "PeekMessageA");
                if (hookTarget) hookedName = "user32!PeekMessageA";
            }
            if (!hookTarget)
            {
                hookTarget = GetExportByName(user32, "GetMessageW");
                if (hookTarget) hookedName = "user32!GetMessageW";
            }
        }

        if (!hookTarget)
        {
            ULONG64 ntdll = FindModuleBase(peb, "ntdll.dll");
            if (!ntdll) ntdll = FindModuleBase(peb, "NTDLL.DLL");
            if (ntdll)
            {
                hookTarget = GetExportByName(ntdll, "NtWaitForSingleObject");
                if (hookTarget) hookedName = "ntdll!NtWaitForSingleObject";
                if (!hookTarget)
                {
                    hookTarget = GetExportByName(ntdll, "NtDelayExecution");
                    if (hookTarget) hookedName = "ntdll!NtDelayExecution";
                }
            }
        }
        if (!hookTarget) { Log("Hook: no suitable export"); __leave; }

        RtlCopyMemory(orig, (PVOID)hookTarget, 12);
        Log("Hook: target=%s @0x%llX bytes %02X %02X %02X %02X",
            hookedName, hookTarget, orig[0], orig[1], orig[2], orig[3]);

        PVOID base = ShellAddr;
        SIZE_T sz = sizeof(SHELLCODE_BLOCK);
        ULONG oldP = 0;
        ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_READWRITE, &oldP);

        RtlZeroMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code, sizeof(((SHELLCODE_BLOCK*)ShellAddr)->Code));
        RtlCopyMemory(((SHELLCODE_BLOCK*)ShellAddr)->Code, g_HookShell, sizeof(g_HookShell));
        *(ULONG64*)((PUCHAR)ShellAddr + 0x12) = (ULONG64)ShellAddr;
        // Reenter original function after restore (not a stolen-byte trampoline)
        *(ULONG64*)((PUCHAR)ShellAddr + 0x6E) = hookTarget;

        ((SHELLCODE_BLOCK*)ShellAddr)->ContinueRip = hookTarget;
        ((SHELLCODE_BLOCK*)ShellAddr)->Done = 0;
        RtlCopyMemory(((SHELLCODE_BLOCK*)ShellAddr)->OrigBytes, orig, 12);

        UCHAR jmp[12];
        jmp[0] = 0x48; jmp[1] = 0xB8;
        *(ULONG64*)(jmp + 2) = (ULONG64)ShellAddr;
        jmp[10] = 0xFF; jmp[11] = 0xE0;

        base = (PVOID)hookTarget; sz = 12; oldP = 0;
        ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
        RtlCopyMemory((PVOID)hookTarget, jmp, 12);
        // Leave HookTarget RWX so usermode shell can restore orig bytes.

        base = ShellAddr; sz = sizeof(SHELLCODE_BLOCK); oldP = 0;
        ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);

        ready = TRUE;
        Log("Hook: installed (reenter) %s -> shell 0x%p", hookedName, ShellAddr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Hook: exception during install");
    }
    KeUnstackDetachProcess(&apc);

    if (!ready) return STATUS_UNSUCCESSFUL;

    if (WaitShellDone(ShellAddr, 200))
    {
        Log("Hook: DllMain OK (no new thread)");
        // Best-effort: set hook page back to RX
        KeStackAttachProcess(g_State.TargetProcess, &apc);
        __try {
            if (hookTarget) {
                PVOID base = (PVOID)hookTarget; SIZE_T sz = 12; ULONG oldP = 0;
                ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READ, &oldP);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        KeUnstackDetachProcess(&apc);
        return STATUS_SUCCESS;
    }

    KeStackAttachProcess(g_State.TargetProcess, &apc);
    __try {
        if (hookTarget)
        {
            PVOID base = (PVOID)hookTarget; SIZE_T sz = 12; ULONG oldP = 0;
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
            RtlCopyMemory((PVOID)hookTarget, orig, 12);
            base = (PVOID)hookTarget; sz = 12;
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READ, &oldP);
            Log("Hook: restored original bytes after timeout");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    KeUnstackDetachProcess(&apc);

    Log("Hook: timeout");
    return STATUS_UNSUCCESSFUL;
}

static BOOLEAN BeaconFlagExists()
{
    if (g_BeaconEpoch.QuadPart == 0)
        return FALSE;

    static const UNICODE_STRING kBeacons[] = {
        RTL_CONSTANT_STRING(L"\\??\\C:\\oak\\dayz\\dll_attach.flag"),
        RTL_CONSTANT_STRING(L"\\??\\C:\\oak\\dll_attach.flag"),
    };
    for (ULONG i = 0; i < ARRAYSIZE(kBeacons); i++)
    {
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, (PUNICODE_STRING)&kBeacons[i],
            OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        HANDLE h = NULL;
        IO_STATUS_BLOCK ios = {0};
        NTSTATUS st = ZwCreateFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &ios, NULL,
            FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        if (!NT_SUCCESS(st) || !h)
            continue;
        FILE_BASIC_INFORMATION fbi = {0};
        IO_STATUS_BLOCK qio = {0};
        st = ZwQueryInformationFile(h, &qio, &fbi, sizeof(fbi), FileBasicInformation);
        ZwClose(h);
        if (NT_SUCCESS(st) && fbi.LastWriteTime.QuadPart >= g_BeaconEpoch.QuadPart)
            return TRUE;
    }
    return FALSE;
}

static NTSTATUS ExecuteViaDirectRipHijack(HANDLE Pid, ULONG64 DllBase, ULONG64 Entry)
{
    // Set an existing thread's RIP = OakMapEntry, RCX = DllBase, plant return addr.
    // No indirect CALL → no CFG check. Quieter than new anonymous StartAddress.
    if (!ResolveHijackApis())
        return STATUS_NOT_SUPPORTED;
    if (!DllBase || !Entry)
        return STATUS_INVALID_PARAMETER;

    HANDLE tids[48] = {0};
    ULONG tidCount = 0;
    NTSTATUS st = CollectProcessTids(Pid, tids, 48, &tidCount);
    if (!NT_SUCCESS(st) || tidCount == 0)
        return STATUS_NOT_FOUND;

    Log("DirectHijack: threads=%lu Entry=0x%llX", tidCount, Entry);
    NTSTATUS last = STATUS_UNSUCCESSFUL;
    ULONG tried = 0;

    for (ULONG t = 0; t < tidCount && tried < 16; t++)
    {
        PETHREAD eth = NULL;
        if (!NT_SUCCESS(g_PsLookupThreadByThreadId(tids[t], &eth)) || !eth)
            continue;

        HANDLE thr = NULL;
        st = ObOpenObjectByPointer(eth, OBJ_KERNEL_HANDLE, NULL,
            THREAD_ALL_ACCESS, *PsThreadType, KernelMode, &thr);
        if (!NT_SUCCESS(st) || !thr)
        {
            ObDereferenceObject(eth);
            continue;
        }

        st = g_ZwSuspendThread(thr, NULL);
        if (!NT_SUCCESS(st))
        {
            if (tried < 3) Log("DirectHijack: Suspend tid=%p 0x%X", tids[t], st);
            last = st;
            ZwClose(thr);
            ObDereferenceObject(eth);
            continue;
        }
        tried++;

        // CONTEXT must be 16-byte aligned or PsGetContextThread fails randomly
        PUCHAR raw = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, sizeof(CONTEXT) + 64, DRIVER_TAG);
        if (!raw)
        {
            g_ZwResumeThread(thr, NULL);
            ZwClose(thr);
            ObDereferenceObject(eth);
            break;
        }
        PCONTEXT ctx = (PCONTEXT)(((ULONG_PTR)raw + 15) & ~(ULONG_PTR)15);
        RtlZeroMemory(ctx, sizeof(CONTEXT));
        ctx->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;

        st = g_PsGetContextThread(eth, ctx, KernelMode);
        if (!NT_SUCCESS(st))
        {
            if (tried <= 3) Log("DirectHijack: GetCtx tid=%p 0x%X", tids[t], st);
            last = st;
            ExFreePoolWithTag(raw, DRIVER_TAG);
            g_ZwResumeThread(thr, NULL);
            ZwClose(thr);
            ObDereferenceObject(eth);
            continue;
        }

        // Accept any usermode RIP (game module OR system DLL)
        if (ctx->Rip < 0x10000ULL || ctx->Rip > 0x00007FFFFFFFFFFFULL)
        {
            last = STATUS_INVALID_THREAD;
            ExFreePoolWithTag(raw, DRIVER_TAG);
            g_ZwResumeThread(thr, NULL);
            ZwClose(thr);
            ObDereferenceObject(eth);
            continue;
        }

        ULONG64 continueRip = ctx->Rip;
        // x64 ABI: at function entry RSP % 16 == 8
        ULONG64 rsp = ctx->Rsp & ~0xFULL;
        rsp -= 8;
        BOOLEAN stacked = FALSE;

        KAPC_STATE apc;
        KeStackAttachProcess(g_State.TargetProcess, &apc);
        __try {
            *(ULONG64*)rsp = continueRip;
            stacked = TRUE;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            stacked = FALSE;
        }
        KeUnstackDetachProcess(&apc);

        if (!stacked)
        {
            if (tried <= 3) Log("DirectHijack: stack write fail tid=%p rsp=0x%llX", tids[t], rsp);
            ExFreePoolWithTag(raw, DRIVER_TAG);
            g_ZwResumeThread(thr, NULL);
            ZwClose(thr);
            ObDereferenceObject(eth);
            continue;
        }

        ctx->Rsp = rsp;
        ctx->Rip = Entry;
        ctx->Rcx = DllBase;
        // Clear volatile state that can confuse resumed code paths
        ctx->Rax = 0;
        st = g_PsSetContextThread(eth, ctx, KernelMode);
        ExFreePoolWithTag(raw, DRIVER_TAG);
        if (!NT_SUCCESS(st))
        {
            Log("DirectHijack: SetCtx tid=%p 0x%X", tids[t], st);
            last = st;
            g_ZwResumeThread(thr, NULL);
            ZwClose(thr);
            ObDereferenceObject(eth);
            continue;
        }

        Log("DirectHijack: tid=%p wasRIP=0x%llX -> Entry, resume", tids[t], continueRip);
        g_ZwResumeThread(thr, NULL);
        ZwClose(thr);
        ObDereferenceObject(eth);

        if (WaitInjectSuccess(120))
        {
            Log("DirectHijack: DllMain OK (no new thread)");
            return STATUS_SUCCESS;
        }
        last = STATUS_IO_TIMEOUT;
        // Try next thread if beacon missed (hijacked a dying/stuck thread)
    }

    Log("DirectHijack: failed last=0x%X tried=%lu", last, tried);
    return last;
}

static NTSTATUS ExecuteViaSpoofedModuleThread(ULONG64 DllBase, ULONG64 Entry)
{
    // RtlCreateUserThread with StartAddress INSIDE user32.dll (codecave).
    // BE StartAddress checks see a legitimate module, not anonymous RX.
    // Note: cave still CALL Entry — may hit CFG; DirectRipHijack is preferred.
    if (!g_State.TargetProcess || !DllBase || !Entry)
        return STATUS_INVALID_PARAMETER;

    UNICODE_STRING fn = RTL_CONSTANT_STRING(L"RtlCreateUserThread");
    typedef NTSTATUS(NTAPI* tRtlCreateUserThread)(
        HANDLE, PSECURITY_DESCRIPTOR, BOOLEAN, ULONG, PSIZE_T, PSIZE_T,
        PVOID, PVOID, PHANDLE, PVOID);
    tRtlCreateUserThread pCreate = (tRtlCreateUserThread)MmGetSystemRoutineAddress(&fn);
    if (!pCreate) return STATUS_NOT_SUPPORTED;

    HANDLE processHandle = NULL;
    NTSTATUS st = ObOpenObjectByPointer(g_State.TargetProcess, OBJ_KERNEL_HANDLE, NULL,
        PROCESS_ALL_ACCESS, *PsProcessType, KernelMode, &processHandle);
    if (!NT_SUCCESS(st) || !processHandle) return st;

    ULONG64 cave = 0;
    UCHAR origCave[48] = {0};
    UCHAR stub[48] = {0};
    // rcx=DllBase (StartParameter). call OakMapEntry, ret.
    // 48 83 EC 28             sub rsp,28h
    // 48 B8 imm64             mov rax, Entry
    // FF D0                   call rax
    // 48 83 C4 28             add rsp,28h
    // 31 C0                   xor eax,eax
    // C3                      ret
    stub[0] = 0x48; stub[1] = 0x83; stub[2] = 0xEC; stub[3] = 0x28;
    stub[4] = 0x48; stub[5] = 0xB8;
    *(ULONG64*)(stub + 6) = Entry;
    stub[14] = 0xFF; stub[15] = 0xD0;
    stub[16] = 0x48; stub[17] = 0x83; stub[18] = 0xC4; stub[19] = 0x28;
    stub[20] = 0x31; stub[21] = 0xC0;
    stub[22] = 0xC3;
    const ULONG stubLen = 23;

    KAPC_STATE apc;
    KeStackAttachProcess(g_State.TargetProcess, &apc);
    __try {
        PPEB peb = PsGetProcessPeb(g_State.TargetProcess);
        ULONG64 user32 = FindModuleBase(peb, "user32.dll");
        if (!user32) user32 = FindModuleBase(peb, "USER32.DLL");
        if (!user32) { Log("Spoof: user32 missing"); __leave; }

        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)user32;
        PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)(user32 + dos->e_lfanew);
        PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
        for (USHORT i = 0; i < nt->FileHeader.NumberOfSections && !cave; i++)
        {
            if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;
            ULONG64 secBase = user32 + sec[i].VirtualAddress;
            ULONG secSize = sec[i].Misc.VirtualSize;
            if (secSize < 64) continue;
            // Prefer trailing padding (zeros / int3)
            for (ULONG off = secSize; off > 64; off--)
            {
                ULONG start = off - 64;
                BOOLEAN ok = TRUE;
                for (ULONG j = 0; j < 64; j++)
                {
                    UCHAR b = *(UCHAR*)(secBase + start + j);
                    if (b != 0x00 && b != 0xCC) { ok = FALSE; break; }
                }
                if (ok)
                {
                    cave = secBase + start;
                    break;
                }
            }
        }
        if (!cave)
        {
            Log("Spoof: no codecave in user32");
            __leave;
        }

        RtlCopyMemory(origCave, (PVOID)cave, stubLen);
        PVOID base = (PVOID)cave; SIZE_T sz = stubLen; ULONG oldP = 0;
        ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
        RtlCopyMemory((PVOID)cave, stub, stubLen);
        base = (PVOID)cave; sz = stubLen;
        ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READ, &oldP);
        Log("Spoof: cave=0x%llX in user32 (StartAddress spoof)", cave);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Spoof: exception finding cave");
        cave = 0;
    }
    KeUnstackDetachProcess(&apc);

    if (!cave)
    {
        ZwClose(processHandle);
        return STATUS_NOT_FOUND;
    }

    HANDLE threadHandle = NULL;
    st = pCreate(processHandle, NULL, FALSE, 0, NULL, NULL,
        (PVOID)cave, (PVOID)DllBase, &threadHandle, NULL);
    Log("Spoof: Create=0x%X thr=%p start=user32!cave", st, threadHandle);

    BOOLEAN ok = FALSE;
    if (NT_SUCCESS(st) && threadHandle)
    {
        ok = WaitInjectSuccess(200);
        if (!ok)
        {
            LARGE_INTEGER timeout;
            timeout.QuadPart = -50000000LL;
            ZwWaitForSingleObject(threadHandle, FALSE, &timeout);
            ok = WaitInjectSuccess(50);
        }
        ZwClose(threadHandle);
    }

    // Restore cave bytes
    KeStackAttachProcess(g_State.TargetProcess, &apc);
    __try {
        PVOID base = (PVOID)cave; SIZE_T sz = stubLen; ULONG oldP = 0;
        ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
        RtlCopyMemory((PVOID)cave, origCave, stubLen);
        base = (PVOID)cave; sz = stubLen;
        ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READ, &oldP);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    KeUnstackDetachProcess(&apc);

    ZwClose(processHandle);
    if (ok || BeaconFlagExists())
    {
        Log("Spoof: DllMain OK (module StartAddress)");
        return STATUS_SUCCESS;
    }
    Log("Spoof: timeout");
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS ExecuteDllMainShell(HANDLE Pid, PVOID ShellAddr, ULONG64 DllBase, ULONG64 Entry)
{
    // Quiet-first execution (BE cares about NEW threads with anonymous StartAddress).
    // Success = C:\oak\dll_attach.flag beacon. Never read usermode Done (SMAP).
    if (!DllBase || !Entry)
    {
        Log("Exec: missing DllBase/Entry");
        return STATUS_INVALID_PARAMETER;
    }
    Log("Exec: quiet-first Entry=0x%llX DllBase=0x%llX shell=0x%p", Entry, DllBase, ShellAddr);
    ResetInjectBeacon();

    {
        KAPC_STATE apc;
        KeStackAttachProcess(g_State.TargetProcess, &apc);
        __try {
            PVOID base = ShellAddr; SIZE_T sz = sizeof(SHELLCODE_BLOCK); ULONG oldP = 0;
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_READWRITE, &oldP);
            ((SHELLCODE_BLOCK*)ShellAddr)->DllBase = DllBase;
            ((SHELLCODE_BLOCK*)ShellAddr)->EntryPoint = Entry;
            ((SHELLCODE_BLOCK*)ShellAddr)->Done = 0;
            base = ShellAddr; sz = sizeof(SHELLCODE_BLOCK);
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldP);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        KeUnstackDetachProcess(&apc);
    }

    NTSTATUS st;
    NTSTATUS last = STATUS_UNSUCCESSFUL;

    // 1) Direct RIP hijack — quietest (no new thread, no CFG call)
    Log("Exec[1/4]: direct RIP hijack -> OakMapEntry...");
    st = ExecuteViaDirectRipHijack(Pid, DllBase, Entry);
    if (NT_SUCCESS(st) || BeaconFlagExists())
    {
        Log("Exec: SUCCESS via direct RIP hijack");
        return STATUS_SUCCESS;
    }
    last = st;

    // 2) Spoofed StartAddress inside user32 (proven under BE vanilla)
    Log("Exec[2/4]: spoofed StartAddress in user32...");
    st = ExecuteViaSpoofedModuleThread(DllBase, Entry);
    if (NT_SUCCESS(st) || BeaconFlagExists())
    {
        Log("Exec: SUCCESS via spoofed module thread");
        return STATUS_SUCCESS;
    }
    last = st;

    // 3) PeekMessage hook (needs CFG mark)
    Log("Exec[3/4]: inline hook (PeekMessage)...");
    st = ExecuteViaInlineHook(Pid, ShellAddr);
    if (NT_SUCCESS(st) || BeaconFlagExists())
    {
        Log("Exec: SUCCESS via inline hook");
        return STATUS_SUCCESS;
    }
    last = st;

    // 4) Loud anonymous StartAddress — skip when BEClient is loaded
    BOOLEAN hasBe = TargetHasModule("BEClient_x64.dll") || TargetHasModule("BEClient.dll");
    if (hasBe)
    {
        Log("Exec[4/4]: SKIP loud RtlThread (BEClient present) last=0x%X", last);
        if (BeaconFlagExists())
            return STATUS_SUCCESS;
        // One more spoof retry before giving up
        Log("Exec: spoof retry under BE...");
        st = ExecuteViaSpoofedModuleThread(DllBase, Entry);
        if (NT_SUCCESS(st) || BeaconFlagExists())
        {
            Log("Exec: SUCCESS via spoof retry");
            return STATUS_SUCCESS;
        }
        return !NT_SUCCESS(last) ? last : STATUS_UNSUCCESSFUL;
    }

    Log("Exec[4/4]: RtlCreateUserThread(OakMapEntry) loud fallback (NoBE)...");
    UNICODE_STRING fn = RTL_CONSTANT_STRING(L"RtlCreateUserThread");
    typedef NTSTATUS(NTAPI* tRtlCreateUserThread)(
        HANDLE, PSECURITY_DESCRIPTOR, BOOLEAN, ULONG, PSIZE_T, PSIZE_T,
        PVOID, PVOID, PHANDLE, PVOID);
    tRtlCreateUserThread pCreate = (tRtlCreateUserThread)MmGetSystemRoutineAddress(&fn);
    if (!pCreate)
        return last;

    HANDLE processHandle = NULL;
    st = ObOpenObjectByPointer(g_State.TargetProcess, OBJ_KERNEL_HANDLE, NULL,
        PROCESS_ALL_ACCESS, *PsProcessType, KernelMode, &processHandle);
    if (!NT_SUCCESS(st) || !processHandle)
        return st;

    if (PsGetProcessExitStatus(g_State.TargetProcess) != STATUS_PENDING)
    {
        ZwClose(processHandle);
        return STATUS_PROCESS_IS_TERMINATING;
    }

    HANDLE threadHandle = NULL;
    st = pCreate(processHandle, NULL, FALSE, 0, NULL, NULL,
        (PVOID)Entry, (PVOID)DllBase, &threadHandle, NULL);
    Log("RtlThread: Create=0x%X thr=%p (direct OakMapEntry)", st, threadHandle);

    if (NT_SUCCESS(st) && threadHandle)
    {
        BOOLEAN ok = WaitInjectSuccess(200);
        if (!ok)
        {
            LARGE_INTEGER timeout;
            timeout.QuadPart = -50000000LL;
            ZwWaitForSingleObject(threadHandle, FALSE, &timeout);
            ok = WaitInjectSuccess(50);
        }
        ZwClose(threadHandle);
        ZwClose(processHandle);
        if (ok)
        {
            Log("RtlThread: DllMain OK (beacon) — used loud path");
            return STATUS_SUCCESS;
        }
        Log("RtlThread: timeout / no beacon");
        last = STATUS_UNSUCCESSFUL;
    }
    else
    {
        ZwClose(processHandle);
        if (st == STATUS_PROCESS_IS_TERMINATING)
            return st;
        last = st;
    }

    if (BeaconFlagExists())
        return STATUS_SUCCESS;
    Log("Exec: ALL paths failed last=0x%X", last);
    return last;
}

NTSTATUS MapDllToProcess(PVOID DllBuffer, SIZE_T DllSize, PULONG64 MappedBase)
{
    UNREFERENCED_PARAMETER(DllSize);
    
    if (!g_State.TargetProcess || !DllBuffer || !MappedBase)
        return STATUS_INVALID_PARAMETER;
    
    *MappedBase = 0;
    NTSTATUS status;
    
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)DllBuffer;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return STATUS_INVALID_IMAGE_FORMAT;
    
    PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)((PUCHAR)DllBuffer + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return STATUS_INVALID_IMAGE_FORMAT;
    
    SIZE_T imageSize = nt->OptionalHeader.SizeOfImage;
    Log("Image size: 0x%llX", (ULONG64)imageSize);
    
    HANDLE processHandle = NULL;
    status = ObOpenObjectByPointer(g_State.TargetProcess, OBJ_KERNEL_HANDLE, NULL,
        PROCESS_ALL_ACCESS, *PsProcessType, KernelMode, &processHandle);
    if (!NT_SUCCESS(status))
    {
        Log("ERROR: ObOpenObjectByPointer: 0x%X", status);
        return status;
    }
    
    PVOID remoteBase = NULL;
    SIZE_T regionSize = imageSize;
    status = ZwAllocateVirtualMemory(processHandle, &remoteBase, 0, &regionSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!NT_SUCCESS(status))
    {
        ZwClose(processHandle);
        Log("ERROR: ZwAllocateVirtualMemory: 0x%X", status);
        return status;
    }
    Log("Allocated at 0x%p (RW)", remoteBase);
    
    KAPC_STATE apc;
    KeStackAttachProcess(g_State.TargetProcess, &apc);
    
    __try { RtlCopyMemory(remoteBase, DllBuffer, nt->OptionalHeader.SizeOfHeaders); }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("ERROR: Header copy failed"); }
    
    PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
    for (USHORT i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        if (sec[i].SizeOfRawData)
        {
            __try {
                RtlCopyMemory((PUCHAR)remoteBase + sec[i].VirtualAddress,
                    (PUCHAR)DllBuffer + sec[i].PointerToRawData, sec[i].SizeOfRawData);
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    Log("Sections copied");
    
    ULONG64 delta = (ULONG64)remoteBase - nt->OptionalHeader.ImageBase;
    if (delta)
    {
        PIMAGE_DATA_DIRECTORY relocDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (relocDir->Size)
        {
            PIMAGE_BASE_RELOCATION reloc = (PIMAGE_BASE_RELOCATION)((PUCHAR)remoteBase + relocDir->VirtualAddress);
            __try {
                while (reloc->VirtualAddress && reloc->SizeOfBlock)
                {
                    ULONG cnt = (reloc->SizeOfBlock - 8) / 2;
                    PUSHORT list = (PUSHORT)((PUCHAR)reloc + 8);
                    for (ULONG j = 0; j < cnt; j++)
                    {
                        if ((list[j] >> 12) == IMAGE_REL_BASED_DIR64)
                        {
                            PULONG64 p = (PULONG64)((PUCHAR)remoteBase + reloc->VirtualAddress + (list[j] & 0xFFF));
                            *p += delta;
                        }
                    }
                    reloc = (PIMAGE_BASE_RELOCATION)((PUCHAR)reloc + reloc->SizeOfBlock);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
        Log("Relocations done (delta=0x%llX)", delta);
    }
    
    PPEB peb = PsGetProcessPeb(g_State.TargetProcess);
    PIMAGE_DATA_DIRECTORY impDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (peb && impDir->Size)
    {
        PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)((PUCHAR)remoteBase + impDir->VirtualAddress);
        __try {
            while (imp->Name)
            {
                char* modName = (char*)((PUCHAR)remoteBase + imp->Name);
                const char* actualName = modName;
                if (_strnicmp(modName, "api-ms-win-crt-", 15) == 0)
                    actualName = "ucrtbase.dll";
                else if (_strnicmp(modName, "d3dcompiler", 11) == 0)
                { imp++; continue; }
                
                ULONG64 modBase = FindModuleBase(peb, actualName);
                if (modBase)
                {
                    PIMAGE_THUNK_DATA64 oft = (PIMAGE_THUNK_DATA64)((PUCHAR)remoteBase + imp->OriginalFirstThunk);
                    PIMAGE_THUNK_DATA64 ft = (PIMAGE_THUNK_DATA64)((PUCHAR)remoteBase + imp->FirstThunk);
                    while (oft->u1.AddressOfData)
                    {
                        if (!(oft->u1.Ordinal & IMAGE_ORDINAL_FLAG64))
                        {
                            PIMAGE_IMPORT_BY_NAME ibn = (PIMAGE_IMPORT_BY_NAME)((PUCHAR)remoteBase + oft->u1.AddressOfData);
                            ULONG64 func = GetExportByName(modBase, ibn->Name);
                            if (func) ft->u1.Function = func;
                        }
                        oft++; ft++;
                    }
                }
                imp++;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        Log("Imports resolved");
    }
    
    {
        PIMAGE_SECTION_HEADER secProt = IMAGE_FIRST_SECTION(nt);
        for (USHORT i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            ULONG prot = PAGE_READONLY;
            BOOLEAN exec = (secProt[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            BOOLEAN write = (secProt[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
            if (exec && write) prot = PAGE_EXECUTE_READWRITE;
            else if (exec) prot = PAGE_EXECUTE_READ;
            else if (write) prot = PAGE_READWRITE;

            PVOID base = (PUCHAR)remoteBase + secProt[i].VirtualAddress;
            SIZE_T sz = secProt[i].Misc.VirtualSize;
            if (sz == 0) continue;
            ULONG oldProt = 0;
            ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, prot, &oldProt);
        }
        Log("Section protections applied");
    }
    
    PVOID shellAddr = NULL;
    SIZE_T shellSize = sizeof(SHELLCODE_BLOCK);
    status = ZwAllocateVirtualMemory(ZwCurrentProcess(), &shellAddr, 0, &shellSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!NT_SUCCESS(status))
    {
        KeUnstackDetachProcess(&apc);
        ZwClose(processHandle);
        Log("ERROR: Shellcode alloc: 0x%X", status);
        return status;
    }
    
    SHELLCODE_BLOCK block = {0};
    RtlCopyMemory(block.Code, g_EntryShell, sizeof(g_EntryShell));
    block.DllBase = (ULONG64)remoteBase;
    block.EntryPoint = (ULONG64)remoteBase + nt->OptionalHeader.AddressOfEntryPoint;

    // Prefer exported OakMapEntry — skips CRT/static TLS (manual-map killer on Win11)
    {
        ULONG64 oakEntry = GetExportByName((ULONG64)remoteBase, "OakMapEntry");
        if (oakEntry)
        {
            block.EntryPoint = oakEntry;
            Log("Entry: OakMapEntry @ 0x%llX (CRT bypass)", oakEntry);
        }
        else
        {
            Log("Entry: PE AddressOfEntryPoint @ 0x%llX (no OakMapEntry export)", block.EntryPoint);
        }
    }

    // Exception directory → RtlAddFunctionTable (x64 SEH)
    {
        PIMAGE_DATA_DIRECTORY exc = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (exc->VirtualAddress && exc->Size >= sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY))
        {
            ULONG64 ntdll = FindModuleBase(peb, "ntdll.dll");
            if (!ntdll) ntdll = FindModuleBase(peb, "NTDLL.DLL");
            ULONG64 pAdd = ntdll ? GetExportByName(ntdll, "RtlAddFunctionTable") : 0;
            if (pAdd)
            {
                block.pRtlAddFunctionTable = pAdd;
                block.ExceptionTable = (ULONG64)remoteBase + exc->VirtualAddress;
                block.ExceptionCount = exc->Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
                Log("EH: RtlAddFunctionTable=%p entries=%lu", (PVOID)pAdd, block.ExceptionCount);
            }
        }
    }

    // TLS directory ignored — OakMapEntry bypasses CRT; manual TLS callbacks without
    // LdrpHandleTlsData are unsafe and were double-invoked with CRT previously.

    RtlCopyMemory(shellAddr, &block, sizeof(block));
    {
        ULONG oldProt = 0;
        PVOID base = shellAddr;
        SIZE_T sz = shellSize;
        ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, PAGE_EXECUTE_READWRITE, &oldProt);
    }
    
    Log("Shellcode at 0x%p Entry=0x%llX", shellAddr, block.EntryPoint);
    KeUnstackDetachProcess(&apc);
    
    // CFG: allow call/jmp into mapped image + shell from existing threads (PeekMessage hook)
    MarkMappedImageCfg(processHandle, remoteBase, imageSize, block.EntryPoint);
    MarkShellCfgValid(processHandle, shellAddr);

    status = ExecuteDllMainShell(g_State.TargetProcessId, shellAddr, block.DllBase, block.EntryPoint);
    if (!NT_SUCCESS(status))
    {
        Log("DllMain exec failed 0x%X", status);
        ZwClose(processHandle);
        return status;
    }

    // Skip PE header wipe for now — can race with early cheat init / crash tools
    Log("PE headers retained (wipe disabled)");
    /*
    {
        KAPC_STATE wipeApc;
        KeStackAttachProcess(g_State.TargetProcess, &wipeApc);
        __try {
            SIZE_T wipeSize = nt->OptionalHeader.SizeOfHeaders;
            if (wipeSize > 0x1000) wipeSize = 0x1000;
            RtlZeroMemory(remoteBase, wipeSize);
            Log("PE headers wiped post-entry");
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        KeUnstackDetachProcess(&wipeApc);
    }
    */

    {
        LARGE_INTEGER d; d.QuadPart = -5000000LL; // 0.5s
        KeDelayExecutionThread(KernelMode, FALSE, &d);
        SIZE_T freeSize = 0;
        PVOID freeBase = shellAddr;
        ZwFreeVirtualMemory(processHandle, &freeBase, &freeSize, MEM_RELEASE);
        Log("Shellcode freed");
    }
    
    ZwClose(processHandle);
    *MappedBase = (ULONG64)remoteBase;
    return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
}


ULONG64 FindModuleBase(PPEB Peb, const char* Name)
{
    if (!Peb || !Name) return 0;
    __try {
        PPEB_LDR_DATA_FULL ldr = *(PPEB_LDR_DATA_FULL*)((PUCHAR)Peb + 0x18);
        if (!ldr) return 0;
        PLIST_ENTRY head = &ldr->InLoadOrderModuleList;
        for (PLIST_ENTRY cur = head->Flink; cur != head; cur = cur->Flink)
        {
            PLDR_DATA_TABLE_ENTRY_FULL e = CONTAINING_RECORD(cur, LDR_DATA_TABLE_ENTRY_FULL, InLoadOrderLinks);
            if (e->BaseDllName.Buffer)
            {
                ANSI_STRING as = {0};
                if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&as, &e->BaseDllName, TRUE)))
                {
                    BOOLEAN m = (_stricmp(as.Buffer, Name) == 0);
                    PVOID b = e->DllBase;
                    RtlFreeAnsiString(&as);
                    if (m) return (ULONG64)b;
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}


ULONG64 GetExportByName(ULONG64 Base, const char* Name)
{
    if (!Base || !Name) return 0;
    __try {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)Base;
        PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)(Base + dos->e_lfanew);
        PIMAGE_DATA_DIRECTORY ed = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!ed->Size) return 0;
        PIMAGE_EXPORT_DIRECTORY exp = (PIMAGE_EXPORT_DIRECTORY)(Base + ed->VirtualAddress);
        PULONG names = (PULONG)(Base + exp->AddressOfNames);
        PUSHORT ords = (PUSHORT)(Base + exp->AddressOfNameOrdinals);
        PULONG funcs = (PULONG)(Base + exp->AddressOfFunctions);
        for (ULONG i = 0; i < exp->NumberOfNames; i++)
        {
            if (strcmp((char*)(Base + names[i]), Name) == 0)
                return Base + funcs[ords[i]];
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

ULONG64 GetExportByOrdinal(ULONG64 Base, USHORT Ord) { UNREFERENCED_PARAMETER(Base); UNREFERENCED_PARAMETER(Ord); return 0; }
