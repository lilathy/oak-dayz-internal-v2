#pragma once

#include <windows.h>

// Install VEH + unhandled filter + Present watchdog. Safe to call once from MainThread.
void OakCrashHunt_Init(HMODULE module);

void OakCrashHunt_Shutdown();

// Call at start of each Present overlay frame (heartbeat for hang detection).
void OakCrashHunt_PresentPulse();

// Log from __except handlers — pass GetExceptionCode() from the __except block.
void OakCrashGate_Log(const char* tag, DWORD code);

// Periodic crash-hunt summary (counts, last gate, present age).
void OakCrashHunt_LogSummary();

// Counters for crash-matrix / soak scripts (monotonic for process lifetime).
long OakCrashHunt_GateCount();
long OakCrashHunt_FatalCount();
long OakCrashHunt_VehCount();

// --- Native call safety ------------------------------------------------------
// True only when `target` is the *entry point* of a real function, per its module's
// .pdata unwind table.
//
// A stale native RVA after a game update usually still points at valid code, just
// past the function's prologue. Calling it skips the pushes/sub that set up the
// frame, so the callee runs on a stack skewed by 8; some deeper engine routine then
// executes `movaps [rsp+N], xmm` and faults with address 0xFFFFFFFFFFFFFFFF. A
// __try around the call does NOT save you: by then the engine has run on a corrupt
// frame. Validate before calling, and skip the feature if this returns false.
bool OakNativeEntryOk(uintptr_t target, const char* what);

// --- Fault attribution -------------------------------------------------------
// Name the calling thread once (e.g. "present", "main", "fov") so fault reports
// can say which thread died instead of only a raw TID.
void OakCrashHunt_NoteThread(const char* role);

// Breadcrumbs: records "this thread most recently entered <tag>". Deliberately a
// plain marker, not an RAII scope — most of these call sites live inside __try
// blocks, and any object with a destructor there is a hard C2712.
void OakCrashHunt_Mark(const char* tag);

#define OAK_MARK(tag) OakCrashHunt_Mark(tag)
