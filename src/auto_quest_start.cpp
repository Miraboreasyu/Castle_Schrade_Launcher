// Castle Schrade Launcher: native startup, saved gathering settings and quest entry.
// Version-specific in-memory patches; no IN-Q bridge or loadout customization.
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "hook.h"

namespace {

// MHW 15.23 / Steam build 15539686. All addresses are RVAs.
constexpr uintptr_t kOnePhaseJump = 0x25FF03;
constexpr uintptr_t kQuestId1 = 0x25FF0D;
constexpr uintptr_t kQuestId2 = 0x25FF17;
constexpr uintptr_t kTitleStage = 0x25E0D7;
constexpr uintptr_t kLogoExecute = 0x24C740;
constexpr unsigned char kLogoExecuteExpected[] = {
    0x4C, 0x8B, 0xDC,             // mov r11,rsp
    0x56,                         // push rsi
    0x48, 0x81, 0xEC, 0xA0, 0, 0, 0, // sub rsp,0xA0
};
constexpr uintptr_t kDlcInitCall = 0x25FD83;
constexpr uintptr_t kPSave = 0x5013950;
constexpr uintptr_t kPNet = 0x51C46B8;
constexpr uintptr_t kPArea = 0x51C42B8;
constexpr uintptr_t kHubDestinationTable = 0x30AC950;
constexpr uintptr_t kSaveStartAreaPreference = 0x2691A4;
constexpr uintptr_t kSavePreviousArea = 0x269150;
constexpr uintptr_t kPendingArea = 0xD344;
constexpr uint64_t kSaveSlotSize = 0x26CC00;
constexpr unsigned char kOnePhaseExpected[] = {0x75, 0x18};
constexpr unsigned char kOnePhasePatched[] = {0x90, 0x90};
constexpr unsigned char kQuestIdExpected[] = {1, 0, 0, 0};
constexpr unsigned char kDlcInitExpected[] = {0xE8, 0xA8, 0x73, 0x67, 0x01};
constexpr unsigned char kDlcInitPatched[] = {0x90, 0x90, 0x90, 0x90, 0x90};
constexpr unsigned char kStageExpected[] = {
    0x48, 0x89, 0x5C, 0x24, 0x78, 0x48, 0x89, 0x74, 0x24, 0x60,
};
constexpr uintptr_t kLogoStart       = 0x1E7E890; // uGUILogo matching start-event handler
constexpr unsigned char kLogoStartExpected[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, // mov [rsp+8],rbx
    0x57,                         // push rdi
    0x48, 0x83, 0xEC, 0x20,       // sub rsp,20
};

struct Config {
    bool enabled = true;
    bool title_screen_only = false;
    bool title_only_hub = false;
    bool rehook_on_return = true;
    int save_slot = 0;
    int quest_id = 66845;
};
Config g_cfg;
uintptr_t g_exeBase = 0;
bool g_titleHookInstalled = false;
bool g_logoExecuteHookInstalled = false;
bool g_startupLogoDone = false;
bool g_dlcEnumerated = false;
bool g_dlcTried = false;
bool g_titleContinued = false;
uint64_t g_titleFirstSeen = 0;
bool g_returnTitlePolicyApplied = false;
bool g_returnTitleRestoreWarning = false;

void LogLine(const char* fmt, ...) {
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LogLine), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "auto_quest_start.log");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(f, "[%llu ms] ", static_cast<unsigned long long>(GetTickCount64()));
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
}

std::string DllDir() {
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LogLine), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *slash = '\0';
    return path;
}

std::string Trim(std::string s) {
    const char* ws = " \t\r\n";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

int IniGet(const std::string& path, const char* section, const char* key, int def) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return def;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string data;
    if (n > 0) {
        data.resize(static_cast<size_t>(n));
        fread(&data[0], 1, static_cast<size_t>(n), f);
    }
    fclose(f);
    size_t pos = 0;
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        pos = 3;
    }
    bool in_section = false;
    const std::string want_section = std::string("[") + section + "]";
    while (pos < data.size()) {
        size_t e = data.find('\n', pos);
        if (e == std::string::npos) e = data.size();
        std::string line = Trim(data.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            in_section = (line == want_section);
            continue;
        }
        if (!in_section) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (Trim(line.substr(0, eq)) != key) continue;
        return atoi(line.c_str() + eq + 1);
    }
    return def;
}

Config LoadConfig() {
    Config c;
    const std::string ini = DllDir() + "\\auto_quest_start.ini";
    c.enabled = IniGet(ini, "auto_quest_start", "enabled", 1) != 0;
    if (!c.enabled) return c;
    // Old INIs keep their title-screen-only behavior unless the new key is set.
    const int legacyTitleOnly = IniGet(ini, "auto_quest_start", "title_only", 0);
    c.title_screen_only = IniGet(ini, "auto_quest_start", "title_screen_only", legacyTitleOnly) != 0;
    c.title_only_hub = IniGet(ini, "auto_quest_start", "title_only_hub", 0) != 0;
    c.rehook_on_return = IniGet(ini, "auto_quest_start", "rehook_on_return", 1) != 0;
    c.save_slot = IniGet(ini, "auto_quest_start", "save_slot", 0);
    c.quest_id = IniGet(ini, "auto_quest_start", "quest_id", 66845);
    if (c.save_slot < 0 || c.save_slot > 2) {
        LogLine("config: invalid save_slot=%d; using slot 0", c.save_slot);
        c.save_slot = 0;
    }
    if (c.quest_id < 101 || c.quest_id > 67809) {
        LogLine("config: invalid quest_id=%d; using 66845", c.quest_id);
        c.quest_id = 66845;
    }
    LogLine("config: enabled=1 title_screen_only=%u title_only_hub=%u rehook_on_return=%u save_slot=%d quest_id=%d",
            c.title_screen_only ? 1u : 0u, c.title_only_hub ? 1u : 0u,
            c.rehook_on_return ? 1u : 0u, c.save_slot, c.quest_id);
    return c;
}

bool Readable(uintptr_t a, size_t n) {
    if (a < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return a + n <= end;
}

uintptr_t P(uintptr_t a) {
    uintptr_t v = 0;
    if (Readable(a, sizeof(v))) memcpy(&v, reinterpret_cast<const void*>(a), sizeof(v));
    return v;
}

uint32_t Rd32(uintptr_t a) {
    uint32_t v = 0;
    if (Readable(a, sizeof(v))) memcpy(&v, reinterpret_cast<const void*>(a), sizeof(v));
    return v;
}

bool Wr32(uintptr_t a, uint32_t v) {
    if (!Readable(a, sizeof(v))) return false;
    memcpy(reinterpret_cast<void*>(a), &v, sizeof(v));
    return true;
}

void ApplySavedHubDestination() {
    if (!g_cfg.title_only_hub) return;
    const uintptr_t user = P(g_exeBase + kPSave);
    const uintptr_t area = P(g_exeBase + kPArea);
    if (!user || !area) return;
    const uintptr_t save = reinterpret_cast<uintptr_t (*)(uintptr_t)>(
        g_exeBase + 0x1B8DBB0)(user);
    if (!save || !Readable(save + kSaveStartAreaPreference, 4)) return;
    const uint32_t preference = Rd32(save + kSaveStartAreaPreference);
    // Mirror native menu FUN_140ACC140, which we bypass when creating a hub.
    // Six entries: previous area, then five explicit destinations. Entry zero
    // maps to zero so aTitle retains its saved-area fallback. Do not edit the
    // save or bypass aTitle's progression and destination checks.
    if (preference >= 6) {
        LogLine("hub-area: invalid saved preference=%u; retaining native destination", preference);
        return;
    }
    const uintptr_t entry = g_exeBase + kHubDestinationTable + preference * 12;
    if (!Readable(entry, 4)) return;
    const uint32_t destination = Rd32(entry);
    if (Wr32(area + kPendingArea, destination)) {
        LogLine("hub-area: saved preference=%u pending=0x%X previous=0x%X",
                preference, destination, Rd32(save + kSavePreviousArea));
    }
}

// Only shorten a positive finite transition timer. Leave state writes, fade
// callbacks and asynchronous readiness checks to the original update function.
void ClearTransitionWait(uintptr_t timer, const char* phase) {
    if (!Readable(timer, sizeof(float))) return;
    float seconds = 0;
    memcpy(&seconds, reinterpret_cast<const void*>(timer), sizeof(seconds));
    if (!(seconds > 0.0f && seconds <= 10.0f)) return;
    if (Wr32(timer, 0)) LogLine("timer-skip: %s removed %.3f s", phase, seconds);
}

// Save-operation +38 is a 30-second error timeout: NEVER change it.
// +3C is a separate minimum display timer. Only shorten it after native
// completion polling has advanced to the result phase and I/O succeeded.
void ClearCompletedSaveWait(uintptr_t operation, const char* phase) {
    if (!Readable(operation, 0x40)) return;
    const uint32_t kind = Rd32(operation + 8);
    bool atResult = false;
    if (kind == 0 || kind == 6) atResult = Rd32(operation + 0x24) == 2;
    else if (kind == 1) atResult = Rd32(operation + 0x28) == 2;
    else if (kind == 2) atResult = Rd32(operation + 0x2C) == 2;
    else if (kind == 3 || kind == 4) atResult = Rd32(operation + 0x30) == 6;
    if (!atResult) return; // operation 5 has multiple phases; not covered here
    const uintptr_t user = P(g_exeBase + kPSave);
    if (!user || !Readable(user + 0x1F8D0, 8) ||
        Rd32(user + 0x1F8D0) == 1 || Rd32(user + 0x1F8D4) != 1) return;
    ClearTransitionWait(operation + 0x3C, phase);
}

void PollTitlePrefetch();
void HandleTitleReturnPrepare(uintptr_t title);
void ApplyReturnTitlePolicy();
void AdvanceResidentBuild(uintptr_t logo, uint32_t logoState);
void CloseStartupFontWindow();
#include "startup_profile.h"
#include "startup_resident_budget.h"
#include "startup_font.h"
#include "startup_prefetch.h"
#include "startup_scene_skip.h"
#include "startup_credits_trim.h"

void OnLogoExecute(hook::Ctx* c) {
    if (!g_cfg.enabled || g_startupLogoDone) return;
    const uintptr_t logo = static_cast<uintptr_t>(c->rcx);
    if (!logo || !Readable(logo + 0x50, 4)) return;
    const uint32_t state = Rd32(logo + 0x50);
    if (!g_cfg.title_screen_only) {
        if (state == 0x16) StopTitlePrefetchAttempts();
        else StartTitlePrefetch();
        PollTitlePrefetch();
    }
    if (state == 0x16) {
        if (Rd32(logo + 0xA0) == 2) g_fontStartupWindow = true;
        ProfileStage(2, "font-prepare", Rd32(logo + 0xA0));
        if (Rd32(logo + 0xA0) >= 1) ProfileTicket(2, "font", Rd32(logo + 0x4C));
    }
    static uint32_t previous = ~0u;
    if (state != previous) {
        previous = state;
        LogLine("boot-trace: aLogo state=0x%X", state);
    }
    if (state == 0x17)
        ClearTransitionWait(logo + 0x1C8, "aLogo transition");
    // Only inspect the operation while its owning aLogo case is polling it.
    const bool polling = (state == 5 && (Rd32(logo + 0x58) == 5 || Rd32(logo + 0x58) == 8)) ||
        (state == 6 && Rd32(logo + 0x5C) == 2) ||
        (state == 7 && Rd32(logo + 0x60) == 2) ||
        (state == 8 && (Rd32(logo + 0x64) == 2 || Rd32(logo + 0x64) == 6)) ||
        (state == 0x14 && Rd32(logo + 0x98) == 1);
    if (polling) ClearCompletedSaveWait(logo + 0xA8, "aLogo completed save minimum");
}

// Automate the title's native Continue and session-start operations. Keep the
// game's asynchronous session driver running; never fabricate a Session/DB.
void OnTitleStage(hook::Ctx* c) {
    if (!g_cfg.enabled) return;
    if (!g_startupLogoDone) FinishTitlePrefetch();
    g_startupLogoDone = true;
    const uintptr_t t = static_cast<uintptr_t>(c->rcx);
    if (!t || !Readable(t + 0x74, 4)) return;
    const uint32_t st = Rd32(t + 0x74);
    // PREPARE normally restores the return policy first. Cover soft title
    // re-entry too, even if the engine reuses the same aTitle object.
    if (g_titleContinued && st <= 8) ApplyReturnTitlePolicy();
    static uint32_t previousState = ~0u;
    static uint32_t previousMenu = ~0u;
    const uint32_t menu = Rd32(t + 0xB4);
    if (st != previousState || (st == 4 && menu != previousMenu)) {
        previousState = st;
        previousMenu = menu;
        LogLine("boot-trace: aTitle state=0x%X menu=%u", st, menu);
    }
    if (st == 4 && menu == 1) {
        const uintptr_t user = P(g_exeBase + kPSave);
        const uintptr_t content = user ? user + 0x1F980 : 0;
        if (content && Readable(content, 0x1555)) {
            const uint32_t contentState = Rd32(content + 0x50);
            const uint8_t alternate = *reinterpret_cast<const uint8_t*>(content + 0x1554);
            static uint32_t previousContent = ~0u;
            const uint32_t key = contentState | (static_cast<uint32_t>(alternate) << 24);
            if (key != previousContent) {
                previousContent = key;
                LogLine("boot-trace: title content state=%u alternate=%u", contentState, alternate);
            }
            // 1402999D0 -> 14029AD00 state6, or 14029B040 byte-state2.
            // Both consume +8 as a 1.5-second message hold, before native work.
            if (contentState == 6 || alternate == 2)
                ClearTransitionWait(content + 8, "title content message minimum");
            // 14029AFA0 owns the embedded save operation at +10.
            if (Rd32(content + 0xC) == 2)
                ClearCompletedSaveWait(content + 0x10, "title content completed save minimum");
        }
    }
    if (st == 0x18) ClearTransitionWait(t + 0x320, "aTitle transition");
    if (g_cfg.title_screen_only) {
        if (st == 1 || st == 5 || st == 6 || (st == 4 && menu == 2)) {
            static bool readyLogged = false;
            if (!readyLogged) {
                readyLogged = true;
                LogLine("title-only: native title boot ready; waiting for player input");
                FlushStartupProfile();
            }
        }
        return;
    }
    if (g_titleContinued) return;
    if (WaitForCreditsRestore(c, t, st)) return;
    ObserveCreditsTrim(t, st);
    ObserveOmittedTitleScene(t, st);
    if (st == 0x18 || st == 0x12) return;   // transition / post-continue state
    // Preserve native startup initialization before selecting the save.
    if (st < 4) return;

    if (!g_dlcEnumerated) {
        const uintptr_t save = P(g_exeBase + kPSave);
        if (save && Readable(save + 0x524 + 8, 1)) {
            // FUN_1418D7130 zeros the 0x200 DLC-content flags and repopulates
            // them from sDLC.  The save's old flags include paid content
            // (emotes), so save them and OR them back afterwards.
            uint8_t oldFlags[0x200];
            for (int i = 0; i < 0x200; ++i) {
                const uintptr_t a = save + 0x524 + 8 + i;
                oldFlags[i] = Readable(a, 1) ? *reinterpret_cast<uint8_t*>(a) : 0;
            }
            const auto fn = reinterpret_cast<void (*)(void*)>(g_exeBase + 0x18D7130);
            fn(reinterpret_cast<void*>(save + 0x524));
            uint32_t preserved = 0;
            for (int i = 0; i < 0x200; ++i) {
                if (!oldFlags[i]) continue;
                const uintptr_t a = save + 0x524 + 8 + i;
                if (Readable(a, 1)) {
                    *reinterpret_cast<uint8_t*>(a) |= oldFlags[i];
                    ++preserved;
                }
            }
            LogLine("title: DLC init with flag merge (%u old flags preserved)", preserved);
            g_dlcEnumerated = true;
            g_dlcTried = true;
        } else {
            if (!g_titleFirstSeen) {
                g_titleFirstSeen = GetTickCount64();
                LogLine("title: waiting for sUserData/DLC content table");
            }
            if (GetTickCount64() - g_titleFirstSeen < 10000) return;
            if (!g_dlcTried) {
                g_dlcTried = true;
                LogLine("title: DLC table still unavailable after 10 s; forcing Continue");
            }
        }
    }

    static bool continueSelected = false;
    static bool nativeNetworkInit = false;
    static bool hubCreateRequested = false;
    static bool sessionWaitLogged = false;
    static uint64_t lastSessionTrace = 0;
    using VoidTitleFn = void (*)(uintptr_t);
    using NativeTransitionFn = void (*)(uintptr_t, uint32_t, float, float, uint32_t, float, uint8_t);

    // aTitle::execute has already loaded its switch state into RAX here.
    // 0x1A is beyond its last case (0x19), so this skips
    // ONLY a native call that we have already dispatched, not the game's
    // independent network/resource update loops.
    auto skipDuplicateTitleCall = [c]() { c->rax = 0x1A; };

    // Wait for native title boot, then select the explicitly configured
    // slot with the game's own selector. Never silently switch to another
    // save: an unoccupied configured slot is reported and left untouched.
    if (st == 4 && !continueSelected && Rd32(t + 0xB4) == 2) {
        const uintptr_t user = P(g_exeBase + kPSave);
        const uintptr_t saves = user ? P(user + 0xA8) : 0;
        const uint32_t slot = static_cast<uint32_t>(g_cfg.save_slot);
        const uintptr_t record = saves ? saves + slot * kSaveSlotSize : 0;
        const bool occupied = record && Readable(record + 0x38, 1) &&
            (*reinterpret_cast<volatile uint8_t*>(record + 0x38) & 1) != 0;
        if (occupied) {
            reinterpret_cast<void (*)(uintptr_t, uint32_t)>(
                g_exeBase + 0x1B8F190)(user, slot);
            Wr32(t + 0x7C, 2);
            reinterpret_cast<NativeTransitionFn>(g_exeBase + 0x25C5C0)(
                t, 0xF, 0.0f, 0.0f, 0, 0.0f, 0);
            continueSelected = true;
            skipDuplicateTitleCall();
            LogLine("title-fp: configured save selected after native title boot (slot=%u)", slot);
        } else {
            static bool saveSlotWarningLogged = false;
            if (!saveSlotWarningLogged) {
                saveSlotWarningLogged = true;
                LogLine("title-fp: configured save slot %u is not occupied/ready; no fallback slot selected (saves=%p)",
                        slot, reinterpret_cast<void*>(saves));
            }
        }
        return;
    }

    // Case 4 performs selected-save/TUS initialization. Once that native
    // stage has run, use the actual session-create entry with its normal
    // hub arguments (net, 1, null callback, null callback context).
    // Its own entry guard accepts service states 2, 4 and 5 only. Waiting
    // for that guard avoids the old invalid-state call and fixed sleeps.
    if (st == 0xF && continueSelected) {
        const uint32_t substate = Rd32(t + 0xD0);
        if (substate == 4 && !nativeNetworkInit) {
            reinterpret_cast<VoidTitleFn>(g_exeBase + 0x261180)(t);
            nativeNetworkInit = true;
            skipDuplicateTitleCall();
            LogLine("title-fp: native selected-save/network base initialization requested");
        } else if (substate >= 5 && nativeNetworkInit && !hubCreateRequested) {
            const uintptr_t net = P(g_exeBase + kPNet);
            if (net && Rd32(net + 0x51AD4) != 4) {
                const auto serviceState = reinterpret_cast<int (*)(
                    uintptr_t, uint32_t, uint32_t)>(g_exeBase + 0x22AD900);
                const int service = serviceState(net, 0, 0);
                if (service == 2 || service == 4 || service == 5) {
                    // Use the native saved-lobby-settings loader, rather than
                    // guessing a privacy field. With scratch+0x250 == 0 its
                    // only external input is getSelectedSave(); it writes the
                    // 0x20-byte settings block at scratch+0x230 and never
                    // dereferences the scratch GUI vtable or parent pointer.
                    alignas(16) uint8_t savedLobbyUi[0x258] = {};
                    reinterpret_cast<uint8_t* (*)(uint8_t*)>(
                        g_exeBase + 0x18CA820)(savedLobbyUi + 0x230);
                    reinterpret_cast<void (*)(uint8_t*)>(
                        g_exeBase + 0x0620C30)(savedLobbyUi);
                    reinterpret_cast<void (*)(uintptr_t, uint8_t*, uint8_t, uint8_t, uint8_t)>(
                        g_exeBase + 0x1B0F5A0)(net, savedLobbyUi + 0x230, 0, 1, 0);

                    const auto createHub = reinterpret_cast<char (*)(
                        uintptr_t, uint32_t, uintptr_t, uintptr_t)>(g_exeBase + 0x1AFE690);
                    ApplySavedHubDestination();
                    const char accepted = createHub(net, 1, 0, 0);
                    LogLine("title-fp: native hub create(net,1,0,0) returned %d (service=%d saved-lobby-settings=1)",
                            static_cast<int>(accepted), service);
                    if (accepted) {
                        Wr32(t + 0x74, 0x13);
                        hubCreateRequested = true;
                        skipDuplicateTitleCall();
                    }
                } else {
                    // Suppress only the title's menu case while the native
                    // service reaches the create function's own precondition.
                    skipDuplicateTitleCall();
                }
            }
        }
        return;
    }

    // Native state 0x13 waits for mSelfIndex's member-ready value to become
    // 1 (net+0x549F4[selfIndex*4]). Its original transition additionally
    // requires the online attribute. For a genuinely offline Session, keep
    // the native wait but do not invent that attribute. Network errors are
    // left to the original title error handler.
    if (st == 0x13 && continueSelected && nativeNetworkInit) {
        const uintptr_t net = P(g_exeBase + kPNet);
        const uintptr_t session = net ? P(net + 0x260) : 0;
        const uintptr_t db = session ? P(session + 0x10) : 0;
        int self = -1;
        uint32_t ready = 0;
        uint32_t attribute = 0;
        if (net && db && Readable(db + 0xBFEC, 8)) {
            self = static_cast<int>(Rd32(db + 0xBFEC));
            attribute = Rd32(db + 0xBFF0);
            if (self >= 0 && self < 16) ready = Rd32(net + 0x549F4 + self * 4);
        }
        if (ready == 1 && (attribute & 1) == 0) {
            reinterpret_cast<NativeTransitionFn>(g_exeBase + 0x25C5C0)(
                t, 0x11, 0.0f, 0.0f, 0, 0.0f, 0);
            *reinterpret_cast<volatile uint8_t*>(t + 0x389) = 1;
            skipDuplicateTitleCall();
            LogLine("title-fp: offline hub Session ready; native quest transition requested");
            return;
        }
        if (ready != 1) {
            const auto hasError = reinterpret_cast<bool (*)(uintptr_t, uint32_t)>(
                g_exeBase + 0x1B000A0);
            const bool error = net && (hasError(net, 1) ||
                *reinterpret_cast<volatile uint8_t*>(net + 0x51BF8) != 0);
            if (!error) {
                const uint64_t now = GetTickCount64();
                if (!sessionWaitLogged || now - lastSessionTrace >= 5000) {
                    sessionWaitLogged = true;
                    lastSessionTrace = now;
                    LogLine("title-fp: waiting for native hub Session (self=%d ready=%u attr=0x%X driver=%d op=%d)",
                            self, ready, attribute,
                            session ? static_cast<int>(Rd32(session + 0x168)) : -1,
                            net ? static_cast<int>(Rd32(net + 0x53E68)) : -1);
                }
                skipDuplicateTitleCall();
                return;
            }
        }
        return;
    }
    if (st == 0x11) {
        g_titleContinued = true;
        LogLine("title-fp: native %s-entry state reached", g_cfg.title_only_hub ? "hub" : "quest");
        FlushStartupProfile();
    }
    return;
}

bool PatchBytes(HMODULE exe, uintptr_t rva, const unsigned char* expected,
                const unsigned char* patched, size_t len) {
    auto* p = reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(exe) + rva);
    if (!Readable(reinterpret_cast<uintptr_t>(p), len)) return false;
    if (memcmp(p, patched, len) == 0) return true;
    if (memcmp(p, expected, len) != 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) {
        LogLine("VirtualProtect failed at %p (err %lu)", p, GetLastError());
        return false;
    }
    memcpy(p, patched, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    LogLine("patched %p", p);
    return true;
}

bool PatchAll(const Config& cfg, HMODULE exe) {
    if (!cfg.enabled || cfg.title_screen_only) return true;
    // OnTitleStage performs this initialization with preservation of DLC flags.
    if (!PatchBytes(exe, kDlcInitCall, kDlcInitExpected, kDlcInitPatched,
                    sizeof(kDlcInitPatched))) return false;
    // Hub mode uses the same accelerated save/session startup but retains the
    // native progression/hub dispatch instead of redirecting it to a quest.
    if (cfg.title_only_hub) return true;
    if (!PatchBytes(exe, kOnePhaseJump, kOnePhaseExpected, kOnePhasePatched,
                    sizeof(kOnePhasePatched))) return false;
    unsigned char id[4];
    const uint32_t qid = static_cast<uint32_t>(cfg.quest_id);
    memcpy(id, &qid, sizeof(id));
    return PatchBytes(exe, kQuestId1, kQuestIdExpected, id, sizeof(id)) &&
           PatchBytes(exe, kQuestId2, kQuestIdExpected, id, sizeof(id));
}

void ApplyReturnTitlePolicy() {
    if (!g_cfg.enabled || g_cfg.title_screen_only || !g_titleContinued || g_returnTitlePolicyApplied) return;
    unsigned char id[4];
    const uint32_t qid = static_cast<uint32_t>(g_cfg.quest_id);
    memcpy(id, &qid, sizeof(id));
    const uintptr_t rvas[] = {kDlcInitCall,kOnePhaseJump,kQuestId1,kQuestId2};
    const unsigned char* original[] = {kDlcInitExpected,kOnePhaseExpected,kQuestIdExpected,kQuestIdExpected};
    const unsigned char* applied[] = {kDlcInitPatched,kOnePhasePatched,id,id};
    const size_t lengths[] = {5,2,4,4};
    const bool redirectQuest = g_cfg.rehook_on_return && !g_cfg.title_only_hub;
    const unsigned count = redirectQuest ? 1u : 4u;
    for (unsigned i=0;i<count;++i) {
        const uintptr_t site=g_exeBase+rvas[i];
        if (!Readable(site,lengths[i]) ||
            (memcmp(reinterpret_cast<const void*>(site),applied[i],lengths[i]) &&
             memcmp(reinterpret_cast<const void*>(site),original[i],lengths[i]))) {
            if (!g_returnTitleRestoreWarning) {
                g_returnTitleRestoreWarning=true;
                LogLine("return-title: restore refused; startup site changed at RVA %llX",static_cast<unsigned long long>(rvas[i]));
            }
            return;
        }
    }
    // All four sites share this page. Preflight the complete set, then restore
    // together on the title thread before the player can select a save.
    const uintptr_t page=g_exeBase+0x25F000;
    DWORD old=0;
    if (!VirtualProtect(reinterpret_cast<void*>(page),0x1000,PAGE_EXECUTE_READWRITE,&old)) {
        if (!g_returnTitleRestoreWarning) {
            g_returnTitleRestoreWarning=true;
            LogLine("return-title: could not make startup patch page writable");
        }
        return;
    }
    for (unsigned i=0;i<count;++i)
        memcpy(reinterpret_cast<void*>(g_exeBase+rvas[i]),original[i],lengths[i]);
    DWORD ignored=0;
    VirtualProtect(reinterpret_cast<void*>(page),0x1000,old,&ignored);
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(page),0x1000);
    g_returnTitlePolicyApplied=true;
    LogLine("return-title: %s; native DLC initialization restored",
            redirectQuest ? "save selection still redirects to configured quest" : "native save-to-hub entry restored");
}

void HandleTitleReturnPrepare(uintptr_t title) {
    if (title && Readable(title+0x60,4) && Rd32(title+0x60)==0) ApplyReturnTitlePolicy();
}

bool g_logoHookInstalled = false;

// Called on the GUI thread, after native resource/object/animation setup.
// Consume only the same non-null event that the original handler accepts.
// The original handler still runs, but receives a null event so it cannot
// overwrite stage 5 with stage 1/2 or start an entrance animation. The next
// native logo update performs its complete finished/GUI/texture-state branch.
void OnLogoStart(hook::Ctx* c) {
    if (!g_cfg.enabled) return;
    const uintptr_t gui = static_cast<uintptr_t>(c->rcx);
    const uintptr_t event = static_cast<uintptr_t>(c->rdx);
    if (!gui || !event || !Readable(gui + 0x290, 8) ||
        !P(gui + 0x290) || !Readable(gui + 0x28F0, 0x28)) return;
    const uintptr_t startEvent = reinterpret_cast<uintptr_t (*)(uintptr_t, int)>(
        g_exeBase + 0x2353680)(gui, 5);
    if (!startEvent || event != startEvent) return;
    // All four bindings are installed by native 141E7E670 before activation.
    if (!P(gui + 0x28F8) || !P(gui + 0x2900) ||
        !P(gui + 0x2908) || !P(gui + 0x2910)) return;
    Wr32(gui + 0x28F0, 5);
    c->rdx = 0;
    LogLine("logo-skip: matched native start event; queued native completion (gui=%p)",
            reinterpret_cast<void*>(gui));
}

bool InstallHookOnce(HMODULE exe) {
    if (!g_cfg.enabled) return true;
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    g_exeBase = base;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;

    if (!hook::InitArena(base, end, 0x1000)) {
        LogLine("hook: could not allocate an executable arena near the module");
        return false;
    }

    // Install independently of the later title hook: the worker must not
    // report ready and exit before this earlier startup handler is patched.
    const uintptr_t profileSites[2] = {base + 0x24C4E0, base + 0x25DA80};
    const unsigned char profileBytes[2][10] = {
        {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20},
        {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x57, 0x41, 0x57, 0},
    };
    const size_t profileLengths[2] = {10, 9};
    const hook::Handler profileHandlers[2] = {&OnLogoPrepare, &OnTitlePrepare};
    for (unsigned i = 0; i < 2; ++i) {
        if (g_profileHooksInstalled[i]) continue;
        if (!Readable(profileSites[i], profileLengths[i]) ||
            memcmp(reinterpret_cast<const void*>(profileSites[i]), profileBytes[i], profileLengths[i]) != 0)
            return false;
        if (!hook::InstallCtxHook(profileSites[i], profileLengths[i], profileHandlers[i],
                                  profileBytes[i], profileLengths[i])) return false;
        g_profileHooksInstalled[i] = true;
        LogLine("resource-profile: prepare hook %u installed", i);
    }

    if (!g_logoExecuteHookInstalled) {
        const uintptr_t site = base + kLogoExecute;
        if (!Readable(site, sizeof(kLogoExecuteExpected)) ||
            memcmp(reinterpret_cast<const void*>(site), kLogoExecuteExpected,
                   sizeof(kLogoExecuteExpected)) != 0) return false;
        if (!hook::InstallCtxHook(site, sizeof(kLogoExecuteExpected), &OnLogoExecute,
                                  kLogoExecuteExpected, sizeof(kLogoExecuteExpected))) return false;
        g_logoExecuteHookInstalled = true;
        LogLine("timer-skip: startup aLogo transition hook installed");
    }

    if (!g_logoHookInstalled) {
        const uintptr_t site = base + kLogoStart;
        if (!Readable(site, sizeof(kLogoStartExpected)) ||
            memcmp(reinterpret_cast<const void*>(site), kLogoStartExpected,
                   sizeof(kLogoStartExpected)) != 0) return false;
        if (!hook::InstallCtxHook(site, sizeof(kLogoStartExpected), &OnLogoStart,
                                  kLogoStartExpected, sizeof(kLogoStartExpected))) {
            LogLine("logo-skip: hook installation failed at %p", reinterpret_cast<void*>(site));
            return false;
        }
        g_logoHookInstalled = true;
        LogLine("logo-skip: native start-event hook installed at %p", reinterpret_cast<void*>(site));
    }

    if (!g_titleHookInstalled) {
        const uintptr_t site = base + kTitleStage;
        if (!Readable(site, sizeof(kStageExpected))) return false;
        if (memcmp(reinterpret_cast<const void*>(site), kStageExpected, sizeof(kStageExpected)) != 0) {
            return false;
        }
        if (!hook::InstallCtxHook(site, sizeof(kStageExpected), &OnTitleStage,
                                  kStageExpected, sizeof(kStageExpected))) {
            LogLine("hook: title hook InstallCtxHook failed at %p", reinterpret_cast<void*>(site));
            return false;
        }
        g_titleHookInstalled = true;
        LogLine("title hook installed at %p", reinterpret_cast<void*>(site));
    }

    if (g_cfg.title_screen_only)
        return g_logoExecuteHookInstalled && g_logoHookInstalled && g_titleHookInstalled;
    if (!g_sceneRequestHookTried) {
        g_sceneRequestHookTried = true;
        const uintptr_t site = base + kTitleSceneRequest;
        if (Readable(site, sizeof(kTitleSceneRequestExpected)) &&
            memcmp(reinterpret_cast<const void*>(site), kTitleSceneRequestExpected,
                   sizeof(kTitleSceneRequestExpected)) == 0) {
            // The handler implements the displaced CALL itself. Copying a
            // relative CALL into the trampoline would target the wrong address.
            const unsigned char noCall[] = {0x90, 0x90, 0x90, 0x90, 0x90};
            g_sceneRequestHookInstalled = hook::InstallCtxHook(
                site, sizeof(kTitleSceneRequestExpected), &OnTitleSceneRequest, noCall, sizeof(noCall));
        }
        LogLine("title-scene: request gate %s", g_sceneRequestHookInstalled ? "installed" : "unavailable; retaining native request");
    }
    if (!g_creditsGatesTried) {
        g_creditsGatesTried = true;
        const uintptr_t sites[] = {base + 0x25DD2E, base + 0x25D67E};
        const uintptr_t targets[] = {base + 0x1B80C70, base + 0xA7E980};
        const hook::Handler handlers[] = {&OnCreditsSubmit, &OnCreditsUnitCreate};
        const unsigned char noCall[] = {0x90,0x90,0x90,0x90,0x90};
        for (unsigned i = 0; i < 2; ++i) {
            if (!Readable(sites[i], 5) || *reinterpret_cast<const uint8_t*>(sites[i]) != 0xE8) continue;
            int32_t rel = 0; memcpy(&rel,reinterpret_cast<const void*>(sites[i]+1),4);
            if (static_cast<int64_t>(sites[i]+5)+rel != static_cast<int64_t>(targets[i])) continue;
            g_creditsGateInstalled[i] = hook::InstallCtxHook(sites[i],5,handlers[i],noCall,5);
        }
        g_trimCreditsPrefetch = g_creditsGateInstalled[0] && g_creditsGateInstalled[1];
        LogLine("credits-trim: request/creation gates=%u/%u armed=%u", g_creditsGateInstalled[0] ? 1u:0u,
                g_creditsGateInstalled[1] ? 1u:0u,g_trimCreditsPrefetch ? 1u:0u);
    }
    InstallStartupFontHooks();
    return g_logoExecuteHookInstalled && g_logoHookInstalled && g_titleHookInstalled;
}

DWORD WINAPI Worker(LPVOID) {
    g_cfg = LoadConfig();
    if (!g_cfg.enabled) return 0;
    HMODULE exe = GetModuleHandleW(L"MonsterHunterWorld.exe");
    if (!exe) return 0;
    // Wait for the Steam DRM stub to decrypt the expected instruction bytes.
    for (int i = 0; i < 2400; ++i) {
        const bool patched = PatchAll(g_cfg, exe);
        const bool hooked = InstallHookOnce(exe);
        if (patched && hooked) {
            LogLine("ready");
            return 0;
        }
        Sleep(50);
    }
    LogLine("timed out; wrong game version or another mod changed the patch sites");
    return 1;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
