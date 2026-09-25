#pragma once
// Oak Engine microscope — RTTI + Present poll-watch + HWBP write traps + object dump.
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void OakEngine_Init(HMODULE gameModule);
void OakEngine_Shutdown(void);
void OakEngine_OnPresent(void);

// Resolve MSVC RTTI class name for an object with a vtable. Returns 1 on success.
int OakEngine_NameObject(uintptr_t obj, char* out, int outMax);
int OakEngine_NameVtable(uintptr_t vtable, char* out, int outMax);

// Present-polled change watch (weak attribution — stack is Present thread).
int OakEngine_Watch(uintptr_t addr, unsigned size, const char* tag);
void OakEngine_Unwatch(int slot);
void OakEngine_UnwatchAll(void);
int OakEngine_WatchCount(void);

// Snapshot floats on an object; second call diffs and logs changed offsets (settings-FOV hunt).
void OakEngine_FloatSnap(uintptr_t obj, unsigned bytes, const char* tag);
void OakEngine_FloatDiff(uintptr_t obj, unsigned bytes, const char* tag);

// Hardware breakpoint write-trap (Dr0–Dr3). Hits on the WRITING thread with real RIP.
// size: 1, 2, 4, or 8. oneshot=1 disables after first hit (safe default for discovery).
// Returns slot 0..3 or -1.
int OakEngine_HwbpWatch(uintptr_t addr, unsigned size, const char* tag, int oneshot);
void OakEngine_HwbpClear(int slot);
void OakEngine_HwbpClearAll(void);
int OakEngine_HwbpCount(void);

// Hex + float/ptr annotate dump of an object (RTTI header if present).
void OakEngine_DumpObject(uintptr_t obj, unsigned bytes, const char* tag);

// Dump a window around an address (for HWBP hits).
void OakEngine_DumpAround(uintptr_t addr, unsigned before, unsigned after, const char* tag);

// Last HWBP writer RIP / tag — game-thread hook scout input.
uintptr_t OakEngine_LastHwbpRip(void);
const char* OakEngine_LastHwbpTag(void);

void OakEngine_Census(uintptr_t world, uintptr_t localPlayer, uintptr_t camera);
int OakEngine_FindNamedChild(uintptr_t parent, const char* nameSubstr, uintptr_t offLo, uintptr_t offHi);

void OakEngine_SetHot(uintptr_t world, uintptr_t localPlayer, uintptr_t camera);
void OakEngine_CensusHot(void);
uintptr_t OakEngine_GetHotCamera(void);
uintptr_t OakEngine_GetHotLocal(void);

const char* OakEngine_LastCensusLine(void);

// UserFOV graph hunt (settings slider 0.75..1.30). After HUNT DIFF, targets are writable.
void OakEngine_UserFovSnap(uintptr_t fovContextObj);
void OakEngine_UserFovDiff(void);
int OakEngine_UserFovTargetCount(void);
uintptr_t OakEngine_UserFovTarget(int index);

#ifdef __cplusplus
}
#endif
