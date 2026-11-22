
#include "../src/auto_quest_start.cpp"
#include <cassert>
#include <vector>
void WriteIni(const char* content){FILE* f=nullptr;assert(!fopen_s(&f,(DllDir()+"\\auto_quest_start.ini").c_str(),"wb"));fputs(content,f);fclose(f);}
void WP(uintptr_t a,uintptr_t p){memcpy((void*)a,&p,8);}
void NativeSites(){
 memcpy((void*)(g_exeBase+kOnePhaseJump),kOnePhaseExpected,2);
 memcpy((void*)(g_exeBase+kQuestId1),kQuestIdExpected,4);memcpy((void*)(g_exeBase+kQuestId2),kQuestIdExpected,4);
 memcpy((void*)(g_exeBase+kDlcInitCall),kDlcInitExpected,5);
}
void CheckSites(bool redirect,bool dlcPatched){
 assert(!memcmp((void*)(g_exeBase+kOnePhaseJump),redirect?kOnePhasePatched:kOnePhaseExpected,2));
 for(auto site:{kQuestId1,kQuestId2})assert(Rd32(g_exeBase+site)==(redirect?(uint32_t)g_cfg.quest_id:1u));
 assert(!memcmp((void*)(g_exeBase+kDlcInitCall),dlcPatched?kDlcInitPatched:kDlcInitExpected,5));
}
void FullReset(bool rehook){
 NativeSites();g_cfg={};g_cfg.quest_id=12345;g_cfg.rehook_on_return=rehook;
 g_titleContinued=false;g_startupLogoDone=true;g_returnTitlePolicyApplied=false;g_returnTitleRestoreWarning=false;
 g_guiTitlePlayHookChecked=false;
 assert(PatchAll(g_cfg,(HMODULE)g_exeBase));CheckSites(true,true);
}
int64_t budgetTestTime = 0, budgetTestCost = 0;
unsigned budgetTestCalls = 0, budgetTestFinish = 0;
int64_t BudgetTestClock() { return budgetTestTime; }
uint64_t BudgetTestStep(uintptr_t resident, uint32_t mode) {
 assert(mode == 3 && Rd32(resident + 0x38) == 4);
 ++budgetTestCalls; budgetTestTime += budgetTestCost;
 if (budgetTestFinish && budgetTestCalls == budgetTestFinish) Wr32(resident + 0x38, 5);
 return 1; // Return value alone must not fabricate resident completion.
}
void CheckResidentSlices() {
 unsigned char resident[0x80]{};
 const auto r = (uintptr_t)resident;
 Wr32(r + 0x38, 4);
 auto slice = RunResidentBuildSlice(r, &BudgetTestStep, &BudgetTestClock, 2000);
 assert(slice.calls == 7 && budgetTestCalls == 7 && Rd32(r + 0x38) == 4);
 budgetTestCalls = 0; budgetTestTime = 0; budgetTestCost = 900;
 slice = RunResidentBuildSlice(r, &BudgetTestStep, &BudgetTestClock, 2000);
 assert(slice.calls == 3 && slice.elapsed == 2700); // Soft limit, checked between calls.
 budgetTestCalls = 0; budgetTestTime = 0; budgetTestCost = 100; budgetTestFinish = 2;
 slice = RunResidentBuildSlice(r, &BudgetTestStep, &BudgetTestClock, 2000);
 assert(slice.calls == 2 && Rd32(r + 0x38) == 5);
 for (uint32_t state : {2u, 3u, 5u, 9u, 11u}) {
  Wr32(r + 0x38, state); budgetTestCalls = 0;
  slice = RunResidentBuildSlice(r, &BudgetTestStep, &BudgetTestClock, 2000);
  assert(!slice.calls && !budgetTestCalls && Rd32(r + 0x38) == state);
 }
 puts("PASS: resident extra-call/time bounds and stop before native I/O phases.");
}
unsigned fontTestForwarded = 0;
void TestFontWarmNative(uintptr_t manager, uintptr_t chars, uintptr_t font, uint32_t width, uint32_t height, uintptr_t size) {
 assert(manager == 11 && chars != 0 && font == 33 && width == 32 && height == 48 && size == 66);
 ++fontTestForwarded;
}
void TestFontRefreshNative(uintptr_t gui, uint32_t language) {
 assert(gui == 77 && language == 8);
 unsigned char chars[0xB0]{}; Wr32((uintptr_t)chars + 0xAC, 123);
 hook::Ctx c{}; c.rcx=11; c.rdx=(uintptr_t)chars; c.r8=33; c.r9=32;
 HandleFontWarm(&c, 0, &TestFontWarmNative, 48, 66);
 HandleFontWarm(&c, 1, &TestFontWarmNative, 48, 66);
}
void CheckFontScope() {
 g_cfg={}; g_fontStartupWindow=true;
 for (auto& installed:g_fontHooks) installed=true;
 hook::Ctx c{}; c.rcx=77; c.rdx=8;
 HandleFontRefresh(&c,&TestFontRefreshNative);
 assert(!fontTestForwarded && g_fontRefreshUsed && !g_fontDeferScope);
 assert(g_fontWarmCalls[0]==1 && g_fontWarmCharacters[0]==123 && g_fontWarmCharacters[1]==123);
 HandleFontRefresh(&c,&TestFontRefreshNative); // Subsequent language refresh is fully native.
 assert(fontTestForwarded==2);
 g_fontRefreshUsed=false; g_cfg.title_screen_only=true;
 HandleFontRefresh(&c,&TestFontRefreshNative); assert(fontTestForwarded==4 && !g_fontRefreshUsed);
 g_cfg.title_screen_only=false; g_fontHooks[2]=false;
 HandleFontRefresh(&c,&TestFontRefreshNative); assert(fontTestForwarded==6);
 g_fontHooks[2]=true; CloseStartupFontWindow();
 HandleFontRefresh(&c,&TestFontRefreshNative); assert(fontTestForwarded==8);
 g_fontRefreshUsed=false; g_fontStartupWindow=false;
 for (auto& installed:g_fontHooks) installed=false;
 for (auto& n:g_fontWarmCalls) n=0;
 for (auto& n:g_fontWarmCharacters) n=0;
 puts("PASS: font warm deferral limited to first startup refresh, with complete native forwarding otherwise.");
}
unsigned fontCtorNativeCalls = 0;
void TestFontCtorNative(uintptr_t gui, uint32_t language) {
 assert(gui == 88 && (language == 8 || language == 0));
 ++fontCtorNativeCalls;
 unsigned char chars[0xB0]{}; Wr32((uintptr_t)chars + 0xAC, 50);
 hook::Ctx c{}; c.rcx=11; c.rdx=(uintptr_t)chars; c.r8=33; c.r9=32;
 HandleFontWarm(&c, 0, &TestFontWarmNative, 48, 66);
 HandleFontWarm(&c, 1, &TestFontWarmNative, 48, 66);
}
void CheckFontCtorAndDuplicateSkip() {
 g_cfg={}; g_startupLogoDone=false; g_titleContinued=false;
 g_fontStartupWindow=true; g_fontRefreshUsed=false;
 g_fontCtorHookInstalled=true; g_fontCtorRefreshUsed=false; g_fontCtorBoundLang=~0u;
 for (auto& installed : g_fontHooks) installed=true;
 fontTestForwarded=0; fontCtorNativeCalls=0;
 for (auto& n : g_fontWarmCalls) n=0;
 for (auto& n : g_fontWarmCharacters) n=0;
 hook::Ctx c{}; c.rcx=88; c.rdx=8;
 HandleFontRefreshCtor(&c, &TestFontCtorNative);
 assert(fontCtorNativeCalls==1 && fontTestForwarded==0 && g_fontCtorRefreshUsed && g_fontCtorBoundLang==8);
 // Same language at aLogo 0x16 skips redundant cache wipe and font reload.
 HandleFontRefresh(&c, &TestFontCtorNative);
 assert(fontCtorNativeCalls==1 && g_fontRefreshUsed);
 // Different language at aLogo 0x16 still runs native font binding with prewarm deferred.
 g_fontRefreshUsed=false; c.rdx=0;
 HandleFontRefresh(&c, &TestFontCtorNative);
 assert(fontCtorNativeCalls==2 && fontTestForwarded==0 && g_fontRefreshUsed);
 g_fontCtorHookInstalled=false; g_fontCtorRefreshUsed=false; g_fontCtorBoundLang=~0u;
 g_fontRefreshUsed=false; g_fontStartupWindow=false;
 for (auto& installed : g_fontHooks) installed=false;
 for (auto& n : g_fontWarmCalls) n=0;
 for (auto& n : g_fontWarmCharacters) n=0;
 fontTestForwarded=0;
 puts("PASS: ctor font deferral (0x1AEE62D), same-language skip, and changed-language deferred refresh.");
}
unsigned contentNativeCalls = 0;
void TestContentNative(uintptr_t content) { ++contentNativeCalls; Wr32(content + 0x50, 0x14); }
void CheckSaveFastAndContentSkip() {
 unsigned char logo[0x120]{};
 const auto lg = (uintptr_t)logo;
 g_cfg={}; g_startupLogoDone=false; g_titleContinued=false; g_silentOp1Applied=false;
 Wr32(lg + 0xA8 + 8, 1); Wr32(lg + 0xA8 + 0x28, 0); Wr32(lg + 0xA8 + 0xC, 0);
 assert(!TryApplySilentLogoSaveLoad(lg, 4) && Rd32(lg + 0xA8 + 0xC) == 0);
 g_cfg.title_screen_only = true;
 assert(!TryApplySilentLogoSaveLoad(lg, 5) && Rd32(lg + 0xA8 + 0xC) == 0);
 g_cfg.title_screen_only = false;
 assert(TryApplySilentLogoSaveLoad(lg, 5) && Rd32(lg + 0xA8 + 0xC) == 1);
 Wr32(lg + 0xA8 + 8, 0); Wr32(lg + 0xA8 + 0xC, 0);
 assert(!TryApplySilentLogoSaveLoad(lg, 5) && Rd32(lg + 0xA8 + 0xC) == 0);

 auto fakeExe = (unsigned char*)VirtualAlloc(nullptr, 0x51C5000, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
 assert(fakeExe);
 const uintptr_t savedBase = g_exeBase;
 g_exeBase = (uintptr_t)fakeExe;
 std::vector<unsigned char> userBuf(0x22000, 0x33), slotsBuf(kSaveSlotSize * 3, 0x55);
 const uintptr_t u = (uintptr_t)userBuf.data();
 WP(g_exeBase + kPSave, u);
 WP(u + 0xA8, (uintptr_t)slotsBuf.data());
 g_saveBaseline = ComputeUserDataDigest(u);
 assert(g_saveBaseline.valid);

 std::vector<unsigned char> contentBuf(0x1200, 0);
 const uintptr_t content = (uintptr_t)contentBuf.data();
 hook::Ctx c{}; c.rcx = content;
 contentNativeCalls = 0;
 Wr32(content + 0x50, 0x12); Wr32(content + 0xC, 1);
 HandleContentCheckSave(&c, &TestContentNative);
 assert(contentNativeCalls == 0 && Rd32(content + 0x50) == 0x17 && Rd32(content + 0xC) == 0);

 slotsBuf[kSaveSlotSize * 2 + 0x100] ^= 0x01;
 HandleContentCheckSave(&c, &TestContentNative);
 assert(contentNativeCalls == 1 && Rd32(content + 0x50) == 0x14);
 slotsBuf[kSaveSlotSize * 2 + 0x100] ^= 0x01;

 contentBuf[0x924] = 1;
 HandleContentCheckSave(&c, &TestContentNative);
 assert(contentNativeCalls == 2);
 contentBuf[0x924] = 0;
 Wr32(content + 0x112C, 1);
 HandleContentCheckSave(&c, &TestContentNative);
 assert(contentNativeCalls == 3);

 g_saveBaseline = {};
 g_exeBase = savedBase;
 VirtualFree(fakeExe, 0, MEM_RELEASE);
 puts("PASS: silent op1 guard and lossless hash-checked content op0 skip (dirty bit / DLC / claims preserved).");
}
unsigned loadAllQuestsCalls = 0;
uint8_t __fastcall FakeLoadAllQuests(uintptr_t quest) {
 ++loadAllQuestsCalls;
 Wr32(quest + 0x29EA0, Rd32(quest + 0x29EA0) + 1);
 return 0;
}
unsigned prefetchAddedCount = 0;
void __fastcall FakeAddResource(uintptr_t, uint32_t, uintptr_t, const char*) { ++prefetchAddedCount; }
void CheckPlayableOverlapAndDx11Helpers() {
 std::vector<unsigned char> questBuf(0x2A000, 0);
 const uintptr_t q = (uintptr_t)questBuf.data();
 g_playableQuestPrefetched = false; loadAllQuestsCalls = 0;
 assert(TriggerPlayableQuestPrefetchOnce(q, 0x12345, &FakeLoadAllQuests));
 assert(loadAllQuestsCalls == 1 && Rd32(q + 0x29EA0) == 1);
 assert(!TriggerPlayableQuestPrefetchOnce(q, 0x12345, &FakeLoadAllQuests));
 assert(loadAllQuestsCalls == 1 && Rd32(q + 0x29EA0) == 1);
 g_playableQuestPrefetched = false;

 std::vector<unsigned char> guiBuf(0x16000, 0);
 const char* paths[3] = {"ui\\hud\\hud_main", "ui\\credit\\staff", "ui\\opt\\variant"};
 struct GuiEntry { uintptr_t dti; const char* path; uint32_t flags; uint32_t pad; };
 GuiEntry entries[3] = {{1, paths[0], 0, 0}, {2, paths[1], 0, 0}, {3, paths[2], 8, 0}};
 const uintptr_t g = (uintptr_t)guiBuf.data();
 WP(g + 0x14FB0 + 1 * 0x10, (uintptr_t)entries);
 Wr32(g + 0x14FB0 + 1 * 0x10 + 8, 3);
 prefetchAddedCount = 0;
 assert(AppendRegisteredGuiGroupPaths(0x10000, g, 7, 1, false, &FakeAddResource) == 2);
 assert(prefetchAddedCount == 2);
 prefetchAddedCount = 0;
 assert(AppendRegisteredGuiGroupPaths(0x10000, g, 7, 1, true, &FakeAddResource) == 1);
 assert(prefetchAddedCount == 1);

 static unsigned psoCalls = 0;
 auto fakePso = [](uintptr_t) { ++psoCalls; };
 auto retDx11 = []() -> uint8_t { return 0; };
 auto retDx12 = []() -> uint8_t { return 1; };
 hook::Ctx c{}; c.rcx = 0x20000;
 g_cfg = {}; psoCalls = 0;
 HandleDx11PsoLoad(&c, retDx11, fakePso); assert(psoCalls == 0);
 HandleDx11PsoLoad(&c, retDx12, fakePso); assert(psoCalls == 1);
 g_cfg.title_screen_only = true;
 HandleDx11PsoLoad(&c, retDx11, fakePso); assert(psoCalls == 2);
 g_cfg.title_screen_only = false;
 puts("PASS: LoadAllQuests single-shot invariant, registered GUI group walker, and DX11 PSO guard.");
}

struct FakeSplHookState {
 uintptr_t fnAddr = 0;
 uintptr_t trampAddr = 0;
 unsigned detourCalls = 0;
 uintptr_t lastRcx = 0;
 bool selfUnhook = false;
};
FakeSplHookState g_fakeSplHook;

void __fastcall FakeSplDetour(uintptr_t rcx) {
 ++g_fakeSplHook.detourCalls;
 g_fakeSplHook.lastRcx = rcx;
 reinterpret_cast<void (__fastcall*)(uintptr_t)>(g_fakeSplHook.trampAddr)(rcx);
 if (g_fakeSplHook.selfUnhook) {
  DWORD oldProt = 0;
  assert(VirtualProtect(reinterpret_cast<void*>(g_fakeSplHook.fnAddr), 5, PAGE_EXECUTE_READWRITE, &oldProt));
  std::memcpy(reinterpret_cast<void*>(g_fakeSplHook.fnAddr), kGuiTitlePlayHeadExpected, 5);
  DWORD ignored = 0;
  VirtualProtect(reinterpret_cast<void*>(g_fakeSplHook.fnAddr), 5, oldProt, &ignored);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_fakeSplHook.fnAddr), 5);
 }
}

void CheckHookedGuiTitlePlayNotify() {
 auto* buf = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
 assert(buf != nullptr);
 const uintptr_t fn = reinterpret_cast<uintptr_t>(buf);
 std::memcpy(buf, kGuiTitlePlayHeadExpected, sizeof(kGuiTitlePlayHeadExpected));
 std::memcpy(buf + kGuiTitlePlayRetGateOffset, kGuiTitlePlayTailExpected, sizeof(kGuiTitlePlayTailExpected));
 // Place a trap sentinel right after the 29-byte prologue; reaching it crashes/aborts the test.
 buf[sizeof(kGuiTitlePlayHeadExpected) + sizeof(kGuiTitlePlayTailExpected)] = 0xCC;

 // Subtest A: Unhooked vanilla bytes must return false with 0 detour calls.
 DWORD oldProt = 0;
 assert(VirtualProtect(buf, 0x1000, PAGE_EXECUTE_READ, &oldProt));
 FlushInstructionCache(GetCurrentProcess(), buf, 0x1000);
 g_fakeSplHook = {};
 assert(!TryNotifyHookedGuiTitlePlay(fn, 0x12345678ULL));
 assert(g_fakeSplHook.detourCalls == 0);
 assert(std::memcmp(buf, kGuiTitlePlayHeadExpected, sizeof(kGuiTitlePlayHeadExpected)) == 0);
 assert(std::memcmp(buf + kGuiTitlePlayRetGateOffset, kGuiTitlePlayTailExpected, sizeof(kGuiTitlePlayTailExpected)) == 0);

 // Subtest B: 5-byte E9 rel32 self-unhooking hook (exact SharpPluginLoader OnGuiTitlePlay pattern).
 assert(VirtualProtect(buf, 0x1000, PAGE_EXECUTE_READWRITE, &oldProt));
 const uintptr_t trampB = reinterpret_cast<uintptr_t>(buf + 0x100);
 std::memcpy(buf + 0x100, kGuiTitlePlayHeadExpected, 5);
 buf[0x105] = 0xE9;
 const int32_t trampBackRel = static_cast<int32_t>(static_cast<int64_t>(fn + 5) - static_cast<int64_t>(trampB + 10));
 std::memcpy(buf + 0x106, &trampBackRel, 4);

 // x64 thunk at buf + 0x200: mov rax, imm64; jmp rax
 const uintptr_t thunkB = reinterpret_cast<uintptr_t>(buf + 0x200);
 const uintptr_t detourAddr = reinterpret_cast<uintptr_t>(&FakeSplDetour);
 buf[0x200] = 0x48; buf[0x201] = 0xB8;
 std::memcpy(buf + 0x202, &detourAddr, 8);
 buf[0x20A] = 0xFF; buf[0x20B] = 0xE0;

 buf[0] = 0xE9;
 const int32_t hookRel = static_cast<int32_t>(static_cast<int64_t>(thunkB) - static_cast<int64_t>(fn + 5));
 std::memcpy(buf + 1, &hookRel, 4);
 g_fakeSplHook = {fn, trampB, 0, 0, true};
 assert(VirtualProtect(buf, 0x1000, PAGE_EXECUTE_READ, &oldProt));
 FlushInstructionCache(GetCurrentProcess(), buf, 0x1000);

 assert(TryNotifyHookedGuiTitlePlay(fn, 0x12345678ULL));
 assert(g_fakeSplHook.detourCalls == 1 && g_fakeSplHook.lastRcx == 0x12345678ULL);
 assert(std::memcmp(buf, kGuiTitlePlayHeadExpected, sizeof(kGuiTitlePlayHeadExpected)) == 0);
 assert(std::memcmp(buf + kGuiTitlePlayRetGateOffset, kGuiTitlePlayTailExpected, sizeof(kGuiTitlePlayTailExpected)) == 0);

 // Subtest C: 14-byte FF 25 00 00 00 00 <8-byte addr> indirect-jump hook + NOP pad.
 assert(VirtualProtect(buf, 0x1000, PAGE_EXECUTE_READWRITE, &oldProt));
 const uintptr_t trampC = reinterpret_cast<uintptr_t>(buf + 0x300);
 std::memcpy(buf + 0x300, kGuiTitlePlayHeadExpected, 15);
 buf[0x30F] = 0xE9;
 const int32_t trampCBackRel = static_cast<int32_t>(static_cast<int64_t>(fn + 15) - static_cast<int64_t>(trampC + 20));
 std::memcpy(buf + 0x310, &trampCBackRel, 4);

 buf[0] = 0xFF; buf[1] = 0x25;
 std::memset(buf + 2, 0, 4);
 std::memcpy(buf + 6, &detourAddr, 8);
 buf[14] = 0x90;
 g_fakeSplHook = {fn, trampC, 0, 0, false};
 assert(VirtualProtect(buf, 0x1000, PAGE_EXECUTE_READ, &oldProt));
 FlushInstructionCache(GetCurrentProcess(), buf, 0x1000);

 assert(TryNotifyHookedGuiTitlePlay(fn, 0xABCDEF01ULL));
 assert(g_fakeSplHook.detourCalls == 1 && g_fakeSplHook.lastRcx == 0xABCDEF01ULL);
 assert(std::memcmp(buf + kGuiTitlePlayRetGateOffset, kGuiTitlePlayTailExpected, sizeof(kGuiTitlePlayTailExpected)) == 0);

 // Subtest D: NotifyHookedGuiTitlePlayOnce wrapper guards.
 auto fakeImage = static_cast<unsigned char*>(VirtualAlloc(nullptr, kGuiTitlePlayRva + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
 assert(fakeImage != nullptr);
 const uintptr_t savedBase = g_exeBase;
 g_exeBase = reinterpret_cast<uintptr_t>(fakeImage);
 const uintptr_t imgFn = g_exeBase + kGuiTitlePlayRva;
 std::memcpy(reinterpret_cast<void*>(imgFn), kGuiTitlePlayHeadExpected, sizeof(kGuiTitlePlayHeadExpected));
 std::memcpy(reinterpret_cast<void*>(imgFn + kGuiTitlePlayRetGateOffset), kGuiTitlePlayTailExpected, sizeof(kGuiTitlePlayTailExpected));
 fakeImage[kGuiTitlePlayRva + sizeof(kGuiTitlePlayHeadExpected) + sizeof(kGuiTitlePlayTailExpected)] = 0xCC;

 // 15-byte trampoline inside fakeImage + 0x400 (within +-2GB rel32 range)
 const uintptr_t trampD = g_exeBase + kGuiTitlePlayRva + 0x400;
 std::memcpy(reinterpret_cast<void*>(trampD), kGuiTitlePlayHeadExpected, 15);
 *reinterpret_cast<unsigned char*>(trampD + 15) = 0xE9;
 const int32_t trampDBackRel = static_cast<int32_t>(static_cast<int64_t>(imgFn + 15) - static_cast<int64_t>(trampD + 20));
 std::memcpy(reinterpret_cast<void*>(trampD + 16), &trampDBackRel, 4);

 *reinterpret_cast<unsigned char*>(imgFn) = 0xFF;
 *reinterpret_cast<unsigned char*>(imgFn + 1) = 0x25;
 std::memset(reinterpret_cast<void*>(imgFn + 2), 0, 4);
 std::memcpy(reinterpret_cast<void*>(imgFn + 6), &detourAddr, 8);
 *reinterpret_cast<unsigned char*>(imgFn + 14) = 0x90;
 FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(imgFn), 0x500);

 unsigned char fakeTitle[0x100]{}, fakeGui[0x40]{};
 WP(reinterpret_cast<uintptr_t>(fakeTitle) + 0xD8, reinterpret_cast<uintptr_t>(fakeGui));
 Wr32(reinterpret_cast<uintptr_t>(fakeGui) + 0x0C, 2);

 g_cfg = {};
 g_cfg.enabled = true;
 g_cfg.title_screen_only = true;
 g_guiTitlePlayHookChecked = false;
 g_fakeSplHook = {imgFn, trampD, 0, 0, false};
 NotifyHookedGuiTitlePlayOnce(reinterpret_cast<uintptr_t>(fakeTitle));
 assert(!g_guiTitlePlayHookChecked && g_fakeSplHook.detourCalls == 0);

 g_cfg.title_screen_only = false;
 NotifyHookedGuiTitlePlayOnce(reinterpret_cast<uintptr_t>(fakeTitle));
 assert(g_guiTitlePlayHookChecked && g_fakeSplHook.detourCalls == 1 &&
        g_fakeSplHook.lastRcx == reinterpret_cast<uintptr_t>(fakeGui));
 NotifyHookedGuiTitlePlayOnce(reinterpret_cast<uintptr_t>(fakeTitle));
 assert(g_fakeSplHook.detourCalls == 1);

 g_guiTitlePlayHookChecked = false;
 g_exeBase = savedBase;
 VirtualFree(fakeImage, 0, MEM_RELEASE);
 VirtualFree(buf, 0, MEM_RELEASE);
 puts("PASS: hooked GUITitle:Play notification (5-byte E9 self-unhook, 14-byte FF25, unhooked skip, wrapper guards).");
}

int main(){
 CheckHookedGuiTitlePlayNotify();
 CheckFontScope();
 CheckFontCtorAndDuplicateSkip();
 CheckSaveFastAndContentSkip();
 CheckPlayableOverlapAndDx11Helpers();
 CheckResidentSlices();
 // Missing new keys retain the previous complete-auto behavior (and dx11_fast/dx12_fast default to true).
 WriteIni("[auto_quest_start]\nenabled=1\nsave_slot=1\nquest_id=12345\n");
 auto cfg=LoadConfig();assert(cfg.enabled&&!cfg.title_screen_only&&cfg.rehook_on_return&&cfg.save_slot==1&&cfg.quest_id==12345&&cfg.dx11_fast&&cfg.dx12_fast);
 for(int only=0;only<2;++only)for(int rehook=0;rehook<2;++rehook)for(int d11=0;d11<2;++d11)for(int d12=0;d12<2;++d12){
  char ini[320];sprintf_s(ini,"\xEF\xBB\xBF[auto_quest_start]\nenabled=1\ntitle_screen_only=%d\nrehook_on_return=%d\nsave_slot=0\nquest_id=66845\ndx11_fast=%d\ndx12_fast=%d\n",only,rehook,d11,d12);
  WriteIni(ini);cfg=LoadConfig();assert(cfg.title_screen_only==(only!=0)&&cfg.rehook_on_return==(rehook!=0)&&cfg.dx11_fast==(d11!=0)&&cfg.dx12_fast==(d12!=0));
 }
 WriteIni("[auto_quest_start]\nenabled=0\ntitle_screen_only=1\n");assert(!LoadConfig().enabled);
 auto image=(unsigned char*)VirtualAlloc(nullptr,0x51C5000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(image);g_exeBase=(uintptr_t)image;
 unsigned char title[0x400]{},logo[0x200]{};std::vector<unsigned char> user(0x22000),saves(kSaveSlotSize*3);
 auto t=(uintptr_t)title,u=(uintptr_t)user.data();WP(g_exeBase+kPSave,u);WP(u+0xA8,(uintptr_t)saves.data());saves[0x38]=1;
 hook::Ctx c{};c.rcx=t;
 // Do not undo the first automatic entry while the original title is preparing.
 FullReset(false);OnTitlePrepare(&c);CheckSites(true,true);assert(!g_returnTitlePolicyApplied);
 // Restoring on PREPARE supports address reuse and happens before manual save choice.
 g_titleContinued=true;OnTitlePrepare(&c);CheckSites(false,false);assert(g_returnTitlePolicyApplied);
 FullReset(true);g_titleContinued=true;OnTitlePrepare(&c);CheckSites(true,false);
 // A soft return detected from execute is covered even without PREPARE.
 FullReset(false);g_titleContinued=true;c.rax=0;OnTitleStage(&c);CheckSites(false,false);assert(c.rax==0);
 // Unknown edits cause an all-or-nothing refusal; no unrelated patch is overwritten.
 FullReset(false);g_titleContinued=true;*(unsigned char*)(g_exeBase+kOnePhaseJump)=0xCC;
 unsigned char before[0x1A0];memcpy(before,(void*)(g_exeBase+0x25FD80),sizeof(before));
 ApplyReturnTitlePolicy();assert(!g_returnTitlePolicyApplied&&!memcmp(before,(void*)(g_exeBase+0x25FD80),sizeof(before)));
 // Title-only ignores quest and DLC patches, and dominates rehook_on_return.
 NativeSites();g_cfg={};g_cfg.title_screen_only=true;g_cfg.rehook_on_return=true;g_titleContinued=false;g_startupLogoDone=false;
 assert(PatchAll(g_cfg,(HMODULE)g_exeBase));CheckSites(false,false);
 g_cfg.enabled=false;assert(PatchAll(g_cfg,(HMODULE)g_exeBase));CheckSites(false,false);g_cfg.enabled=true;
 // No prefetch request, resource cut, save selection or quest automation occurs.
 Wr32((uintptr_t)logo+0x50,0x17);float timer=.5f;memcpy(logo+0x1C8,&timer,4);c={};c.rcx=(uintptr_t)logo;
 OnLogoExecute(&c);assert(Rd32((uintptr_t)logo+0x1C8)==0&&!g_titlePrefetch.attempted);
 g_trimCreditsPrefetch=true;assert(!IsInitialCreditsTrimTitle(t));assert(!ShouldOmitStartupScene(&c));g_trimCreditsPrefetch=false;
 Wr32(u+0xEC,2);user[0x524+8]=0xAA;
 Wr32(t+0x74,4);Wr32(t+0xB4,1);Wr32(u+0x1F980+0x50,6);
 timer=1.5f;memcpy((void*)(u+0x1F980+8),&timer,4);c={};c.rcx=t;c.rax=4;
 OnTitleStage(&c);assert(Rd32(u+0x1F980+8)==0&&Rd32(t+0x74)==4&&c.rax==4);
 assert(Rd32(u+0xEC)==2&&user[0x524+8]==0xAA&&!g_dlcEnumerated&&!g_titleContinued);
 Wr32(t+0xB4,2);OnTitleStage(&c);assert(c.rax==4&&Rd32(t+0x74)==4&&Rd32(u+0xEC)==2);
 CheckSites(false,false);assert(g_startupProfileFlushed);
 // Install the actual title-only hook set in a private fake module. Optional
 // scene/credits CALL sites and quest/DLC instructions must remain unmodified.
 auto dos=(IMAGE_DOS_HEADER*)image;dos->e_lfanew=0x80;
 auto nt=(IMAGE_NT_HEADERS64*)(image+0x80);nt->OptionalHeader.SizeOfImage=0x51C5000;
 const unsigned char logoPrepare[]={0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x74,0x24,0x20};
 const unsigned char titlePrepare[]={0x48,0x89,0x5C,0x24,0x20,0x55,0x57,0x41,0x57};
 memcpy(image+0x24C4E0,logoPrepare,sizeof(logoPrepare));memcpy(image+0x25DA80,titlePrepare,sizeof(titlePrepare));
 memcpy(image+kLogoExecute,kLogoExecuteExpected,sizeof(kLogoExecuteExpected));
 memcpy(image+kLogoStart,kLogoStartExpected,sizeof(kLogoStartExpected));memcpy(image+kTitleStage,kStageExpected,sizeof(kStageExpected));
 memcpy(image+kTitleSceneRequest,kTitleSceneRequestExpected,5);
 const unsigned char submit[]={0xE8,0x3D,0x2F,0x92,0x01},create[]={0xE8,0xFD,0x12,0x82,0};
 memcpy(image+0x25DD2E,submit,5);memcpy(image+0x25D67E,create,5);
 assert(InstallHookOnce((HMODULE)image));
 assert(g_titleHookInstalled&&g_logoHookInstalled&&g_logoExecuteHookInstalled);
 assert(!g_sceneRequestHookInstalled&&!g_sceneRequestHookTried&&!g_creditsGatesTried&&!g_trimCreditsPrefetch);
 assert(!memcmp(image+kTitleSceneRequest,kTitleSceneRequestExpected,5)&&!memcmp(image+0x25DD2E,submit,5)&&!memcmp(image+0x25D67E,create,5));
 CheckSites(false,false);

 // Now populate all remaining optional + new hook sites and verify ALL 30 hooks
 // install cleanly inside the 0x2000-byte hook arena!
 g_cfg.title_screen_only = false;
 const unsigned char fontRefresh[] = {0xE8,0xC3,0xF1,0xB1,0x00};
 const unsigned char fontWarm[]    = {0xE8,0x29,0xE7,0xC1,0x00};
 const unsigned char fontWarmDf[]  = {0xE8,0x95,0x06,0xA0,0x00};
 const unsigned char fontCtor[]    = {0xE8,0xCE,0xB1,0xFE,0xFF};
 const unsigned char contentCall[] = {0xE8,0xF0,0x00,0x00,0x00};
 const unsigned char psoCall[]     = {0xE8,0x0E,0xFB,0x8B,0x00};
 const unsigned char shaderCall[]  = {0xE8,0x36,0xD5,0xFF,0xFF};
 const unsigned char dx12FreeCall[]= {0xE8,0x05,0xE5,0x13,0x00};
 const unsigned char playableP[]   = {0x40,0x55,0x56,0x41,0x56};
 const unsigned char villageP[]    = {0x40,0x53,0x48,0x83,0xEC,0x30,0x48,0x8B,0xD9};
 const unsigned char stageP[]      = {0x48,0x89,0x5C,0x24,0x18,0x55,0x57,0x41,0x57};
 const unsigned char stageInitP[]  = {0x40,0x55,0x41,0x56,0x48,0x8D,0x6C,0x24,0xB1};
 memcpy(image + 0xFBA638, fontRefresh, 5);
 memcpy(image + 0x1AD9952, fontWarm, 5);
 memcpy(image + 0x1AD99F6, fontWarmDf, 5);
 memcpy(image + 0x1AEE62D, fontCtor, 5);
 memcpy(image + kUserDataUnpackRva, kUserDataUnpackExpected, sizeof(kUserDataUnpackExpected));
 memcpy(image + kContentCheckSaveCallRva, contentCall, 5);
 memcpy(image + kDx11PsoLoadCallRva, psoCall, 5);
 memcpy(image + kDx11PredAllocRva, kDx11PredAllocExpected, sizeof(kDx11PredAllocExpected));
 memcpy(image + kDx11PredInitRva, kDx11PredInitExpected, sizeof(kDx11PredInitExpected));
 memcpy(image + kDx11SetShadersRva, kDx11SetShadersExpected, sizeof(kDx11SetShadersExpected));
 memcpy(image + kDx11ShaderCleanupRva, kDx11ShaderCleanupExpected, sizeof(kDx11ShaderCleanupExpected));
 memcpy(image + kDx11ShaderBuildCallRva, shaderCall, 5);
 memcpy(image + kDx12TexFreeCallRva, dx12FreeCall, 5);
 memcpy(image + kDx12TexCreateRva, kDx12TexCreateExpected, sizeof(kDx12TexCreateExpected));
 memcpy(image + kDx12BufReleaseRva, kDx12BufReleaseExpected, sizeof(kDx12BufReleaseExpected));
 memcpy(image + kDx12IbCreateRva, kDx12BufCreateR13Expected, sizeof(kDx12BufCreateR13Expected));
 memcpy(image + kDx12VbCreateRva, kDx12BufCreateR13Expected, sizeof(kDx12BufCreateR13Expected));
 memcpy(image + kDx12SbCreateRva, kDx12BufCreateR12Expected, sizeof(kDx12BufCreateR12Expected));
 memcpy(image + 0x2505D0, playableP, sizeof(playableP));
 memcpy(image + 0x24F340, villageP, sizeof(villageP));
 memcpy(image + 0x24AE70, stageP, sizeof(stageP));
 memcpy(image + 0x253560, stageInitP, sizeof(stageInitP));

 // Verify dx11_fast=0 and dx12_fast=0 skip installing all DX11/DX12 hooks and leave bytes untouched.
 g_cfg.dx11_fast = false;
 g_cfg.dx12_fast = false;
 InstallDx11FastHooks();
 InstallDx12FastHooks();
 assert(!g_dx11PsoHookInstalled && !g_dx11PredAllocHookInstalled && !g_dx11PredInitHookInstalled &&
        !g_dx11SetShadersHookInstalled && !g_dx11ShaderCleanupHookInstalled && !g_dx11ShaderBuildHookInstalled);
 assert(!g_dx12TexFreeHookInstalled && !g_dx12TexCreateHookInstalled &&
        !g_dx12BufReleaseHookInstalled && !g_dx12IbCreateHookInstalled &&
        !g_dx12VbCreateHookInstalled && !g_dx12SbCreateHookInstalled);
 assert(!memcmp(image + kDx11PsoLoadCallRva, psoCall, 5));
 assert(!memcmp(image + kDx12TexCreateRva, kDx12TexCreateExpected, sizeof(kDx12TexCreateExpected)));
 assert(!memcmp(image + kDx12IbCreateRva, kDx12BufCreateR13Expected, sizeof(kDx12BufCreateR13Expected)));
 g_dx11FastHooksTried = false;
 g_dx12FastHooksTried = false;
 g_cfg.dx11_fast = true;
 g_cfg.dx12_fast = true;

 assert(InstallHookOnce((HMODULE)image));
 assert(g_sceneRequestHookInstalled && g_trimCreditsPrefetch);
 assert(g_fontHooks[0] && g_fontHooks[1] && g_fontHooks[2] && g_fontCtorHookInstalled);
 assert(g_saveParallelHookInstalled && g_contentSkipHookInstalled);
 assert(g_dx11PsoHookInstalled && g_dx11PredAllocHookInstalled && g_dx11PredInitHookInstalled);
 assert(g_dx11SetShadersHookInstalled && g_dx11ShaderCleanupHookInstalled && g_dx11ShaderBuildHookInstalled);
 assert(g_dx12TexFreeHookInstalled && g_dx12TexCreateHookInstalled);
 assert(g_dx12BufReleaseHookInstalled && g_dx12IbCreateHookInstalled &&
        g_dx12VbCreateHookInstalled && g_dx12SbCreateHookInstalled);
 assert(g_postEntryHooksInstalled[0] && g_postEntryHooksInstalled[1] &&
        g_postEntryHooksInstalled[2] && g_postEntryHooksInstalled[3]);

 // Unit-test DX12 placed-heap free-list coalescing & handle packing.
 {
     std::vector<Dx12FreeSpan> fl{{0, 1024}};
     uint32_t a = 0, b = 0, cOff = 0;
     assert(Dx12PlacedTexPool::TakeFromFreeList(fl, 100, a) && a == 0);
     assert(Dx12PlacedTexPool::TakeFromFreeList(fl, 200, b) && b == 100);
     assert(Dx12PlacedTexPool::TakeFromFreeList(fl, 50, cOff) && cOff == 300);
     Dx12PlacedTexPool::InsertAndCoalesce(fl, 0, 100);
     Dx12PlacedTexPool::InsertAndCoalesce(fl, 300, 50);
     assert(fl.size() == 2 && fl[0].off == 0 && fl[0].len == 100 && fl[1].off == 300 && fl[1].len == 724);
     Dx12PlacedTexPool::InsertAndCoalesce(fl, 100, 200);
     assert(fl.size() == 1 && fl[0].off == 0 && fl[0].len == 1024);
     const uint64_t h = Dx12PlacedTexPool::PackHandle(7, 300, 64);
     assert((h & kDx12PlacedTagBit) != 0);
     assert(((h >> 32) & 0xFFFFu) == 7 && ((h >> 16) & 0xFFFFu) == 300 && (h & 0xFFFFu) == 64);
 }

 // Unit-test DX11 lazy predicate skip and BuildDx11UsedShaderMask.
 {
     std::vector<unsigned char> fakeD3d(0x42000, 0xCC);
     hook::Ctx pc{};
     pc.rsi = reinterpret_cast<uintptr_t>(fakeD3d.data());
     pc.rbx = pc.rsi + 0x1688;
     pc.r15 = pc.rsi + 0x41688;
     HandleDx11PredicateInit(&pc);
     assert(pc.rbx == pc.r15);
     assert(fakeD3d[0x1688] == 0 && fakeD3d[0x41687] == 0 && fakeD3d[0x1687] == 0xCC);

     unsigned char rShader[0x100]{};
     unsigned char techs[2 * 0x20]{};
     std::memset(techs, 0xFF, sizeof(techs));
     const char* dx11Name = "NormalTech";
     const char* dx12Name = "SomeTech__d3d12_binary";
     WP(reinterpret_cast<uintptr_t>(techs), reinterpret_cast<uintptr_t>(dx11Name));
     *reinterpret_cast<uint16_t*>(techs + 0x10) = 3;
     WP(reinterpret_cast<uintptr_t>(techs + 0x20), reinterpret_cast<uintptr_t>(dx12Name));
     *reinterpret_cast<uint16_t*>(techs + 0x20 + 0x10) = 5;
     Wr32(reinterpret_cast<uintptr_t>(rShader) + 0xF4, 2);
     WP(reinterpret_cast<uintptr_t>(rShader) + 0xB8, reinterpret_cast<uintptr_t>(techs));
     std::vector<uint8_t> mask;
     BuildDx11UsedShaderMask(reinterpret_cast<uintptr_t>(rShader), 8, mask);
     assert(mask.size() == 8 && mask[3] == 1 && mask[5] == 0 && mask[0] == 0);
 }

 VirtualFree(image,0,MEM_RELEASE);
 puts("PASS: actual INI parsing/defaults, first entry retained, both return policies, soft re-entry, unknown-edit refusal.");
 puts("PASS: title-only timers without resource/selection/quest changes; real optional hooks absent; disabled behavior and mode precedence.");
 puts("PASS: all 30 startup + post-entry + DX11 lazy + DX12 placed-heap hooks fit cleanly within the 0x2000-byte hook arena.");
}
