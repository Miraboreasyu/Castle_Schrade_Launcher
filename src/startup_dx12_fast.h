// Included inside the implementation namespace after startup_dx11_fast.h.
// DX12 Placed-Heap Sub-Allocators (Textures + Buffers):
//   Live DX12 profiling revealed that 3,650 calls to ID3D12Device::CreateCommittedResource
//   in FUN_1424E7A10 (0x1424E82AF) for textures > 48 KB cost ~1,687 ms total (~0.46 ms per
//   WDDM kernel allocation: 652 ms pre-0x11, 203 ms playable, 566 ms village, 265 ms stagePrep),
//   and DX12 index/vertex/shader buffers (FUN_1424E6540 / FUN_1424E87C0 / FUN_1424E6E50)
//   likewise call CreateCommittedResource individually.
//
//   We reuse a single 64 MB ID3D12Heap sub-allocator template for two Tier-1-compatible pools:
//     1) Texture Pool (D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES):
//        - Vtable slot 27 (ID3D12Device::CreateCommittedResource) when _ReturnAddress() is
//          g_exeBase + 0x24E82B5 (FUN_1424E7A10): sub-allocate DEFAULT-heap non-RT/DS/MSAA textures
//          <= 32 MB via CreatePlacedResource; store bit-63-tagged handle in param_1[5] so
//          mpResidencyManager automatically skips individual MakeResident/Evict calls.
//        - Hook 0x1424E9286 (~Texture12): balance global VRAM counter + reclaim via 8-frame GPU quarantine.
//     2) Buffer Pool (D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS):
//        - Vtable slot 27 when _ReturnAddress() is g_exeBase + 0x24E672B (IB), 0x24E89AB (VB), or
//          0x24E6FA7 (SB): sub-allocate DEFAULT-heap buffers <= 32 MB via CreatePlacedResource; leave
//          param_1[5] == 0 so SRV/UAV/upload paths remain 100% native, clear residency flag byte
//          [param_1+0x0C] = 0 so mpResidencyManager skips them, and track ID3D12Resource* in side map.
//        - Hook 0x14255397F (FUN_1425538E0): reclaim placed buffer handles via 8-frame quarantine.
//   Note: Hooking ID3D12Device vtable slot 27 instead of inline-patching inside CreateTextureD3D12
//   or CreateShaderBufferD3D12 leaves all hashed function bodies unmodified for mhw-dlss-update.

constexpr uintptr_t kDx12TexFreeCallRva = 0x24E9286;
constexpr uintptr_t kDx12TexFreeTargetRva = 0x2627790;
constexpr uintptr_t kDx12TexMemStatRva = 0x51CCDC0;

constexpr uintptr_t kDx12BufReleaseRva = 0x255397F;
constexpr unsigned char kDx12BufReleaseExpected[7] = {0x48, 0x8B, 0x4B, 0x20, 0x48, 0x85, 0xC9};

constexpr uintptr_t kDx12RenderInstanceRva = 0x51C7050;
constexpr size_t kDx12CreateCommittedSlot = 27;

// Return addresses immediately after `call qword ptr [rax + 0xD8]` inside native constructors.
constexpr uintptr_t kDx12TexCreateRetRva = 0x24E82B5;
constexpr uintptr_t kDx12IbCreateRetRva  = 0x24E672B;
constexpr uintptr_t kDx12VbCreateRetRva  = 0x24E89AB;
constexpr uintptr_t kDx12SbCreateRetRva  = 0x24E6FA7;

// Lazy-attach factory trigger prologues (outside all mhw-dlss-update code gates).
constexpr uintptr_t kDx12TexFactoryRva = 0x26237C0;
constexpr unsigned char kDx12TexFactoryExpected[6] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
constexpr uintptr_t kDx12SbFactoryRva = 0x26235B0;
constexpr uintptr_t kDx12IbFactoryRva = 0x26232A0;
constexpr uintptr_t kDx12VbFactoryRva = 0x2623900;
constexpr unsigned char kDx12BufFactoryExpected[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr uint64_t kDx12PlacedTagBit = 0x8000000000000000ULL;
constexpr uint32_t kDx12HeapUnitShift = 16; // 64 KB units (D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
constexpr uint32_t kDx12HeapUnitBytes = 1u << kDx12HeapUnitShift;
constexpr uint32_t kDx12HeapUnits = 1024;   // 64 MB per ID3D12Heap
constexpr uint64_t kDx12HeapSizeBytes = static_cast<uint64_t>(kDx12HeapUnits) << kDx12HeapUnitShift;
constexpr uint64_t kDx12MaxPlacedBytes = 32ULL * 1024ULL * 1024ULL;
constexpr uint64_t kDx12QuarantineFrames = 8;

struct Dx12FreeSpan {
    uint32_t off;
    uint32_t len;
};

struct Dx12PlacedHeap {
    ID3D12Heap* heap = nullptr;
    std::vector<Dx12FreeSpan> freeList;
    uint32_t usedUnits = 0;
};

struct Dx12QuarantineBlock {
    uint32_t heapId;
    uint32_t offUnits;
    uint32_t lenUnits;
    uint64_t retireFrame;
};

struct Dx12PlacedHeapPool {
    D3D12_HEAP_FLAGS allowFlags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    SRWLOCK lock = SRWLOCK_INIT;
    ID3D12Device* ownerDev = nullptr;
    std::vector<Dx12PlacedHeap> heaps;
    std::vector<Dx12QuarantineBlock> quarantine;
    size_t quarantineHead = 0;
    bool supportsNotZeroed = true;

    std::atomic<uint32_t> placedCount{0};
    std::atomic<uint32_t> fallbackCount{0};
    std::atomic<uint32_t> heapsCreated{0};
    std::atomic<uint32_t> heapsReleased{0};
    std::atomic<uint32_t> freedCount{0};

    explicit Dx12PlacedHeapPool(D3D12_HEAP_FLAGS flags) : allowFlags(flags) {}

    static void InsertAndCoalesce(std::vector<Dx12FreeSpan>& list, uint32_t off, uint32_t len) {
        size_t pos = 0;
        while (pos < list.size() && list[pos].off < off) ++pos;
        list.insert(list.begin() + static_cast<ptrdiff_t>(pos), Dx12FreeSpan{off, len});
        if (pos + 1 < list.size() && list[pos].off + list[pos].len == list[pos + 1].off) {
            list[pos].len += list[pos + 1].len;
            list.erase(list.begin() + static_cast<ptrdiff_t>(pos + 1));
        }
        if (pos > 0 && list[pos - 1].off + list[pos - 1].len == list[pos].off) {
            list[pos - 1].len += list[pos].len;
            list.erase(list.begin() + static_cast<ptrdiff_t>(pos));
        }
    }

    static bool TakeFromFreeList(std::vector<Dx12FreeSpan>& list, uint32_t units, uint32_t& outOff) {
        size_t bestIdx = list.size();
        uint32_t bestLen = 0xFFFFFFFFu;
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].len >= units && list[i].len < bestLen) {
                bestLen = list[i].len;
                bestIdx = i;
                if (bestLen == units) break;
            }
        }
        if (bestIdx == list.size()) return false;
        outOff = list[bestIdx].off;
        if (list[bestIdx].len == units) {
            list.erase(list.begin() + static_cast<ptrdiff_t>(bestIdx));
        } else {
            list[bestIdx].off += units;
            list[bestIdx].len -= units;
        }
        return true;
    }

    void DrainQuarantineLocked(uint64_t curFrame, std::vector<ID3D12Heap*>& toRelease) {
        while (quarantineHead < quarantine.size() && quarantine[quarantineHead].retireFrame <= curFrame) {
            const Dx12QuarantineBlock q = quarantine[quarantineHead++];
            if (q.heapId < heaps.size() && heaps[q.heapId].heap != nullptr) {
                auto& h = heaps[q.heapId];
                InsertAndCoalesce(h.freeList, q.offUnits, q.lenUnits);
                if (h.usedUnits >= q.lenUnits) h.usedUnits -= q.lenUnits;
                else h.usedUnits = 0;
            }
        }
        if (quarantineHead > 64 && quarantineHead * 2 >= quarantine.size()) {
            quarantine.erase(quarantine.begin(), quarantine.begin() + static_cast<ptrdiff_t>(quarantineHead));
            quarantineHead = 0;
        }
        for (auto& h : heaps) {
            if (h.heap != nullptr && h.usedUnits == 0) {
                toRelease.push_back(h.heap);
                h.heap = nullptr;
                h.freeList.clear();
                heapsReleased.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    bool AllocBlock(ID3D12Device* dev, uint32_t units, uint64_t curFrame,
                    ID3D12Heap*& outHeap, uint32_t& outHeapId, uint32_t& outOffUnits) {
        if (!dev || units == 0 || units > kDx12HeapUnits) return false;
        std::vector<ID3D12Heap*> toRelease;
        ID3D12Heap* pickedHeap = nullptr;
        uint32_t pickedId = 0;
        uint32_t pickedOff = 0;

        ::AcquireSRWLockExclusive(&lock);
        if (ownerDev == nullptr) {
            ownerDev = dev;
        } else if (ownerDev != dev) {
            ::ReleaseSRWLockExclusive(&lock);
            return false;
        }
        DrainQuarantineLocked(curFrame, toRelease);

        for (size_t i = 0; i < heaps.size(); ++i) {
            if (heaps[i].heap != nullptr &&
                (kDx12HeapUnits - heaps[i].usedUnits) >= units &&
                TakeFromFreeList(heaps[i].freeList, units, pickedOff)) {
                heaps[i].usedUnits += units;
                pickedHeap = heaps[i].heap;
                pickedHeap->AddRef();
                pickedId = static_cast<uint32_t>(i);
                break;
            }
        }
        const bool tryNotZeroed = supportsNotZeroed;
        ::ReleaseSRWLockExclusive(&lock);

        for (ID3D12Heap* h : toRelease) h->Release();

        if (!pickedHeap) {
            D3D12_HEAP_DESC hd{};
            hd.SizeInBytes = kDx12HeapSizeBytes;
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            hd.Properties.CreationNodeMask = 1;
            hd.Properties.VisibleNodeMask = 1;
            hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            hd.Flags = tryNotZeroed
                ? static_cast<D3D12_HEAP_FLAGS>(allowFlags | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED)
                : allowFlags;

            ID3D12Heap* newHeap = nullptr;
            HRESULT hr = dev->CreateHeap(&hd, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&newHeap));
            if (FAILED(hr) && tryNotZeroed) {
                hd.Flags = allowFlags;
                hr = dev->CreateHeap(&hd, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&newHeap));
                if (SUCCEEDED(hr)) {
                    ::AcquireSRWLockExclusive(&lock);
                    supportsNotZeroed = false;
                    ::ReleaseSRWLockExclusive(&lock);
                }
            }
            if (FAILED(hr) || !newHeap) return false;
            heapsCreated.fetch_add(1, std::memory_order_relaxed);

            ::AcquireSRWLockExclusive(&lock);
            size_t slot = heaps.size();
            for (size_t i = 0; i < heaps.size(); ++i) {
                if (heaps[i].heap == nullptr) { slot = i; break; }
            }
            if (slot == heaps.size()) {
                if (heaps.size() >= 0xFFFEu) {
                    ::ReleaseSRWLockExclusive(&lock);
                    newHeap->Release();
                    return false;
                }
                heaps.emplace_back();
            }
            auto& h = heaps[slot];
            h.heap = newHeap;
            h.freeList.clear();
            if (units < kDx12HeapUnits) {
                h.freeList.push_back(Dx12FreeSpan{units, kDx12HeapUnits - units});
            }
            h.usedUnits = units;
            pickedHeap = newHeap;
            pickedHeap->AddRef();
            pickedId = static_cast<uint32_t>(slot);
            pickedOff = 0;
            ::ReleaseSRWLockExclusive(&lock);
        }

        outHeap = pickedHeap;
        outHeapId = pickedId;
        outOffUnits = pickedOff;
        return true;
    }

    void ReturnBlockImmediate(uint32_t heapId, uint32_t offUnits, uint32_t lenUnits) {
        ::AcquireSRWLockExclusive(&lock);
        if (heapId < heaps.size() && heaps[heapId].heap != nullptr) {
            InsertAndCoalesce(heaps[heapId].freeList, offUnits, lenUnits);
            if (heaps[heapId].usedUnits >= lenUnits) heaps[heapId].usedUnits -= lenUnits;
            else heaps[heapId].usedUnits = 0;
        }
        ::ReleaseSRWLockExclusive(&lock);
    }

    void QueueFree(uint64_t handle, uint64_t curFrame) {
        const uint64_t payload = handle & ~kDx12PlacedTagBit;
        const uint32_t heapId = static_cast<uint32_t>((payload >> 32) & 0xFFFFu);
        const uint32_t offUnits = static_cast<uint32_t>((payload >> 16) & 0xFFFFu);
        const uint32_t lenUnits = static_cast<uint32_t>(payload & 0xFFFFu);
        if (lenUnits == 0) return;

        std::vector<ID3D12Heap*> toRelease;
        ::AcquireSRWLockExclusive(&lock);
        quarantine.push_back(Dx12QuarantineBlock{heapId, offUnits, lenUnits, curFrame + kDx12QuarantineFrames});
        DrainQuarantineLocked(curFrame, toRelease);
        ::ReleaseSRWLockExclusive(&lock);
        for (ID3D12Heap* h : toRelease) h->Release();
        freedCount.fetch_add(1, std::memory_order_relaxed);
    }

    static uint64_t PackHandle(uint32_t heapId, uint32_t offUnits, uint32_t lenUnits) {
        return kDx12PlacedTagBit |
               (static_cast<uint64_t>(heapId & 0xFFFFu) << 32) |
               (static_cast<uint64_t>(offUnits & 0xFFFFu) << 16) |
               static_cast<uint64_t>(lenUnits & 0xFFFFu);
    }
};

using Dx12PlacedTexPool = Dx12PlacedHeapPool;

Dx12PlacedHeapPool g_dx12TexPool(D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES);
Dx12PlacedHeapPool g_dx12BufPool(D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);

SRWLOCK g_dx12BufMapLock = SRWLOCK_INIT;
std::unordered_map<ID3D12Resource*, uint64_t> g_dx12PlacedBufMap;
std::atomic<uint32_t> g_dx12PlacedBufLive{0};

using CreateCommittedResourceFn = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
    REFIID, void**);

SRWLOCK g_dx12VtableLock = SRWLOCK_INIT;
std::atomic<CreateCommittedResourceFn> g_origDx12CreateCommitted{nullptr};
void** g_dx12HookedVtableSlot = nullptr;
std::atomic<bool> g_dx12VtableHooked{false};

bool g_dx12FastHooksTried = false;
bool g_dx12TexFreeHookInstalled = false;
bool g_dx12BufReleaseHookInstalled = false;
bool g_dx12TexFactoryHookInstalled = false;
bool g_dx12IbFactoryHookInstalled = false;
bool g_dx12VbFactoryHookInstalled = false;
bool g_dx12SbFactoryHookInstalled = false;

using NativeTexSmallFreeFn = void (__fastcall*)(uintptr_t, uint64_t);

uint64_t ReadDx12FrameCounter() {
    if (!g_exeBase || !Readable(g_exeBase + kDx12RenderInstanceRva, 8)) return 0;
    const uintptr_t d3d = P(g_exeBase + kDx12RenderInstanceRva);
    if (!d3d || !Readable(d3d + 0x1E8, 8)) return 0;
    return *reinterpret_cast<const uint64_t*>(d3d + 0x1E8);
}

void OnDx12TexFree(hook::Ctx* c) {
    const uint64_t handle = static_cast<uint64_t>(c->rdx);
    if ((handle & kDx12PlacedTagBit) != 0) {
        const uintptr_t tex = static_cast<uintptr_t>(c->rbx);
        if (tex && Readable(tex + 0x30, 4)) {
            const uint32_t tracked = Rd32(tex + 0x30);
            if (tracked != 0 && g_exeBase && Readable(g_exeBase + kDx12TexMemStatRva, 8)) {
                ::InterlockedExchangeAdd64(
                    reinterpret_cast<volatile LONG64*>(g_exeBase + kDx12TexMemStatRva),
                    -static_cast<LONG64>(tracked));
            }
        }
        g_dx12TexPool.QueueFree(handle, ReadDx12FrameCounter());
        return;
    }
    reinterpret_cast<NativeTexSmallFreeFn>(g_exeBase + kDx12TexFreeTargetRva)(
        static_cast<uintptr_t>(c->rcx), handle);
}

HRESULT DispatchDx12CreateCommitted(
    uintptr_t retAddr, ID3D12Device* dev,
    const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource,
    void** ppvResource, CreateCommittedResourceFn orig) {
    if (!g_cfg.enabled || g_cfg.title_screen_only || !g_cfg.dx12_fast ||
        !dev || !pHeapProperties || !pDesc || !ppvResource ||
        !g_exeBase || retAddr < g_exeBase) {
        return orig(dev, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                    pOptimizedClearValue, riidResource, ppvResource);
    }

    const uintptr_t retRva = retAddr - g_exeBase;
    if (retRva == kDx12TexCreateRetRva) {
        if (!g_dx12TexFreeHookInstalled ||
            riidResource != __uuidof(ID3D12Resource) ||
            HeapFlags != D3D12_HEAP_FLAG_NONE ||
            pHeapProperties->Type != D3D12_HEAP_TYPE_DEFAULT ||
            pHeapProperties->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN ||
            pHeapProperties->MemoryPoolPreference != D3D12_MEMORY_POOL_UNKNOWN ||
            pOptimizedClearValue != nullptr ||
            pDesc->Dimension < D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
            pDesc->Dimension > D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
            pDesc->Flags != D3D12_RESOURCE_FLAG_NONE ||
            pDesc->SampleDesc.Count != 1) {
            g_dx12TexPool.fallbackCount.fetch_add(1, std::memory_order_relaxed);
            return orig(dev, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                        pOptimizedClearValue, riidResource, ppvResource);
        }

        D3D12_RESOURCE_DESC desc = *pDesc;
        desc.Alignment = 0;
        const D3D12_RESOURCE_ALLOCATION_INFO info = dev->GetResourceAllocationInfo(0, 1, &desc);
        if (info.SizeInBytes > 0 && info.SizeInBytes != UINT64_MAX &&
            info.SizeInBytes <= kDx12MaxPlacedBytes &&
            info.Alignment <= kDx12HeapUnitBytes) {
            const uint32_t units = static_cast<uint32_t>((info.SizeInBytes + (kDx12HeapUnitBytes - 1)) >> kDx12HeapUnitShift);
            ID3D12Heap* heap = nullptr;
            uint32_t heapId = 0;
            uint32_t offUnits = 0;
            if (g_dx12TexPool.AllocBlock(dev, units, ReadDx12FrameCounter(), heap, heapId, offUnits)) {
                ID3D12Resource* res = nullptr;
                const HRESULT hr = dev->CreatePlacedResource(
                    heap,
                    static_cast<uint64_t>(offUnits) << kDx12HeapUnitShift,
                    &desc,
                    InitialResourceState,
                    nullptr,
                    riidResource,
                    reinterpret_cast<void**>(&res));
                heap->Release();
                if (SUCCEEDED(hr) && res) {
                    *ppvResource = res;
                    const uintptr_t tex = reinterpret_cast<uintptr_t>(ppvResource) - 0x20;
                    *reinterpret_cast<uint64_t*>(tex + 0x28) =
                        Dx12PlacedHeapPool::PackHandle(heapId, offUnits, units);
                    g_dx12TexPool.placedCount.fetch_add(1, std::memory_order_relaxed);
                    return S_OK;
                }
                g_dx12TexPool.ReturnBlockImmediate(heapId, offUnits, units);
            }
        }
        g_dx12TexPool.fallbackCount.fetch_add(1, std::memory_order_relaxed);
        return orig(dev, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                    pOptimizedClearValue, riidResource, ppvResource);
    }

    if (retRva == kDx12IbCreateRetRva ||
        retRva == kDx12VbCreateRetRva ||
        retRva == kDx12SbCreateRetRva) {
        if (g_dx12BufReleaseHookInstalled &&
            riidResource == __uuidof(ID3D12Resource) &&
            HeapFlags == D3D12_HEAP_FLAG_NONE &&
            pHeapProperties->Type == D3D12_HEAP_TYPE_DEFAULT &&
            pHeapProperties->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
            pHeapProperties->MemoryPoolPreference == D3D12_MEMORY_POOL_UNKNOWN &&
            pOptimizedClearValue == nullptr &&
            pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            pDesc->Width > 0 && pDesc->Width <= kDx12MaxPlacedBytes) {
            D3D12_RESOURCE_DESC desc = *pDesc;
            desc.Alignment = 0;
            const D3D12_RESOURCE_ALLOCATION_INFO info = dev->GetResourceAllocationInfo(0, 1, &desc);
            if (info.SizeInBytes > 0 && info.SizeInBytes != UINT64_MAX &&
                info.SizeInBytes <= kDx12MaxPlacedBytes &&
                info.Alignment <= kDx12HeapUnitBytes) {
                const uint32_t units = static_cast<uint32_t>((info.SizeInBytes + (kDx12HeapUnitBytes - 1)) >> kDx12HeapUnitShift);
                ID3D12Heap* heap = nullptr;
                uint32_t heapId = 0;
                uint32_t offUnits = 0;
                if (g_dx12BufPool.AllocBlock(dev, units, ReadDx12FrameCounter(), heap, heapId, offUnits)) {
                    ID3D12Resource* res = nullptr;
                    const HRESULT hr = dev->CreatePlacedResource(
                        heap,
                        static_cast<uint64_t>(offUnits) << kDx12HeapUnitShift,
                        &desc,
                        InitialResourceState,
                        nullptr,
                        riidResource,
                        reinterpret_cast<void**>(&res));
                    heap->Release();
                    if (SUCCEEDED(hr) && res) {
                        const uint64_t handle = Dx12PlacedHeapPool::PackHandle(heapId, offUnits, units);
                        ::AcquireSRWLockExclusive(&g_dx12BufMapLock);
                        g_dx12PlacedBufMap[res] = handle;
                        ::ReleaseSRWLockExclusive(&g_dx12BufMapLock);
                        g_dx12PlacedBufLive.fetch_add(1, std::memory_order_release);

                        *ppvResource = res;
                        const uintptr_t bufObj = reinterpret_cast<uintptr_t>(ppvResource) - 0x20;
                        // Clear residency-managed bit so mpResidencyManager skips this placed buffer
                        // while param_1[5] stays 0 for native SRV/UAV/upload paths.
                        *reinterpret_cast<uint8_t*>(bufObj + 0x0C) = 0;
                        g_dx12BufPool.placedCount.fetch_add(1, std::memory_order_relaxed);
                        return S_OK;
                    }
                    g_dx12BufPool.ReturnBlockImmediate(heapId, offUnits, units);
                }
            }
            g_dx12BufPool.fallbackCount.fetch_add(1, std::memory_order_relaxed);
        } else if (pHeapProperties->Type == D3D12_HEAP_TYPE_DEFAULT) {
            g_dx12BufPool.fallbackCount.fetch_add(1, std::memory_order_relaxed);
        }
        return orig(dev, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                    pOptimizedClearValue, riidResource, ppvResource);
    }

    return orig(dev, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                pOptimizedClearValue, riidResource, ppvResource);
}

HRESULT STDMETHODCALLTYPE HookedDx12CreateCommittedResource(
    ID3D12Device* self,
    const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riidResource,
    void** ppvResource) {
    const CreateCommittedResourceFn orig = g_origDx12CreateCommitted.load(std::memory_order_acquire);
    if (!orig) return E_FAIL;
    return DispatchDx12CreateCommitted(
        reinterpret_cast<uintptr_t>(_ReturnAddress()),
        self, pHeapProperties, HeapFlags, pDesc,
        InitialResourceState, pOptimizedClearValue, riidResource, ppvResource, orig);
}

bool TryInstallDx12CreateCommittedHook(ID3D12Device* dev = nullptr) {
    if (g_dx12VtableHooked.load(std::memory_order_acquire)) return true;
    if (!dev) {
        if (!g_exeBase || !Readable(g_exeBase + kDx12RenderInstanceRva, 8)) return false;
        const uintptr_t d3d = P(g_exeBase + kDx12RenderInstanceRva);
        if (!d3d || !Readable(d3d + 0x18, 8)) return false;
        dev = *reinterpret_cast<ID3D12Device* const*>(d3d + 0x18);
    }
    if (!dev || !Readable(reinterpret_cast<uintptr_t>(dev), sizeof(void*))) return false;
    void** vtable = *reinterpret_cast<void***>(dev);
    if (!vtable || !Readable(reinterpret_cast<uintptr_t>(&vtable[kDx12CreateCommittedSlot]), sizeof(void*))) {
        return false;
    }

    ::AcquireSRWLockExclusive(&g_dx12VtableLock);
    if (g_dx12VtableHooked.load(std::memory_order_relaxed)) {
        ::ReleaseSRWLockExclusive(&g_dx12VtableLock);
        return true;
    }
    void* cur = vtable[kDx12CreateCommittedSlot];
    if (!cur) {
        ::ReleaseSRWLockExclusive(&g_dx12VtableLock);
        return false;
    }
    if (cur == reinterpret_cast<void*>(&HookedDx12CreateCommittedResource)) {
        g_dx12HookedVtableSlot = &vtable[kDx12CreateCommittedSlot];
        g_dx12VtableHooked.store(true, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&g_dx12VtableLock);
        return true;
    }

    g_origDx12CreateCommitted.store(reinterpret_cast<CreateCommittedResourceFn>(cur), std::memory_order_release);
    DWORD oldProt = 0;
    if (!VirtualProtect(&vtable[kDx12CreateCommittedSlot], sizeof(void*), PAGE_READWRITE, &oldProt)) {
        g_origDx12CreateCommitted.store(nullptr, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&g_dx12VtableLock);
        return false;
    }
    InterlockedExchangePointer(&vtable[kDx12CreateCommittedSlot],
                               reinterpret_cast<void*>(&HookedDx12CreateCommittedResource));
    DWORD ignored = 0;
    VirtualProtect(&vtable[kDx12CreateCommittedSlot], sizeof(void*), oldProt, &ignored);
    g_dx12HookedVtableSlot = &vtable[kDx12CreateCommittedSlot];
    g_dx12VtableHooked.store(true, std::memory_order_release);
    ::ReleaseSRWLockExclusive(&g_dx12VtableLock);

    LogLine("dx12-fast: hooked ID3D12Device::CreateCommittedResource vtable slot %zu (dev=%p orig=%p)",
            kDx12CreateCommittedSlot, dev, cur);
    return true;
}

void OnDx12FactoryTrigger(hook::Ctx*) {
    if (!g_dx12VtableHooked.load(std::memory_order_acquire) &&
        g_cfg.enabled && !g_cfg.title_screen_only && g_cfg.dx12_fast) {
        TryInstallDx12CreateCommittedHook();
    }
}

void OnDx12BufRelease(hook::Ctx* c) {
    if (g_dx12PlacedBufLive.load(std::memory_order_acquire) == 0) return;
    const uintptr_t obj = static_cast<uintptr_t>(c->rbx);
    if (!obj) return;
    auto* res = *reinterpret_cast<ID3D12Resource* const*>(obj + 0x20);
    if (!res) return;

    uint64_t handle = 0;
    ::AcquireSRWLockExclusive(&g_dx12BufMapLock);
    auto it = g_dx12PlacedBufMap.find(res);
    if (it != g_dx12PlacedBufMap.end()) {
        handle = it->second;
        g_dx12PlacedBufMap.erase(it);
    }
    ::ReleaseSRWLockExclusive(&g_dx12BufMapLock);

    if (handle != 0) {
        g_dx12PlacedBufLive.fetch_sub(1, std::memory_order_acq_rel);
        g_dx12BufPool.QueueFree(handle, ReadDx12FrameCounter());
    }
}

void RecordDx12PlacedStats() {
    const uint32_t tPlaced = g_dx12TexPool.placedCount.load(std::memory_order_relaxed);
    const uint32_t bPlaced = g_dx12BufPool.placedCount.load(std::memory_order_relaxed);
    if (tPlaced == 0 && bPlaced == 0 &&
        g_dx12TexPool.fallbackCount.load(std::memory_order_relaxed) == 0) return;
    char msg[300];
    std::snprintf(msg, sizeof(msg),
                  "dx12-placed: tex=%u(fb=%u,heaps=%u,rel=%u,freed=%u) buf=%u(fb=%u,heaps=%u,rel=%u,freed=%u)",
                  tPlaced, g_dx12TexPool.fallbackCount.load(std::memory_order_relaxed),
                  g_dx12TexPool.heapsCreated.load(std::memory_order_relaxed),
                  g_dx12TexPool.heapsReleased.load(std::memory_order_relaxed),
                  g_dx12TexPool.freedCount.load(std::memory_order_relaxed),
                  bPlaced, g_dx12BufPool.fallbackCount.load(std::memory_order_relaxed),
                  g_dx12BufPool.heapsCreated.load(std::memory_order_relaxed),
                  g_dx12BufPool.heapsReleased.load(std::memory_order_relaxed),
                  g_dx12BufPool.freedCount.load(std::memory_order_relaxed));
    ProfileRecord(msg);
}

void InstallDx12FastHooks() {
    if (g_dx12FastHooksTried || !g_cfg.enabled || g_cfg.title_screen_only) return;
    g_dx12FastHooksTried = true;
    if (!g_cfg.dx12_fast) {
        LogLine("dx12-fast: disabled by config (dx12_fast=0)");
        return;
    }

    const unsigned char noCall5[5] = {0x90, 0x90, 0x90, 0x90, 0x90};

    // Texture free-hook (5-byte CALL replacement outside all hashed code gates).
    const uintptr_t freeSite = g_exeBase + kDx12TexFreeCallRva;
    const uintptr_t freeTarget = g_exeBase + kDx12TexFreeTargetRva;
    if (Readable(freeSite, 5) && *reinterpret_cast<const uint8_t*>(freeSite) == 0xE8) {
        int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(freeSite + 1), 4);
        if (static_cast<int64_t>(freeSite + 5) + rel == static_cast<int64_t>(freeTarget)) {
            g_dx12TexFreeHookInstalled = hook::InstallCtxHook(freeSite, 5, &OnDx12TexFree, noCall5, 5);
        }
    }

    // Buffer release-hook (7 bytes inside ReleaseResourceBaseD3D12, outside all hashed code gates).
    const uintptr_t bufRelSite = g_exeBase + kDx12BufReleaseRva;
    if (Readable(bufRelSite, sizeof(kDx12BufReleaseExpected)) &&
        std::memcmp(reinterpret_cast<const void*>(bufRelSite), kDx12BufReleaseExpected, sizeof(kDx12BufReleaseExpected)) == 0) {
        g_dx12BufReleaseHookInstalled = hook::InstallCtxHook(
            bufRelSite, sizeof(kDx12BufReleaseExpected), &OnDx12BufRelease,
            kDx12BufReleaseExpected, sizeof(kDx12BufReleaseExpected));
    }

    bool vtableImmediate = false;
    if (g_dx12TexFreeHookInstalled || g_dx12BufReleaseHookInstalled) {
        vtableImmediate = TryInstallDx12CreateCommittedHook();

        // Install lightweight lazy triggers on the 4 factory wrappers (>4 KB away from any code gate)
        // in case dinput8.dll was preloaded before cD3DRender::initForD3D12 initialized ID3D12Device.
        const uintptr_t texFac = g_exeBase + kDx12TexFactoryRva;
        if (Readable(texFac, sizeof(kDx12TexFactoryExpected)) &&
            std::memcmp(reinterpret_cast<const void*>(texFac), kDx12TexFactoryExpected, sizeof(kDx12TexFactoryExpected)) == 0) {
            g_dx12TexFactoryHookInstalled = hook::InstallCtxHook(
                texFac, sizeof(kDx12TexFactoryExpected), &OnDx12FactoryTrigger,
                kDx12TexFactoryExpected, sizeof(kDx12TexFactoryExpected));
        }
        const uintptr_t ibFac = g_exeBase + kDx12IbFactoryRva;
        if (Readable(ibFac, sizeof(kDx12BufFactoryExpected)) &&
            std::memcmp(reinterpret_cast<const void*>(ibFac), kDx12BufFactoryExpected, sizeof(kDx12BufFactoryExpected)) == 0) {
            g_dx12IbFactoryHookInstalled = hook::InstallCtxHook(
                ibFac, sizeof(kDx12BufFactoryExpected), &OnDx12FactoryTrigger,
                kDx12BufFactoryExpected, sizeof(kDx12BufFactoryExpected));
        }
        const uintptr_t vbFac = g_exeBase + kDx12VbFactoryRva;
        if (Readable(vbFac, sizeof(kDx12BufFactoryExpected)) &&
            std::memcmp(reinterpret_cast<const void*>(vbFac), kDx12BufFactoryExpected, sizeof(kDx12BufFactoryExpected)) == 0) {
            g_dx12VbFactoryHookInstalled = hook::InstallCtxHook(
                vbFac, sizeof(kDx12BufFactoryExpected), &OnDx12FactoryTrigger,
                kDx12BufFactoryExpected, sizeof(kDx12BufFactoryExpected));
        }
        const uintptr_t sbFac = g_exeBase + kDx12SbFactoryRva;
        if (Readable(sbFac, sizeof(kDx12BufFactoryExpected)) &&
            std::memcmp(reinterpret_cast<const void*>(sbFac), kDx12BufFactoryExpected, sizeof(kDx12BufFactoryExpected)) == 0) {
            g_dx12SbFactoryHookInstalled = hook::InstallCtxHook(
                sbFac, sizeof(kDx12BufFactoryExpected), &OnDx12FactoryTrigger,
                kDx12BufFactoryExpected, sizeof(kDx12BufFactoryExpected));
        }
    }

    LogLine("dx12-fast: rel-tex=%u rel-buf=%u triggers=%u/%u/%u/%u vtable=%u",
            g_dx12TexFreeHookInstalled ? 1u : 0u,
            g_dx12BufReleaseHookInstalled ? 1u : 0u,
            g_dx12TexFactoryHookInstalled ? 1u : 0u,
            g_dx12IbFactoryHookInstalled ? 1u : 0u,
            g_dx12VbFactoryHookInstalled ? 1u : 0u,
            g_dx12SbFactoryHookInstalled ? 1u : 0u,
            vtableImmediate ? 1u : 0u);
}
