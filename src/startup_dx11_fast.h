// Included inside the implementation namespace after startup_save_fast.h.
// DX11 Startup Accelerations:
//   9A: Skip wasted DX11 PSOCache + PSOCacheBackground sync loads (0x141AED52D).
//   9B: Lazy + 1-thread background DX11 ShaderPackage compilation (0x1423D7655 +
//       0x14259A030 SetShaders + 0x1423D4DD0 sShader teardown): allocates native
//       0x20-byte DX11 wrappers immediately on the main thread (~2 ms), compiles
//       all ~8,912 shaders on 1 background thread during the ~4.5 s logo/title
//       window (avoiding multi-thread D3D11 driver lock contention), and compiles
//       on-demand in SetShaders for the ~20 UI shaders drawn before completion.
//   9C: Lazy DX11 ID3D11Predicate pool (0x1423A47F1 initForD3D11 + 0x1423A2B2D
//       predicate allocator): skips 32,768 upfront CreatePredicate calls and
//       creates predicates on first use per (threadSlot, index) cell; native
//       shutdown (0x1423A7450) already null-checks every slot before Release().

constexpr uintptr_t kDx11PsoLoadCallRva = 0x1AED52D;
constexpr uintptr_t kDx11PsoLoadTargetRva = 0x23AD040;

constexpr uintptr_t kDx11PredInitRva = 0x23A47F1;
constexpr unsigned char kDx11PredInitExpected[7] = {0xC7, 0x45, 0x8C, 0x01, 0x00, 0x00, 0x00};

constexpr uintptr_t kDx11PredAllocRva = 0x23A2B29;
constexpr unsigned char kDx11PredAllocExpected[8] = {0x48, 0x8B, 0x84, 0xC3, 0x88, 0x16, 0x00, 0x00};

constexpr uintptr_t kDx11ShaderBuildCallRva = 0x23D7655;
constexpr uintptr_t kDx11ShaderBuildTargetRva = 0x23D4B90;

constexpr uintptr_t kDx11SetShadersRva = 0x259A030;
constexpr unsigned char kDx11SetShadersExpected[9] = {0x40, 0x53, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30};

constexpr uintptr_t kDx11ShaderCleanupRva = 0x23D4DD0;
constexpr unsigned char kDx11ShaderCleanupExpected[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};

constexpr uintptr_t kDx11ShaderWrapperVftRva = 0x357C680;
constexpr uintptr_t kDx11WrapperAllocRva = 0x25539F0;
constexpr uintptr_t kDx11WrapperInitRva = 0x2552660;
constexpr uintptr_t kDx11IsD3D11Rva = 0x24E9620;
constexpr uintptr_t kDx11IsD3D12Rva = 0x24E9630;
constexpr uint32_t kDx11LazyTagMagic = 0x5A110000u;

struct Dx11LazyShaderEntry {
    uint64_t* wrapper = nullptr;
    size_t codeOffset = 0;
    uint32_t size = 0;
    uint8_t stage = 0;
    std::atomic<uint8_t> state{0}; // 0=pending, 1=compiling, 2=done

    Dx11LazyShaderEntry() = default;
    Dx11LazyShaderEntry(uint64_t* w, size_t off, uint32_t sz, uint8_t st)
        : wrapper(w), codeOffset(off), size(sz), stage(st), state(0) {}
    Dx11LazyShaderEntry(const Dx11LazyShaderEntry& o)
        : wrapper(o.wrapper), codeOffset(o.codeOffset), size(o.size),
          stage(o.stage), state(o.state.load(std::memory_order_relaxed)) {}
};

bool g_dx11FastHooksTried = false;
bool g_dx11PsoHookInstalled = false;
bool g_dx11PredInitHookInstalled = false;
bool g_dx11PredAllocHookInstalled = false;
bool g_dx11ShaderBuildHookInstalled = false;
bool g_dx11SetShadersHookInstalled = false;
bool g_dx11ShaderCleanupHookInstalled = false;

std::atomic<uint32_t> g_dx11PredCreated{0};

std::vector<uint8_t> g_dx11BytecodeArena;
std::vector<Dx11LazyShaderEntry> g_dx11LazyShaders;
std::atomic<uint32_t> g_dx11LazyPending{0};
std::atomic<uint32_t> g_dx11OnDemandCount{0};
std::atomic<uint32_t> g_dx11OnDemandWaiters{0};
std::atomic<bool> g_dx11LazyStop{false};
SRWLOCK g_dx11CreateLock = SRWLOCK_INIT;
SRWLOCK g_dx11StateLock = SRWLOCK_INIT;
CONDITION_VARIABLE g_dx11StateCv = CONDITION_VARIABLE_INIT;
HANDLE g_dx11BgThread = nullptr;

using PsoLoadFn = void (__fastcall*)(uintptr_t);
using IsD3D12Fn = uint8_t (__fastcall*)();
using IsD3D11Fn = uint8_t (__fastcall*)();
using ShaderBuildFn = void (__fastcall*)(uintptr_t, uint32_t, uint32_t);
using WrapperAllocFn = void* (__fastcall*)(size_t);
using WrapperInitFn = void* (__fastcall*)(void*);
using CreateDx11ShaderFn = void* (__fastcall*)(const void*, uint32_t, const char*);

void HandleDx11PsoLoad(hook::Ctx* c, IsD3D12Fn isD3D12, PsoLoadFn nativePsoLoad) {
    if (g_cfg.enabled && !g_cfg.title_screen_only && g_cfg.dx11_fast && isD3D12 && isD3D12() == 0) {
        ProfileRecord("dx11-pso-skip: skipped wasted PSOCache + PSOCacheBackground sync loads on DX11");
        return;
    }
    nativePsoLoad(static_cast<uintptr_t>(c->rcx));
}

void OnDx11PsoLoad(hook::Ctx* c) {
    HandleDx11PsoLoad(c,
                      reinterpret_cast<IsD3D12Fn>(g_exeBase + kDx11IsD3D12Rva),
                      reinterpret_cast<PsoLoadFn>(g_exeBase + kDx11PsoLoadTargetRva));
}

void HandleDx11PredicateInit(hook::Ctx* c) {
    if (!g_cfg.enabled || g_cfg.title_screen_only || !g_cfg.dx11_fast) return;
    if (c->rsi == 0 || c->rbx != c->rsi + 0x1688 || c->r15 != c->rsi + 0x41688) return;
    std::memset(reinterpret_cast<void*>(c->rbx), 0, 0x40000);
    c->rbx = c->r15;
    g_dx11PredCreated.store(0, std::memory_order_relaxed);
    ProfileRecord("dx11-pred-lazy: zeroed 32768 predicate slots (skipped upfront CreatePredicate)");
}

void OnDx11PredicateInit(hook::Ctx* c) {
    HandleDx11PredicateInit(c);
}

void OnDx11PredicateAlloc(hook::Ctx* c) {
    if (c->rbx == 0 || c->rax >= 0x8000) return;
    void* volatile* slot = reinterpret_cast<void* volatile*>(c->rbx + 0x1688 + c->rax * 8);
    if (*slot != nullptr) return;

    void* dev = *reinterpret_cast<void* const*>(c->rbx + 0x13F8);
    if (!dev) return;

    struct QueryDesc {
        uint32_t query;
        uint32_t miscFlags;
    };
    const QueryDesc desc{5u, 1u}; // D3D11_QUERY_OCCLUSION_PREDICATE, D3D11_QUERY_MISC_PREDICATEHINT
    using CreatePredicateFn = int32_t (__stdcall*)(void*, const QueryDesc*, void**);
    auto vtbl = *reinterpret_cast<uintptr_t* const*>(dev);
    auto createPred = reinterpret_cast<CreatePredicateFn>(vtbl[25]); // offset 0xC8 = 200
    void* pred = nullptr;
    if (createPred(dev, &desc, &pred) >= 0 && pred != nullptr) {
        if (::InterlockedCompareExchangePointer(const_cast<PVOID*>(reinterpret_cast<PVOID volatile*>(slot)),
                                                pred, nullptr) != nullptr) {
            using ReleaseFn = uint32_t (__stdcall*)(void*);
            auto pvtbl = *reinterpret_cast<uintptr_t* const*>(pred);
            reinterpret_cast<ReleaseFn>(pvtbl[2])(pred);
        } else {
            g_dx11PredCreated.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void BuildDx11UsedShaderMask(uintptr_t rShader, uint32_t totalShaders, std::vector<uint8_t>& usedByDx11Tech) {
    usedByDx11Tech.assign(totalShaders, 0);
    if (!rShader || totalShaders == 0) return;
    const uint32_t techCount = *reinterpret_cast<const uint32_t*>(rShader + 0xF4);
    const uintptr_t techArr = *reinterpret_cast<const uintptr_t*>(rShader + 0xB8);
    if (!techArr || techCount == 0) return;

    for (uint32_t t = 0; t < techCount; ++t) {
        const uintptr_t tech = techArr + static_cast<uintptr_t>(t) * 0x20;
        const char* techName = *reinterpret_cast<const char* const*>(tech);
        if (techName && std::strstr(techName, "__d3d12_binary") != nullptr) continue;
        for (uint32_t s = 0; s < 6; ++s) {
            const uint16_t idx = *reinterpret_cast<const uint16_t*>(tech + 0x10 + s * 2);
            if (idx < totalShaders) usedByDx11Tech[idx] = 1;
        }
    }
}

void CompileOneDx11LazyShader(Dx11LazyShaderEntry& e, bool isOnDemand) {
    static constexpr uintptr_t kCreateRvas[6] = {
        0x2551B20, // 0: CreateVertexShader
        0x2551AD0, // 1: CreatePixelShader
        0x2551A30, // 2: CreateGeometryShader
        0x2551A80, // 3: CreateHullShader
        0x25519E0, // 4: CreateDomainShader
        0x2551990, // 5: CreateComputeShader
    };

    if (isOnDemand) g_dx11OnDemandWaiters.fetch_add(1, std::memory_order_acq_rel);
    ::AcquireSRWLockExclusive(&g_dx11CreateLock);
    if (isOnDemand) g_dx11OnDemandWaiters.fetch_sub(1, std::memory_order_acq_rel);

    void* obj = nullptr;
    if (!g_dx11LazyStop.load(std::memory_order_acquire) &&
        e.size > 0 && e.stage < 6 && e.codeOffset + e.size <= g_dx11BytecodeArena.size()) {
        auto createFn = reinterpret_cast<CreateDx11ShaderFn>(g_exeBase + kCreateRvas[e.stage]);
        obj = createFn(g_dx11BytecodeArena.data() + e.codeOffset, e.size, nullptr);
    }
    ::ReleaseSRWLockExclusive(&g_dx11CreateLock);

    ::InterlockedExchangePointer(reinterpret_cast<PVOID*>(e.wrapper + 2), obj);

    ::AcquireSRWLockExclusive(&g_dx11StateLock);
    e.state.store(2, std::memory_order_release);
    ::ReleaseSRWLockExclusive(&g_dx11StateLock);
    ::WakeAllConditionVariable(&g_dx11StateCv);

    g_dx11LazyPending.fetch_sub(1, std::memory_order_acq_rel);
}

void EnsureDx11LazyShaderCompiled(uint64_t* wrapper) {
    if (!wrapper) return;
    if (*reinterpret_cast<void* const volatile*>(wrapper + 2) != nullptr) return;
    const uint32_t tag = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(wrapper) + 0x1C);
    if ((tag & 0xFFFF0000u) != kDx11LazyTagMagic) return;
    const uint32_t idx = tag & 0xFFFFu;
    if (idx >= g_dx11LazyShaders.size()) return;
    Dx11LazyShaderEntry& e = g_dx11LazyShaders[idx];
    if (e.wrapper != wrapper) return;
    if (e.state.load(std::memory_order_acquire) == 2) return;

    uint8_t expected = 0;
    if (e.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        CompileOneDx11LazyShader(e, true);
        g_dx11OnDemandCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    ::AcquireSRWLockExclusive(&g_dx11StateLock);
    while (e.state.load(std::memory_order_acquire) != 2) {
        ::SleepConditionVariableSRW(&g_dx11StateCv, &g_dx11StateLock, INFINITE, 0);
    }
    ::ReleaseSRWLockExclusive(&g_dx11StateLock);
}

DWORD WINAPI Dx11ShaderBgThread(LPVOID) {
    LARGE_INTEGER freq{}, t0{}, t1{};
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&t0);

    const uint32_t total = static_cast<uint32_t>(g_dx11LazyShaders.size());
    uint32_t bgCompiled = 0;
    for (uint32_t i = 0; i < total; ++i) {
        if (g_dx11LazyStop.load(std::memory_order_acquire)) break;
        while (g_dx11OnDemandWaiters.load(std::memory_order_acquire) > 0 &&
               !g_dx11LazyStop.load(std::memory_order_acquire)) {
            ::SwitchToThread();
        }
        Dx11LazyShaderEntry& e = g_dx11LazyShaders[i];
        uint8_t expected = 0;
        if (e.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            CompileOneDx11LazyShader(e, false);
            ++bgCompiled;
        }
    }

    // Wait until any concurrent on-demand compilation has finished before freeing bytecode arena.
    while (g_dx11LazyPending.load(std::memory_order_acquire) > 0 &&
           !g_dx11LazyStop.load(std::memory_order_acquire)) {
        ::SwitchToThread();
    }
    if (!g_dx11LazyStop.load(std::memory_order_acquire)) {
        std::vector<uint8_t>().swap(g_dx11BytecodeArena);
    }

    ::QueryPerformanceCounter(&t1);
    const double ms = freq.QuadPart ? (static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) : 0.0;
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "dx11-shader-bg: done total=%u bg=%u ondemand=%u preds=%u ms=%.2f",
                  total, bgCompiled, g_dx11OnDemandCount.load(std::memory_order_relaxed),
                  g_dx11PredCreated.load(std::memory_order_relaxed), ms);
    ProfileRecord(msg);
    return 0;
}

void StopDx11ShaderBgThread() {
    g_dx11LazyStop.store(true, std::memory_order_release);
    if (g_dx11BgThread) {
        ::WaitForSingleObject(g_dx11BgThread, INFINITE);
        ::CloseHandle(g_dx11BgThread);
        g_dx11BgThread = nullptr;
    }
    g_dx11LazyPending.store(0, std::memory_order_release);
    g_dx11LazyShaders.clear();
    std::vector<uint8_t>().swap(g_dx11BytecodeArena);
}

void OnDx11SetShaders(hook::Ctx* c) {
    if (g_dx11LazyPending.load(std::memory_order_acquire) == 0) return;
    if (c->rdx == 0 || c->r8 == 0) return;
    if (*reinterpret_cast<const uintptr_t*>(c->rdx + 0x20) == c->r8) return;

    const uintptr_t techPair = *reinterpret_cast<const uintptr_t*>(c->r8 + 0x08);
    if (!techPair) return;
    const uintptr_t tech = *reinterpret_cast<const uintptr_t*>(techPair);
    if (!tech) return;

    const uint32_t drawFlags = *reinterpret_cast<const uint32_t*>(c->r8);
    for (uint32_t s = 0; s < 6; ++s) {
        if (s == 1 && (drawFlags & 2u) != 0) continue;
        auto* wrapper = *reinterpret_cast<uint64_t* const*>(tech + 0x50 + static_cast<uintptr_t>(s) * 8);
        if (wrapper && *reinterpret_cast<void* const volatile*>(wrapper + 2) == nullptr) {
            EnsureDx11LazyShaderCompiled(wrapper);
        }
    }
}

void OnDx11ShaderCleanup(hook::Ctx*) {
    if (g_dx11BgThread || !g_dx11LazyShaders.empty()) {
        ProfileRecord("dx11-shader-cleanup: joining background compiler thread");
        StopDx11ShaderBgThread();
    }
}

void OnDx11ShaderBuild(hook::Ctx* c) {
    auto nativeBuild = reinterpret_cast<ShaderBuildFn>(g_exeBase + kDx11ShaderBuildTargetRva);
    const uintptr_t sShader = static_cast<uintptr_t>(c->rcx);
    const uint32_t startIdx = static_cast<uint32_t>(c->rdx);
    const uint32_t endIdx = static_cast<uint32_t>(c->r8);

    auto isD3D11 = reinterpret_cast<IsD3D11Fn>(g_exeBase + kDx11IsD3D11Rva);
    if (!g_cfg.enabled || g_cfg.title_screen_only || !g_cfg.dx11_fast || !isD3D11 || isD3D11() == 0 ||
        endIdx <= startIdx || (endIdx - startIdx) < 64 || !g_dx11LazyShaders.empty()) {
        nativeBuild(sShader, startIdx, endIdx);
        return;
    }

    const uintptr_t rShader = *reinterpret_cast<const uintptr_t*>(sShader + 0x38);
    const uintptr_t outArr = *reinterpret_cast<const uintptr_t*>(sShader + 0x2448);
    if (!rShader || !outArr) {
        nativeBuild(sShader, startIdx, endIdx);
        return;
    }
    const uint32_t totalShaders = *reinterpret_cast<const uint32_t*>(rShader + 0xF8);
    const uintptr_t shaderArr = *reinterpret_cast<const uintptr_t*>(rShader + 0xC0);
    if (!shaderArr || totalShaders == 0 || endIdx > totalShaders || totalShaders >= 0xFFFFu) {
        nativeBuild(sShader, startIdx, endIdx);
        return;
    }

    LARGE_INTEGER freq{}, t0{}, t1{};
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&t0);

    std::vector<uint8_t> usedByDx11Tech;
    BuildDx11UsedShaderMask(rShader, totalShaders, usedByDx11Tech);

    auto allocWrapper = reinterpret_cast<WrapperAllocFn>(g_exeBase + kDx11WrapperAllocRva);
    auto initWrapper = reinterpret_cast<WrapperInitFn>(g_exeBase + kDx11WrapperInitRva);
    const uintptr_t dx11Vft = g_exeBase + kDx11ShaderWrapperVftRva;
    auto* outSlots = reinterpret_cast<uintptr_t*>(outArr);

    g_dx11LazyShaders.clear();
    g_dx11LazyShaders.reserve(endIdx - startIdx);
    g_dx11BytecodeArena.clear();
    size_t totalBytes = 0;
    for (uint32_t i = startIdx; i < endIdx; ++i) {
        const uintptr_t entry = shaderArr + static_cast<uintptr_t>(i) * 0x50;
        const char* name = *reinterpret_cast<const char* const*>(entry);
        if (name && std::strstr(name, "__d3d12_binary") != nullptr && !usedByDx11Tech[i]) continue;
        const uint32_t stage = *reinterpret_cast<const uint32_t*>(entry + 0x1C) & 0xFu;
        if (stage > 5u) continue;
        const void* bytecode = *reinterpret_cast<const void* const*>(entry + 0x10);
        const uint32_t codeSize = *reinterpret_cast<const uint32_t*>(entry + 0x18);
        if (bytecode && codeSize > 0) {
            totalBytes += (static_cast<size_t>(codeSize) + 15u) & ~size_t(15u);
        }
    }
    g_dx11BytecodeArena.reserve(totalBytes);
    uint32_t skippedD3d12 = 0;

    for (uint32_t i = startIdx; i < endIdx; ++i) {
        const uintptr_t entry = shaderArr + static_cast<uintptr_t>(i) * 0x50;
        const char* name = *reinterpret_cast<const char* const*>(entry);
        const bool isD3d12Name = (name != nullptr && std::strstr(name, "__d3d12_binary") != nullptr);
        if (isD3d12Name && !usedByDx11Tech[i]) {
            outSlots[i] = 0;
            ++skippedD3d12;
            continue;
        }
        const uint32_t stage = *reinterpret_cast<const uint32_t*>(entry + 0x1C) & 0xFu;
        if (stage > 5u) continue;
        const void* bytecode = *reinterpret_cast<const void* const*>(entry + 0x10);
        const uint32_t codeSize = *reinterpret_cast<const uint32_t*>(entry + 0x18);
        if (!bytecode || codeSize == 0) {
            outSlots[i] = 0;
            continue;
        }
        auto* w = static_cast<uint64_t*>(allocWrapper(0x20));
        if (!w) {
            outSlots[i] = 0;
            continue;
        }
        initWrapper(w);
        w[0] = dx11Vft;
        w[2] = 0;
        *reinterpret_cast<uint8_t*>(w + 3) = 0;
        const size_t alignedOff = (g_dx11BytecodeArena.size() + 15u) & ~size_t(15u);
        g_dx11BytecodeArena.resize(alignedOff + codeSize);
        std::memcpy(g_dx11BytecodeArena.data() + alignedOff, bytecode, codeSize);
        const uint32_t lazyIdx = static_cast<uint32_t>(g_dx11LazyShaders.size());
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(w) + 0x1C) = kDx11LazyTagMagic | lazyIdx;
        g_dx11LazyShaders.emplace_back(w, alignedOff, codeSize, static_cast<uint8_t>(stage));
        outSlots[i] = reinterpret_cast<uintptr_t>(w);
    }

    const uint32_t deferredCount = static_cast<uint32_t>(g_dx11LazyShaders.size());
    g_dx11LazyStop.store(false, std::memory_order_release);
    g_dx11OnDemandCount.store(0, std::memory_order_release);
    g_dx11OnDemandWaiters.store(0, std::memory_order_release);
    g_dx11LazyPending.store(deferredCount, std::memory_order_release);

    if (deferredCount > 0) {
        g_dx11BgThread = ::CreateThread(nullptr, 0, &Dx11ShaderBgThread, nullptr, 0, nullptr);
        if (!g_dx11BgThread) {
            // Fallback: compile synchronously if CreateThread failed.
            for (uint32_t i = 0; i < deferredCount; ++i) {
                uint8_t exp = 0;
                if (g_dx11LazyShaders[i].state.compare_exchange_strong(exp, 1, std::memory_order_acq_rel)) {
                    CompileOneDx11LazyShader(g_dx11LazyShaders[i], false);
                }
            }
        }
    }

    ::QueryPerformanceCounter(&t1);
    const double ms = freq.QuadPart ? (static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) : 0.0;
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "dx11-shader-lazy: deferred=%u skippedD3d12=%u total=%u bytes=%zu setupMs=%.2f bgThread=%u",
                  deferredCount, skippedD3d12, endIdx - startIdx, g_dx11BytecodeArena.size(), ms,
                  g_dx11BgThread ? 1u : 0u);
    ProfileRecord(msg);
}

void InstallDx11FastHooks() {
    if (g_dx11FastHooksTried || !g_cfg.enabled || g_cfg.title_screen_only) return;
    g_dx11FastHooksTried = true;
    if (!g_cfg.dx11_fast) {
        LogLine("dx11-fast: disabled by config (dx11_fast=0)");
        return;
    }

    const unsigned char noCall[5] = {0x90, 0x90, 0x90, 0x90, 0x90};

    // 9A: Skip wasted PSOCache + PSOCacheBackground sync loads on DX11.
    const uintptr_t psoSite = g_exeBase + kDx11PsoLoadCallRva;
    const uintptr_t psoTarget = g_exeBase + kDx11PsoLoadTargetRva;
    if (Readable(psoSite, 5) && *reinterpret_cast<const uint8_t*>(psoSite) == 0xE8) {
        int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(psoSite + 1), 4);
        if (static_cast<int64_t>(psoSite + 5) + rel == static_cast<int64_t>(psoTarget)) {
            g_dx11PsoHookInstalled = hook::InstallCtxHook(psoSite, 5, &OnDx11PsoLoad, noCall, 5);
        }
    }

    // 9C: Lazy DX11 ID3D11Predicate pool (install allocator BEFORE skipping upfront loop!).
    const uintptr_t predAllocSite = g_exeBase + kDx11PredAllocRva;
    const uintptr_t predInitSite = g_exeBase + kDx11PredInitRva;
    if (Readable(predAllocSite, sizeof(kDx11PredAllocExpected)) &&
        std::memcmp(reinterpret_cast<const void*>(predAllocSite), kDx11PredAllocExpected, sizeof(kDx11PredAllocExpected)) == 0 &&
        Readable(predInitSite, sizeof(kDx11PredInitExpected)) &&
        std::memcmp(reinterpret_cast<const void*>(predInitSite), kDx11PredInitExpected, sizeof(kDx11PredInitExpected)) == 0) {
        g_dx11PredAllocHookInstalled = hook::InstallCtxHook(
            predAllocSite, sizeof(kDx11PredAllocExpected), &OnDx11PredicateAlloc,
            kDx11PredAllocExpected, sizeof(kDx11PredAllocExpected));
        if (g_dx11PredAllocHookInstalled) {
            g_dx11PredInitHookInstalled = hook::InstallCtxHook(
                predInitSite, sizeof(kDx11PredInitExpected), &OnDx11PredicateInit,
                kDx11PredInitExpected, sizeof(kDx11PredInitExpected));
        }
    }

    // 9B: Lazy + 1-thread background DX11 shader compilation (install SetShaders + cleanup FIRST!).
    const uintptr_t setShadersSite = g_exeBase + kDx11SetShadersRva;
    const uintptr_t cleanupSite = g_exeBase + kDx11ShaderCleanupRva;
    const uintptr_t buildSite = g_exeBase + kDx11ShaderBuildCallRva;
    const uintptr_t buildTarget = g_exeBase + kDx11ShaderBuildTargetRva;
    if (Readable(setShadersSite, sizeof(kDx11SetShadersExpected)) &&
        std::memcmp(reinterpret_cast<const void*>(setShadersSite), kDx11SetShadersExpected, sizeof(kDx11SetShadersExpected)) == 0 &&
        Readable(cleanupSite, sizeof(kDx11ShaderCleanupExpected)) &&
        std::memcmp(reinterpret_cast<const void*>(cleanupSite), kDx11ShaderCleanupExpected, sizeof(kDx11ShaderCleanupExpected)) == 0 &&
        Readable(buildSite, 5) && *reinterpret_cast<const uint8_t*>(buildSite) == 0xE8) {
        int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(buildSite + 1), 4);
        if (static_cast<int64_t>(buildSite + 5) + rel == static_cast<int64_t>(buildTarget)) {
            g_dx11SetShadersHookInstalled = hook::InstallCtxHook(
                setShadersSite, sizeof(kDx11SetShadersExpected), &OnDx11SetShaders,
                kDx11SetShadersExpected, sizeof(kDx11SetShadersExpected));
            g_dx11ShaderCleanupHookInstalled = hook::InstallCtxHook(
                cleanupSite, sizeof(kDx11ShaderCleanupExpected), &OnDx11ShaderCleanup,
                kDx11ShaderCleanupExpected, sizeof(kDx11ShaderCleanupExpected));
            if (g_dx11SetShadersHookInstalled && g_dx11ShaderCleanupHookInstalled) {
                g_dx11ShaderBuildHookInstalled = hook::InstallCtxHook(
                    buildSite, 5, &OnDx11ShaderBuild, noCall, 5);
            }
        }
    }

    LogLine("dx11-fast: pso-skip=%u pred-lazy=%u/%u shader-lazy=%u/%u/%u",
            g_dx11PsoHookInstalled ? 1u : 0u,
            g_dx11PredAllocHookInstalled ? 1u : 0u,
            g_dx11PredInitHookInstalled ? 1u : 0u,
            g_dx11SetShadersHookInstalled ? 1u : 0u,
            g_dx11ShaderCleanupHookInstalled ? 1u : 0u,
            g_dx11ShaderBuildHookInstalled ? 1u : 0u);
}
