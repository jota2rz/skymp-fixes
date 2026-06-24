// SkyMP Fixes - Combined SKSE plugin for Skyrim SE 1.6.1170
//
// Contains three crash fixes for issues caused by SkyMP client's
// aggressive actor spawn/delete patterns:
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

#include <Windows.h>
#include <cstdint>
#include <cstring>

// ── Minimal SKSE64 AE plugin API ───────────────────────────────────────────

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

// ── Shared state ───────────────────────────────────────────────────────────

static uintptr_t g_baseAddr   = 0;
static uintptr_t g_moduleEnd  = 0;

static bool IsInModule(uintptr_t addr) {
    return addr >= g_baseAddr && addr < g_moduleEnd;
}

// ════════════════════════════════════════════════════════════════════════════
// FIX 1: FaceGen Crash (two variants)
// ════════════════════════════════════════════════════════════════════════════

// Variant A: Corrupted vtable jump — RIP lands at garbage address,
//   stack has face gen return addresses.
// Variant B: Invalid pointer in face gen helper — RIP is valid but a register
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
    { 0x0429E69, 4, kFGBadReg_RDI_FFFFFFFF }, // C6 47 1C 01           — mov byte [rdi+0x1C], 1
    { 0x04328E9, 3, kFGBadReg_RCX_Null     }, // 48 8B 01              — mov rax, [rcx]
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
        // the FaceGen helper range — that caller is prepared to handle a
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
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Fallback: just skip the faulting instruction and zero the result.
        ctx->Rip += site.insnLen;
        ctx->Rax  = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Variant A: RIP at garbage address (corrupted vtable jump)

    uintptr_t faultAddr = ctx->Rip;

    // If RIP is inside a known module, this isn't our crash
    if (IsInModule(faultAddr))
        return EXCEPTION_CONTINUE_SEARCH;

    // If RIP is in high module address range, not our crash
    if (faultAddr > 0x7FF000000000ULL)
        return EXCEPTION_CONTINUE_SEARCH;

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
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Unwind to the safe return point
    ctx->Rip = safeReturn;
    ctx->Rsp = safeRsp;
    ctx->Rax = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ════════════════════════════════════════════════════════════════════════════
// FIX 2: WorldClean Crash (NPC deletion during cell init)
// ════════════════════════════════════════════════════════════════════════════

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
            ctx->EFlags &= ~0x40u;  // clear ZF → "type mismatch" path
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// ════════════════════════════════════════════════════════════════════════════
// FIX 3: Texture Queue Crash (BSTaskManagerThread null deref)
// ════════════════════════════════════════════════════════════════════════════
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
// then checks the result — a null return causes it to skip processing
// this queue entry gracefully.
//
// Additionally, a second null check at +0E02792: mov rcx, [rbx+0x20]
// In the crash log this appears as the next frame — if RBX's queued entry
// has a null member at +0x20, the subsequent deref of that will also crash.
// We handle both sites.

static constexpr uintptr_t kTexQueueOffsets[] = {
    0x0E02786,  // mov rax, [rcx+0x28] — primary crash site
    0x0E02792,  // mov rcx, [rbx+0x20] — secondary (next instruction after check)
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
            return EXCEPTION_CONTINUE_EXECUTION;
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
                    // [rbx+0x20] is null — loading it into RCX will cause
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
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// ════════════════════════════════════════════════════════════════════════════
// SKSE Entry Points
// ════════════════════════════════════════════════════════════════════════════

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

    // Register all exception handlers (priority 1 = called first)
    AddVectoredExceptionHandler(1, FaceGenExceptionHandler);
    AddVectoredExceptionHandler(1, WorldCleanExceptionHandler);
    AddVectoredExceptionHandler(1, TextureQueueExceptionHandler);

    return true;
}

}  // extern "C"
