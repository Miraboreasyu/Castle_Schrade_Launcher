// Included inside the implementation namespace after startup_profile.h.
// Submit ordinary native tickets during idle Loader 1 windows:
//   1. Title prefetch (Group 5 GUIs + save icon + title env) during aLogo.
//   2. Resident prefetch (Opp 7: 212 localized resident items + Group 0 GUIs +
//      rEquipVisualWp + m_face000) during aLogo's idle window after title prefetch.
//   3. Universal aPlayable overlap (Opp 6: LoadAllQuests 0->1 once + Group 1 HUD
//      GUIs + fixed aPlayable case 0 paths) during aTitle 0x13 hub-create wait.
// All three are 100% destination-agnostic (zero map/hub prediction).
struct TitlePrefetchApi {
    uint32_t (__fastcall* available)(uintptr_t);
    uint32_t (__fastcall* appendGuiPaths)(uintptr_t, uint32_t);
    void (__fastcall* addResource)(uintptr_t, uint32_t, uintptr_t, const char*);
    uint8_t (__fastcall* submit)(uintptr_t, uint32_t, uint64_t, uint32_t, uint8_t, uint8_t);
    uint8_t (__fastcall* release)(uintptr_t, uint32_t*);
};
struct PrefetchTicketState {
    bool attempted = false;
    uint64_t lastTry = 0;
    const char* lastFailure = nullptr;
    uint32_t id = ~0u;
    uintptr_t manager = 0;
    uintptr_t block = 0;
};
PrefetchTicketState g_titlePrefetch;
PrefetchTicketState g_residentPrefetch;
PrefetchTicketState g_playablePrefetch;
bool g_playableQuestPrefetched = false;

// Armed only when both optional credits request/creation gates are installed.
bool g_trimCreditsPrefetch = false;
bool IsCreditsResourcePath(const char* path) {
    return strncmp(path, "ui\\credit\\", 10) == 0 ||
        strcmp(path, "common\\ui\\staffRollDataSlugger") == 0;
}

uint32_t __fastcall AppendTitleGuiPrefetch(uintptr_t gui, uint32_t id);

TitlePrefetchApi NativeTitlePrefetchApi() {
    return {
        reinterpret_cast<decltype(TitlePrefetchApi::available)>(g_exeBase + 0x1B80780),
        &AppendTitleGuiPrefetch,
        reinterpret_cast<decltype(TitlePrefetchApi::addResource)>(g_exeBase + 0x1B80820),
        reinterpret_cast<decltype(TitlePrefetchApi::submit)>(g_exeBase + 0x1B80C70),
        reinterpret_cast<decltype(TitlePrefetchApi::release)>(g_exeBase + 0x1B81180),
    };
}

uint32_t AppendRegisteredGuiGroupPaths(uintptr_t manager, uintptr_t gui, uint32_t id,
                                       uint32_t groupIndex, bool skipCredits,
                                       decltype(TitlePrefetchApi::addResource) addResource) {
    // Registered GUI groups sit at gui + 0x14FB0 + groupIndex * 0x10:
    // {tablePtr, count}, entry stride 24: {unit DTI, GUI path, flags}.
    if (!manager || !gui || id >= 0x400 || groupIndex > 8) return 0;
    const uintptr_t group = gui + 0x14FB0 + static_cast<uintptr_t>(groupIndex) * 0x10;
    if (!Readable(group, 16)) return 0;
    const uint32_t count = Rd32(group + 8);
    const uintptr_t table = P(group);
    if (!table || count > 256) return 0;
    uint32_t appended = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t entry = table + i * 24;
        if (!Readable(entry, 24)) continue;
        if (Rd32(entry + 16) & 8) continue; // variant depends on saved system options
        const uintptr_t path = P(entry + 8);
        if (!path || !Readable(path, 80)) continue;
        char copy[80];
        memcpy(copy, reinterpret_cast<const void*>(path), sizeof(copy));
        const char* end = static_cast<const char*>(memchr(copy, 0, sizeof(copy)));
        if (!end || end - copy <= 1) continue;
        if (skipCredits && IsCreditsResourcePath(copy)) continue;
        addResource(manager, id, g_exeBase + 0x51C4608, copy); // rGUI::DTI::Instance
        ++appended;
    }
    return appended;
}

uint32_t AppendRegisteredTitleGuiPaths(uintptr_t manager, uintptr_t gui, uint32_t id,
                                       decltype(TitlePrefetchApi::addResource) addResource) {
    return AppendRegisteredGuiGroupPaths(manager, gui, id, 5, g_trimCreditsPrefetch, addResource);
}

uint32_t __fastcall AppendTitleGuiPrefetch(uintptr_t gui, uint32_t id) {
    return AppendRegisteredTitleGuiPaths(P(g_exeBase + 0x50137C8), gui, id,
                                         NativeTitlePrefetchApi().addResource);
}

bool TitlePrefetchCodeMatches() {
    // 141AD8D20 is intentionally absent: prefetch never calls or patches it.
    const uintptr_t rvas[] = {0x1B80780, 0x1B80820, 0x1B80C70, 0x1B81180};
    const unsigned char bytes[][10] = {
        {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20},
        {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10},
        {0x83,0xFA,0xFF,0x74,0x37,0x8B,0xC2,0x48,0x8D,0x14},
        {0x8B,0x02,0x83,0xF8,0xFF,0x74,0x20,0x4C,0x8D,0x04},
    };
    for (unsigned i = 0; i < 4; ++i) {
        if (!Readable(g_exeBase + rvas[i], 10) ||
            memcmp(reinterpret_cast<const void*>(g_exeBase + rvas[i]), bytes[i], 10)) {
            char actual[21] = "unreadable";
            if (Readable(g_exeBase + rvas[i], 10)) {
                const auto* p = reinterpret_cast<const unsigned char*>(g_exeBase + rvas[i]);
                const char hex[] = "0123456789ABCDEF";
                for (unsigned j = 0; j < 10; ++j) {
                    actual[j * 2] = hex[p[j] >> 4];
                    actual[j * 2 + 1] = hex[p[j] & 15];
                }
                actual[20] = 0;
            }
            char line[240];
            sprintf_s(line, "title-prefetch skipped=code-mismatch rva=%llX actual=%s",
                      static_cast<unsigned long long>(rvas[i]), actual);
            ProfileRecord(line);
            return false;
        }
    }
    return true;
}

bool OwnPrefetchTicket(const PrefetchTicketState& st) {
    if (st.id >= 0x400 || !st.manager || P(g_exeBase + 0x50137C8) != st.manager) return false;
    const uintptr_t ticket = st.manager + 0x38 + st.id * 0x28;
    return Readable(ticket, 0x28) && (Rd32(ticket + 0x10) & 0xFF) != 0 &&
        P(ticket) == st.block;
}

bool OwnTitlePrefetch() { return OwnPrefetchTicket(g_titlePrefetch); }

void ReleasePrefetchTicket(PrefetchTicketState& st, const TitlePrefetchApi& api, const char* label) {
    if (st.id == ~0u) return;
    char line[240];
    if (OwnPrefetchTicket(st)) {
        api.release(st.manager, &st.id);
        sprintf_s(line, "%s native release requested", label);
    } else {
        sprintf_s(line, "%s ownership changed; handle discarded", label);
    }
    ProfileRecord(line);
    st.id = ~0u;
}

void BeginTitlePrefetch(uintptr_t manager, uintptr_t gui, const TitlePrefetchApi& api) {
    g_titlePrefetch.attempted = true;
    const uint32_t id = api.available(manager);
    if (id >= 0x400) {
        ProfileRecord("title-prefetch skipped=no-ticket");
        return;
    }
    g_titlePrefetch.id = id;
    g_titlePrefetch.manager = manager;
    const uint32_t guiPaths = api.appendGuiPaths(gui, id);
    api.addResource(manager, id, g_exeBase + 0x51C3B00, "ui\\save_icon\\save_pos");
    api.addResource(manager, id, g_exeBase + 0x51C46F8, "sound\\env\\title\\title_envb");
    g_titlePrefetch.block = P(manager + 0x38 + id * 0x28);
    // Same flags=2 and ordinary channel as native aTitle PREPARE.
    if (!api.submit(manager, id, 0, 2, 0, 0)) {
        api.release(manager, &g_titlePrefetch.id);
        g_titlePrefetch.id = ~0u;
        ProfileRecord("title-prefetch skipped=submit-failed");
        return;
    }
    char line[240];
    sprintf_s(line, "title-prefetch submitted id=%u gui-paths=%u plus=save-icon,title-env mode=registered-list",
              id, guiPaths);
    ProfileRecord(line);
}

// Opportunity 7: Prefetch aResident::prepare's 212 localized items (table 0x1450111C0)
// + Group 0 GUIs + m_face000 after aLogo state 5 updates sMhGUI->mLanguageId to the
// saved language. Deliberately omits addFolder("wp") to avoid early rEffectList loading.
void StartResidentLocalizedPrefetch() {
    if (g_cfg.title_screen_only || g_startupLogoDone || g_titleContinued || g_residentPrefetch.attempted)
        return;
    g_residentPrefetch.attempted = true;
    const uintptr_t manager = P(g_exeBase + 0x50137C8);
    const uintptr_t gui = P(g_exeBase + 0x51C4640);
    if (!manager || !gui || !Readable(g_exeBase + 0x50111C0, 0xD4 * 16) || !TitlePrefetchCodeMatches()) return;
    const auto api = NativeTitlePrefetchApi();
    const uint32_t id = api.available(manager);
    if (id >= 0x400) {
        ProfileRecord("resident-prefetch skipped=no-ticket");
        return;
    }
    g_residentPrefetch.id = id;
    g_residentPrefetch.manager = manager;

    const uint32_t group0 = AppendRegisteredGuiGroupPaths(manager, gui, id, 0, false, api.addResource);
    api.addResource(manager, id, g_exeBase + 0x51C4330, "pl\\m_face\\face000\\mod\\m_face000");

    const auto formatLangPath = reinterpret_cast<void (__fastcall*)(
        uintptr_t, char*, char*, uint32_t)>(g_exeBase + 0x22AB570);
    uint32_t locCount = 0;
    const uintptr_t table = g_exeBase + 0x50111C0;
    for (uint32_t i = 0; i < 0xD4; ++i) {
        const uintptr_t dti = P(table + i * 16);
        const uintptr_t rawPath = P(table + i * 16 + 8);
        if (!dti || !rawPath || !Readable(rawPath, 2)) continue;
        char buf[80]{};
        for (size_t k = 0; k < 79; ++k) {
            if (!Readable(rawPath + k, 1)) break;
            const char ch = *reinterpret_cast<const char*>(rawPath + k);
            buf[k] = ch;
            if (ch == '\0') break;
        }
        buf[79] = '\0';
        if (!buf[0]) continue;
        formatLangPath(gui, buf, buf, 0);
        api.addResource(manager, id, dti, buf);
        ++locCount;
    }

    g_residentPrefetch.block = P(manager + 0x38 + id * 0x28);
    if (!api.submit(manager, id, 0, 2, 0, 0)) {
        api.release(manager, &g_residentPrefetch.id);
        g_residentPrefetch.id = ~0u;
        ProfileRecord("resident-prefetch skipped=submit-failed");
        return;
    }
    char line[240];
    sprintf_s(line, "resident-prefetch submitted id=%u group0=%u localized=%u plus=m_face000",
              id, group0, locCount);
    ProfileRecord(line);
}

void StartResidentLocalizedPrefetchIfLanguageReady() {
    if (g_residentPrefetch.attempted) return;
    const uintptr_t gui = P(g_exeBase + 0x51C4640);
    const uintptr_t user = P(g_exeBase + kPSave);
    if (!gui || !user || !Readable(gui + 0x146BC, 4) || !Readable(user + 0x108, 4)) return;
    // If saved language matches current GUI language, prefetch immediately during aLogo 0x9/0x15;
    // otherwise wait until aLogo 0x16 updates sMhGUI->mLanguageId to the saved language.
    if (Rd32(gui + 0x146BC) == Rd32(user + 0x108)) {
        StartResidentLocalizedPrefetch();
    }
}

const char* TitlePrefetchInputs(uintptr_t manager, uintptr_t gui, uint32_t& count) {
    // Query only the fields inspected here. A valid native allocation may span
    // adjacent VirtualQuery regions, so a whole-object single-region test fails.
    if (!manager) return "manager-null";
    if (!Readable(manager + 0x30, 1)) return "manager-head-unreadable";
    if (!Readable(manager + 0x38, 0x28)) return "manager-descriptor-unreadable";
    if (!Readable(manager + 0xA240, 0x10)) return "manager-channels-unreadable";
    if (!gui) return "gui-null";
    const uintptr_t group = gui ? gui + 0x14FB0 + 5 * 0x10 : 0;
    if (!Readable(group, 16)) return "gui-group-unreadable";
    count = Rd32(group + 8);
    if (!count) return "gui-group-empty";
    if (count > 256) return "gui-group-count-invalid";
    const uintptr_t table = P(group);
    if (!table) return "gui-table-null";
    if (!Readable(table, 24) || !Readable(table + (count - 1) * 24, 24))
        return "gui-table-unreadable";
    return nullptr;
}

void StartTitlePrefetch() {
    if (g_cfg.title_screen_only) return;
    if (g_titlePrefetch.attempted) return;
    const uint64_t now = GetTickCount64();
    if (g_titlePrefetch.lastTry && now - g_titlePrefetch.lastTry < 100) return;
    g_titlePrefetch.lastTry = now;
    const uintptr_t manager = P(g_exeBase + 0x50137C8);
    const uintptr_t gui = P(g_exeBase + 0x51C4640);
    uint32_t count = 0;
    const char* failure = TitlePrefetchInputs(manager, gui, count);
    if (failure) {
        if (!g_titlePrefetch.lastFailure || strcmp(g_titlePrefetch.lastFailure, failure)) {
            char line[240];
            sprintf_s(line, "title-prefetch deferred=%s manager=%p gui=%p group-count=%u",
                      failure, reinterpret_cast<void*>(manager), reinterpret_cast<void*>(gui), count);
            ProfileRecord(line);
            g_titlePrefetch.lastFailure = failure;
        }
        return;
    }
    if (!TitlePrefetchCodeMatches()) {
        g_titlePrefetch.attempted = true;
        return;
    }
    BeginTitlePrefetch(manager, gui, NativeTitlePrefetchApi());
}

void StopTitlePrefetchAttempts() {
    if (g_titlePrefetch.attempted) return;
    g_titlePrefetch.attempted = true;
    char line[240];
    sprintf_s(line, "title-prefetch skipped=font-deadline last=%s",
              g_titlePrefetch.lastFailure ? g_titlePrefetch.lastFailure : "not-started");
    ProfileRecord(line);
}

void PollTitlePrefetch() {
    if (OwnTitlePrefetch()) {
        ProfileTicket(5, "title-prefetch", g_titlePrefetch.id);
    }
    if (OwnPrefetchTicket(g_residentPrefetch)) {
        ProfileTicket(2, "resident-prefetch", g_residentPrefetch.id);
    }
}

void EndTitlePrefetch(const TitlePrefetchApi& api) {
    ReleasePrefetchTicket(g_titlePrefetch, api, "title-prefetch");
    ReleasePrefetchTicket(g_residentPrefetch, api, "resident-prefetch");
}

void FinishTitlePrefetch() {
    PollTitlePrefetch();
    EndTitlePrefetch(NativeTitlePrefetchApi());
}

// Opportunity 6: Universal aPlayable overlap during aTitle 0x13 hub-create wait.
// 100% destination-agnostic (aPlayable is parent activity [1] for every Quest & Hub).
// CRITICAL INVARIANT: sQuest::LoadAllQuests (0x141B5AB10) is invoked AT MOST ONCE
// (0 -> 1) and NEVER polled during aTitle so aPlayable::prepare case 2 natively
// executes 1 -> 2 on its first frame.
using LoadAllQuestsFn = uint8_t (__fastcall*)(uintptr_t);

bool TriggerPlayableQuestPrefetchOnce(uintptr_t quest, uintptr_t questTable, LoadAllQuestsFn fn) {
    if (g_playableQuestPrefetched || !quest || !questTable ||
        !Readable(quest + 0x29EA0, 4) || Rd32(quest + 0x29EA0) != 0)
        return false;
    fn(quest);
    g_playableQuestPrefetched = true;
    return true;
}

void StartPlayableOverlapPrefetch() {
    if (!g_cfg.enabled || g_cfg.title_screen_only || g_titleContinued) return;

    const uintptr_t quest = P(g_exeBase + 0x500ED30);
    const uintptr_t questTable = P(g_exeBase + 0x5012D80);
    const auto loadAllQuests = reinterpret_cast<LoadAllQuestsFn>(g_exeBase + 0x1B5AB10);
    if (TriggerPlayableQuestPrefetchOnce(quest, questTable, loadAllQuests)) {
        char line[240];
        sprintf_s(line, "playable-prefetch: LoadAllQuests phase 0->1 submitted (ticket=%u state=%u)",
                  Readable(quest + 0x12FF4, 4) ? Rd32(quest + 0x12FF4) : ~0u,
                  Readable(quest + 0x29EA0, 4) ? Rd32(quest + 0x29EA0) : ~0u);
        ProfileRecord(line);
    }

    if (g_playablePrefetch.attempted) return;
    g_playablePrefetch.attempted = true;
    const uintptr_t manager = P(g_exeBase + 0x50137C8);
    const uintptr_t gui = P(g_exeBase + 0x51C4640);
    if (!manager || !gui || !TitlePrefetchCodeMatches()) return;
    const auto api = NativeTitlePrefetchApi();
    const uint32_t id = api.available(manager);
    if (id >= 0x400) {
        ProfileRecord("playable-prefetch skipped=no-ticket");
        return;
    }
    g_playablePrefetch.id = id;
    g_playablePrefetch.manager = manager;

    const uint32_t hudPaths = AppendRegisteredGuiGroupPaths(manager, gui, id, 1, false, api.addResource);
    // Queue universal aPlayable::prepare case 0 assets (100% destination-agnostic):
    // gimmick/animal tables (0x141B233E0, 0x1416EAA70) + radar + common playable resources.
    const uintptr_t gimmickMgr = P(g_exeBase + 0x500EBB0);
    if (gimmickMgr) {
        reinterpret_cast<void (__fastcall*)(uintptr_t, uint32_t)>(g_exeBase + 0x1B233E0)(gimmickMgr, id);
    }
    reinterpret_cast<void (__fastcall*)(uint32_t)>(g_exeBase + 0x16EAA70)(id);
    api.addResource(manager, id, g_exeBase + 0x500B6A0, "ui\\hud\\hud_radar");
    api.addResource(manager, id, g_exeBase + 0x500B238, "ec\\common\\collision\\animal");
    api.addResource(manager, id, g_exeBase + 0x51C3B00, "light\\sdl\\COM_GetDressed");
    api.addResource(manager, id, g_exeBase + 0x51C3B00, "light\\sdl\\COM_GetDressed_otomo");
    api.addResource(manager, id, g_exeBase + 0x51C3B00, "light\\sdl\\COM_GetDressed_weapon");
    api.addResource(manager, id, g_exeBase + 0x51C4680, "event\\etc0110\\mot\\wepon_base");
    api.addResource(manager, id, g_exeBase + 0x51C5468, "ui\\quest\\tex\\coliseum_bg_HDR_NOMIP");
    const auto addFolder = reinterpret_cast<void (__fastcall*)(
        uintptr_t, uint32_t, const char*, uint8_t, uintptr_t, uintptr_t)>(g_exeBase + 0x1B808D0);
    addFolder(manager, id, "ec\\ec011\\00", 1, 0, 0);
    addFolder(manager, id, "ec\\ec011\\epv", 1, 0, 0);
    addFolder(manager, id, "ec\\ec011\\mot", 1, 0, 0);

    g_playablePrefetch.block = P(manager + 0x38 + id * 0x28);
    if (!api.submit(manager, id, 0, 2, 0, 0)) {
        api.release(manager, &g_playablePrefetch.id);
        g_playablePrefetch.id = ~0u;
        ProfileRecord("playable-prefetch skipped=submit-failed");
        return;
    }
    char line[240];
    sprintf_s(line, "playable-prefetch submitted id=%u group1-hud=%u plus=radar,playable-common",
              id, hudPaths);
    ProfileRecord(line);
}

void PollPlayableOverlapPrefetch() {
    if (OwnPrefetchTicket(g_playablePrefetch))
        ProfileTicket(8, "playable-hud-prefetch", g_playablePrefetch.id);
    if (g_playableQuestPrefetched) {
        const uintptr_t quest = P(g_exeBase + 0x500ED30);
        if (quest && Readable(quest + 0x12FF4, 4))
            ProfileTicket(9, "all-quests-prefetch", Rd32(quest + 0x12FF4));
    }
}

void ReleasePlayablePrefetchIfReady(uint32_t playableState) {
    if (playableState >= 2 && g_playablePrefetch.id != ~0u) {
        ReleasePrefetchTicket(g_playablePrefetch, NativeTitlePrefetchApi(), "playable-prefetch");
    }
}

// Error-abort cleanup if aTitle returns to menu (st <= 8) before entering aPlayable.
void AbortPlayableOverlapPrefetch() {
    if (g_playablePrefetch.id != ~0u) {
        ReleasePrefetchTicket(g_playablePrefetch, NativeTitlePrefetchApi(), "playable-prefetch-abort");
    }
    if (g_playableQuestPrefetched) {
        g_playableQuestPrefetched = false;
        const uintptr_t quest = P(g_exeBase + 0x500ED30);
        if (quest) {
            reinterpret_cast<void (__fastcall*)(uintptr_t)>(g_exeBase + 0x1B6B380)(quest);
            LogLine("playable-prefetch: aborted before aPlayable; reset LoadAllQuests via 0x141B6B380");
        }
    }
}
