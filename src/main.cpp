// SkyMP Fixes - Combined SKSE plugin for Skyrim SE 1.6.1170
//
// Contains fixes for issues caused by SkyMP client's
// aggressive actor spawn/delete patterns plus a shutdown-hang fix:
//
// 1. FaceGen Crash Fix
//    Prevents CTD when DoReset3D/QueueNiNodeUpdate is called on an actor
//    whose 3D has not been loaded yet. The face gen TRI loading system
//    dereferences uninitialized data and jumps to an invalid address.
//    Crash signature: RIP at garbage address (e.g. 0x000003480001),
//    stack contains BSFaceGenNiNode*, BSFaceGenModel* etc.
//    Also covers direct null-deref variants on the job-thread FaceGen
//    pipeline (e.g. +0429E69, +04328E9) seen when BSFaceGenMorphDataHead
//    is processed for a partially-loaded actor 3D.
//    Also covers the DEP / instruction-fetch variant where the corrupted
//    pointer happens to land inside SkyrimSE.exe at a non-executable
//    .data/.rdata offset (seen on HairLine face-gen TriShapes, AV says
//    "Tried to execute memory at ...", ExceptionInformation[0] == 8).
//
// 2. WorldClean Crash Fix
//    Prevents CTD from NPC deletion race condition during load.
//    Crash sites: SkyrimSE.exe+02D1922 and +02D19A1
//    cmp dword ptr [rax+0x08], imm8 with rax=0.
//
// 3. Texture Queue Crash Fix
//    Prevents CTD when BSTaskManagerThread processes a queued texture
//    load for a reference that has been deleted.
//    Crash site: SkyrimSE.exe+0E02786
//    mov rax, [rcx+0x28] with rcx=0.
//
// 7. MovementController Job Crash Fix (same root cause as FaceGen)
//    Prevents CTD when a BSJobs::JobThread work item dispatches a
//    movement-controller message (IMovementMessageInterface) against an
//    actor that SkyMP has already despawned (kDeleted, FormID=0). Same
//    root cause as fix 1, different subsystem.
//    Crash site: SkyrimSE.exe+0783642
//    cmp qword ptr [rcx+0x1F8], 0x00 with rcx=0.
//    Also covers water-collision variant at SkyrimSE.exe+051E253
//    cmp qword ptr [rcx+0x10], 0x00 with a stale/sentinel rcx value.
//
// 8. JobThread memcpy Crash Fix (idle animation variant of fixes 1/7)
//    Prevents CTD when a BSJobs::JobThread work item dispatches an idle
//    animation update (e.g. HorseIdleHeadShake) whose animation buffer
//    has been freed by an actor teardown during fast travel. memcpy is
//    called with a corrupt size and reads unmapped memory.
//    Crash site: VCRUNTIME140.dll+0x12251 (AVX2 memcpy)
//    Caller: SkyrimSE.exe+0x2F782C (idle animation dispatch).
//
// 4. MpClientPlugin Shutdown Hang Fix
//    Prevents the SkyrimSE.exe process from getting stuck forever (game
//    window gone, only SkyrimPlatform Console visible, ~5-14 GB RAM held)
//    when the game exits via ExitProcess (e.g. JS process.exit / V8 fatal
//    error in SkyrimPlatform). During DLL_PROCESS_DETACH the CRT destroys
//    a static MpClientPlugin::State, which calls RakPeer::Shutdown(),
//    which performs WaitForSingleObjectEx(thread, INFINITE) to join its
//    recv thread -- but ExitProcess has already terminated that thread
//    with abandoned locks, so the wait deadlocks forever.
//    Fix: patch MpClientPlugin.dll's import table so its WaitForSingleObject
//    and WaitForSingleObjectEx calls are routed through a wrapper that
//    caps INFINITE waits to a short timeout whenever
//    RtlDllShutdownInProgress() reports the process is exiting.
//
// 5. V8 JavaScript Heap Limit (configurable)
//    Raises Node.js / V8's `max-old-space-size` so long SkyrimPlatform
//    sessions don't hit "FATAL ERROR: JavaScript heap out of memory" and
//    abort the process. V8's auto-computed default on x64 is ~4 GB
//    regardless of how much physical RAM the box has, and SkyMP's JS
//    client routinely lives long enough to reach that cap.
//    Configured via SkyMPFixes.ini next to this DLL:
//        [V8]
//        MaxOldSpaceMB = 8192
//    If the file or key is missing, or the value is 0, V8's default is
//    left untouched.
//
// 6. Hang Watchdog (configurable)
//    Detects when the main game window stops responding (Windows'
//    IsHungAppWindow) for longer than a configurable threshold and takes
//    action so the user isn't stuck staring at a frozen game for minutes.
//    Options: log the hang, save a mini-dump, kill the process, or
//    dump+kill. Useful for the SkyrimPlatform "hook waiting on a busy JS
//    thread" freeze pattern, which SkyMPFixes cannot patch externally in
//    a surgical way (that fix lives in the SkyMP source tree).
//    Also includes a second detector based on a "game-loop heartbeat"
//    file that memprofile.js touches every SP update tick -- catches
//    freezes where the window still pumps messages but the game logic
//    is stuck (the typical SkyrimPlatform hook deadlock, which never
//    trips IsHungAppWindow).
//    Configured via SkyMPFixes.ini:
//        [HangWatchdog]
//        Enabled = 1
//        HangThresholdSec  = 30   ; window pump stuck for this long
//        HeartbeatStaleSec = 10   ; game-loop heartbeat stale for this long
//        Action = dumpAndKill   ; one of: log, dump, kill, dumpAndKill
//
// All steps are logged to SkyMPFixes.log next to the DLL so the actual
// applied value can be verified in-game.

#include <Windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// â”€â”€ Minimal SKSE64 AE plugin API â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

struct SKSEPluginVersionData {
    enum { kVersion = 1 };

    uint32_t dataVersion;
    uint32_t pluginVersion;
    char     name[256];
    char     author[256];
    char     supportEmail[252];
    uint32_t versionIndependenceEx;
    uint32_t versionIndependence;
    uint32_t compatibleVersions[16];
    uint32_t seVersionRequired;
};

struct SKSEInterface {
    uint32_t skseVersion;
    uint32_t runtimeVersion;
    uint32_t editorVersion;
    uint32_t isEditor;
};

// â”€â”€ Shared state â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static uintptr_t g_baseAddr   = 0;
static uintptr_t g_moduleEnd  = 0;

static bool IsInModule(uintptr_t addr) {
    return addr >= g_baseAddr && addr < g_moduleEnd;
}

// ----- Plugin file paths (resolved once at SKSEPlugin_Load) ----------------
static char g_dllDir [MAX_PATH] = {0}; // e.g. ...\Data\SKSE\Plugins
static char g_iniPath[MAX_PATH] = {0}; // ...\SkyMPFixes.ini
static char g_logPath[MAX_PATH] = {0}; // ...\SkyMPFixes.log

static void ResolvePluginPaths() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCSTR>(&ResolvePluginPaths),
                            &self)) {
        return;
    }

    char dllPath[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(self, dllPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;

    char* slash = nullptr;
    for (char* p = dllPath; *p; ++p) {
        if (*p == '\\' || *p == '/')
            slash = p;
    }
    if (!slash)
        return;
    *slash = '\0';
    std::strncpy(g_dllDir, dllPath, MAX_PATH - 1);

    std::snprintf(g_iniPath, MAX_PATH, "%s\\SkyMPFixes.ini", g_dllDir);
    std::snprintf(g_logPath, MAX_PATH, "%s\\SkyMPFixes.log", g_dllDir);
}

// ----- Tiny line-buffered file logger --------------------------------------
static CRITICAL_SECTION g_logLock;
static bool             g_logInited = false;

static void LogInit() {
    if (g_logInited) return;
    InitializeCriticalSection(&g_logLock);
    g_logInited = true;

    // Truncate the log at startup so it reflects only the current session.
    if (g_logPath[0]) {
        if (FILE* f = std::fopen(g_logPath, "w")) {
            std::fprintf(f, "[SkyMPFixes] log opened\n");
            std::fclose(f);
        }
    }
}

static void Log(const char* fmt, ...) {
    if (!g_logInited || !g_logPath[0]) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    EnterCriticalSection(&g_logLock);
    if (FILE* f = std::fopen(g_logPath, "a")) {
        std::fprintf(f, "[%02u:%02u:%02u.%03u] %s\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     buf);
        std::fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Diagnostic counters for exception handlers (lock-free)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//
// Each handler bumps its slot on a successful absorb. A background thread
// dumps deltas to the log every 5s if anything changed. We also emit ONE
// "first fire" line per handler the moment it first fires, so we know
// immediately what pattern the game is hitting during load.
//
// Counters are only touched from exception context, so lock-free atomics
// only. Never call Log() from the fast path -- Log takes a critical
// section that would serialize thousands of exception handler invocations.

enum HandlerSlot {
    kHS_FaceGen         = 0,
    kHS_WorldClean      = 1,
    kHS_TextureQueue    = 2,
    kHS_MovementJob     = 3,
    kHS_JobMemcpy       = 4,
    kHS_Count
};

static const char* const kHandlerNames[kHS_Count] = {
    "FaceGen", "WorldClean", "TextureQueue", "MovementJob",
    "JobMemcpy"
};

static volatile LONG g_handlerFires[kHS_Count]      = {0};
static volatile LONG g_handlerFirstLog[kHS_Count]   = {0};

// Called from inside each handler right before returning
// EXCEPTION_CONTINUE_EXECUTION. Never call Log() unless this is the FIRST
// fire (guarded by CAS). Safe to call at very high frequency.
static inline void DiagBumpHandler(HandlerSlot slot) {
    InterlockedIncrement(&g_handlerFires[slot]);
    if (InterlockedCompareExchange(&g_handlerFirstLog[slot], 1, 0) == 0) {
        // First fire ever -- one log line so the user can immediately see
        // that the handler is being triggered during load.
        Log("[diag] handler '%s' first fired", kHandlerNames[slot]);
    }
}

// Convenience: bump the counter and return EXCEPTION_CONTINUE_EXECUTION in
// one shot, so each handler's absorb sites can just say
// `return AbsorbAndReturn(kHS_XXX);` instead of duplicating the two lines.
static inline LONG AbsorbAndReturn(HandlerSlot slot) {
    DiagBumpHandler(slot);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Background thread that reports per-handler activity every 5s if it
// changed. Also detects sustained-loop conditions (any handler firing
// > 500 times in 5s => probably livelock).
static DWORD WINAPI DiagStatsThread(LPVOID) {
    LONG last[kHS_Count] = {0};
    for (int iter = 0; iter < 720; ++iter) {  // 720 * 5s = 1h max
        Sleep(5000);
        char line[512] = {0};
        char* p = line;
        int changed = 0;
        LONG maxDelta = 0;
        int maxIdx = -1;
        for (int i = 0; i < kHS_Count; ++i) {
            LONG cur = g_handlerFires[i];
            LONG d = cur - last[i];
            if (d > 0) {
                int n = std::snprintf(p, sizeof(line) - (p - line),
                                      " %s=+%ld(%ld)",
                                      kHandlerNames[i], d, cur);
                if (n > 0) p += n;
                changed++;
                if (d > maxDelta) { maxDelta = d; maxIdx = i; }
            }
            last[i] = cur;
        }
        if (changed > 0) {
            Log("[diag] 5s handler activity:%s", line);
            if (maxDelta > 500) {
                Log("[diag] WARNING: handler '%s' fired %ld times in 5s -- "
                    "probable livelock, this handler needs a limit",
                    kHandlerNames[maxIdx], maxDelta);
            }
        }
    }
    return 0;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// FIX 1: FaceGen Crash (two variants)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// Variant A: Corrupted vtable jump â€” RIP lands at garbage address,
//   stack has face gen return addresses.
// Variant B: Invalid pointer in face gen helper â€” RIP is valid but a register
//   holds an invalid/sentinel value (e.g. 0xFFFFFFFF from failed lookup, or
//   simply NULL because the owning actor 3D was torn down on another thread).
//   Known crash sites:
//     +0429E69  mov byte ptr [rdi+0x1C], 0x01   RDI=0xFFFFFFFF  (fn 26789)
//     +04328E9  mov rax, [rcx]                  RCX=0           (fn 26988)
//   The second site is reached from the BSJobs::JobThread FaceGen pipeline
//   (callers: +0CF61DA, +0CF7745, +0CF7888, +0CF7E51) when a queued morph
//   job runs after the BSFaceGenMorphDataHead's referenced object has been
//   freed (typical of SkyMP's rapid actor spawn/delete).

static bool IsInFaceGenRange(uintptr_t addr) {
    uintptr_t offset = addr - g_baseAddr;
    // Covers both the original Variant B site (~+0x429xxx) and the newer
    // job-thread FaceGen helper around +0x4328E9 / +0x04334F8.
    return offset >= 0x428000 && offset <= 0x434000;
}

// Direct crash sites for Variant B (valid RIP, bad register).
// Each entry: (offset, instruction length, expected-bad-register check).
enum FaceGenBadRegKind : uint8_t {
    kFGBadReg_RDI_FFFFFFFF = 0,  // RDI == 0xFFFFFFFF
    kFGBadReg_RCX_Null     = 1,  // RCX == 0
};

struct FaceGenDirectSite {
    uintptr_t          offset;
    uint32_t           insnLen;
    FaceGenBadRegKind  badReg;
};

static constexpr FaceGenDirectSite kFaceGenDirectSites[] = {
    { 0x0429E69, 4, kFGBadReg_RDI_FFFFFFFF }, // C6 47 1C 01           â€” mov byte [rdi+0x1C], 1
    { 0x04328E9, 3, kFGBadReg_RCX_Null     }, // 48 8B 01              â€” mov rax, [rcx]
};

static LONG CALLBACK FaceGenExceptionHandler(EXCEPTION_POINTERS* a_ex) {
    const auto* rec = a_ex->ExceptionRecord;
    auto*       ctx = a_ex->ContextRecord;

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    // Variant B: known crash sites with invalid register values
    for (const auto& site : kFaceGenDirectSites) {
        if (ctx->Rip != g_baseAddr + site.offset)
            continue;

        // Confirm the expected bad-register condition. If something else
        // triggered the AV at this address, fall through to the other
        // handlers rather than masking an unrelated bug.
        bool matches = false;
        switch (site.badReg) {
            case kFGBadReg_RDI_FFFFFFFF: matches = (ctx->Rdi == 0xFFFFFFFFu); break;
            case kFGBadReg_RCX_Null:     matches = (ctx->Rcx == 0);            break;
        }
        if (!matches)
            return EXCEPTION_CONTINUE_SEARCH;

        // Unwind to the first stack-frame return address that lives outside
        // the FaceGen helper range â€” that caller is prepared to handle a
        // "not found / null" result from this helper chain.
        uintptr_t* stack = reinterpret_cast<uintptr_t*>(ctx->Rsp);
        uintptr_t safeReturn = 0;
        uintptr_t safeRsp = 0;

        for (int j = 0; j < 64; j++) {
            uintptr_t val = stack[j];
            if (IsInModule(val) && !IsInFaceGenRange(val)) {
                safeReturn = val;
                safeRsp = ctx->Rsp + (j + 1) * 8;
                break;
            }
        }

        if (safeReturn) {
            ctx->Rip = safeReturn;
            ctx->Rsp = safeRsp;
            ctx->Rax = 0;
            return AbsorbAndReturn(kHS_FaceGen);
        }

        // Fallback: just skip the faulting instruction and zero the result.
        ctx->Rip += site.insnLen;
        ctx->Rax  = 0;
        return AbsorbAndReturn(kHS_FaceGen);
    }

    // Variant A: RIP at non-executable address (corrupted vtable / function
    // pointer call in the FaceGen job pipeline).
    //
    // Two sub-flavours we have to recognise:
    //
    //   A.1 "garbage RIP" â€” RIP lands at a completely bogus address outside
    //       any loaded module (e.g. 0x000003480001). Caught by the heuristic
    //       gates below.
    //
    //   A.2 "non-exec RIP inside SkyrimSE" (added 2026-06-25) â€” the
    //       corrupted pointer happens to land at a valid-looking SkyrimSE
    //       offset that is read-only data (e.g. +0x32580C4 -- two zero bytes
    //       which disassemble as 'add [rax], al'). The CPU raises an
    //       instruction-fetch DEP fault; the heuristic gates below would
    //       otherwise wrongly bail because RIP is inside SkyrimSE.
    //       Identifiable by ExceptionInformation[0] == 8.

    const bool isExecuteAv = (rec->NumberParameters >= 1) &&
                             (rec->ExceptionInformation[0] == 8);

    uintptr_t faultAddr = ctx->Rip;

    if (!isExecuteAv) {
        // Apply the heuristic gates for A.1 only. For A.2 the kernel has
        // already told us this is a bad-jump fault, so any RIP is fair game.

        // If RIP is inside a known module, this isn't our crash
        if (IsInModule(faultAddr))
            return EXCEPTION_CONTINUE_SEARCH;

        // If RIP is in high module address range, not our crash
        if (faultAddr > 0x7FF000000000ULL)
            return EXCEPTION_CONTINUE_SEARCH;
    }

    // Verify this came from face gen by checking the stack
    uintptr_t* stack = reinterpret_cast<uintptr_t*>(ctx->Rsp);
    bool fromFaceGen = false;
    for (int i = 0; i < 8; i++) {
        if (IsInFaceGenRange(stack[i])) {
            fromFaceGen = true;
            break;
        }
    }

    if (!fromFaceGen)
        return EXCEPTION_CONTINUE_SEARCH;

    // Find a safe return address outside face gen but inside SkyrimSE
    uintptr_t safeReturn = 0;
    uintptr_t safeRsp = 0;

    for (int i = 0; i < 64; i++) {
        uintptr_t val = stack[i];
        if (IsInModule(val) && !IsInFaceGenRange(val)) {
            safeReturn = val;
            safeRsp = ctx->Rsp + (i + 1) * 8;
            break;
        }
    }

    if (!safeReturn) {
        // Fallback: skip faulting call by returning to [RSP]
        ctx->Rip = stack[0];
        ctx->Rsp += 8;
        ctx->Rax = 0;
        return AbsorbAndReturn(kHS_FaceGen);
    }

    // Unwind to the safe return point
    ctx->Rip = safeReturn;
    ctx->Rsp = safeRsp;
    ctx->Rax = 0;
    return AbsorbAndReturn(kHS_FaceGen);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// FIX 2: WorldClean Crash (NPC deletion during cell init)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// Crash offsets: cmp dword ptr [rax+0x08], imm8 (4 bytes each)
static constexpr uintptr_t kWorldCleanOffsets[] = { 0x02D1922, 0x02D19A1 };
static constexpr uint32_t  kWorldCleanInsnLen   = 4;

static LONG CALLBACK WorldCleanExceptionHandler(EXCEPTION_POINTERS* a_ex) {
    const auto* rec = a_ex->ExceptionRecord;
    auto*       ctx = a_ex->ContextRecord;

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || ctx->Rax != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    for (auto offset : kWorldCleanOffsets) {
        if (ctx->Rip == g_baseAddr + offset) {
            ctx->Rip    += kWorldCleanInsnLen;
            ctx->EFlags &= ~0x40u;  // clear ZF â†’ "type mismatch" path
            return AbsorbAndReturn(kHS_WorldClean);
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// FIX 3: Texture Queue Crash (BSTaskManagerThread null deref)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//
// Crash at SkyrimSE.exe+0E02786: mov rax, [rcx+0x28]  (RCX=0)
// This is in function 75739 (texture queue processing on BSTaskManagerThread).
// The function receives a pointer to a queued texture handle entry. When the
// owning reference has been deleted, this pointer is null.
//
// The instruction is 3 bytes: 48 8B 41 28 (actually 4 bytes for REX+opcode+modrm+disp8)
// Actually: REX.W(48) MOV(8B) ModRM(41) disp8(28) = 4 bytes
//
// Fix: If RIP matches the crash site and RCX==0, skip the instruction and
// set RAX=0. The caller (function 75636 at +0DFDA00) sets state to 0x05
// then checks the result â€” a null return causes it to skip processing
// this queue entry gracefully.
//
// Additionally, a second null check at +0E02792: mov rcx, [rbx+0x20]
// In the crash log this appears as the next frame â€” if RBX's queued entry
// has a null member at +0x20, the subsequent deref of that will also crash.
// We handle both sites.

static constexpr uintptr_t kTexQueueOffsets[] = {
    0x0E02786,  // mov rax, [rcx+0x28] â€” primary crash site
    0x0E02792,  // mov rcx, [rbx+0x20] â€” secondary (next instruction after check)
};

static LONG CALLBACK TextureQueueExceptionHandler(EXCEPTION_POINTERS* a_ex) {
    const auto* rec = a_ex->ExceptionRecord;
    auto*       ctx = a_ex->ContextRecord;

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    // Primary site: mov rax, [rcx+0x28] with RCX=0
    if (ctx->Rip == g_baseAddr + kTexQueueOffsets[0]) {
        if (ctx->Rcx == 0) {
            // Skip the instruction (4 bytes: 48 8B 41 28)
            ctx->Rip += 4;
            ctx->Rax  = 0;
            return AbsorbAndReturn(kHS_TextureQueue);
        }
    }

    // Secondary site: mov rcx, [rbx+0x20] with [rbx+0x20]=0 leading to deref
    if (ctx->Rip == g_baseAddr + kTexQueueOffsets[1]) {
        // RBX points to QueuedHead, [rbx+0x20] is the pointer being loaded into RCX.
        // If that pointer is null, the next instruction will crash.
        // We need to check if [rbx+0x20] is accessible and null.
        uintptr_t rbx = ctx->Rbx;
        if (rbx != 0) {
            uintptr_t* memberPtr = reinterpret_cast<uintptr_t*>(rbx + 0x20);
            // Verify the memory is readable
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(memberPtr, &mbi, sizeof(mbi)) &&
                (mbi.State == MEM_COMMIT) &&
                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
                if (*memberPtr == 0) {
                    // [rbx+0x20] is null â€” loading it into RCX will cause
                    // the primary crash site to fire on next iteration.
                    // Skip instruction (4 bytes: 48 8B 4B 20) and set RCX=0,
                    // then let the primary handler catch it, OR just unwind.
                    ctx->Rip += 4;
                    ctx->Rcx  = 0;
                    // Now RIP points to the instruction after, which is likely
                    // the primary crash site again. Set RAX=0 and skip past
                    // the whole sequence by returning to the caller.
                    // Actually, let's just skip this instruction. The primary
                    // handler will catch the next one if needed.
                    return AbsorbAndReturn(kHS_TextureQueue);
                }
            }
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// FIX 7: MovementController Job Crash (same class as FaceGen, different site)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//
// Crash at SkyrimSE.exe+0x0783642:
//   cmp qword ptr [rcx+0x1F8], 0x00        (8 bytes: 48 83 B9 F8 01 00 00 00)
// with RCX=0.
//
// Objects in scope at crash time:
//   R15 = Character* "Rabbit" with Flags: kDeleted, FormID = 0x00000000
//         (i.e. an actor that MpClientPlugin has already despawned)
//   RBX = MovementControllerNPC*
//   R8  = "IMovementMessageInterface" (the interface being asked for)
//   Stack contains the same BSJobs::JobThread pipeline frames as the FaceGen
//   crashes: +0CF7888, +0CF7E51, +0CF61DA, +0CD0DBD.
//
// Same root cause as FIX 1 (FaceGen): SkyMP's aggressive spawn/delete race
// leaves an actor kDeleted / FormID=0 while a BSJobs::JobThread work item
// (this time a movement-controller message dispatch) still references it.
// The dispatch reaches a null vtable / message interface and dereferences
// RCX=0 in the compare above.
//
// Recovery is identical in spirit to FIX 1 Variant B: find a stack frame
// belonging to the JobThread dispatcher (SkyrimSE.exe+0xCF6000..+0xCF8000)
// and unwind to it, so the job simply appears to have "done nothing" and
// the dispatcher moves on to the next item. If no dispatcher frame is
// found within a reasonable scan depth, fall back to skipping the compare
// and forcing ZF=1 so the immediate caller takes the "null field" path.

static constexpr uintptr_t kMovementJobCrashOffsetA = 0x0783642;
static constexpr uint32_t  kMovementJobCrashInsnLenA = 8;
// New 2026-07-11 variant from CrashLogger:
//   SkyrimSE.exe+0x0CF672C : add rcx, [rdi] with RDI=0
// Same job-thread movement-controller race as site A, different basic block.
static constexpr uintptr_t kMovementJobCrashOffsetB = 0x0CF672C;
static constexpr uint32_t  kMovementJobCrashInsnLenB = 3;
// New 2026-07-15 variant from CrashLogger:
//   SkyrimSE.exe+0x051E253 : cmp qword ptr [rcx+0x10], 0
// Same stale-object race family (observed with BGSWaterCollisionManager path).
static constexpr uintptr_t kMovementJobCrashOffsetC = 0x051E253;
static constexpr uint32_t  kMovementJobCrashInsnLenC = 5;

// The BSJobs::JobThread work-item dispatcher lives in this range. Frames
// here are the ones we want to unwind to: the dispatcher treats a returning
// job as "done, next" and never touches the fields we just skipped.
static bool IsInJobThreadDispatcher(uintptr_t addr, uintptr_t baseAddr,
                                    uintptr_t moduleEnd) {
    if (addr < baseAddr || addr >= moduleEnd)
        return false;
    uintptr_t off = addr - baseAddr;
    // Widened 2026-07-05 from +0xCF6000..+0xCF8000 after observing crashes
    // at +0xCF813E and +0xCF78A7 (both inside the same BSJobs::JobThread
    // dispatcher function). A generous +0xCF6000..+0xCF9000 covers the
    // whole dispatcher including future sites we haven't seen yet.
    return off >= 0x00CF6000 && off <= 0x00CF9000;
}

static LONG CALLBACK MovementJobExceptionHandler(EXCEPTION_POINTERS* a_ex) {
    const auto* rec = a_ex->ExceptionRecord;
    auto*       ctx = a_ex->ContextRecord;

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    // Accept two known movement-job crash sites with strict null-register
    // checks to avoid swallowing unrelated AVs in nearby code.
    bool matchedSite = false;
    uint32_t matchedInsnLen = 0;

    if (ctx->Rip == g_baseAddr + kMovementJobCrashOffsetA) {
        // Site A: cmp qword ptr [rcx+0x1F8], 0 with RCX=0.
        if (ctx->Rcx != 0)
            return EXCEPTION_CONTINUE_SEARCH;
        matchedSite = true;
        matchedInsnLen = kMovementJobCrashInsnLenA;
    } else if (ctx->Rip == g_baseAddr + kMovementJobCrashOffsetB) {
        // Site B: add rcx, [rdi] with RDI=0.
        if (ctx->Rdi != 0)
            return EXCEPTION_CONTINUE_SEARCH;
        matchedSite = true;
        matchedInsnLen = kMovementJobCrashInsnLenB;
    } else if (ctx->Rip == g_baseAddr + kMovementJobCrashOffsetC) {
        // Site C: cmp qword ptr [rcx+0x10], 0. The observed crash carries
        // a stale/sentinel RCX; verify the AV fault address matches [rcx+0x10]
        // so we don't swallow unrelated faults at the same RIP.
        if (rec->NumberParameters < 2 || rec->ExceptionInformation[0] != 0)
            return EXCEPTION_CONTINUE_SEARCH;
        const uintptr_t expectedFault = ctx->Rcx + 0x10;
        if (static_cast<uintptr_t>(rec->ExceptionInformation[1]) != expectedFault)
            return EXCEPTION_CONTINUE_SEARCH;
        matchedSite = true;
        matchedInsnLen = kMovementJobCrashInsnLenC;
    }

    if (!matchedSite)
        return EXCEPTION_CONTINUE_SEARCH;

    // Scan the stack for a JobThread dispatcher return address. Depth 384
    // slots = 3 KB, which covers the ~1.5 KB gap we saw in the crash dump
    // between the leaf frame and the dispatcher.
    uintptr_t* stack = reinterpret_cast<uintptr_t*>(ctx->Rsp);
    uintptr_t safeReturn = 0;
    uintptr_t safeRsp = 0;

    for (int j = 0; j < 384; ++j) {
        uintptr_t val = stack[j];
        if (IsInJobThreadDispatcher(val, g_baseAddr, g_moduleEnd)) {
            safeReturn = val;
            safeRsp = ctx->Rsp + (j + 1) * 8;
            break;
        }
    }

    if (safeReturn) {
        ctx->Rip = safeReturn;
        ctx->Rsp = safeRsp;
        ctx->Rax = 0;
        return AbsorbAndReturn(kHS_MovementJob);
    }

    // Fallback: skip the faulting instruction and bias flags toward the
    // caller's "null/missing field" path. For cmp sites this means ZF=1.
    ctx->Rip    += matchedInsnLen;
    ctx->EFlags |= 0x40u;  // set ZF
    ctx->Rax     = 0;
    return AbsorbAndReturn(kHS_MovementJob);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// FIX 8: JobThread Crash Inside CRT memcpy (Idle Animation Variant)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//
// Third variant of the same actor-delete race as FIX 1 (FaceGen) and FIX 7
// (MovementController). This time the JobThread work item is dispatching
// an idle-animation update (TESIdleForm "HorseIdleHeadShake" was observed
// on the horse whose data got torn down mid-fast-travel), which internally
// calls memcpy. By the time memcpy runs, the actor's animation buffer has
// been freed and the size argument has been overwritten with garbage.
//
// Crash example:
//   VCRUNTIME140.dll+0x12251   vmovdqu ymm5, [rdx+r8*1-0x20]  ; AVX2 memcpy
//   r8 = 0xB8481008 (~3 GB) -- corrupt size argument
//   caller at SkyrimSE.exe+0x02F782C (idle animation dispatch)
//   BSJobs::JobThread frames further up the stack (same as fixes 1/7).
//
// Match criteria (deliberately narrow to avoid absorbing unrelated CRT
// crashes):
//   1. AV that is a READ (ExceptionInformation[0] == 0)
//   2. RIP is outside SkyrimSE.exe (i.e. in a runtime DLL like VCRUNTIME140)
//   3. The immediate return address on the stack is in the small window
//      around SkyrimSE.exe+0x2F7600..+0x2F7900 (idle-anim dispatch fn)
//   4. A BSJobs::JobThread dispatcher frame is present on the stack
//
// Recovery is identical to FIX 7: unwind to the JobThread dispatcher frame
// so the work item is treated as "done" and the dispatcher moves on. No
// fallback path here -- if we can't find a dispatcher frame we return
// EXCEPTION_CONTINUE_SEARCH and let the game crash normally, because
// resuming inside a partially-executed memcpy is unsafe.

static constexpr uintptr_t kFix8CallerRangeStart = 0x02F7600;
static constexpr uintptr_t kFix8CallerRangeEnd   = 0x02F7900;

static LONG CALLBACK JobMemcpyExceptionHandler(EXCEPTION_POINTERS* a_ex) {
    const auto* rec = a_ex->ExceptionRecord;
    auto*       ctx = a_ex->ContextRecord;

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    // ExceptionInformation[0]: 0 = read, 1 = write, 8 = DEP.
    // We only handle reads -- resuming after a partial write in memcpy
    // would leave the destination corrupted.
    if (rec->NumberParameters < 2 || rec->ExceptionInformation[0] != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    // Only when the fault is inside a runtime DLL, NOT inside SkyrimSE
    // (fixes 1 and 7 handle the in-SkyrimSE crash sites already).
    if (ctx->Rip >= g_baseAddr && ctx->Rip < g_moduleEnd)
        return EXCEPTION_CONTINUE_SEARCH;

    // Check that the immediate caller is the idle-animation dispatch site.
    // This keeps us from accidentally swallowing unrelated CRT crashes.
    uintptr_t* stack = reinterpret_cast<uintptr_t*>(ctx->Rsp);
    uintptr_t ra = 0;
    __try {
        ra = stack[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ra < g_baseAddr + kFix8CallerRangeStart ||
        ra >= g_baseAddr + kFix8CallerRangeEnd)
        return EXCEPTION_CONTINUE_SEARCH;

    // Same recovery as FIX 7: unwind to the JobThread dispatcher frame.
    uintptr_t safeReturn = 0;
    uintptr_t safeRsp = 0;

    for (int j = 0; j < 384; ++j) {
        uintptr_t val = 0;
        __try {
            val = stack[j];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (IsInJobThreadDispatcher(val, g_baseAddr, g_moduleEnd)) {
            safeReturn = val;
            safeRsp = ctx->Rsp + (j + 1) * 8;
            break;
        }
    }

    if (safeReturn) {
        ctx->Rip = safeReturn;
        ctx->Rsp = safeRsp;
        ctx->Rax = 0;
        return AbsorbAndReturn(kHS_JobMemcpy);
    }

    // Couldn't find a safe frame to return to -- let the crash proceed
    // so CrashLoggerSSE captures it and we get more info.
    return EXCEPTION_CONTINUE_SEARCH;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// FIX 4: MpClientPlugin DllMain Shutdown Hang
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//
// During ExitProcess, the loader calls DLL_PROCESS_DETACH on each DLL.
// MpClientPlugin's CRT runs static destructors which destroy a static
// MpClientPlugin::State; the destructor of its `Client` calls
// `peer->Shutdown(0)` (SkyMP: Networking.cpp:85), which internally calls
// `WaitForSingleObjectEx(thread, INFINITE)` to join RakNet's recv thread.
// That thread has already been killed by ExitProcess (with abandoned locks),
// so the wait deadlocks and the whole process hangs forever in DllMain.
//
// Observed stack (one surviving thread, 13.7 GB held):
//   00 ntdll!NtWaitForSingleObject
//   01 KERNELBASE!WaitForSingleObjectEx
//   02..0f MpClientPlugin!SLNet::* (RakPeer::Shutdown -> SocketLayer)
//   10 ntdll!LdrpCallInitRoutineInternal      (loader DllMain dispatch)
//   11 ntdll!LdrpCallInitRoutine
//   12 ntdll!LdrShutdownProcess
//   13 ntdll!RtlExitUserProcess
//   14 kernel32!ExitProcessImplementation
//   15..1d libnode!...                         (V8/Node called ExitProcess)
//
// Fix: patch MpClientPlugin's IAT entries for kernel32!WaitForSingleObject
// and WaitForSingleObjectEx to point at wrapper functions that cap any
// `dwMilliseconds == INFINITE` to a short bound whenever
// RtlDllShutdownInProgress() is true. During normal operation the waits
// pass through unchanged, so MP networking is unaffected.
//
// Notes:
//  * MpClientPlugin may or may not be loaded by the time SKSEPlugin_Load
//    runs (SKSE plugin load order is not specified). If not yet loaded we
//    spawn a one-shot watcher thread that polls for it.
//  * Our DLL must outlive MpClientPlugin so the wrapper code remains mapped
//    when MpClientPlugin's DllMain detach runs. We pin SkyMPFixes with
//    GET_MODULE_HANDLE_EX_FLAG_PIN.

using RtlDllShutdownInProgressFn = BOOLEAN (NTAPI*)();
using WaitForSingleObjectFn       = DWORD (WINAPI*)(HANDLE, DWORD);
using WaitForSingleObjectExFn     = DWORD (WINAPI*)(HANDLE, DWORD, BOOL);

static RtlDllShutdownInProgressFn g_pRtlDllShutdownInProgress = nullptr;
static WaitForSingleObjectFn      g_origWaitForSingleObject   = nullptr;
static WaitForSingleObjectExFn    g_origWaitForSingleObjectEx = nullptr;

// Cap any INFINITE wait initiated from MpClientPlugin during process
// shutdown to this many milliseconds. 1 second is generous: RakPeer
// typically does only one or two such joins.
static constexpr DWORD kShutdownWaitCapMs = 1000;

static bool IsShutdownInProgress() {
    return g_pRtlDllShutdownInProgress && g_pRtlDllShutdownInProgress();
}

static DWORD WINAPI WaitForSingleObject_Wrapper(HANDLE h, DWORD ms) {
    if (ms == INFINITE && IsShutdownInProgress())
        ms = kShutdownWaitCapMs;
    return g_origWaitForSingleObject
        ? g_origWaitForSingleObject(h, ms)
        : WaitForSingleObject(h, ms);
}

static DWORD WINAPI WaitForSingleObjectEx_Wrapper(HANDLE h, DWORD ms, BOOL alertable) {
    if (ms == INFINITE && IsShutdownInProgress())
        ms = kShutdownWaitCapMs;
    return g_origWaitForSingleObjectEx
        ? g_origWaitForSingleObjectEx(h, ms, alertable)
        : WaitForSingleObjectEx(h, ms, alertable);
}

// Patch one IAT entry. Returns true if the slot was successfully redirected.
static bool PatchIatSlot(void** iatSlot, void* newFn, void** outOriginal) {
    DWORD oldProt = 0;
    if (!VirtualProtect(iatSlot, sizeof(void*), PAGE_READWRITE, &oldProt))
        return false;
    if (outOriginal && !*outOriginal)
        *outOriginal = *iatSlot;
    *iatSlot = newFn;
    DWORD tmp = 0;
    VirtualProtect(iatSlot, sizeof(void*), oldProt, &tmp);
    return true;
}

// Walk a module's import directory and redirect every kernel32!WaitForSingleObject
// and WaitForSingleObjectEx entry to our wrappers.
// Returns the number of slots patched (0..N).
static int PatchModuleWaitImports(HMODULE hMod) {
    if (!hMod) return 0;

    auto base = reinterpret_cast<uint8_t*>(hMod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    const auto& importDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size == 0)
        return 0;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + importDir.VirtualAddress);

    int patched = 0;

    for (; desc->Name != 0; ++desc) {
        // We don't filter by imported DLL name: api-set redirections mean
        // the synch primitives can come from kernel32.dll, KernelBase.dll,
        // or api-ms-win-core-synch-*.dll. The function NAME is what matters.
        auto* origThunk = desc->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
            : reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        auto* iatThunk  = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + desc->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
            // Skip imports by ordinal
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + origThunk->u1.AddressOfData);
            const char* name =
                reinterpret_cast<const char*>(importByName->Name);

            void** slot = reinterpret_cast<void**>(&iatThunk->u1.Function);

            if (std::strcmp(name, "WaitForSingleObjectEx") == 0) {
                if (PatchIatSlot(
                        slot,
                        reinterpret_cast<void*>(&WaitForSingleObjectEx_Wrapper),
                        reinterpret_cast<void**>(&g_origWaitForSingleObjectEx)))
                    ++patched;
            } else if (std::strcmp(name, "WaitForSingleObject") == 0) {
                if (PatchIatSlot(
                        slot,
                        reinterpret_cast<void*>(&WaitForSingleObject_Wrapper),
                        reinterpret_cast<void**>(&g_origWaitForSingleObject)))
                    ++patched;
            }
        }
    }

    return patched;
}

// Background thread that waits for MpClientPlugin.dll to appear, then patches
// it. Runs at most ~60s before giving up (plugin probably isn't installed).
static DWORD WINAPI MpClientWaiterThread(LPVOID) {
    for (int i = 0; i < 120; ++i) {
        if (HMODULE mp = GetModuleHandleA("MpClientPlugin.dll")) {
            PatchModuleWaitImports(mp);
            return 0;
        }
        Sleep(500);
    }
    return 0;
}

static void InstallMpClientShutdownFix() {
    // Resolve RtlDllShutdownInProgress -- mandatory for the wrappers to be
    // safe (so we never alter normal-runtime waits).
    if (HMODULE ntdll = GetModuleHandleA("ntdll.dll")) {
        g_pRtlDllShutdownInProgress =
            reinterpret_cast<RtlDllShutdownInProgressFn>(
                GetProcAddress(ntdll, "RtlDllShutdownInProgress"));
    }
    if (!g_pRtlDllShutdownInProgress) {
        // Without a shutdown probe we cannot safely cap any waits.
        return;
    }

    // Pin SkyMPFixes.dll so our wrapper code stays mapped through every
    // other DLL's DLL_PROCESS_DETACH (in particular, MpClientPlugin's).
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN |
                           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCSTR>(&InstallMpClientShutdownFix),
                       &self);

    if (HMODULE mp = GetModuleHandleA("MpClientPlugin.dll")) {
        PatchModuleWaitImports(mp);
    } else {
        if (HANDLE t = CreateThread(
                nullptr, 0, &MpClientWaiterThread, nullptr, 0, nullptr))
            CloseHandle(t);
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// FIX 5: V8 JavaScript Heap Limit (configurable via SkyMPFixes.ini)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//
// V8 (inside libnode.dll, loaded by SkyrimPlatformImpl.dll) chooses its
// `max-old-space-size` automatically based on physical RAM. On x64 the
// computed cap tops out at ~4 GB regardless of how much RAM is installed.
// When the SkyMP JS client lives long enough (~3+ hours), the old generation
// grows past that cap and V8's default fatal-error handler fires:
//
//     FATAL ERROR: ... JavaScript heap out of memory
//     Allocation failed - JavaScript heap out of memory
//
// V8 then calls ExitProcess, which combined with FIX 4's deadlock would
// leave a multi-GB zombie SkyrimSE.exe; even with FIX 4 the player still
// loses their session.
//
// The fix: call libnode!v8::V8::SetFlagsFromString("--max-old-space-size=N")
// before V8 is initialized. V8 parses flags during V8::Initialize(), and
// SkyrimPlatformImpl invokes V8::Initialize() lazily on the first JsEngine
// construction. We have two opportunities to set the flag in time:
//
//   1. If libnode.dll is already mapped when our SKSEPlugin_Load runs,
//      resolve SetFlagsFromString and call it immediately.
//   2. Otherwise, register an LdrRegisterDllNotification callback. When
//      libnode is loaded later (as part of SkyrimPlatform's chain) the
//      loader notifies us synchronously, before any SkyrimPlatform code
//      has had a chance to call V8::Initialize().
//
// Both branches log their actions to SkyMPFixes.log so you can see the
// configured value at runtime.
//
// NOTE: SkyrimPlatform passes `kDisableNodeOptionsEnv` to
// node::InitializeOncePerProcess, so the standard NODE_OPTIONS env var
// is intentionally ignored by the embedded Node. V8::SetFlagsFromString
// bypasses that filter (it's a V8-level API, not a Node-level one).
//
// CONFIG:
//   SkyMPFixes.ini  (in the same folder as SkyMPFixes.dll)
//   ----------------------------------------------------
//   [V8]
//   MaxOldSpaceMB = 8192
//
//   Missing file, missing key, or value 0 -> leave V8's default in place.

using SetFlagsFromStringFn  = void (*)(const char* str);
using SetFlagsFromStringLFn = void (*)(const char* str, size_t length);
using V8InitializeFn        = bool (*)();
using V8IsInitializedFn     = bool (*)();

// NTSTATUS is declared in <ntdef.h>/<winternl.h> which Windows.h does NOT
// pull in by default. Define it locally so we don't have to pull in extra
// headers.
typedef LONG SP_NTSTATUS;

static int                   g_v8MaxOldSpaceMB    = 0;     // read from INI
static volatile LONG         g_v8FlagApplied      = 0;     // 0 = pending, 1 = done
static PVOID                 g_dllNotifyCookie    = nullptr;

// IMPORTANT: the LdrRegisterDllNotification callback runs while holding the
// Windows loader lock. Calling GetProcAddress and then a function inside the
// just-loaded DLL from that context is unsafe (it caused a hard CTD before
// CrashLoggerSSE could even write a log). Instead, the callback does the
// minimum possible (publish the base address and signal an event), and a
// pre-spawned worker thread does the actual GetProcAddress + V8 call after
// the loader lock is released.
static HMODULE volatile      g_libnodeBase        = nullptr;
static HANDLE                g_libnodeLoadedEvent = nullptr;
static HANDLE                g_v8WorkerThread     = nullptr;

// LdrRegisterDllNotification plumbing (ntdll, semi-documented)
typedef struct _SP_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} SP_UNICODE_STRING, *PSP_UNICODE_STRING;
typedef const SP_UNICODE_STRING* PCSP_UNICODE_STRING;

typedef struct _SP_LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG               Flags;
    PCSP_UNICODE_STRING FullDllName;
    PCSP_UNICODE_STRING BaseDllName;
    PVOID               DllBase;
    ULONG               SizeOfImage;
} SP_LDR_DLL_LOADED_NOTIFICATION_DATA;

typedef union _SP_LDR_DLL_NOTIFICATION_DATA {
    SP_LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
    SP_LDR_DLL_LOADED_NOTIFICATION_DATA Unloaded; // same layout
} SP_LDR_DLL_NOTIFICATION_DATA;
typedef const SP_LDR_DLL_NOTIFICATION_DATA* PCSP_LDR_DLL_NOTIFICATION_DATA;

#define SP_LDR_DLL_NOTIFICATION_REASON_LOADED 1

typedef VOID (NTAPI *PSP_LDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG NotificationReason,
    PCSP_LDR_DLL_NOTIFICATION_DATA NotificationData,
    PVOID Context);

using LdrRegisterDllNotificationFn = SP_NTSTATUS (NTAPI*)(
    ULONG, PSP_LDR_DLL_NOTIFICATION_FUNCTION, PVOID, PVOID*);
using LdrUnregisterDllNotificationFn = SP_NTSTATUS (NTAPI*)(PVOID);

// Try both mangled overloads of V8::SetFlagsFromString. Returns true on
// success and writes the chosen flag string into chosenFlag for logging.
static bool ApplyV8MaxOldSpaceFlag(HMODULE libnode, int maxOldSpaceMB,
                                   char* chosenFlag, size_t chosenFlagSize) {
    if (!libnode || maxOldSpaceMB <= 0)
        return false;

    if (InterlockedCompareExchange(&g_v8FlagApplied, 1, 0) != 0) {
        // Already applied on another path -- treat as success.
        return true;
    }

    // Mangled (MSVC x64) for:
    //   void v8::V8::SetFlagsFromString(const char*)
    auto pSet = reinterpret_cast<SetFlagsFromStringFn>(
        GetProcAddress(libnode, "?SetFlagsFromString@V8@v8@@SAXPEBD@Z"));

    // Fallback to the length-taking overload:
    //   void v8::V8::SetFlagsFromString(const char*, size_t)
    auto pSetL = reinterpret_cast<SetFlagsFromStringLFn>(
        GetProcAddress(libnode, "?SetFlagsFromString@V8@v8@@SAXPEBD_K@Z"));

    if (!pSet && !pSetL) {
        Log("[V8] ERROR: libnode.dll has neither SetFlagsFromString overload "
            "exported (looked up ?SetFlagsFromString@V8@v8@@SAXPEBD@Z and "
            "?SetFlagsFromString@V8@v8@@SAXPEBD_K@Z). Heap cap NOT raised.");
        // Reset the gate so we can try again if libnode reloads.
        InterlockedExchange(&g_v8FlagApplied, 0);
        return false;
    }

    // Helpful warning if V8 is already initialized (flag may be ignored).
    if (auto pIsInit = reinterpret_cast<V8IsInitializedFn>(
            GetProcAddress(libnode, "?IsInitialized@V8@v8@@SA_NXZ"))) {
        if (pIsInit()) {
            Log("[V8] WARNING: V8::IsInitialized() == true BEFORE we set the "
                "flag. --max-old-space-size will likely be ignored for the "
                "live isolate.");
        }
    }

    std::snprintf(chosenFlag, chosenFlagSize,
                  "--max-old-space-size=%d", maxOldSpaceMB);

    if (pSet) {
        pSet(chosenFlag);
        Log("[V8] Applied flag via SetFlagsFromString(const char*): \"%s\"",
            chosenFlag);
    } else {
        pSetL(chosenFlag, std::strlen(chosenFlag));
        Log("[V8] Applied flag via SetFlagsFromString(const char*, size_t): "
            "\"%s\"", chosenFlag);
    }
    return true;
}

static VOID NTAPI LibnodeLoadCallback(
    ULONG reason,
    PCSP_LDR_DLL_NOTIFICATION_DATA data,
    PVOID /*ctx*/) {
    if (reason != SP_LDR_DLL_NOTIFICATION_REASON_LOADED || !data)
        return;
    if (!data->Loaded.BaseDllName || !data->Loaded.BaseDllName->Buffer)
        return;

    // Compare BaseDllName (UTF-16) case-insensitively against "libnode.dll".
    const wchar_t* base = data->Loaded.BaseDllName->Buffer;
    const wchar_t  want[] = L"libnode.dll";
    bool match = true;
    for (int i = 0; want[i] || base[i]; ++i) {
        wchar_t a = base[i], b = want[i];
        if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + (L'a' - L'A'));
        if (b >= L'A' && b <= L'Z') b = (wchar_t)(b + (L'a' - L'A'));
        if (a != b) { match = false; break; }
    }
    if (!match)
        return;

    // CRITICAL: we are holding the Windows loader lock here. Do NOT call
    // GetProcAddress / Log / any libnode code from this callback. Just
    // publish the base address and wake the worker thread that's waiting
    // outside the lock.
    g_libnodeBase = reinterpret_cast<HMODULE>(data->Loaded.DllBase);
    if (g_libnodeLoadedEvent)
        SetEvent(g_libnodeLoadedEvent);
}

static DWORD WINAPI V8FlagWorkerThread(LPVOID) {
    Log("[V8] worker thread started; waiting for libnode.dll to load (up to "
        "300s)");

    if (!g_libnodeLoadedEvent) {
        Log("[V8] worker: event handle is null, bailing out.");
        return 0;
    }

    DWORD wait = WaitForSingleObject(g_libnodeLoadedEvent, 300 * 1000);
    if (wait != WAIT_OBJECT_0) {
        Log("[V8] worker: timed out waiting for libnode (WaitForSingleObject "
            "returned 0x%X). Heap cap NOT applied.", wait);
        return 0;
    }

    HMODULE libnode = g_libnodeBase;
    if (!libnode) {
        Log("[V8] worker: event signalled but g_libnodeBase is null. Heap "
            "cap NOT applied.");
        return 0;
    }

    Log("[V8] worker: libnode.dll detected at 0x%p -- applying flag",
        (void*)libnode);

    char chosenFlag[64] = {0};
    ApplyV8MaxOldSpaceFlag(libnode, g_v8MaxOldSpaceMB,
                           chosenFlag, sizeof(chosenFlag));
    return 0;
}

static void InstallV8HeapLimitFix() {
    // 1. Read configured value (ini may not exist -> returns 0 -> no-op).
    g_v8MaxOldSpaceMB = (int)GetPrivateProfileIntA(
        "V8", "MaxOldSpaceMB", 0, g_iniPath);

    Log("[V8] Config path: %s", g_iniPath[0] ? g_iniPath : "(unresolved)");
    Log("[V8] [V8]MaxOldSpaceMB = %d (0 means: leave V8 default in place)",
        g_v8MaxOldSpaceMB);

    if (g_v8MaxOldSpaceMB <= 0) {
        Log("[V8] Heap limit fix disabled (no value configured). V8 will use "
            "its built-in default (typically ~4 GB on x64 regardless of "
            "physical RAM).");
        return;
    }

    // Bound-check to something sane. V8 accepts MB; we cap at 64 GB to avoid
    // typos like \"MaxOldSpaceMB = 80000000\" silently nuking V8.
    if (g_v8MaxOldSpaceMB > 65536) {
        Log("[V8] WARNING: MaxOldSpaceMB=%d looks unrealistic; clamping to "
            "65536 (64 GB).", g_v8MaxOldSpaceMB);
        g_v8MaxOldSpaceMB = 65536;
    }

    // 2. Create the event + worker thread up front. The worker handles BOTH
    //    paths (libnode already loaded vs. loaded later), keeping the actual
    //    cross-DLL call out of the loader-lock-holding notification callback.
    g_libnodeLoadedEvent = CreateEventW(nullptr, /*manual=*/TRUE,
                                        /*initial=*/FALSE, nullptr);
    if (!g_libnodeLoadedEvent) {
        Log("[V8] ERROR: CreateEvent failed (gle=%u). Heap cap NOT applied.",
            GetLastError());
        return;
    }

    g_v8WorkerThread = CreateThread(nullptr, 0, &V8FlagWorkerThread, nullptr,
                                    0, nullptr);
    if (!g_v8WorkerThread) {
        Log("[V8] ERROR: CreateThread failed (gle=%u). Heap cap NOT applied.",
            GetLastError());
        CloseHandle(g_libnodeLoadedEvent);
        g_libnodeLoadedEvent = nullptr;
        return;
    }

    // 3. If libnode is already mapped (we loaded after SkyrimPlatformImpl),
    //    publish its base and signal the worker immediately.
    if (HMODULE libnode = GetModuleHandleA("libnode.dll")) {
        Log("[V8] libnode.dll already loaded at 0x%p -- signalling worker",
            (void*)libnode);
        g_libnodeBase = libnode;
        SetEvent(g_libnodeLoadedEvent);
        return;
    }

    // 4. Otherwise register a DLL-load notification. The callback will only
    //    SetEvent (loader-lock safe); the worker thread does the real work.
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) {
        Log("[V8] ERROR: ntdll.dll not found. Cannot register load notify.");
        return;
    }
    auto pReg = reinterpret_cast<LdrRegisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (!pReg) {
        Log("[V8] ERROR: ntdll!LdrRegisterDllNotification not exported. "
            "Cannot defer V8 flag application.");
        return;
    }

    SP_NTSTATUS st = pReg(0, &LibnodeLoadCallback, nullptr, &g_dllNotifyCookie);
    if (st < 0) {
        Log("[V8] ERROR: LdrRegisterDllNotification failed with NTSTATUS "
            "0x%08X.", (unsigned)st);
        return;
    }

    Log("[V8] libnode.dll not yet loaded -- registered load notification "
        "(cookie=0x%p). Worker thread is waiting for the signal.",
        g_dllNotifyCookie);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// FIX 6: Hang Watchdog (configurable via SkyMPFixes.ini)
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//
// When a SkyrimPlatform hook is called on the game's main thread and the JS
// worker pool is exhausted (or a JS handler is slow), the game locks up
// forever: the main thread sits in a Sleep loop waiting for a worker that
// will never come. The proper fix lives in the SkyrimPlatform source
// (Hook.cpp -- see the upstream 'hook-deadlock-fix' branch). This runtime
// version can't reach into that code, but it can at least NOTICE the freeze
// and take action so the user isn't staring at a hung window for minutes.
//
// The mechanism is Windows' own IsHungAppWindow(): whenever the main game
// window has not pumped its message queue for a few seconds Windows returns
// TRUE. A background thread polls that every 2 seconds. When the game has
// been unresponsive for [HangWatchdog]HangThresholdSec seconds continuously,
// we take the configured Action:
//
//   log         -- just note it in SkyMPFixes.log
//   dump        -- write a full-memory mini-dump next to SkyMPFixes.log
//   kill        -- TerminateProcess on ourselves (immediate exit)
//   dumpAndKill -- dump first, then kill
//
// Defaults are Enabled=1, ThresholdSec=30, Action=dumpAndKill. That
// combination captures a mini-dump for later analysis and then ends the
// frozen session so the user can restart the game. Change Action=dump if
// you'd rather keep the hung process running and decide when to close it.

using SP_IsHungAppWindowFn = BOOL (WINAPI*)(HWND);

enum HangAction {
    kHangAction_Log         = 0,
    kHangAction_Dump        = 1,
    kHangAction_Kill        = 2,
    kHangAction_DumpAndKill = 3,
};

static bool                 g_hangEnabled        = true;
static DWORD                g_hangThresholdSec   = 30;
static HangAction           g_hangAction         = kHangAction_DumpAndKill;
static SP_IsHungAppWindowFn g_pIsHungAppWindow   = nullptr;
static HWND                 g_gameWindow         = nullptr;

// ----- Game-loop heartbeat check (complements IsHungAppWindow) -----------
// memprofile.js touches this file on every SP "update" tick (throttled to
// ~1 Hz). If it goes stale for more than g_heartbeatStaleSec seconds while
// the file exists, we treat it as a game-loop hang -- catches freezes where
// the window pump keeps running (so IsHungAppWindow reports OK) but the
// game's update loop is stuck (typical SkyrimPlatform hook deadlock).
//
// If the file NEVER exists (memprofile.js not installed) the check silently
// skips -- no false positives from users who don't run the profiler.
//
// Freshness rule: we only "count" a heartbeat as seen when its mtime is
// newer than the watchdog's own start time. That way a stale file left
// over from a previous session (e.g. a game that crashed) cannot trigger
// an immediate false positive on the next launch.
static DWORD                g_heartbeatStaleSec  = 10;      // 0 disables
static char                 g_heartbeatPath[MAX_PATH] = {0};
static bool                 g_heartbeatSeen      = false;   // true once we've
                                                            // seen a fresh
                                                            // (post-startup)
                                                            // heartbeat
static ULONGLONG            g_watchdogStartFt    = 0;       // FILETIME ticks
                                                            // at watchdog init

// EnumWindows callback: pick the first top-level, visible, non-owned window
// belonging to our own process. That's the game's main render window.
static BOOL CALLBACK HangWatchdog_FindWindowProc(HWND hwnd, LPARAM /*lp*/) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    g_gameWindow = hwnd;
    return FALSE; // stop enumeration
}

// MiniDumpWriteDump prototype (avoid pulling in dbghelp.h)
using SP_MiniDumpWriteDumpFn = BOOL (WINAPI*)(
    HANDLE hProcess, DWORD pid, HANDLE hFile, DWORD dumpType,
    PVOID exceptionParam, PVOID userStreamParam, PVOID callbackParam);

static void HangWatchdog_WriteDump() {
    if (!g_dllDir[0]) {
        Log("[watchdog] cannot write dump -- DLL directory not resolved.");
        return;
    }

    char dumpPath[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(dumpPath, MAX_PATH,
                  "%s\\SkyMPFixes_hang_%04u%02u%02u_%02u%02u%02u.dmp",
                  g_dllDir,
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);

    HANDLE hFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        Log("[watchdog] failed to create dump file %s (gle=%u)",
            dumpPath, GetLastError());
        return;
    }

    HMODULE dbg = GetModuleHandleA("dbghelp.dll");
    if (!dbg) dbg = LoadLibraryA("dbghelp.dll");
    if (!dbg) {
        Log("[watchdog] failed to load dbghelp.dll (gle=%u)", GetLastError());
        CloseHandle(hFile);
        return;
    }

    auto pWriteDump = reinterpret_cast<SP_MiniDumpWriteDumpFn>(
        GetProcAddress(dbg, "MiniDumpWriteDump"));
    if (!pWriteDump) {
        Log("[watchdog] MiniDumpWriteDump not exported by dbghelp.");
        CloseHandle(hFile);
        return;
    }

    // Flags: MiniDumpWithFullMemory | WithHandleData | WithUnloadedModules
    //      | WithFullMemoryInfo | WithThreadInfo
    const DWORD kType = 0x00000002 | 0x00000004 | 0x00000020 | 0x00000800
                      | 0x00001000;
    Log("[watchdog] writing dump to %s (this can take a while) ...", dumpPath);
    BOOL ok = pWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                         hFile, kType, nullptr, nullptr, nullptr);
    CloseHandle(hFile);
    Log("[watchdog] dump %s (%s)", ok ? "written" : "FAILED", dumpPath);
}

static const char* HangActionName(HangAction a) {
    switch (a) {
        case kHangAction_Log:         return "log";
        case kHangAction_Dump:        return "dump";
        case kHangAction_Kill:        return "kill";
        case kHangAction_DumpAndKill: return "dumpAndKill";
    }
    return "?";
}

// Take the configured action for a detected hang. Shared by both the window
// hang path and the heartbeat-stale path.
static void HangWatchdog_TakeAction(const char* reason) {
    Log("[watchdog] taking action=%s -- reason: %s",
        HangActionName(g_hangAction), reason);
    if (g_hangAction == kHangAction_Dump ||
        g_hangAction == kHangAction_DumpAndKill) {
        HangWatchdog_WriteDump();
    }
    if (g_hangAction == kHangAction_Kill ||
        g_hangAction == kHangAction_DumpAndKill) {
        Log("[watchdog] terminating process (exit code 0xDEAD).");
        TerminateProcess(GetCurrentProcess(), 0xDEADu);
    }
}

// Returns the age of the heartbeat file in seconds, or -1 if the file
// doesn't exist / can't be stat'd / is only a stale leftover from a
// previous session. On first successful read of a FRESH heartbeat we
// latch g_heartbeatSeen so a later disappearance of the file (e.g.
// plugin crashed) still counts as staleness rather than "not installed".
static long long HangWatchdog_HeartbeatAgeSec() {
    if (!g_heartbeatPath[0]) return -1;

    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExA(g_heartbeatPath, GetFileExInfoStandard, &attr)) {
        // File missing. If we've never seen a fresh one, skip silently --
        // most likely memprofile.js is not installed. If we HAVE seen it,
        // treat as very stale (something removed it while running).
        return g_heartbeatSeen ? (long long)0x7FFFFFFF : -1;
    }

    ULARGE_INTEGER ftMtime;
    ftMtime.LowPart  = attr.ftLastWriteTime.dwLowDateTime;
    ftMtime.HighPart = attr.ftLastWriteTime.dwHighDateTime;

    // If the file's mtime is older than the watchdog's start time, it's a
    // leftover from a previous session. Ignore it until memprofile.js
    // touches it and pushes the mtime forward.
    if (g_watchdogStartFt && ftMtime.QuadPart < g_watchdogStartFt) {
        return -1;
    }

    g_heartbeatSeen = true;

    // Compare file's last-write-time against system time.
    ULARGE_INTEGER ftNow;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ftNow.LowPart  = nowFt.dwLowDateTime;
    ftNow.HighPart = nowFt.dwHighDateTime;

    if (ftNow.QuadPart <= ftMtime.QuadPart) return 0;
    // FILETIME is 100-ns ticks; divide by 10,000,000 for seconds.
    return (long long)((ftNow.QuadPart - ftMtime.QuadPart) / 10000000ULL);
}

static DWORD WINAPI HangWatchdogThread(LPVOID) {
    // Record our start time so HangWatchdog_HeartbeatAgeSec() can ignore
    // heartbeat files left over from previous sessions.
    {
        FILETIME ftNow;
        GetSystemTimeAsFileTime(&ftNow);
        ULARGE_INTEGER u;
        u.LowPart  = ftNow.dwLowDateTime;
        u.HighPart = ftNow.dwHighDateTime;
        g_watchdogStartFt = u.QuadPart;
    }

    // Wait for the game window to appear. It usually shows within seconds,
    // but a slow SkyPatcher/mod init or an ENB pre-load can delay it, so we
    // give it up to 5 minutes.
    for (int i = 0; i < 150 && !g_gameWindow; ++i) {
        EnumWindows(&HangWatchdog_FindWindowProc, 0);
        if (!g_gameWindow) Sleep(2000);
    }
    if (!g_gameWindow) {
        Log("[watchdog] could not find the game window after 5 min -- "
            "watchdog disabled for this session.");
        return 0;
    }

    Log("[watchdog] watching HWND=0x%p (windowHangSec=%u, heartbeatStaleSec=%u,"
        " action=%s)",
        g_gameWindow, g_hangThresholdSec, g_heartbeatStaleSec,
        HangActionName(g_hangAction));
    if (g_heartbeatPath[0])
        Log("[watchdog] heartbeat file: %s", g_heartbeatPath);

    constexpr DWORD kPollMs = 2000;
    const DWORD ticksNeeded = (g_hangThresholdSec * 1000u) / kPollMs;
    if (ticksNeeded == 0) return 0;

    DWORD hungTicks               = 0;
    bool  triggeredAction         = false;
    bool  triggeredHeartbeatAction = false;

    for (;;) {
        Sleep(kPollMs);

        // If the window is gone we're done -- game has already exited (or
        // recreated its window; either way our HWND is stale).
        if (!IsWindow(g_gameWindow)) {
            Log("[watchdog] game window closed -- watchdog exiting.");
            return 0;
        }

        // --- Detector A: window message pump stuck ---
        BOOL hung = g_pIsHungAppWindow(g_gameWindow);
        if (hung) {
            ++hungTicks;
            if (hungTicks == 1) {
                Log("[watchdog] game window became unresponsive (first "
                    "detection).");
            }
            if (hungTicks >= ticksNeeded && !triggeredAction) {
                triggeredAction = true;
                DWORD hungSec = (hungTicks * kPollMs) / 1000;
                char reason[128];
                std::snprintf(reason, sizeof(reason),
                              "window unresponsive for %us", hungSec);
                HangWatchdog_TakeAction(reason);
            }
        } else {
            if (hungTicks > 0) {
                DWORD hungSec = (hungTicks * kPollMs) / 1000;
                Log("[watchdog] game window recovered after %us hung.",
                    hungSec);
            }
            hungTicks       = 0;
            triggeredAction = false;
        }

        // --- Detector B: game-loop heartbeat stale ---
        // Runs INDEPENDENTLY of detector A so a hang that keeps the message
        // pump alive but stalls the game loop (typical SkyrimPlatform hook
        // deadlock) still gets caught.
        if (g_heartbeatStaleSec > 0) {
            long long ageSec = HangWatchdog_HeartbeatAgeSec();
            if (ageSec >= 0 && ageSec >= (long long)g_heartbeatStaleSec) {
                if (!triggeredHeartbeatAction) {
                    triggeredHeartbeatAction = true;
                    char reason[128];
                    std::snprintf(reason, sizeof(reason),
                                  "game-loop heartbeat stale for %llds",
                                  ageSec);
                    HangWatchdog_TakeAction(reason);
                }
            } else if (ageSec >= 0) {
                // Reset the latch once heartbeat becomes fresh again so a
                // subsequent stall can trigger a new dump.
                triggeredHeartbeatAction = false;
            }
        }
    }
}

static void InstallHangWatchdog() {
    // Read config
    g_hangEnabled =
        (GetPrivateProfileIntA("HangWatchdog", "Enabled", 1, g_iniPath) != 0);
    int thresholdSec =
        GetPrivateProfileIntA("HangWatchdog", "HangThresholdSec", 30, g_iniPath);
    int heartbeatSec =
        GetPrivateProfileIntA("HangWatchdog", "HeartbeatStaleSec", 10, g_iniPath);

    char actionStr[32] = {0};
    GetPrivateProfileStringA("HangWatchdog", "Action", "dumpAndKill",
                             actionStr, sizeof(actionStr), g_iniPath);
    if (_stricmp(actionStr, "log") == 0)              g_hangAction = kHangAction_Log;
    else if (_stricmp(actionStr, "dump") == 0)         g_hangAction = kHangAction_Dump;
    else if (_stricmp(actionStr, "kill") == 0)         g_hangAction = kHangAction_Kill;
    else if (_stricmp(actionStr, "dumpAndKill") == 0)  g_hangAction = kHangAction_DumpAndKill;
    else                                                g_hangAction = kHangAction_DumpAndKill;

    Log("[watchdog] Enabled=%d ThresholdSec=%d HeartbeatStaleSec=%d Action=%s",
        (int)g_hangEnabled, thresholdSec, heartbeatSec, actionStr);

    if (!g_hangEnabled) {
        Log("[watchdog] disabled by config.");
        return;
    }

    // Clamp to sensible bounds (5s..1h for window hang, 0..1h for heartbeat)
    if (thresholdSec < 5)    thresholdSec = 5;
    if (thresholdSec > 3600) thresholdSec = 3600;
    g_hangThresholdSec = (DWORD)thresholdSec;

    if (heartbeatSec < 0)    heartbeatSec = 0;
    if (heartbeatSec > 3600) heartbeatSec = 3600;
    g_heartbeatStaleSec = (DWORD)heartbeatSec;

    // Compute the heartbeat file path. memprofile.js writes it to
    // <SkyrimDir>\Data\Platform\SkyMPFixes.heartbeat; we're at
    // <SkyrimDir>\Data\SKSE\Plugins so back up two levels.
    if (g_dllDir[0]) {
        char skyrimDir[MAX_PATH];
        std::strncpy(skyrimDir, g_dllDir, MAX_PATH - 1);
        skyrimDir[MAX_PATH - 1] = '\0';
        // Trim "\Plugins"
        for (int i = (int)std::strlen(skyrimDir) - 1; i >= 0; --i) {
            if (skyrimDir[i] == '\\' || skyrimDir[i] == '/') {
                skyrimDir[i] = '\0';
                break;
            }
        }
        // Trim "\SKSE"
        for (int i = (int)std::strlen(skyrimDir) - 1; i >= 0; --i) {
            if (skyrimDir[i] == '\\' || skyrimDir[i] == '/') {
                skyrimDir[i] = '\0';
                break;
            }
        }
        // Trim "\Data"
        for (int i = (int)std::strlen(skyrimDir) - 1; i >= 0; --i) {
            if (skyrimDir[i] == '\\' || skyrimDir[i] == '/') {
                skyrimDir[i] = '\0';
                break;
            }
        }
        std::snprintf(g_heartbeatPath, MAX_PATH,
                      "%s\\Data\\Platform\\SkyMPFixes.heartbeat", skyrimDir);
    }

    // Resolve IsHungAppWindow (user32). It's been there since Windows XP,
    // but we probe defensively anyway.
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) user32 = LoadLibraryA("user32.dll");
    if (!user32) {
        Log("[watchdog] user32.dll not loadable -- watchdog disabled.");
        return;
    }
    g_pIsHungAppWindow = reinterpret_cast<SP_IsHungAppWindowFn>(
        GetProcAddress(user32, "IsHungAppWindow"));
    if (!g_pIsHungAppWindow) {
        Log("[watchdog] user32!IsHungAppWindow not found -- watchdog "
            "disabled.");
        return;
    }

    if (HANDLE t = CreateThread(nullptr, 0, &HangWatchdogThread, nullptr,
                                0, nullptr)) {
        CloseHandle(t);
    } else {
        Log("[watchdog] CreateThread failed (gle=%u) -- watchdog disabled.",
            GetLastError());
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// SKSE Entry Points
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

#define RUNTIME_1_6_1170 ((1u << 24) | (6u << 16) | (1170u << 4))

extern "C" {

__declspec(dllexport) SKSEPluginVersionData SKSEPlugin_Version = {
    SKSEPluginVersionData::kVersion,    // dataVersion
    1,                                   // pluginVersion
    "SkyMP Fixes",                      // name
    "jota2rz",                          // author
    "",                                  // supportEmail
    0,                                   // versionIndependenceEx
    0,                                   // versionIndependence
    { RUNTIME_1_6_1170, 0 },           // compatibleVersions
    0,                                   // seVersionRequired
};

__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* a_skse) {
    if (a_skse->isEditor)
        return false;

    HMODULE hMod = GetModuleHandleA(nullptr);
    g_baseAddr = reinterpret_cast<uintptr_t>(hMod);

    // Get module size from PE header
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(g_baseAddr + dos->e_lfanew);
    g_moduleEnd = g_baseAddr + nt->OptionalHeader.SizeOfImage;

    // Resolve plugin paths and bring up the file logger before anything else
    // so all subsequent steps land in SkyMPFixes.log.
    ResolvePluginPaths();
    LogInit();
    Log("[boot] SkyMPFixes loaded. skse=%u runtime=%u editor=%u",
        a_skse->skseVersion, a_skse->runtimeVersion, a_skse->isEditor);
    Log("[boot] DLL dir: %s", g_dllDir[0] ? g_dllDir : "(unresolved)");

    // Register all exception handlers (priority 1 = called first)
    AddVectoredExceptionHandler(1, FaceGenExceptionHandler);
    AddVectoredExceptionHandler(1, WorldCleanExceptionHandler);
    AddVectoredExceptionHandler(1, TextureQueueExceptionHandler);
    AddVectoredExceptionHandler(1, MovementJobExceptionHandler);
    AddVectoredExceptionHandler(1, JobMemcpyExceptionHandler);
    Log("[boot] Crash-fix exception handlers installed (FaceGen, WorldClean, "
        "TextureQueue, MovementJob, JobMemcpy).");

    // Diagnostic stats thread -- reports which handlers are firing and
    // flags likely livelocks. Cheap; low priority; auto-exits after 1h.
    if (HANDLE t = CreateThread(nullptr, 0, &DiagStatsThread, nullptr, 0, nullptr)) {
        CloseHandle(t);
        Log("[diag] handler-stats thread started (5s reporting interval)");
    }

    // Install non-crash fixes
    InstallMpClientShutdownFix();
    InstallV8HeapLimitFix();
    InstallHangWatchdog();

    Log("[boot] SKSEPlugin_Load complete.");
    return true;
}

}  // extern "C"
