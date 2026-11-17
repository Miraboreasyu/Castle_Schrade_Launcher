// Included inside the implementation namespace after the memory helpers.
// Read-only owner-scoped snapshots; sampled on the game's startup thread.
struct ProfileEvent { uint64_t tick; char line[240]; };
ProfileEvent g_profileEvents[256]{};
unsigned g_profileCount = 0;
unsigned g_profileDropped = 0;
bool g_profileHooksInstalled[2]{};
bool g_startupProfileFlushed = false;

void ProfileRecord(const char* line) {
    if (g_startupProfileFlushed) return;
    if (g_profileCount == 256) { ++g_profileDropped; return; }
    auto& e = g_profileEvents[g_profileCount++];
    e.tick = GetTickCount64();
    strcpy_s(e.line, line);
}

void ProfileStage(unsigned channel, const char* name, uint32_t state) {
    static uint32_t previous[4] = {~0u, ~0u, ~0u, ~0u};
    if (channel >= 4) return;
    if (previous[channel] == state) return;
    previous[channel] = state;
    char line[240];
    sprintf_s(line, "phase=%s state=%u", name, state);
    ProfileRecord(line);
}

void ProfileTicket(unsigned channel, const char* name, uint32_t id) {
    static uint64_t last[8]{};
    static char previous[8][240]{};
    const uint64_t now = GetTickCount64();
    if (channel >= 8 || now - last[channel] < 50 || id >= 0x400) return;
    last[channel] = now;
    const uintptr_t manager = P(g_exeBase + 0x50137C8);
    const uintptr_t ticket = manager ? manager + 0x38 + id * 0x28 : 0;
    if (!ticket || !Readable(ticket, 0x28)) return;
    unsigned total = 0, issued = 0, ready = 0;
    char firstPending[80]{};
    uintptr_t block = P(ticket);
    unsigned blocks = 0;
    const unsigned maxBlocks = channel == 7 ? 32 : 8;
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

void OnTitlePrepare(hook::Ctx* c) {
    if (!g_cfg.enabled) return;
    CloseStartupFontWindow();
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

void FlushStartupProfile() {
    if (g_startupProfileFlushed) return;
    g_startupProfileFlushed = true;
    LogLine("resource-profile: samples=%u dropped=%u (at=sample time, 50ms ticket sampling)",
            g_profileCount, g_profileDropped);
    for (unsigned i = 0; i < g_profileCount; ++i)
        LogLine("resource-profile: at=%llu %s", static_cast<unsigned long long>(g_profileEvents[i].tick),
                g_profileEvents[i].line);
}
