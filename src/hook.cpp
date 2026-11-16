#include "hook.h"

#include <windows.h>
#include <cstring>
#include <mutex>

namespace hook {
namespace {

std::uintptr_t g_arena = 0;
std::size_t g_arenaSize = 0;
std::size_t g_arenaUsed = 0;
std::mutex g_mx;

bool InRel32(std::uintptr_t from, std::uintptr_t to)
{
    const std::int64_t d = (std::int64_t)to - (std::int64_t)from;
    return d >= INT32_MIN && d <= INT32_MAX;
}

std::uintptr_t TryAllocAt(std::uintptr_t addr, std::size_t size)
{
    void* p = ::VirtualAlloc(reinterpret_cast<void*>(addr), size,
                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    return reinterpret_cast<std::uintptr_t>(p);
}

} // namespace

bool InitArena(std::uintptr_t lo, std::uintptr_t hi, std::size_t size)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_arena) return true;
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;   // 64KB

    // 先往模块下面找（CT 用的就是 0x13FF80000），再往上面找。整段都要在 rel32 内。
    // 2026-07: 有的机器上 ±1.75GB 里一个 64KB 的空洞都没有（4K + DX12 + Steam 壳），
    // 于是扫完再用「让系统随便给、自己验距离」兜一下（见函数末尾）。
    const std::uintptr_t span = 0x7F000000;   // 留余量，别贴着 2GB 边
    for (std::uintptr_t a = (lo - gran) & ~(gran - 1); a > gran && lo - a < span; a -= gran) {
        if (!InRel32(a, hi) || !InRel32(a + size, lo)) continue;
        if (std::uintptr_t p = TryAllocAt(a, size)) { g_arena = p; break; }
    }
    if (!g_arena) {
        for (std::uintptr_t a = (hi + gran) & ~(gran - 1); a - hi < span; a += gran) {
            if (!InRel32(lo, a + size)) break;
            if (std::uintptr_t p = TryAllocAt(a, size)) { g_arena = p; break; }
        }
    }
    if (!g_arena) {
        // 64KB 网格扫不到，不代表没有：地址空间里可能只剩不落在网格上的缝。
        // VirtualAlloc(nullptr) 让系统自己挑，挑到哪算哪，验一下 rel32，不在就还回去。
        for (int i = 0; i < 64 && !g_arena; ++i) {
            void* p = ::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (!p) break;
            const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
            if (InRel32(a, hi) && InRel32(a + size, lo)) g_arena = a;
            else ::VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    if (!g_arena) return false;
    g_arenaSize = size;
    g_arenaUsed = 0;
    std::memset(reinterpret_cast<void*>(g_arena), 0xCC, size);
    return true;
}

std::uintptr_t Commit(const Emitter& e)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_arena) return 0;
    const std::size_t at = (g_arenaUsed + 15) & ~std::size_t(15);
    if (at + e.size() > g_arenaSize) return 0;
    std::memcpy(reinterpret_cast<void*>(g_arena + at), e.bytes().data(), e.size());
    g_arenaUsed = at + e.size();
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(g_arena + at), e.size());
    return g_arena + at;
}

bool WriteMem(std::uintptr_t addr, const void* bytes, std::size_t n)
{
    DWORD old = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(addr), bytes, n);
    DWORD tmp = 0;
    ::VirtualProtect(reinterpret_cast<void*>(addr), n, old, &tmp);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
    return true;
}

bool PatchJump(std::uintptr_t site, std::size_t len, std::uintptr_t stub)
{
    if (len < 5 || len > 16 || !stub || !InRel32(site + 5, stub)) return false;
    std::uint8_t p[16];
    p[0] = 0xE9;
    const std::int32_t rel = (std::int32_t)((std::int64_t)stub - (std::int64_t)(site + 5));
    std::memcpy(p + 1, &rel, 4);
    for (std::size_t i = 5; i < len; ++i) p[i] = 0x90;
    return WriteMem(site, p, len);
}

bool InstallCtxHook(std::uintptr_t site, std::size_t len, Handler h,
                    const std::uint8_t* displaced, std::size_t dlen)
{
    Emitter e(256);
    // ---- 保存现场 ----
    e.b({0x9C});                                        // pushfq
    e.b({0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57});    // push rax rcx rdx rbx rbp rsi rdi
    for (std::uint8_t r = 0; r < 8; ++r) e.b({0x41, std::uint8_t(0x50 + r)});   // push r8..r15
    e.b({0x48, 0x89, 0xE5});                            // mov rbp, rsp      (rbp = &Ctx)
    e.b({0x48, 0x83, 0xEC, 0x60});                      // sub rsp, 0x60
    e.b({0x48, 0x83, 0xE4, 0xF0});                      // and rsp, -16
    for (std::uint8_t n = 0; n < 6; ++n)                // movdqa [rsp+n*16], xmmN
        e.b({0x66, 0x0F, 0x7F, std::uint8_t(0x44 | (n << 3)), 0x24, std::uint8_t(n * 16)});
    e.b({0x48, 0x83, 0xEC, 0x20});                      // sub rsp, 0x20     (shadow space)
    e.b({0x48, 0x89, 0xE9});                            // mov rcx, rbp
    e.b({0x48, 0xB8}).u64(reinterpret_cast<std::uint64_t>(h));   // mov rax, handler
    e.b({0xFF, 0xD0});                                  // call rax
    e.b({0x48, 0x83, 0xC4, 0x20});                      // add rsp, 0x20
    for (std::uint8_t n = 0; n < 6; ++n)                // movdqa xmmN, [rsp+n*16]
        e.b({0x66, 0x0F, 0x6F, std::uint8_t(0x44 | (n << 3)), 0x24, std::uint8_t(n * 16)});
    e.b({0x48, 0x89, 0xEC});                            // mov rsp, rbp
    for (int r = 7; r >= 0; --r) e.b({0x41, std::uint8_t(0x58 + r)});   // pop r15..r8
    e.b({0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58});    // pop rdi rsi rbp rbx rdx rcx rax
    e.b({0x9D});                                        // popfq
    // ---- 被覆盖的原指令，然后回去 ----
    e.raw(displaced, dlen);
    e.jmpAbs(site + len);

    const std::uintptr_t stub = Commit(e);
    return stub && PatchJump(site, len, stub);
}

} // namespace hook
