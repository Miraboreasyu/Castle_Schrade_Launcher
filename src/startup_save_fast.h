// Included inside the implementation namespace after startup_resident_budget.h.
// Opportunity 4A: Parallel 3-slot save inner Blowfish decrypt + checksum (0x141B8E2A0).
// Opportunity 4B: Native silent flag (op+0xC = 1) on aLogo initial save load (op1).
// Opportunity 5 : Lossless hash-checked skip of redundant 11.28 MB op0 re-save in
//                 aTitle state 4 content check (0x14029A0AB -> 0x14029A1A0).

constexpr uintptr_t kUserDataUnpackRva = 0x1B8E2A0;
constexpr unsigned char kUserDataUnpackExpected[12] = {
    0x4C, 0x8B, 0xDC,             // mov r11,rsp
    0x53,                         // push rbx
    0x56,                         // push rsi
    0x48, 0x81, 0xEC, 0x98, 0, 0, 0, // sub rsp,0x98
};
constexpr uintptr_t kContentCheckSaveCallRva = 0x29A0AB;
constexpr uintptr_t kContentCheckSaveTargetRva = 0x29A1A0;
constexpr uint32_t kRawSaveSlotBytes = 0x209AC0;
constexpr uint32_t kRawSaveTrailerBytes = 0x200;

struct SaveBaselineDigest {
    bool valid = false;
    uint64_t slots = 0;
    uint64_t hdr = 0;
    uint64_t sys = 0;
} g_saveBaseline;

bool g_saveFastHooksTried = false;
bool g_saveParallelHookInstalled = false;
bool g_contentSkipHookInstalled = false;
bool g_silentOp1Applied = false;

bool ReadableSpan(uintptr_t addr, size_t len) {
    if (addr < 0x10000 || len == 0) return false;
    uintptr_t cur = addr;
    size_t rem = len;
    while (rem > 0) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cur) return false;
        const size_t avail = static_cast<size_t>(regionEnd - cur);
        if (avail >= rem) return true;
        cur = regionEnd;
        rem -= avail;
    }
    return true;
}

uint64_t FastHash64(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xCBF29CE484222325ULL ^ static_cast<uint64_t>(len);
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t v = 0;
        memcpy(&v, p + i, 8);
        h ^= v;
        h *= 0x100000001B3ULL;
    }
    for (; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return h;
}

SaveBaselineDigest ComputeUserDataDigest(uintptr_t user) {
    SaveBaselineDigest d{};
    if (!user || !Readable(user + 0xA8, 8)) return d;
    const uintptr_t slots = P(user + 0xA8);
    constexpr size_t kSlotsTotal = static_cast<size_t>(kSaveSlotSize) * 3;
    constexpr size_t kHdrOffset = 0xE0;
    constexpr size_t kHdrLen = 0x3AC8 - 0xE0;
    constexpr size_t kSysOffset = 0x3AC8;
    constexpr size_t kSysLen = 0x1F8B0 - 0x3AC8;
    if (!slots || !ReadableSpan(slots, kSlotsTotal) ||
        !ReadableSpan(user + kHdrOffset, kHdrLen) ||
        !ReadableSpan(user + kSysOffset, kSysLen))
        return d;
    d.slots = FastHash64(reinterpret_cast<const void*>(slots), kSlotsTotal);
    d.hdr = FastHash64(reinterpret_cast<const void*>(user + kHdrOffset), kHdrLen);
    d.sys = FastHash64(reinterpret_cast<const void*>(user + kSysOffset), kSysLen);
    d.valid = true;
    return d;
}

struct SaveSlotDecryptJob {
    uintptr_t user = 0;
    uint8_t* dst = nullptr;
    uintptr_t src = 0;
    uint32_t slot = 0;
    uint8_t checksumOk = 0;
    uint8_t (__fastcall* decryptFn)(uintptr_t, void*, uintptr_t, uint32_t, int) = nullptr;
    uint8_t (__fastcall* checksumFn)(uintptr_t, void*, uint32_t, uint32_t) = nullptr;
};

void ExecuteSaveSlotDecrypt(SaveSlotDecryptJob* job) {
    constexpr uint32_t kPayloadLen = kRawSaveSlotBytes - kRawSaveTrailerBytes;
    job->decryptFn(job->user, job->dst, job->src, kPayloadLen, kRawSaveTrailerBytes);
    job->checksumOk = job->checksumFn(job->user, job->dst, kPayloadLen, job->slot);
}

VOID CALLBACK SaveSlotDecryptWorkCb(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WORK) {
    ExecuteSaveSlotDecrypt(static_cast<SaveSlotDecryptJob*>(ctx));
}

// Replaces FUN_141B8E2A0(sUserData* user) via 1-byte displaced RET (0xC3).
// Parallelizes inner Blowfish decrypt + checksum across 3 slots (mirroring Capcom's
// own parallel save writer FUN_141B8EEF0), then runs MtMemoryStream/MtByteReader/
// FUN_141361D90 unpack sequentially on the main thread so engine allocators,
// MtRandom and sMhGUI stay strictly single-threaded.
void OnUserDataUnpack(hook::Ctx* c) {
    const uintptr_t user = static_cast<uintptr_t>(c->rcx);
    if (!user || !Readable(user + 0x78, 8) || !Readable(user + 0x3C, 12)) {
        c->rax = 8;
        return;
    }
    const uintptr_t rawBase = P(user + 0x78);
    if (!rawBase || Rd32(user + 0x44) == 0) {
        c->rax = 8;
        return;
    }
    const uint32_t expectedVer = Rd32(g_exeBase + 0x324FAB4);
    if (Rd32(user + 0x3C) != expectedVer) {
        const uintptr_t slots = P(user + 0xA8);
        const auto resetSlot = reinterpret_cast<void (__fastcall*)(uintptr_t)>(g_exeBase + 0x1350810);
        for (uint32_t s = 0; s < 3; ++s) {
            if (slots) resetSlot(slots + static_cast<uintptr_t>(s) * kSaveSlotSize);
        }
        c->rax = 8;
        return;
    }

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    const int64_t t0 = ResidentCounter();

    SaveSlotDecryptJob jobs[3]{};
    const auto decryptFn = reinterpret_cast<decltype(SaveSlotDecryptJob::decryptFn)>(g_exeBase + 0x1B8D620);
    const auto checksumFn = reinterpret_cast<decltype(SaveSlotDecryptJob::checksumFn)>(g_exeBase + 0x1B8E9D0);
    bool allocOk = true;
    for (uint32_t s = 0; s < 3; ++s) {
        const uintptr_t slotMeta = user + 0xB0 + static_cast<uintptr_t>(s) * 0x10;
        const uintptr_t slotSrc = rawBase + static_cast<uintptr_t>(s) * kRawSaveSlotBytes;
        memcpy(reinterpret_cast<void*>(slotMeta), &slotSrc, sizeof(slotSrc));
        Wr32(slotMeta + 8, kRawSaveSlotBytes);
        *reinterpret_cast<uint8_t*>(slotMeta + 0xC) = 0;
        jobs[s].user = user;
        jobs[s].dst = static_cast<uint8_t*>(_aligned_malloc(kRawSaveSlotBytes, 16));
        jobs[s].src = slotSrc;
        jobs[s].slot = s;
        jobs[s].decryptFn = decryptFn;
        jobs[s].checksumFn = checksumFn;
        if (!jobs[s].dst) allocOk = false;
    }

    if (!allocOk) {
        for (auto& j : jobs) if (j.dst) _aligned_free(j.dst);
        c->rax = 6;
        return;
    }

    // Submit slots 1 and 2 to Windows ThreadPool; run slot 0 on main thread.
    PTP_WORK works[2]{};
    for (uint32_t w = 0; w < 2; ++w) {
        works[w] = CreateThreadpoolWork(&SaveSlotDecryptWorkCb, &jobs[w + 1], nullptr);
        if (works[w]) SubmitThreadpoolWork(works[w]);
    }
    ExecuteSaveSlotDecrypt(&jobs[0]);
    for (uint32_t w = 0; w < 2; ++w) {
        if (works[w]) {
            WaitForThreadpoolWorkCallbacks(works[w], FALSE);
            CloseThreadpoolWork(works[w]);
        } else {
            ExecuteSaveSlotDecrypt(&jobs[w + 1]);
        }
    }

    const int64_t t1 = ResidentCounter();

    const auto memStreamCtor = reinterpret_cast<void (__fastcall*)(void*, void*, uint64_t, uint32_t)>(g_exeBase + 0x2183070);
    const auto memStreamDtor = reinterpret_cast<void (__fastcall*)(void*)>(g_exeBase + 0x21831A0);
    const auto byteReaderCtor = reinterpret_cast<void (__fastcall*)(void*, void*, uint32_t)>(g_exeBase + 0x21756A0);
    const auto byteReaderSeek = reinterpret_cast<void (__fastcall*)(void*, int64_t, uint32_t)>(g_exeBase + 0x2175D00);
    const auto byteReaderDtor = reinterpret_cast<void (__fastcall*)(void*)>(g_exeBase + 0x21756F0);
    const auto unpackSlot = reinterpret_cast<uint8_t (__fastcall*)(uintptr_t, void*)>(g_exeBase + 0x1361D90);

    const uintptr_t slotsBase = P(user + 0xA8);
    uint32_t okCount = 0;
    for (uint32_t s = 0; s < 3; ++s) {
        if (jobs[s].checksumOk && slotsBase) {
            alignas(16) uint8_t memStream[0x80]{};
            alignas(16) uint8_t byteReader[0x80]{};
            memStreamCtor(memStream, jobs[s].dst, kRawSaveSlotBytes, 1);
            byteReaderCtor(byteReader, memStream, 0x1000);
            byteReaderSeek(byteReader, 0, 0);
            const uint8_t unpacked = unpackSlot(slotsBase + static_cast<uintptr_t>(s) * kSaveSlotSize, byteReader);
            *reinterpret_cast<uint8_t*>(user + 0xBC + static_cast<uintptr_t>(s) * 0x10) = unpacked;
            byteReaderDtor(byteReader);
            memStreamDtor(memStream);
            if (unpacked) ++okCount;
        }
        _aligned_free(jobs[s].dst);
    }

    const int64_t t2 = ResidentCounter();
    const double decMs = freq.QuadPart ? (t1 - t0) * 1000.0 / freq.QuadPart : 0.0;
    const double unpMs = freq.QuadPart ? (t2 - t1) * 1000.0 / freq.QuadPart : 0.0;
    char line[240];
    sprintf_s(line, "save-parallel: decrypt-ms=%.2f unpack-ms=%.2f ok=%u/3",
              decMs, unpMs, okCount);
    ProfileRecord(line);
    c->rax = (okCount == 3) ? 1u : 6u;
}

// Opportunity 4B: Use MHW's native silent flag (op+0xC = 1) on aLogo state 5 op1
// so "Loading save data..." does not open uGUICommonMessageWindow (error dialogs
// in op1 cases 3/5/9 ignore op+0xC and still display normally).
bool TryApplySilentLogoSaveLoad(uintptr_t logo, uint32_t logoState) {
    if (!g_cfg.enabled || g_cfg.title_screen_only || g_startupLogoDone || g_titleContinued) return false;
    if (logoState != 5 || !logo || !Readable(logo + 0xA8, 0x40)) return false;
    const uintptr_t op = logo + 0xA8;
    if (Rd32(op + 8) != 1 || Rd32(op + 0x28) != 0 || Rd32(op + 0xC) != 0) return false;
    if (!Wr32(op + 0xC, 1)) return false;
    if (!g_silentOp1Applied) {
        g_silentOp1Applied = true;
        ProfileRecord("save-silent: enabled native silent flag op1+0xC=1 (error dialogs unaffected)");
    }
    return true;
}

using ContentCheckSaveFn = void (__fastcall*)(uintptr_t);

// Opportunity 5: Skip redundant no-op 11.28 MB op0 re-save in aTitle state 4
// when zero DLC entitlements changed AND every serialized byte in sUserData is
// bit-for-bit identical to the save just loaded in aLogo.
void HandleContentCheckSave(hook::Ctx* c, ContentCheckSaveFn nativeFn) {
    const uintptr_t content = static_cast<uintptr_t>(c->rcx);
    if (g_cfg.enabled && !g_cfg.title_screen_only && !g_titleContinued &&
        g_saveBaseline.valid && content && Readable(content, 0x1130)) {
        const uint8_t dlcChanged = *reinterpret_cast<const uint8_t*>(content + 0x924);
        const uint32_t claimCount = Rd32(content + 0x112C);
        const uintptr_t user = P(g_exeBase + kPSave);
        const SaveBaselineDigest now = ComputeUserDataDigest(user);
        const bool sameSlots = now.valid && now.slots == g_saveBaseline.slots;
        const bool sameHdr = now.valid && now.hdr == g_saveBaseline.hdr;
        const bool sameSys = now.valid && now.sys == g_saveBaseline.sys;
        if (dlcChanged == 0 && claimCount == 0 && sameSlots && sameHdr && sameSys) {
            // Exact mirror of Capcom's native skip branch at 0x14029A1CE.
            Wr32(content + 0x50, 0x17);
            Wr32(content + 0xC, 0);
            ProfileRecord("content-skip: save bit-identical (0 dirty bytes); skipped redundant 11MB op0 re-save");
            return;
        }
        char line[240];
        sprintf_s(line, "content-skip: native op0 retained (dlc=%u claims=%u same=%u/%u/%u)",
                  static_cast<unsigned>(dlcChanged), claimCount,
                  sameSlots ? 1u : 0u, sameHdr ? 1u : 0u, sameSys ? 1u : 0u);
        ProfileRecord(line);
    }
    nativeFn(content);
}

void OnContentCheckSave(hook::Ctx* c) {
    HandleContentCheckSave(c, reinterpret_cast<ContentCheckSaveFn>(g_exeBase + kContentCheckSaveTargetRva));
}

void InstallSaveFastHooks() {
    if (g_saveFastHooksTried || !g_cfg.enabled || g_cfg.title_screen_only) return;
    g_saveFastHooksTried = true;

    const uintptr_t unpackSite = g_exeBase + kUserDataUnpackRva;
    if (Readable(unpackSite, sizeof(kUserDataUnpackExpected)) &&
        memcmp(reinterpret_cast<const void*>(unpackSite), kUserDataUnpackExpected,
               sizeof(kUserDataUnpackExpected)) == 0) {
        const unsigned char retByte[1] = {0xC3};
        g_saveParallelHookInstalled = hook::InstallCtxHook(
            unpackSite, sizeof(kUserDataUnpackExpected), &OnUserDataUnpack, retByte, sizeof(retByte));
    }

    const uintptr_t callSite = g_exeBase + kContentCheckSaveCallRva;
    const uintptr_t callTarget = g_exeBase + kContentCheckSaveTargetRva;
    if (Readable(callSite, 5) && *reinterpret_cast<const uint8_t*>(callSite) == 0xE8) {
        int32_t rel = 0;
        memcpy(&rel, reinterpret_cast<const void*>(callSite + 1), 4);
        if (static_cast<int64_t>(callSite + 5) + rel == static_cast<int64_t>(callTarget)) {
            const unsigned char noCall[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
            g_contentSkipHookInstalled = hook::InstallCtxHook(
                callSite, 5, &OnContentCheckSave, noCall, 5);
        }
    }

    LogLine("save-fast: parallel-unpack=%u content-noop-skip=%u",
            g_saveParallelHookInstalled ? 1u : 0u, g_contentSkipHookInstalled ? 1u : 0u);
}
