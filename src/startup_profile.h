// Included inside the implementation namespace after the memory helpers.
// Read-only owner-scoped snapshots; sampled on the game's startup thread.
struct ProfileEvent { uint64_t tick; char line[240]; };
constexpr unsigned kProfileCapacity = 512;
ProfileEvent g_profileEvents[kProfileCapacity]{};
unsigned g_profileCount = 0;
unsigned g_profileDropped = 0;
bool g_profileHooksInstalled[2]{};
bool g_postEntryHooksInstalled[4]{};
bool g_postEntryHooksTried = false;
bool g_startupProfileFlushed = false;
bool g_stageInitLogged = false;
bool g_stageWatcherStarted = false;
uint64_t g_stageInitTick = 0;
uint32_t g_stageInitAreaId = 0;
uint8_t g_stageInitIsVillage = 0;
uint32_t g_stageInitQuestId = 0;

void ProfileRecord(const char* line) {
    if (g_startupProfileFlushed) return;
    if (g_profileCount == kProfileCapacity) { ++g_profileDropped; return; }
    auto& e = g_profileEvents[g_profileCount++];
    e.tick = GetTickCount64();
    strcpy_s(e.line, line);
}

void ProfileStage(unsigned channel, const char* name, uint32_t state) {
    static uint32_t previous[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    if (channel >= 8) return;
    if (previous[channel] == state) return;
    previous[channel] = state;
    char line[240];
    sprintf_s(line, "phase=%s state=%u", name, state);
    ProfileRecord(line);
}

void ProfileTicket(unsigned channel, const char* name, uint32_t id) {
    static uint64_t last[12]{};
    static char previous[12][240]{};
    const uint64_t now = GetTickCount64();
    if (channel >= 12 || now - last[channel] < 50 || id >= 0x400) return;
    last[channel] = now;
    const uintptr_t manager = P(g_exeBase + 0x50137C8);
    const uintptr_t ticket = manager ? manager + 0x38 + id * 0x28 : 0;
    if (!ticket || !Readable(ticket, 0x28)) return;
    unsigned total = 0, issued = 0, ready = 0;
    char firstPending[80]{};
    uintptr_t block = P(ticket);
    unsigned blocks = 0;
    const unsigned maxBlocks = (channel == 7 || channel >= 8) ? 32 : 8;
    for (; block && blocks < maxBlocks; ++blocks) {
        if (!Readable(block, 0xC10)) break;
        const unsigned count = Rd32(block + 0xC00);
        if (count > 32) break;
        for (unsigned i = 0; i < count; ++i) {
            const uintptr_t entry = block + i * 0x60;
            const uintptr_t resource = P(entry);
            ++total;
            if (resource) ++issued;
            const bool done = resource && Readable(resource + 0x64, 4) &&
                (Rd32(resource + 0x64) & 1) != 0;
            if (done) ++ready;
            else if (!firstPending[0]) {
                memcpy(firstPending, reinterpret_cast<const void*>(entry + 0x10), 79);
                firstPending[79] = 0;
            }
        }
        block = P(block + 0xC08);
    }
    char line[240];
    sprintf_s(line, "ticket=%s id=%u state=%u issued=%u/%u ready=%u partial=%u pending=%.79s",
              name, id, Rd32(ticket + 0x14), issued, total, ready, block ? 1u : 0u, firstPending);
    if (strcmp(previous[channel], line) == 0) return;
    strcpy_s(previous[channel], line);
    ProfileRecord(line);
}

void OnLogoPrepare(hook::Ctx* c) {
    if (g_startupLogoDone || !g_cfg.enabled) return;
    const uintptr_t owner = static_cast<uintptr_t>(c->rcx);
    if (!owner || !Readable(owner + 0x40, 12)) return;
    const uint32_t state = Rd32(owner + 0x40);
    ProfileStage(0, "logo-prepare", state);
    if (state >= 3) {
        // 141B7BFE0(mode=3): state +38, bootstrap ticket +44.
        const uintptr_t resident = P(g_exeBase + 0x5011F00);
        if (resident && Readable(resident + 0x38, 0x10)) {
            const uint32_t inner = Rd32(resident + 0x38);
            ProfileStage(3, "resident-bootstrap", inner);
            if (inner >= 2 && inner <= 4)
                ProfileTicket(7, "resident-main", Rd32(resident + 0x3C));
            if (inner >= 9 && inner <= 11)
                ProfileTicket(6, "resident-bootstrap", Rd32(resident + 0x44));
        }
    }
    if (state >= 2) ProfileTicket(0, "logo", Rd32(owner + 0x44));
    if (state >= 5) ProfileTicket(1, "base-gui", Rd32(owner + 0x48));
    AdvanceResidentBuild(owner, state);
}

void CheckDx12FallbackCleanup();
void ReleasePlayablePrefetchIfReady(uint32_t playableState);
void FlushStartupProfile();

void OnTitlePrepare(hook::Ctx* c) {
    if (!g_cfg.enabled) return;
    CloseStartupFontWindow();
    CheckDx12FallbackCleanup();
    HandleTitleReturnPrepare(static_cast<uintptr_t>(c->rcx));
    if (g_startupLogoDone) return;
    PollTitlePrefetch();
    const uintptr_t owner = static_cast<uintptr_t>(c->rcx);
    if (!owner || !Readable(owner + 0x60, 12)) return;
    const uint32_t state = Rd32(owner + 0x60);
    ProfileStage(1, "title-prepare", state);
    if (state >= 1) ProfileTicket(3, "title-assets", Rd32(owner + 0x64));
    if (state >= 5) ProfileTicket(4, "title-quest-data", Rd32(owner + 0x68));
}

void OnPlayablePrepare(hook::Ctx* c) {
    if (!g_cfg.enabled || !g_titleContinued || g_stageInitLogged) return;
    const uintptr_t playable = static_cast<uintptr_t>(c->rcx);
    if (!playable || !Readable(playable + 0x64, 8)) return;
    const uint32_t state = Rd32(playable + 0x64);
    ProfileStage(4, "playable-prepare", state);
    if (state == 1) ProfileTicket(8, "playable-hud", Rd32(playable + 0x68));
    if (state == 2) {
        const uintptr_t quest = P(g_exeBase + 0x500ED30);
        if (quest && Readable(quest + 0x12FF4, 4))
            ProfileTicket(9, "all-quests", Rd32(quest + 0x12FF4));
    }
    ReleasePlayablePrefetchIfReady(state);
}

void OnMainVillageBasePrepare(hook::Ctx* c) {
    if (!g_cfg.enabled || !g_titleContinued || g_stageInitLogged) return;
    const uintptr_t base = static_cast<uintptr_t>(c->rcx);
    if (!base || !Readable(base + 0x8, 4) || !Readable(base + 0x64, 0x10)) return;
    const uint32_t state = Rd32(base + 0x8);
    ProfileStage(5, "village-prepare", state);
    if (state == 5) ProfileTicket(10, "village-stage", Rd32(base + 0x64));
    else if (state == 7) ProfileTicket(10, "village-npc", Rd32(base + 0x68));
    else if (state == 10) ProfileTicket(10, "village-gui", Rd32(base + 0x70));
}

void OnStagePrepareInner(hook::Ctx* c) {
    if (!g_cfg.enabled || !g_titleContinued || g_stageInitLogged) return;
    const uintptr_t stage = static_cast<uintptr_t>(c->rcx);
    if (!stage || !Readable(stage + 0x8, 1)) return;
    const uint32_t step = *reinterpret_cast<const uint8_t*>(stage + 0x8);
    ProfileStage(6, "stage-prepare", step);
    if (step == 3 && Readable(stage + 0xE8C, 4))
        ProfileTicket(11, "stage-assets", Rd32(stage + 0xE8C));
    if (step == 0xF && Readable(stage + 0xE54, 4)) {
        static bool syncTimerObserved = false;
        if (!syncTimerObserved) {
            syncTimerObserved = true;
            float waitSec = 0.0f;
            memcpy(&waitSec, reinterpret_cast<const void*>(stage + 0xE54), sizeof(waitSec));
            char line[240];
            sprintf_s(line, "stage-sync-wait: observed aStage+0xE54=%.3f s (unmodified)", waitSec);
            ProfileRecord(line);
        }
    }
}

// Lightweight read-only post-init observer: logs exact loading-screen close
// (sMhGUI+0x146DB == 0) and 0.5s screen fade-in completion (sFade+0x54 == 0),
// then emits a deterministic completion cue for automated benchmark runners.
DWORD WINAPI PostInitObserverThread(LPVOID) {
    bool loadingClosed = false;
    bool fadeComplete = false;
    const uint64_t start = g_stageInitTick;
    for (int i = 0; i < 3000; ++i) { // up to 15 s
        const uintptr_t gui = P(g_exeBase + 0x51C4640);
        const uintptr_t fade = P(g_exeBase + 0x500DED0);
        const uint8_t loadingActive = (gui && Readable(gui + 0x146DB, 1))
            ? *reinterpret_cast<const uint8_t*>(gui + 0x146DB) : 0;
        const uint32_t fadeMode = (fade && Readable(fade + 0x54, 4))
            ? Rd32(fade + 0x54) : 0;
        const uint64_t dt = GetTickCount64() - start;
        if (!loadingClosed && loadingActive == 0 && dt >= 10) {
            loadingClosed = true;
            LogLine("stage-ready: loading-screen-closed dt_from_init=%llu ms fadeMode=%u",
                    static_cast<unsigned long long>(dt), fadeMode);
        }
        if (loadingClosed && !fadeComplete && fadeMode == 0) {
            fadeComplete = true;
            LogLine("stage-ready: fade-in-complete dt_from_init=%llu ms stageId=0x%X isVillage=%u",
                    static_cast<unsigned long long>(dt), g_stageInitAreaId,
                    static_cast<unsigned>(g_stageInitIsVillage));
            break;
        }
        Sleep(5);
    }
    LogLine("startup-bench-done: stageId=0x%X isVillage=%u questId=%u loadingClosed=%u fadeComplete=%u",
            g_stageInitAreaId, static_cast<unsigned>(g_stageInitIsVillage), g_stageInitQuestId,
            loadingClosed ? 1u : 0u, fadeComplete ? 1u : 0u);
    return 0;
}

void OnStageInitialize(hook::Ctx*) {
    if (!g_cfg.enabled || !g_titleContinued || g_stageInitLogged) return;
    g_stageInitLogged = true;
    RecordDx12PlacedStats();
    g_stageInitTick = GetTickCount64();
    const uintptr_t area = P(g_exeBase + kPArea);
    const uintptr_t quest = P(g_exeBase + 0x500ED30);
    g_stageInitAreaId = (area && Readable(area + 0xD328, 4)) ? Rd32(area + 0xD328) : 0;
    g_stageInitIsVillage = (area && Readable(area + 0xD2EA, 1))
        ? *reinterpret_cast<const uint8_t*>(area + 0xD2EA) : 0;
    g_stageInitQuestId = (quest && Readable(quest + 0x4C, 4)) ? Rd32(quest + 0x4C) : 0;
    LogLine("stage-init: stageId=0x%X isVillage=%u questId=%u mode=%s",
            g_stageInitAreaId, static_cast<unsigned>(g_stageInitIsVillage), g_stageInitQuestId,
            g_cfg.title_only_hub ? "hub" : "quest");
    FlushStartupProfile();
    if (!g_stageWatcherStarted) {
        g_stageWatcherStarted = true;
        HANDLE t = CreateThread(nullptr, 0, PostInitObserverThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
}

bool InstallPostEntryProfileHooks() {
    if (g_postEntryHooksTried || !g_cfg.enabled || g_cfg.title_screen_only) return true;
    g_postEntryHooksTried = true;
    const uintptr_t sites[4] = {
        g_exeBase + 0x2505D0, // aPlayable::prepare
        g_exeBase + 0x24F340, // aMainVillageBase::prepare
        g_exeBase + 0x24AE70, // aStage::prepare (inner)
        g_exeBase + 0x253560, // aStage::initialize
    };
    const unsigned char expected[4][9] = {
        {0x40, 0x55, 0x56, 0x41, 0x56, 0, 0, 0, 0},
        {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9},
        {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x57, 0x41, 0x57},
        {0x40, 0x55, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB1},
    };
    const size_t lengths[4] = {5, 9, 9, 9};
    const hook::Handler handlers[4] = {
        &OnPlayablePrepare, &OnMainVillageBasePrepare,
        &OnStagePrepareInner, &OnStageInitialize,
    };
    for (unsigned i = 0; i < 4; ++i) {
        if (!Readable(sites[i], lengths[i]) ||
            memcmp(reinterpret_cast<const void*>(sites[i]), expected[i], lengths[i]) != 0)
            continue;
        g_postEntryHooksInstalled[i] = hook::InstallCtxHook(
            sites[i], lengths[i], handlers[i], expected[i], lengths[i]);
    }
    LogLine("post-entry-profile: hooks=%u/%u/%u/%u (playable/village/stage/init)",
            g_postEntryHooksInstalled[0] ? 1u : 0u, g_postEntryHooksInstalled[1] ? 1u : 0u,
            g_postEntryHooksInstalled[2] ? 1u : 0u, g_postEntryHooksInstalled[3] ? 1u : 0u);
    return true;
}

// Single-write buffered flush: avoids 50+ sequential fopen_s/fclose calls on the main thread.
void FlushStartupProfile() {
    if (g_startupProfileFlushed) return;
    g_startupProfileFlushed = true;
    const uint64_t now = GetTickCount64();
    std::string block;
    block.reserve(static_cast<size_t>(g_profileCount + 2) * 140);
    char line[320];
    sprintf_s(line, "[%llu ms] resource-profile: samples=%u dropped=%u (at=sample time, 50ms ticket sampling)\n",
              static_cast<unsigned long long>(now), g_profileCount, g_profileDropped);
    block.append(line);
    for (unsigned i = 0; i < g_profileCount; ++i) {
        sprintf_s(line, "[%llu ms] resource-profile: at=%llu %s\n",
                  static_cast<unsigned long long>(now),
                  static_cast<unsigned long long>(g_profileEvents[i].tick),
                  g_profileEvents[i].line);
        block.append(line);
    }
    LogRawBlock(block);
}
