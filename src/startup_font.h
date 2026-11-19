// Defer the two exhaustive glyph prewarm calls inside both startup language
// refreshes: sMhMain::ctor (0x1AEE62D) and aLogo 0x16 (0xFBA638). Native font
// binding, cache reset and on-demand glyph creation remain active. When aLogo
// 0x16 refreshes the exact same language already bound in sMhMain::ctor, skip
// the redundant cache wipe and font reload. No character-list resource is edited.
bool g_fontStartupWindow = false;
bool g_fontHooksTried = false;
bool g_fontHooks[3]{};
bool g_fontCtorHookInstalled = false;
bool g_fontRefreshUsed = false;
bool g_fontCtorRefreshUsed = false;
uint32_t g_fontCtorBoundLang = ~0u;
thread_local bool g_fontDeferScope = false;
unsigned g_fontWarmCalls[2]{};
uint64_t g_fontWarmCharacters[2]{};
using FontRefreshFn = void (*)(uintptr_t, uint32_t);
using FontWarmFn = void (*)(uintptr_t, uintptr_t, uintptr_t, uint32_t, uint32_t, uintptr_t);

bool ShouldDeferStartupFont() {
    return g_cfg.enabled && !g_cfg.title_screen_only && !g_startupLogoDone &&
        !g_titleContinued && g_fontStartupWindow && !g_fontRefreshUsed &&
        g_fontHooks[0] && g_fontHooks[1] && g_fontHooks[2];
}

bool ShouldDeferCtorFont() {
    return g_cfg.enabled && !g_cfg.title_screen_only && !g_startupLogoDone &&
        !g_titleContinued && !g_fontCtorRefreshUsed &&
        g_fontCtorHookInstalled && g_fontHooks[1] && g_fontHooks[2];
}

void CloseStartupFontWindow() {
    if (g_fontStartupWindow && !g_fontRefreshUsed)
        ProfileRecord("font-startup: refresh not intercepted in startup window");
    g_fontStartupWindow = false;
}

void HandleFontRefreshCtor(hook::Ctx* c, FontRefreshFn native) {
    const bool defer = ShouldDeferCtorFont();
    const bool previous = g_fontDeferScope;
    const uint32_t lang = static_cast<uint32_t>(c->rdx);
    if (defer) g_fontCtorRefreshUsed = true;
    g_fontDeferScope = defer;
    const unsigned beforeCalls[2] = {g_fontWarmCalls[0], g_fontWarmCalls[1]};
    const uint64_t beforeChars[2] = {g_fontWarmCharacters[0], g_fontWarmCharacters[1]};
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const int64_t start = ResidentCounter();
    native(static_cast<uintptr_t>(c->rcx), lang);
    const int64_t elapsed = ResidentCounter() - start;
    g_fontDeferScope = previous;
    if (defer) {
        g_fontCtorBoundLang = lang;
        char line[240];
        sprintf_s(line, "font-startup: site=ctor language=%u refresh-ms=%.3f deferred-calls=%u/%u chars=%llu/%llu native-demand-cache=1",
                  lang, frequency.QuadPart ? elapsed * 1000.0 / frequency.QuadPart : 0.0,
                  g_fontWarmCalls[0] - beforeCalls[0], g_fontWarmCalls[1] - beforeCalls[1],
                  static_cast<unsigned long long>(g_fontWarmCharacters[0] - beforeChars[0]),
                  static_cast<unsigned long long>(g_fontWarmCharacters[1] - beforeChars[1]));
        ProfileRecord(line);
    }
}

void HandleFontRefresh(hook::Ctx* c, FontRefreshFn native) {
    const bool defer = ShouldDeferStartupFont();
    const uint32_t lang = static_cast<uint32_t>(c->rdx);
    if (defer && g_fontCtorRefreshUsed && lang == g_fontCtorBoundLang) {
        g_fontRefreshUsed = true;
        char line[240];
        sprintf_s(line, "font-startup: site=logo language=%u skipped-duplicate-refresh=1 (already bound in ctor)",
                  lang);
        ProfileRecord(line);
        return;
    }
    const bool previous = g_fontDeferScope;
    if (defer) g_fontRefreshUsed = true;
    g_fontDeferScope = defer;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const int64_t start = ResidentCounter();
    native(static_cast<uintptr_t>(c->rcx), lang);
    const int64_t elapsed = ResidentCounter() - start;
    g_fontDeferScope = previous;
    if (defer) {
        char line[240];
        sprintf_s(line, "font-startup: language=%u refresh-ms=%.3f deferred-calls=%u/%u chars=%llu/%llu native-demand-cache=1",
                  lang, frequency.QuadPart ? elapsed * 1000.0 / frequency.QuadPart : 0.0,
                  g_fontWarmCalls[0], g_fontWarmCalls[1],
                  static_cast<unsigned long long>(g_fontWarmCharacters[0]),
                  static_cast<unsigned long long>(g_fontWarmCharacters[1]));
        ProfileRecord(line);
    }
}

void HandleFontWarm(hook::Ctx* c, unsigned kind, FontWarmFn native, uint32_t height, uintptr_t size) {
    if (g_fontDeferScope) {
        ++g_fontWarmCalls[kind];
        if (c->rdx && Readable(static_cast<uintptr_t>(c->rdx) + 0xAC, 4))
            g_fontWarmCharacters[kind] += Rd32(static_cast<uintptr_t>(c->rdx) + 0xAC);
        return;
    }
    native(static_cast<uintptr_t>(c->rcx), static_cast<uintptr_t>(c->rdx),
           static_cast<uintptr_t>(c->r8), static_cast<uint32_t>(c->r9), height, size);
}

void OnFontRefreshCtor(hook::Ctx* c) {
    HandleFontRefreshCtor(c, reinterpret_cast<FontRefreshFn>(g_exeBase + 0x1AD9800));
}
void OnFontRefresh(hook::Ctx* c) {
    HandleFontRefresh(c, reinterpret_cast<FontRefreshFn>(g_exeBase + 0x1AD9800));
}
void ForwardFontWarm(hook::Ctx* c, unsigned kind) {
    const uintptr_t stack = reinterpret_cast<uintptr_t>(c) + sizeof(hook::Ctx);
    HandleFontWarm(c, kind, reinterpret_cast<FontWarmFn>(g_exeBase + (kind ? 0x24DA090 : 0x26F8080)),
                   Rd32(stack + 0x20), P(stack + 0x28));
}
void OnFontWarm(hook::Ctx* c) { ForwardFontWarm(c, 0); }
void OnFontWarmDf(hook::Ctx* c) { ForwardFontWarm(c, 1); }

void InstallStartupFontHooks() {
    if (g_fontHooksTried || !g_cfg.enabled || g_cfg.title_screen_only) return;
    g_fontHooksTried = true;
    const uintptr_t sites[] = {0xFBA638, 0x1AD9952, 0x1AD99F6};
    const unsigned char expected[][5] = {
        {0xE8,0xC3,0xF1,0xB1,0x00}, {0xE8,0x29,0xE7,0xC1,0x00}, {0xE8,0x95,0x06,0xA0,0x00},
    };
    for (unsigned i = 0; i < 3; ++i) {
        if (!Readable(g_exeBase + sites[i], 5) ||
            memcmp(reinterpret_cast<const void*>(g_exeBase + sites[i]), expected[i], 5)) {
            LogLine("font-startup: code mismatch; retaining native prewarm");
            return;
        }
    }
    const hook::Handler handlers[] = {&OnFontRefresh, &OnFontWarm, &OnFontWarmDf};
    const unsigned char noCall[] = {0x90,0x90,0x90,0x90,0x90};
    for (unsigned i = 0; i < 3; ++i)
        g_fontHooks[i] = hook::InstallCtxHook(g_exeBase + sites[i], 5, handlers[i], noCall, 5);
    // Also intercept sMhMain::ctor's pre-logo font refresh (0x141AEE62D -> 0x141AD9800).
    constexpr uintptr_t kCtorFontRefresh = 0x1AEE62D;
    const unsigned char ctorExpected[5] = {0xE8, 0xCE, 0xB1, 0xFE, 0xFF};
    if (g_fontHooks[1] && g_fontHooks[2] && Readable(g_exeBase + kCtorFontRefresh, 5) &&
        memcmp(reinterpret_cast<const void*>(g_exeBase + kCtorFontRefresh), ctorExpected, 5) == 0) {
        g_fontCtorHookInstalled = hook::InstallCtxHook(
            g_exeBase + kCtorFontRefresh, 5, &OnFontRefreshCtor, noCall, 5);
    }
    LogLine("font-startup: refresh/prewarm/ctor gates=%u/%u/%u/%u", g_fontHooks[0] ? 1u:0u,
            g_fontHooks[1] ? 1u:0u, g_fontHooks[2] ? 1u:0u, g_fontCtorHookInstalled ? 1u:0u);
}
