// Included inside the implementation namespace after startup_profile.h.
// Submit one ordinary native ticket while aLogo handles the saved system data.
// aTitle still requests and waits for its complete original ticket.
struct TitlePrefetchApi {
    uint32_t (__fastcall* available)(uintptr_t);
    uint32_t (__fastcall* appendGuiPaths)(uintptr_t, uint32_t);
    void (__fastcall* addResource)(uintptr_t, uint32_t, uintptr_t, const char*);
    uint8_t (__fastcall* submit)(uintptr_t, uint32_t, uint64_t, uint32_t, uint8_t, uint8_t);
    uint8_t (__fastcall* release)(uintptr_t, uint32_t*);
};
struct TitlePrefetchState {
    bool attempted = false;
    uint64_t lastTry = 0;
    const char* lastFailure = nullptr;
    uint32_t id = ~0u;
    uintptr_t manager = 0;
    uintptr_t block = 0;
} g_titlePrefetch;

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

uint32_t AppendRegisteredTitleGuiPaths(uintptr_t manager, uintptr_t gui, uint32_t id,
                                      decltype(TitlePrefetchApi::addResource) addResource) {
    // Group 5 is a static array of {unit DTI, GUI path, flags}, stride 24.
    // Only issue rGUI requests for unambiguous registered paths. The normal
    // title request later handles sound params, option variants and other mods.
    if (!manager || !gui || id >= 0x400 || !Readable(gui + 0x15000, 16)) return 0;
    const uint32_t count = Rd32(gui + 0x15008);
    const uintptr_t table = P(gui + 0x15000);
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
        if (g_trimCreditsPrefetch && IsCreditsResourcePath(copy)) continue;
        addResource(manager, id, g_exeBase + 0x51C4608, copy); // rGUI::DTI::Instance
        ++appended;
    }
    return appended;
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

bool OwnTitlePrefetch() {
    if (g_titlePrefetch.id >= 0x400 || !g_titlePrefetch.manager ||
        P(g_exeBase + 0x50137C8) != g_titlePrefetch.manager) return false;
    const uintptr_t ticket = g_titlePrefetch.manager + 0x38 + g_titlePrefetch.id * 0x28;
    return Readable(ticket, 0x28) && (Rd32(ticket + 0x10) & 0xFF) != 0 &&
        P(ticket) == g_titlePrefetch.block;
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
    if (OwnTitlePrefetch()) ProfileTicket(5, "title-prefetch", g_titlePrefetch.id);
}

void EndTitlePrefetch(const TitlePrefetchApi& api) {
    if (g_titlePrefetch.id == ~0u) return;
    if (OwnTitlePrefetch()) {
        api.release(g_titlePrefetch.manager, &g_titlePrefetch.id);
        ProfileRecord("title-prefetch native release requested after title preparation");
    } else {
        ProfileRecord("title-prefetch ownership changed; handle discarded");
    }
    g_titlePrefetch.id = ~0u;
}

void FinishTitlePrefetch() {
    PollTitlePrefetch();
    EndTitlePrefetch(NativeTitlePrefetchApi());
}
