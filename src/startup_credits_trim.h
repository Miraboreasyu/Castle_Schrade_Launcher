// Owner-scoped credits removal. News and the other title GUI units stay native.
struct CreditsResource { uintptr_t dti; char path[80]; };
struct CreditsTrimState {
    uintptr_t owner = 0;
    CreditsResource removed[128]{};
    unsigned count = 0;
    bool widgetOmitted = false;
    bool restoring = false;
    uint32_t restoreTicket = ~0u;
    uintptr_t manager = 0;
    uintptr_t block = 0;
} g_credits;
bool g_creditsGateInstalled[2]{};
bool g_creditsGatesTried = false;
using CreditsNewUnitFn = uintptr_t (__fastcall*)(uintptr_t, uintptr_t);
using CreditsSubmitFn = uint8_t (__fastcall*)(uintptr_t,uint32_t,uint64_t,uint32_t,uint8_t,uint8_t);

bool IsInitialCreditsTrimTitle(uintptr_t title) {
    if (!g_trimCreditsPrefetch || !g_cfg.enabled || g_cfg.title_screen_only || g_startupLogoDone || g_titleContinued ||
        !title || !Readable(title + 0x60, 8) || g_cfg.save_slot < 0 || g_cfg.save_slot > 2) return false;
    const uintptr_t user = P(g_exeBase + kPSave);
    const uintptr_t saves = user ? P(user + 0xA8) : 0;
    const uintptr_t record = saves ? saves + static_cast<uint32_t>(g_cfg.save_slot) * kSaveSlotSize : 0;
    return record && Readable(record + 0x38, 1) &&
        (*reinterpret_cast<const uint8_t*>(record + 0x38) & 1) != 0;
}

bool TrimCreditsFromTicket(uintptr_t title, uintptr_t manager, uint32_t id) {
    if (!IsInitialCreditsTrimTitle(title) || Rd32(title + 0x60) != 0 || id >= 0x400 ||
        Rd32(title + 0x64) != id || manager != P(g_exeBase + 0x50137C8) ||
        (g_credits.owner && g_credits.owner != title)) return false;
    const uintptr_t ticket = manager + 0x38 + id * 0x28;
    if (!Readable(ticket, 0x28) || !(Rd32(ticket + 0x10) & 0xFF) || Rd32(ticket + 0x14) != 0) return false;
    uintptr_t blocks[8]{};
    uint32_t counts[8]{};
    unsigned n = 0, removed = 0, total = 0;
    uintptr_t block = P(ticket);
    // Validate the whole owner ticket before changing a byte. Already issued
    // resources must retain their original reference ownership and are excluded.
    for (; block && n < 8; ++n) {
        for (unsigned j = 0; j < n; ++j) if (blocks[j] == block) return false;
        if (!Readable(block, 0xC10)) return false;
        blocks[n] = block; counts[n] = Rd32(block + 0xC00);
        if (counts[n] > 32) return false;
        total += counts[n];
        for (uint32_t i = 0; i < counts[n]; ++i) {
            const uintptr_t entry = block + i * 0x60;
            if (P(entry) || !memchr(reinterpret_cast<const void*>(entry + 0x10), 0, 80)) return false;
            if (IsCreditsResourcePath(reinterpret_cast<const char*>(entry + 0x10))) ++removed;
        }
        block = P(block + 0xC08);
    }
    if (block || !removed || removed > 128) return false;
    g_credits.owner = title; g_credits.count = 0;
    for (unsigned b = 0; b < n; ++b) {
        unsigned keep = 0;
        for (uint32_t i = 0; i < counts[b]; ++i) {
            const uintptr_t entry = blocks[b] + i * 0x60;
            const char* path = reinterpret_cast<const char*>(entry + 0x10);
            if (IsCreditsResourcePath(path)) {
                auto& saved = g_credits.removed[g_credits.count++];
                saved.dti = P(entry + 8); memcpy(saved.path, path, 80);
            } else {
                if (keep != i) memmove(reinterpret_cast<void*>(blocks[b] + keep * 0x60),
                                       reinterpret_cast<const void*>(entry), 0x60);
                ++keep;
            }
        }
        memset(reinterpret_cast<void*>(blocks[b] + keep * 0x60), 0, (counts[b] - keep) * 0x60);
        Wr32(blocks[b] + 0xC00, keep);
    }
    char line[240];
    sprintf_s(line, "credits-trim: removed=%u kept=%u from title ticket=%u", removed, total - removed, id);
    ProfileRecord(line);
    return true;
}

void HandleCreditsUnitCreate(hook::Ctx* c, CreditsNewUnitFn nativeNew) {
    const uintptr_t title = static_cast<uintptr_t>(c->rdi);
    if (IsInitialCreditsTrimTitle(title) && c->rdx == g_exeBase + 0x4CCD6A0 &&
        c->rcx == P(g_exeBase + 0x51238C8) &&
        (!g_credits.owner || g_credits.owner == title)) {
        g_credits.owner = title; g_credits.widgetOmitted = true;
        c->rax = 0; // native caller handles a null newUnit result and keeps slot null
        ProfileRecord("credits-trim: initial StaffRollSimple creation omitted");
        return;
    }
    c->rax = nativeNew(static_cast<uintptr_t>(c->rcx), static_cast<uintptr_t>(c->rdx));
}

void OnCreditsUnitCreate(hook::Ctx* c) {
    HandleCreditsUnitCreate(c, reinterpret_cast<CreditsNewUnitFn>(g_exeBase + 0xA7E980));
}

void HandleCreditsSubmit(hook::Ctx* c, CreditsSubmitFn submit, uint8_t arg5, uint8_t arg6) {
    TrimCreditsFromTicket(static_cast<uintptr_t>(c->rdi), static_cast<uintptr_t>(c->rcx),
                          static_cast<uint32_t>(c->rdx));
    c->rax = submit(static_cast<uintptr_t>(c->rcx), static_cast<uint32_t>(c->rdx), c->r8,
                    static_cast<uint32_t>(c->r9), arg5, arg6);
}

void OnCreditsSubmit(hook::Ctx* c) {
    // Hook is before CALL pushes its return address: fifth/sixth arguments are
    // at caller RSP+20/+28. Ctx contains the sixteen pushed 64-bit registers.
    const uintptr_t callerStack = reinterpret_cast<uintptr_t>(c) + sizeof(hook::Ctx);
    const uint8_t arg5 = *reinterpret_cast<const uint8_t*>(callerStack + 0x20);
    const uint8_t arg6 = *reinterpret_cast<const uint8_t*>(callerStack + 0x28);
    HandleCreditsSubmit(c, reinterpret_cast<CreditsSubmitFn>(g_exeBase + 0x1B80C70), arg5, arg6);
}

bool OwnCreditsRestoreTicket() {
    if (g_credits.restoreTicket >= 0x400 || !g_credits.manager ||
        P(g_exeBase + 0x50137C8) != g_credits.manager) return false;
    const uintptr_t ticket = g_credits.manager + 0x38 + g_credits.restoreTicket * 0x28;
    return Readable(ticket, 0x28) && (Rd32(ticket + 0x10) & 0xFF) && P(ticket) == g_credits.block;
}

void ReleaseCreditsRestoreTicket() {
    if (OwnCreditsRestoreTicket()) NativeTitlePrefetchApi().release(g_credits.manager, &g_credits.restoreTicket);
    g_credits.restoreTicket = ~0u;
}

uintptr_t CreateCreditsUnit(uintptr_t title) {
    const uintptr_t manager = P(g_exeBase + 0x51238C8);
    if (!manager) return 0;
    const uintptr_t unit = reinterpret_cast<CreditsNewUnitFn>(g_exeBase + 0xA7E980)(manager, g_exeBase + 0x4CCD6A0);
    if (!unit) return 0;
    reinterpret_cast<void (*)(uintptr_t,uint32_t,uintptr_t,uint32_t,uint8_t)>(g_exeBase + 0x2219070)
        (manager, 0x14, unit, 0x40000000, 0);
    const uintptr_t critical = reinterpret_cast<uintptr_t (*)()>(g_exeBase + 0x2229230)();
    reinterpret_cast<void (*)(uintptr_t,uintptr_t,uintptr_t)>(g_exeBase + 0x2229160)
        (critical, unit, title + 0x2A0);
    return unit;
}

// A manual credits request during the initial title restores the exact removed
// descriptors, waits for their native ticket, then creates the original widget.
bool WaitForCreditsRestore(hook::Ctx* c, uintptr_t title, uint32_t state) {
    if (title != g_credits.owner || state != 0x19) {
        const uintptr_t widget = title == g_credits.owner ? P(title + 0x2A0) : 0;
        if (g_credits.restoring && (!widget || (Rd32(widget + 0xC) & 0xE))) {
            ReleaseCreditsRestoreTicket(); g_credits.restoring = false;
        }
        return false;
    }
    if (!g_credits.widgetOmitted && !g_credits.count) return false;
    if (!g_credits.restoring) {
        const auto api = NativeTitlePrefetchApi();
        const uintptr_t manager = P(g_exeBase + 0x50137C8);
        if (!manager) { c->rax = 0x1A; return true; }
        const uint32_t id = api.available(manager);
        if (id >= 0x400) { c->rax = 0x1A; return true; }
        for (unsigned i = 0; i < g_credits.count; ++i)
            api.addResource(manager, id, g_credits.removed[i].dti, g_credits.removed[i].path);
        g_credits.manager = manager; g_credits.restoreTicket = id;
        g_credits.block = P(manager + 0x38 + id * 0x28);
        if (!api.submit(manager,id,0,2,0,0)) {
            ReleaseCreditsRestoreTicket(); c->rax = 0x1A; return true;
        }
        g_credits.restoring = true;
        LogLine("credits-trim: manual credits requested; restoring %u removed resources", g_credits.count);
    }
    if (!OwnCreditsRestoreTicket()) {
        g_credits.restoreTicket = ~0u; g_credits.restoring = false;
        LogLine("credits-trim: restore ticket ownership changed; retrying without releasing foreign resources");
        c->rax = 0x1A; return true;
    }
    if (Rd32(g_credits.manager + 0x4C + g_credits.restoreTicket * 0x28) != 3) {
        c->rax = 0x1A; return true;
    }
    uintptr_t widget = P(title + 0x2A0);
    if (!widget) widget = CreateCreditsUnit(title);
    if (!widget || !(Rd32(widget + 0xC) & 0xE)) { c->rax = 0x1A; return true; }
    ReleaseCreditsRestoreTicket();
    g_credits.restoring = false; g_credits.widgetOmitted = false; g_credits.count = 0;
    LogLine("credits-trim: original credits widget/resources restored");
    return false;
}

void ObserveCreditsTrim(uintptr_t title, uint32_t state) {
    if (title == g_credits.owner && state == 0x11) {
        char line[240];
        sprintf_s(line,"credits-trim: quest-entry widget=%p removed=%u",
                  reinterpret_cast<void*>(P(title + 0x2A0)),g_credits.count);
        ProfileRecord(line);
    }
}
