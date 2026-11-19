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
//        - Hook 0x1424E8265 (FUN_1424E7A10): sub-allocate DEFAULT-heap non-RT/DS/MSAA textures
//          <= 32 MB via CreatePlacedResource; store bit-63-tagged handle in param_1[5] so
//          mpResidencyManager automatically skips individual MakeResident/Evict calls.
//        - Hook 0x1424E9286 (~Texture12): reclaim tagged handles via 8-frame GPU quarantine.
//     2) Buffer Pool (D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS):
//        - Hook 0x1424E6722 (IB), 0x1424E89A2 (VB), 0x1424E6F9E (SB): sub-allocate DEFAULT-heap
//          buffers <= 32 MB via CreatePlacedResource; leave param_1[5] == 0 so SRV/UAV/upload
//          paths remain 100% native, clear residency flag byte [param_1+0x0C] = 0 so
//          mpResidencyManager skips them, and track ID3D12Resource* -> handle in a side map.
//        - Hook 0x14255397F (FUN_1425538E0): reclaim placed buffer handles via 8-frame quarantine.

constexpr uintptr_t kDx12TexCreateRva = 0x24E8265;
constexpr unsigned char kDx12TexCreateExpected[5] = {0x49, 0x83, 0x7E, 0x20, 0x00};

constexpr uintptr_t kDx12TexFreeCallRva = 0x24E9286;
constexpr uintptr_t kDx12TexFreeTargetRva = 0x2627790;

constexpr uintptr_t kDx12IbCreateRva = 0x24E6722;
constexpr uintptr_t kDx12VbCreateRva = 0x24E89A2;
constexpr unsigned char kDx12BufCreateR13Expected[9] = {0x49, 0x8B, 0xCD, 0xFF, 0x90, 0xD8, 0x00, 0x00, 0x00};

constexpr uintptr_t kDx12SbCreateRva = 0x24E6F9E;
constexpr unsigned char kDx12BufCreateR12Expected[9] = {0x49, 0x8B, 0xCC, 0xFF, 0x90, 0xD8, 0x00, 0x00, 0x00};

constexpr uintptr_t kDx12BufReleaseRva = 0x255397F;
constexpr unsigned char kDx12BufReleaseExpected[7] = {0x48, 0x8B, 0x4B, 0x20, 0x48, 0x85, 0xC9};

constexpr uintptr_t kDx12RenderInstanceRva = 0x51C7050;
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
        uint32_t emptyCount = 0;
        for (auto& h : heaps) {
            if (h.heap != nullptr && h.usedUnits == 0) {
                ++emptyCount;
                if (emptyCount > 2) {
                    toRelease.push_back(h.heap);
                    h.heap = nullptr;
                    h.freeList.clear();
                    heapsReleased.fetch_add(1, std::memory_order_relaxed);
                }
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

bool g_dx12FastHooksTried = false;
bool g_dx12TexCreateHookInstalled = false;
bool g_dx12TexFreeHookInstalled = false;
bool g_dx12BufReleaseHookInstalled = false;
bool g_dx12IbCreateHookInstalled = false;
bool g_dx12VbCreateHookInstalled = false;
bool g_dx12SbCreateHookInstalled = false;

using NativeTexSmallFreeFn = void (__fastcall*)(uintptr_t, uint64_t);

uint64_t ReadDx12FrameCounter() {
    const uintptr_t d3d = P(g_exeBase + kDx12RenderInstanceRva);
    if (!d3d || !Readable(d3d + 0x1E8, 8)) return 0;
    return *reinterpret_cast<const uint64_t*>(d3d + 0x1E8);
}

void OnDx12TexCreate(hook::Ctx* c) {
    if (!g_cfg.enabled || g_cfg.title_screen_only || !g_cfg.dx12_fast) return;
    const uintptr_t tex = static_cast<uintptr_t>(c->r14);
    const uintptr_t bp = static_cast<uintptr_t>(c->rbp);
    if (!tex || !bp) return;
    if (*reinterpret_cast<const uintptr_t*>(tex + 0x20) != 0) return;

    const uintptr_t d3d = P(g_exeBase + kDx12RenderInstanceRva);
    if (!d3d) return;
    auto* dev = *reinterpret_cast<ID3D12Device* const*>(d3d + 0x18);
    if (!dev) return;

    const auto* heapProps = reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(bp + 0x40);
    const void* pClearValue = *reinterpret_cast<void* const*>(bp + 0x30);
    const auto* srcDesc = reinterpret_cast<const D3D12_RESOURCE_DESC*>(bp + 0xA0);

    if (heapProps->Type != D3D12_HEAP_TYPE_DEFAULT ||
        heapProps->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN ||
        heapProps->MemoryPoolPreference != D3D12_MEMORY_POOL_UNKNOWN ||
        pClearValue != nullptr) {
        g_dx12TexPool.fallbackCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (srcDesc->Dimension < D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
        srcDesc->Dimension > D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
        srcDesc->Flags != D3D12_RESOURCE_FLAG_NONE ||
        srcDesc->SampleDesc.Count != 1) {
        g_dx12TexPool.fallbackCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    D3D12_RESOURCE_DESC desc = *srcDesc;
    desc.Alignment = 0;

    D3D12_RESOURCE_ALLOCATION_INFO info = dev->GetResourceAllocationInfo(0, 1, &desc);
    if (info.SizeInBytes == 0 || info.SizeInBytes == UINT64_MAX ||
        info.SizeInBytes > kDx12MaxPlacedBytes ||
        info.Alignment > kDx12HeapUnitBytes) {
        g_dx12TexPool.fallbackCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint32_t units = static_cast<uint32_t>((info.SizeInBytes + (kDx12HeapUnitBytes - 1)) >> kDx12HeapUnitShift);
    const uint64_t curFrame = *reinterpret_cast<const uint64_t*>(d3d + 0x1E8);

    ID3D12Heap* heap = nullptr;
    uint32_t heapId = 0;
    uint32_t offUnits = 0;
    if (!g_dx12TexPool.AllocBlock(dev, units, curFrame, heap, heapId, offUnits)) {
        g_dx12TexPool.fallbackCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    ID3D12Resource* res = nullptr;
    const auto initialState = static_cast<D3D12_RESOURCE_STATES>(static_cast<uint32_t>(c->r13));
    const HRESULT hr = dev->CreatePlacedResource(
        heap,
        static_cast<uint64_t>(offUnits) << kDx12HeapUnitShift,
        &desc,
        initialState,
        nullptr,
        __uuidof(ID3D12Resource),
        reinterpret_cast<void**>(&res));
    heap->Release();

    if (FAILED(hr) || !res) {
        g_dx12TexPool.ReturnBlockImmediate(heapId, offUnits, units);
        g_dx12TexPool.fallbackCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    *reinterpret_cast<ID3D12Resource**>(tex + 0x20) = res;
    *reinterpret_cast<uint64_t*>(tex + 0x28) = Dx12PlacedHeapPool::PackHandle(heapId, offUnits, units);
    *reinterpret_cast<uint32_t*>(tex + 0x10) = static_cast<uint32_t>(info.SizeInBytes);
    *reinterpret_cast<uint32_t*>(tex + 0x30) = 0;
    g_dx12TexPool.placedCount.fetch_add(1, std::memory_order_relaxed);
}

void OnDx12TexFree(hook::Ctx* c) {
    const uint64_t handle = static_cast<uint64_t>(c->rdx);
    if ((handle & kDx12PlacedTagBit) != 0) {
        g_dx12TexPool.QueueFree(handle, ReadDx12FrameCounter());
        return;
    }
    reinterpret_cast<NativeTexSmallFreeFn>(g_exeBase + kDx12TexFreeTargetRva)(
        static_cast<uintptr_t>(c->rcx), handle);
}

void HandleDx12BufCreate(hook::Ctx* c, ID3D12Device* dev) {
    const uintptr_t bufObj = static_cast<uintptr_t>(c->rbx);
    const auto* heapProps = reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(c->rdx);
    const auto heapFlags = static_cast<D3D12_HEAP_FLAGS>(static_cast<uint32_t>(c->r8));
    const auto* srcDesc = reinterpret_cast<const D3D12_RESOURCE_DESC*>(c->r9);
    const auto initialState = static_cast<D3D12_RESOURCE_STATES>(static_cast<uint32_t>(c->rcx));
    auto** ppRes = reinterpret_cast<ID3D12Resource**>(bufObj + 0x20);

    if (g_cfg.enabled && !g_cfg.title_screen_only && g_cfg.dx12_fast && dev && bufObj && heapProps && srcDesc &&
        heapFlags == D3D12_HEAP_FLAG_NONE &&
        heapProps->Type == D3D12_HEAP_TYPE_DEFAULT &&
        heapProps->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
        heapProps->MemoryPoolPreference == D3D12_MEMORY_POOL_UNKNOWN &&
        srcDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        srcDesc->Width > 0 && srcDesc->Width <= kDx12MaxPlacedBytes) {
        D3D12_RESOURCE_DESC desc = *srcDesc;
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
                    initialState,
                    nullptr,
                    __uuidof(ID3D12Resource),
                    reinterpret_cast<void**>(&res));
                heap->Release();
                if (SUCCEEDED(hr) && res) {
                    const uint64_t handle = Dx12PlacedHeapPool::PackHandle(heapId, offUnits, units);
                    ::AcquireSRWLockExclusive(&g_dx12BufMapLock);
                    g_dx12PlacedBufMap[res] = handle;
                    ::ReleaseSRWLockExclusive(&g_dx12BufMapLock);
                    g_dx12PlacedBufLive.fetch_add(1, std::memory_order_release);

                    *ppRes = res;
                    // Clear residency-managed bit so mpResidencyManager skips this placed buffer
                    // while param_1[5] stays 0 for native SRV/UAV/upload paths.
                    *reinterpret_cast<uint8_t*>(bufObj + 0x0C) = 0;
                    g_dx12BufPool.placedCount.fetch_add(1, std::memory_order_relaxed);
                    c->rax = static_cast<uint64_t>(static_cast<uint32_t>(S_OK));
                    return;
                }
                g_dx12BufPool.ReturnBlockImmediate(heapId, offUnits, units);
            }
        }
        g_dx12BufPool.fallbackCount.fetch_add(1, std::memory_order_relaxed);
    } else if (dev && heapProps && heapProps->Type == D3D12_HEAP_TYPE_DEFAULT) {
        g_dx12BufPool.fallbackCount.fetch_add(1, std::memory_order_relaxed);
    }

    const HRESULT hr = dev->CreateCommittedResource(
        heapProps, heapFlags, srcDesc, initialState, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(ppRes));
    c->rax = static_cast<uint64_t>(static_cast<uint32_t>(hr));
}

void OnDx12BufCreateR13(hook::Ctx* c) {
    HandleDx12BufCreate(c, reinterpret_cast<ID3D12Device*>(c->r13));
}

void OnDx12BufCreateR12(hook::Ctx* c) {
    HandleDx12BufCreate(c, reinterpret_cast<ID3D12Device*>(c->r12));
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
    const unsigned char noCall9[9] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

    // Texture placed-heap hooks (install destructor free-hook FIRST!).
    const uintptr_t freeSite = g_exeBase + kDx12TexFreeCallRva;
    const uintptr_t freeTarget = g_exeBase + kDx12TexFreeTargetRva;
    const uintptr_t createSite = g_exeBase + kDx12TexCreateRva;
    if (Readable(freeSite, 5) && *reinterpret_cast<const uint8_t*>(freeSite) == 0xE8 &&
        Readable(createSite, sizeof(kDx12TexCreateExpected)) &&
        std::memcmp(reinterpret_cast<const void*>(createSite), kDx12TexCreateExpected, sizeof(kDx12TexCreateExpected)) == 0) {
        int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(freeSite + 1), 4);
        if (static_cast<int64_t>(freeSite + 5) + rel == static_cast<int64_t>(freeTarget)) {
            g_dx12TexFreeHookInstalled = hook::InstallCtxHook(freeSite, 5, &OnDx12TexFree, noCall5, 5);
            if (g_dx12TexFreeHookInstalled) {
                g_dx12TexCreateHookInstalled = hook::InstallCtxHook(
                    createSite, sizeof(kDx12TexCreateExpected), &OnDx12TexCreate,
                    kDx12TexCreateExpected, sizeof(kDx12TexCreateExpected));
            }
        }
    }

    // Buffer placed-heap hooks (install release-hook FIRST!).
    const uintptr_t bufRelSite = g_exeBase + kDx12BufReleaseRva;
    if (Readable(bufRelSite, sizeof(kDx12BufReleaseExpected)) &&
        std::memcmp(reinterpret_cast<const void*>(bufRelSite), kDx12BufReleaseExpected, sizeof(kDx12BufReleaseExpected)) == 0) {
        g_dx12BufReleaseHookInstalled = hook::InstallCtxHook(
            bufRelSite, sizeof(kDx12BufReleaseExpected), &OnDx12BufRelease,
            kDx12BufReleaseExpected, sizeof(kDx12BufReleaseExpected));
    }
    if (g_dx12BufReleaseHookInstalled) {
        const uintptr_t ibSite = g_exeBase + kDx12IbCreateRva;
        const uintptr_t vbSite = g_exeBase + kDx12VbCreateRva;
        const uintptr_t sbSite = g_exeBase + kDx12SbCreateRva;
        if (Readable(ibSite, sizeof(kDx12BufCreateR13Expected)) &&
            std::memcmp(reinterpret_cast<const void*>(ibSite), kDx12BufCreateR13Expected, sizeof(kDx12BufCreateR13Expected)) == 0) {
            g_dx12IbCreateHookInstalled = hook::InstallCtxHook(ibSite, 9, &OnDx12BufCreateR13, noCall9, 9);
        }
        if (Readable(vbSite, sizeof(kDx12BufCreateR13Expected)) &&
            std::memcmp(reinterpret_cast<const void*>(vbSite), kDx12BufCreateR13Expected, sizeof(kDx12BufCreateR13Expected)) == 0) {
            g_dx12VbCreateHookInstalled = hook::InstallCtxHook(vbSite, 9, &OnDx12BufCreateR13, noCall9, 9);
        }
        if (Readable(sbSite, sizeof(kDx12BufCreateR12Expected)) &&
            std::memcmp(reinterpret_cast<const void*>(sbSite), kDx12BufCreateR12Expected, sizeof(kDx12BufCreateR12Expected)) == 0) {
            g_dx12SbCreateHookInstalled = hook::InstallCtxHook(sbSite, 9, &OnDx12BufCreateR12, noCall9, 9);
        }
    }

    LogLine("dx12-fast: placed-tex=%u/%u placed-buf=%u/%u/%u/%u",
            g_dx12TexFreeHookInstalled ? 1u : 0u,
            g_dx12TexCreateHookInstalled ? 1u : 0u,
            g_dx12BufReleaseHookInstalled ? 1u : 0u,
            g_dx12IbCreateHookInstalled ? 1u : 0u,
            g_dx12VbCreateHookInstalled ? 1u : 0u,
            g_dx12SbCreateHookInstalled ? 1u : 0u);
}
