#pragma once
// Oak Lab — anti-guessing discipline for DayZ internal.
// Ledger, one-armed harness, delta probe, hold-test ownership, claim+confirm contracts.
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum OakLabModule {
    OAK_LAB_NONE = 0,
    OAK_LAB_ESP,
    OAK_LAB_FREECAM,
    OAK_LAB_FOV,
    OAK_LAB_STAMINA,
    OAK_LAB_SPEED,
    OAK_LAB_NOCLIP,
    OAK_LAB_WARP,
    OAK_LAB_THIRDPERSON,
    OAK_LAB_STREAMPROOF,
    OAK_LAB_MB,
    OAK_LAB_SILENT,
    OAK_LAB_WALLBYPASS,
    OAK_LAB_AMMO,
    OAK_LAB_GRENADE,
    OAK_LAB_TRIGGERBOT,
    OAK_LAB_COUNT
};

enum OakLabState {
    OAK_LAB_OFF = 0,
    OAK_LAB_PROBE = 1,
    OAK_LAB_ARMED = 2,
    OAK_LAB_LOCKED = 3
};

enum {
    OAK_LAB_WF_NONE    = 0,
    OAK_LAB_WF_BAN_FVS = 1 << 0,
    OAK_LAB_WF_BAN_NET = 1 << 1,
    OAK_LAB_WF_BAN_IC  = 1 << 2
};

enum OakLabContract {
    OAK_LAB_CONTRACT_NONE  = 0,
    OAK_LAB_CONTRACT_FAIL  = 1,
    OAK_LAB_CONTRACT_PROBE = 2, // memory/candidate only — NOT done
    OAK_LAB_CONTRACT_PASS  = 3  // claim + ownership + soak + gameplay confirm
};

enum OakLabHoldVerdict {
    OAK_LAB_HOLD_IDLE = 0,
    OAK_LAB_HOLD_BUSY = 1,
    OAK_LAB_HOLD_HELD = 2,        // value stuck → client-owned candidate
    OAK_LAB_HOLD_OVERWRITTEN = 3, // game reverted → sim/server owned
    OAK_LAB_HOLD_FAULT = 4
};

void OakLab_Init(void);
void OakLab_Shutdown(void);
void OakLab_OnPresent(void);
void OakLab_SetLocalPlayer(uintptr_t localPlayer);
uintptr_t OakLab_GetLocalPlayer(void);
const char* OakLab_LockReason(int moduleId);

const char* OakLab_ModuleName(int moduleId);
int OakLab_GetState(int moduleId);
int OakLab_GetArmedModule(void);
int OakLab_IsWriteAllowed(int moduleId);

// Claim must be set before Arm. Empty claim → arm denied (anti-guess).
void OakLab_SetClaim(int moduleId, const char* claim);
const char* OakLab_GetClaim(int moduleId);

// Arm: requires claim text. Ownership-sensitive modules also need a recent probe
// unless force=1 (logged as lab[arm-force]).
int OakLab_Arm(int moduleId);
int OakLab_ArmForce(int moduleId, const char* why);
void OakLab_Disarm(void);
void OakLab_Lock(int moduleId, const char* reason);
void OakLab_EnterSafeMode(const char* reason);
void OakLab_EnterPlayMode(const char* reason); // exit safe; normal UI writers work again

// Operator saw the real gameplay effect (visual FOV, hit, HUD, etc.).
void OakLab_ConfirmGameplay(int moduleId, int yes);
int OakLab_HasGameplayConfirm(int moduleId);

// Final gate: PASS only with claim + (hold HELD or N/A) + soak OK + gameplay confirm.
// Memory-only evidence can never reach PASS here.
int OakLab_Finalize(int moduleId);

typedef struct OakLabPolicy {
    int safeMode;
    int allowFov;
    int allowStamina;
    int allowSpeed;
    int allowNoclip;
    int allowWarp;
    int allowThirdPerson;
    int allowStreamProof;
    int allowMb;
    int allowSilent;
    int allowWallBypass;
    int allowAmmo;
    int allowGrenade;
    int allowTriggerbot;
    int armedModule;
    int soakOk;
    int gameplayConfirmed;
    int lastHoldVerdict;
    int probeHits;
} OakLabPolicy;

void OakLab_GetPolicy(OakLabPolicy* out);

void OakLab_PushModule(int moduleId);
void OakLab_PopModule(void);
int OakLab_CurrentModule(void);

void OakLab_NoteWrite(int moduleId, uintptr_t addr, unsigned size,
                      const void* oldBytes, const void* newBytes);

int OakLab_WriteBytes(int moduleId, uintptr_t addr, const void* data, unsigned size, unsigned flags);
int OakLab_WriteFloat(int moduleId, uintptr_t addr, float value, unsigned flags);
int OakLab_WriteU8(int moduleId, uintptr_t addr, unsigned char value, unsigned flags);
int OakLab_WriteVec3(int moduleId, uintptr_t addr, float x, float y, float z, unsigned flags);

// True when an experimental module is armed (not ESP/freecam). Side-effect teleports should stop.
int OakLab_ExperimentalArmed(void);

// Apply known DayZ engine recipe (claim text + log ownership notes). Returns 0 if unknown/locked.
int OakLab_ApplyRecipe(int moduleId);

// Ban flags for address-class asserts (callers must pass when writing that class).
void OakLab_AssertNotNetWrite(const char* where);
void OakLab_AssertNotFvsWrite(const char* where);

void OakLab_DumpWriteLedger(const char* why);

// Read-only delta probe (sprint/fire). Does not prove a fix.
void OakLab_ProbeStart(uintptr_t localPlayer, const char* action);
void OakLab_ProbeTick(uintptr_t localPlayer);
int OakLab_ProbeBusy(void);
int OakLab_ProbeLastHits(void);
DWORD OakLab_ProbeLastDoneTick(void);

// Write-hold ownership: write once, wait, re-read.
// OVERWRITTEN = do not claim client ownership (the stamina/FOV lie detector).
int OakLab_HoldTestStart(int moduleId, uintptr_t addr, float value, DWORD waitMs);
void OakLab_HoldTestTick(void);
int OakLab_HoldTestBusy(void);
int OakLab_HoldTestLastVerdict(void);
uintptr_t OakLab_HoldTestLastAddr(void);
uintptr_t OakLab_LastWriteAddr(int moduleId);

int OakLab_ContractFovMemory(float wantTan, float gotTan, float errPct);
int OakLab_ContractStamina(int sprint, int haveRealTarget, int holdOk);
int OakLab_ContractSpeedStick(int distCm);
int OakLab_ContractMbSnap(int guided, int boneErrCm, int usedFvs);
void OakLab_LogContract(int moduleId, int result, const char* reason);

// True if module needs a recent ownership probe before Arm (stamina/speed/recoil-like).
int OakLab_NeedsProbeBeforeArm(int moduleId);

#ifdef __cplusplus
}
#endif
