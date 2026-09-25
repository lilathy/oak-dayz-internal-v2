#include "oak_crash_hunt.h"
#include "oak_lab.h"

#include <stdio.h>

static HMODULE g_Module = nullptr;
static PVOID g_VehHandle = nullptr;
static HANDLE g_WatchdogThread = nullptr;
static volatile LONG g_WatchdogStop = 0;
static volatile LONG g_PresentPulse = 0;
static volatile DWORD g_LastPresentTick = 0;
static volatile LONG g_GateCount = 0;
static volatile LONG g_FatalCount = 0;
static volatile LONG g_VehCount = 0;
static char g_LastGateTag[64] = {};
static DWORD g_LastGateCode = 0;
static DWORD g_LastGateTick = 0;

static void OakCrashWrite(const char* line)
{
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH))
        return;
    char file[MAX_PATH];
    wsprintfA(file, "%s\\DayZ\\oak_imgui.log", path);
    HANDLE hf = CreateFileA(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE)
        return;
    DWORD wr = 0;
    WriteFile(hf, line, (DWORD)lstrlenA(line), &wr, nullptr);
    WriteFile(hf, "\r\n", 2, &wr, nullptr);
    CloseHandle(hf);
}

static bool OakModuleIsManualMapped(HMODULE mod)
{
    if (!mod)
        return true;
    HMODULE fromAddr = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)mod, &fromAddr))
        return true;
    return fromAddr != mod;
}

static void OakCrashWriteStack(const char* prefix, int skip)
{
    void* frames[12];
    USHORT n = CaptureStackBackTrace((ULONG)skip, 12, frames, nullptr);
    char line[128];
    for (USHORT i = 0; i < n; i++)
    {
        wsprintfA(line, "crashstack[%s] #%u %p", prefix, (unsigned)i, frames[i]);
        OakCrashWrite(line);
    }
}

static const char* OakExceptionName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case 0xE06D7363: return "CPP_EH"; // MSVC C++ exception
    default: return "?";
    }
}

static void OakModRva(uintptr_t addr, char* out, int outN);

// --- Native call safety ------------------------------------------------------
bool OakNativeEntryOk(uintptr_t target, const char* what)
{
    // Log each distinct call site once, whether it passes or fails, so a stale
    // offset shows up in the log as a disabled feature instead of a dead process.
    static const char* s_Seen[48];
    static volatile LONG s_SeenCount = 0;

    bool ok = false;
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = nullptr;
    if (target)
    {
        __try
        {
            rf = RtlLookupFunctionEntry((DWORD64)target, &imageBase, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { rf = nullptr; }
        if (rf && imageBase)
            ok = ((imageBase + rf->BeginAddress) == (DWORD64)target);
    }

    bool logged = false;
    const LONG n = InterlockedCompareExchange(&s_SeenCount, 0, 0);
    for (LONG i = 0; i < n && i < 48; i++)
    {
        if (s_Seen[i] == what)
        {
            logged = true;
            break;
        }
    }
    if (!logged)
    {
        const LONG slot = InterlockedIncrement(&s_SeenCount) - 1;
        if (slot >= 0 && slot < 48)
            s_Seen[slot] = what;

        char line[256];
        char sym[MAX_PATH + 32];
        OakModRva(target, sym, sizeof(sym));
        if (ok)
        {
            wsprintfA(line, "native[%s] OK entry=%s", what ? what : "?", sym);
        }
        else if (rf && imageBase)
        {
            wsprintfA(line,
                "native[%s] REFUSED %s is +0x%X inside function +0x%X — stale offset, call skipped",
                what ? what : "?", sym,
                (unsigned)((DWORD64)target - (imageBase + rf->BeginAddress)),
                (unsigned)rf->BeginAddress);
        }
        else
        {
            wsprintfA(line, "native[%s] REFUSED %s has no unwind entry (not a function) — call skipped",
                what ? what : "?", sym);
        }
        OakCrashWrite(line);
    }
    return ok;
}

// --- Fault attribution -------------------------------------------------------
// No TLS here on purpose: this DLL also ships manual-mapped (no CRT/TLS init),
// so scopes live in a small tid-keyed table instead of __declspec(thread).
enum { kScopeThreads = 24, kScopeRing = 40 };

struct OakScopeThread
{
    volatile LONG tid;
    const char* tag;
    DWORD tagTick;
    LONG marks;
    char role[24];
};
static OakScopeThread g_ScopeThreads[kScopeThreads];

struct OakScopeRingEntry
{
    DWORD tid;
    DWORD tick;
    const char* tag;
};
static OakScopeRingEntry g_ScopeRing[kScopeRing];
static volatile LONG g_ScopeRingPos = 0;

static OakScopeThread* OakScopeSlot(DWORD tid, bool create)
{
    for (int i = 0; i < kScopeThreads; i++)
    {
        if ((DWORD)g_ScopeThreads[i].tid == tid)
            return &g_ScopeThreads[i];
    }
    if (!create)
        return nullptr;
    for (int i = 0; i < kScopeThreads; i++)
    {
        if (InterlockedCompareExchange(&g_ScopeThreads[i].tid, (LONG)tid, 0) == 0)
        {
            g_ScopeThreads[i].tag = nullptr;
            g_ScopeThreads[i].marks = 0;
            return &g_ScopeThreads[i];
        }
    }
    return nullptr;
}

void OakCrashHunt_NoteThread(const char* role)
{
    OakScopeThread* st = OakScopeSlot(GetCurrentThreadId(), true);
    if (st && role)
        lstrcpynA(st->role, role, (int)sizeof(st->role));
}

void OakCrashHunt_Mark(const char* tag)
{
    const DWORD tid = GetCurrentThreadId();
    OakScopeThread* st = OakScopeSlot(tid, true);
    if (!st)
        return;
    st->tag = tag;
    st->tagTick = GetTickCount();
    st->marks++;

    const LONG pos = InterlockedIncrement(&g_ScopeRingPos) - 1;
    OakScopeRingEntry& e = g_ScopeRing[((unsigned)pos) % kScopeRing];
    e.tid = tid;
    e.tick = st->tagTick;
    e.tag = tag;
}

// --- Address -> module+RVA ----------------------------------------------------
static uintptr_t g_OakBase = 0, g_OakEnd = 0;

static void OakModuleRange(HMODULE mod, uintptr_t* base, uintptr_t* end)
{
    *base = 0;
    *end = 0;
    if (!mod)
        return;
    auto dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    *base = (uintptr_t)mod;
    *end = *base + nt->OptionalHeader.SizeOfImage;
}

static void OakModRva(uintptr_t addr, char* out, int outN)
{
    if (outN <= 0)
        return;
    out[0] = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    char path[MAX_PATH] = {};
    if (!addr || !VirtualQuery((void*)addr, &mbi, sizeof(mbi)) || !mbi.AllocationBase
        || !GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH))
    {
        wsprintfA(out, "%p", (void*)addr);
        return;
    }
    const char* base = path;
    for (const char* p = path; *p; p++)
    {
        if (*p == '\\' || *p == '/')
            base = p + 1;
    }
    char tmp[MAX_PATH + 32];
    wsprintfA(tmp, "%s+0x%X", base, (unsigned)(addr - (uintptr_t)mbi.AllocationBase));
    lstrcpynA(out, tmp, outN);
}

static bool OakPeekQword(uintptr_t addr, uintptr_t* out)
{
    if (!addr || (addr & 7))
        return false;
    // VirtualQuery first — raw probe AVs still hit our VEH and spam the log
    // (seen as dayz_internal!OakPeekQword during fault reporting).
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD prot = mbi.Protect & 0xFF;
    if (prot == 0 || prot == PAGE_NOACCESS || prot == PAGE_EXECUTE || prot == PAGE_GUARD)
        return false;
    if ((uintptr_t)mbi.BaseAddress + mbi.RegionSize < addr + 8)
        return false;
    __try
    {
        *out = *(volatile uintptr_t*)addr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The loader-registered unwind info of this DLL does not let RtlVirtualUnwind
// cross our SEH-gated frames, so backtraces stop early. Raw-scan the stack for
// return addresses inside our image: that recovers the real Oak call chain.
static void OakScanStackForOak(uintptr_t rsp)
{
    if (!rsp || !g_OakBase)
        return;
    int found = 0;
    char line[160];
    for (uintptr_t p = (rsp & ~7ull); p < (rsp + 0x8000) && found < 24; p += 8)
    {
        uintptr_t v = 0;
        if (!OakPeekQword(p, &v))
        {
            // Skipped page: jump to the next 4K boundary and keep going.
            p = (p + 0x1000) & ~0xFFFull;
            p -= 8;
            continue;
        }
        if (v > g_OakBase && v < g_OakEnd)
        {
            wsprintfA(line, "crashscan[oak] sp+0x%04X -> dayz_internal+0x%X",
                (unsigned)(p - rsp), (unsigned)(v - g_OakBase));
            OakCrashWrite(line);
            found++;
        }
    }
    if (!found)
        OakCrashWrite("crashscan[oak] no Oak return addresses on this stack (pure engine thread)");
}

// Authoritative stack walk. CaptureStackBackTrace unwinds the *handler* frame and
// dies at the first frame it cannot decode; unwinding the faulting CONTEXT with
// RtlVirtualUnwind is what `k` does in a debugger and crosses module boundaries.
static void OakUnwindFromContext(const char* tag, const CONTEXT* start)
{
    if (!start)
        return;
    CONTEXT cx = *start;
    char line[MAX_PATH + 96];
    char sym[MAX_PATH + 32];
    uintptr_t prevRsp = 0;

    for (int i = 0; i < 40; i++)
    {
        if (!cx.Rip)
            break;
        OakModRva((uintptr_t)cx.Rip, sym, sizeof(sym));
        wsprintfA(line, "crashwalk[%s] #%02d rip=%p rsp=%p %s",
            tag, i, (void*)cx.Rip, (void*)cx.Rsp, sym);
        OakCrashWrite(line);

        if (cx.Rsp <= prevRsp && i > 0)
            break;
        prevRsp = (uintptr_t)cx.Rsp;

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION rf = nullptr;
        __try
        {
            rf = RtlLookupFunctionEntry(cx.Rip, &imageBase, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }

        if (!rf)
        {
            // Leaf function (no unwind data): the return address is at [rsp].
            uintptr_t ret = 0;
            if (!OakPeekQword((uintptr_t)cx.Rsp, &ret) || !ret)
                break;
            cx.Rip = ret;
            cx.Rsp += 8;
            continue;
        }

        PVOID handlerData = nullptr;
        DWORD64 establisher = 0;
        __try
        {
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, cx.Rip, rf,
                &cx, &handlerData, &establisher, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
    }
}

static void OakWriteMiniDump(EXCEPTION_POINTERS* ep, const char* tag)
{
    static volatile LONG s_Once = 0;
    if (InterlockedCompareExchange(&s_Once, 1, 0) != 0)
        return;

    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (!dbg)
        return;
    typedef BOOL (WINAPI *tMiniDumpWriteDump)(HANDLE, DWORD, HANDLE, DWORD, PVOID, PVOID, PVOID);
    auto pWrite = (tMiniDumpWriteDump)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (!pWrite)
        return;

    char local[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH))
        return;
    char file[MAX_PATH];
    wsprintfA(file, "%s\\DayZ\\oak_fault_%s_%u.dmp", local, tag ? tag : "av", GetTickCount());
    HANDLE hf = CreateFileA(file, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE)
        return;

    struct
    {
        DWORD ThreadId;
        EXCEPTION_POINTERS* ExceptionPointers;
        BOOL ClientPointers;
    } mei{ GetCurrentThreadId(), ep, FALSE };

    // Thread contexts + stacks + referenced memory: enough for cdb .ecxr/k/dv
    // without paging out a multi-GB full-memory dump.
    // Keep the flags conservative: exotic combinations (private RW / code segs) make
    // MiniDumpWriteDump fail outright on a live game process.
    const DWORD dumpType =
        0x00000000 /*MiniDumpNormal*/ |
        0x00000100 /*MiniDumpWithThreadInfo*/ |
        0x00000010 /*MiniDumpWithProcessThreadData*/ |
        0x00000080 /*MiniDumpWithFullMemoryInfo*/;
    BOOL ok = pWrite(GetCurrentProcess(), GetCurrentProcessId(), hf, dumpType, &mei, nullptr, nullptr);
    const DWORD err = ok ? 0 : GetLastError();
    CloseHandle(hf);

    char line[MAX_PATH + 96];
    wsprintfA(line, "crashhunt[dump] %s err=0x%08X -> %s",
        ok ? "written" : "FAILED", err, file);
    OakCrashWrite(line);
}

// Full fault report: registers, stack alignment, thread role, active Oak scopes,
// module+RVA backtrace, raw Oak stack scan, and a minidump.
static void OakCrashReport(const char* tag, EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord)
        return;
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* cx = ep->ContextRecord;
    char line[320];
    char sym[MAX_PATH + 32];

    OakModRva((uintptr_t)er->ExceptionAddress, sym, sizeof(sym));
    wsprintfA(line, "crashhunt[%s] code=0x%08X (%s) at=%p (%s) tid=%u",
        tag, er->ExceptionCode, OakExceptionName(er->ExceptionCode),
        er->ExceptionAddress, sym, GetCurrentThreadId());
    OakCrashWrite(line);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
    {
        const ULONG_PTR kind = er->ExceptionInformation[0];
        const ULONG_PTR at = er->ExceptionInformation[1];
        wsprintfA(line, "crashhunt[%s] AV %s addr=%p%s", tag,
            kind == 0 ? "read" : (kind == 1 ? "write" : "execute"), (void*)at,
            at == (ULONG_PTR)-1
                ? "  <- -1 means misaligned SSE or non-canonical address, not an unmapped page"
                : "");
        OakCrashWrite(line);
    }

    if (cx)
    {
        // Mid-function x64 stacks are 16-byte aligned (entry rsp%16==8, then the
        // prologue realigns). rsp%16==8 here means a callee was entered with a
        // skewed stack — i.e. someone called into the middle of a function.
        wsprintfA(line, "crashhunt[%s] rsp=%p rsp&0xF=%u %s rbp=%p rip=%p",
            tag, (void*)cx->Rsp, (unsigned)(cx->Rsp & 0xF),
            ((cx->Rsp & 0xF) == 0) ? "(aligned)" : "(SKEWED: entered mid-function / bad call target)",
            (void*)cx->Rbp, (void*)cx->Rip);
        OakCrashWrite(line);
        wsprintfA(line, "crashhunt[%s] rax=%p rcx=%p rdx=%p rbx=%p",
            tag, (void*)cx->Rax, (void*)cx->Rcx, (void*)cx->Rdx, (void*)cx->Rbx);
        OakCrashWrite(line);
        wsprintfA(line, "crashhunt[%s] r8=%p r9=%p rsi=%p rdi=%p",
            tag, (void*)cx->R8, (void*)cx->R9, (void*)cx->Rsi, (void*)cx->Rdi);
        OakCrashWrite(line);
    }

    // Last Oak subsystem entered per thread ('*' = the thread that faulted).
    const DWORD me = GetCurrentThreadId();
    for (int i = 0; i < kScopeThreads; i++)
    {
        OakScopeThread& st = g_ScopeThreads[i];
        const DWORD tid = (DWORD)st.tid;
        if (!tid)
            continue;
        wsprintfA(line, "crashhunt[%s] mark tid=%u%s role=%s last=%s age=%ums marks=%d",
            tag, tid, tid == me ? "*" : "", st.role[0] ? st.role : "?",
            st.tag ? st.tag : "-",
            st.tagTick ? (GetTickCount() - st.tagTick) : 0u, (int)st.marks);
        OakCrashWrite(line);
    }

    // Recent scope history (helps when the fault lands outside any scope).
    const LONG pos = g_ScopeRingPos;
    const LONG first = (pos > 10) ? pos - 10 : 0;
    for (LONG i = first; i < pos; i++)
    {
        OakScopeRingEntry& e = g_ScopeRing[((unsigned)i) % kScopeRing];
        if (!e.tag)
            continue;
        wsprintfA(line, "crashhunt[%s] trail #%d tid=%u +%ums %s",
            tag, (int)(i - first), e.tid, (unsigned)(GetTickCount() - e.tick), e.tag);
        OakCrashWrite(line);
    }

    OakUnwindFromContext(tag, cx);
    if (cx)
        OakScanStackForOak(cx->Rsp);

    OakWriteMiniDump(ep, tag);
}

static LONG WINAPI OakUnhandledFilter(EXCEPTION_POINTERS* ep)
{
    InterlockedIncrement(&g_FatalCount);
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    char line[192];
    wsprintfA(line, "crashhunt[FATAL] unhandled code=0x%08X (%s) at=%p",
        code, OakExceptionName(code), addr);
    OakCrashWrite(line);
    OakCrashReport("FATAL", ep);
    OakLab_DumpWriteLedger("FATAL-AV");

    // Also mirror to dedicated crash file for post-mortem when imgui log is huge.
    char crashPath[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", crashPath, MAX_PATH))
    {
        char crashFile[MAX_PATH];
        wsprintfA(crashFile, "%s\\DayZ\\oak_crash.log", crashPath);
        HANDLE hf = CreateFileA(crashFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE)
        {
            DWORD wr = 0;
            WriteFile(hf, line, (DWORD)lstrlenA(line), &wr, nullptr);
            WriteFile(hf, "\r\n", 2, &wr, nullptr);
            CloseHandle(hf);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG CALLBACK OakVectoredHandler(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Ignore common benign/debug exceptions.
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP
        || code == 0x40010006 || code == 0x4001000A) // DBG_PRINTEXCEPTION_*
        return EXCEPTION_CONTINUE_SEARCH;

    // Only log hard faults — SEH gates handle most in-frame faults.
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR
        && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    static volatile LONG s_VehBurst = 0;
    InterlockedIncrement(&g_VehCount);
    if (InterlockedIncrement(&s_VehBurst) > 8)
        return EXCEPTION_CONTINUE_SEARCH;

    OakCrashReport("VEH", ep);
    OakLab_DumpWriteLedger("VEH-AV");
    return EXCEPTION_CONTINUE_SEARCH;
}

// One-line snapshot of what every Oak thread is doing right now. Written once a
// second so a silent process death (fail-fast / TerminateProcess never reaches a
// handler) still tells us which subsystem was executing.
static void OakScopeSnapshot(char* out, int outN)
{
    if (outN <= 0)
        return;
    out[0] = 0;
    for (int i = 0; i < kScopeThreads; i++)
    {
        OakScopeThread& st = g_ScopeThreads[i];
        const DWORD tid = (DWORD)st.tid;
        if (!tid || !st.tag)
            continue;
        char frag[96];
        wsprintfA(frag, "%s%u/%s:%s", out[0] ? " " : "", tid,
            st.role[0] ? st.role : "?", st.tag);
        if (lstrlenA(out) + lstrlenA(frag) + 1 >= outN)
            break;
        lstrcatA(out, frag);
    }
}

static DWORD WINAPI OakWatchdogThread(LPVOID)
{
    OakCrashHunt_NoteThread("watchdog");
    unsigned tickNo = 0;
    while (InterlockedCompareExchange(&g_WatchdogStop, 0, 0) == 0)
    {
        Sleep(1000);
        if ((tickNo % 3) == 0)
        {
            char scopes[256];
            OakScopeSnapshot(scopes, sizeof(scopes));
            char line[352];
            wsprintfA(line, "crashhunt[beat] t=%u present=%d age=%ums active=[%s]",
                tickNo++, (int)InterlockedCompareExchange(&g_PresentPulse, 0, 0),
                g_LastPresentTick ? (GetTickCount() - g_LastPresentTick) : 0u,
                scopes[0] ? scopes : "-");
            OakCrashWrite(line);
        }
        if (tickNo % 5)
            continue;
        const LONG pulse = InterlockedCompareExchange(&g_PresentPulse, 0, 0);
        if (pulse <= 0)
            continue;
        const DWORD last = g_LastPresentTick;
        const DWORD now = GetTickCount();
        if (!last)
            continue;
        const DWORD age = now - last;
        if (age > 45000)
        {
            char line[128];
            wsprintfA(line, "crashhunt[HANG] no Present pulse for %u ms (last=%u gates=%d)",
                age, last, (int)InterlockedCompareExchange(&g_GateCount, 0, 0));
            OakCrashWrite(line);
            // Reset so we don't spam every 5s forever.
            InterlockedExchange(&g_PresentPulse, 0);
        }
    }
    return 0;
}

void OakCrashHunt_Init(HMODULE module)
{
    g_Module = module;
    OakModuleRange(module, &g_OakBase, &g_OakEnd);
    OakCrashWrite("crashhunt: init begin");
    {
        char line[128];
        wsprintfA(line, "crashhunt: image %p..%p (RVAs in reports are relative to this)",
            (void*)g_OakBase, (void*)g_OakEnd);
        OakCrashWrite(line);
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    typedef LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI* tSetUnhandledExceptionFilter)(LPTOP_LEVEL_EXCEPTION_FILTER);
    auto pSetFilter = k32
        ? (tSetUnhandledExceptionFilter)GetProcAddress(k32, "SetUnhandledExceptionFilter")
        : nullptr;
    auto pAddVeh = k32
        ? (PVOID(WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER))GetProcAddress(k32, "AddVectoredExceptionHandler")
        : nullptr;

    const bool manualMap = OakModuleIsManualMapped(module);

    // Unhandled filter + VEH both register callbacks that live in our image.
    // Under BattlEye + manual-map that AV's inside hooked kernel32 (seen as
    // InitializeCriticalSection / SetUnhandledExceptionFilter writing to self).
    // Keep watchdog-only diagnostics when manual-mapped.
    if (!manualMap)
    {
        if (pSetFilter)
            pSetFilter(OakUnhandledFilter);
        if (pAddVeh)
            g_VehHandle = pAddVeh(1, OakVectoredHandler);
        else
            OakCrashWrite("crashhunt: VEH skipped (no API)");
    }
    else
    {
        OakCrashWrite("crashhunt: filters skipped (manual-map / BE-safe)");
    }

    InterlockedExchange(&g_WatchdogStop, 0);
    g_WatchdogThread = CreateThread(nullptr, 0, OakWatchdogThread, nullptr, 0, nullptr);
    OakCrashWrite(g_VehHandle ? "crashhunt: init (VEH + unhandled filter + watchdog)"
                              : (manualMap ? "crashhunt: init (watchdog only, manual-map)"
                                           : "crashhunt: init (unhandled filter + watchdog, no VEH)"));
}

void OakCrashHunt_Shutdown()
{
    InterlockedExchange(&g_WatchdogStop, 1);
    if (g_WatchdogThread)
    {
        WaitForSingleObject(g_WatchdogThread, 2000);
        CloseHandle(g_WatchdogThread);
        g_WatchdogThread = nullptr;
    }
    if (g_VehHandle)
    {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        auto pRemove = k32
            ? (ULONG(WINAPI*)(PVOID))GetProcAddress(k32, "RemoveVectoredExceptionHandler")
            : nullptr;
        if (pRemove)
            pRemove(g_VehHandle);
        g_VehHandle = nullptr;
    }
}

void OakCrashHunt_PresentPulse()
{
    InterlockedIncrement(&g_PresentPulse);
    g_LastPresentTick = GetTickCount();
}

void OakCrashGate_Log(const char* tag, DWORD code)
{
    InterlockedIncrement(&g_GateCount);
    g_LastGateCode = code;
    g_LastGateTick = GetTickCount();
    if (tag)
    {
        lstrcpynA(g_LastGateTag, tag, (int)sizeof(g_LastGateTag));
    }
    else
    {
        g_LastGateTag[0] = 0;
    }

    static volatile LONG s_Burst = 0;
    const LONG n = InterlockedIncrement(&s_Burst);
    if (n > 32)
        return;

    char line[160];
    wsprintfA(line, "liveqa[crashgate] %s code=0x%08X (%s) #%d",
        tag ? tag : "?", code, OakExceptionName(code), (int)n);
    OakCrashWrite(line);
    OakCrashWriteStack(tag ? tag : "gate", 3);
}

void OakCrashHunt_LogSummary()
{
    char line[192];
    wsprintfA(line,
        "crashhunt[summary] gates=%d fatal=%d veh=%d lastGate='%s' lastCode=0x%08X ageMs=%u presentPulse=%d",
        (int)InterlockedCompareExchange(&g_GateCount, 0, 0),
        (int)InterlockedCompareExchange(&g_FatalCount, 0, 0),
        (int)InterlockedCompareExchange(&g_VehCount, 0, 0),
        g_LastGateTag,
        g_LastGateCode,
        g_LastGateTick ? (GetTickCount() - g_LastGateTick) : 0u,
        (int)InterlockedCompareExchange(&g_PresentPulse, 0, 0));
    OakCrashWrite(line);
}

long OakCrashHunt_GateCount()
{
    return InterlockedCompareExchange(&g_GateCount, 0, 0);
}

long OakCrashHunt_FatalCount()
{
    return InterlockedCompareExchange(&g_FatalCount, 0, 0);
}

long OakCrashHunt_VehCount()
{
    return InterlockedCompareExchange(&g_VehCount, 0, 0);
}
