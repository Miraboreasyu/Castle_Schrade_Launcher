// Included after the startup profiling/prefetch helpers.
// This gate replaces only aTitle PREPARE's scene addResource CALL. Normal title
// paths are forwarded through that same native API; auto-start omits the request.
constexpr uintptr_t kTitleSceneRequest = 0x25DBDE;
constexpr unsigned char kTitleSceneRequestExpected[] = {0xE8, 0x3D, 0x2C, 0x92, 0x01};
bool g_sceneRequestHookInstalled = false;
bool g_sceneRequestHookTried = false;
uintptr_t g_omittedSceneOwner = 0;
using SceneAddResourceFn = uint64_t (__fastcall*)(uintptr_t, uint32_t, uintptr_t, const char*);

bool ShouldOmitStartupScene(const hook::Ctx* c) {
    if (!g_cfg.enabled || g_cfg.title_screen_only || g_startupLogoDone || g_titleContinued ||
        g_cfg.save_slot < 0 || g_cfg.save_slot > 2) return false;
    const uintptr_t title = static_cast<uintptr_t>(c->rdi);
    if (!title || !Readable(title + 0x60, 8) || !Readable(title + 0x390, 4) ||
        Rd32(title + 0x60) != 0 || c->rdx >= 0x400 ||
        Rd32(title + 0x64) != c->rdx ||
        c->rcx != P(g_exeBase + 0x50137C8) || c->r8 != g_exeBase + 0x51C3B00) return false;
    const uint32_t index = Rd32(title + 0x390);
    if (index >= 7 || c->r9 != P(g_exeBase + 0x3E7FCC0 + index * 24) ||
        !Readable(static_cast<uintptr_t>(c->r9), 12) ||
        memcmp(reinterpret_cast<const void*>(c->r9), "stage\\title\\", 12)) return false;
    const uintptr_t user = P(g_exeBase + kPSave);
    const uintptr_t saves = user ? P(user + 0xA8) : 0;
    const uintptr_t record = saves ? saves + static_cast<uint32_t>(g_cfg.save_slot) * kSaveSlotSize : 0;
    return record && Readable(record + 0x38, 1) &&
        (*reinterpret_cast<const uint8_t*>(record + 0x38) & 1) != 0;
}

void HandleTitleSceneRequest(hook::Ctx* c, SceneAddResourceFn nativeAdd) {
    if (ShouldOmitStartupScene(c)) {
        g_omittedSceneOwner = static_cast<uintptr_t>(c->rdi);
        // The caller never reads this return value. No resource object, ready
        // flag or ticket state is fabricated; the scene entry is simply absent.
        c->rax = 1;
        char line[240];
        sprintf_s(line, "title-scene: omitted startup scene request index=%u",
                  Rd32(g_omittedSceneOwner + 0x390));
        ProfileRecord(line);
        return;
    }
    c->rax = nativeAdd(static_cast<uintptr_t>(c->rcx), static_cast<uint32_t>(c->rdx),
                      static_cast<uintptr_t>(c->r8), reinterpret_cast<const char*>(c->r9));
}

void OnTitleSceneRequest(hook::Ctx* c) {
    HandleTitleSceneRequest(c, reinterpret_cast<SceneAddResourceFn>(g_exeBase + 0x1B80820));
}

void ObserveOmittedTitleScene(uintptr_t title, uint32_t state) {
    if (title != g_omittedSceneOwner) return;
    static bool fallbackLogged = false;
    if (state == 5 && !fallbackLogged) {
        fallbackLogged = true;
        ProfileRecord("title-scene: state5 fallback; native synchronous scene creation retained");
    }
    if (state == 0x11) {
        char line[240];
        sprintf_s(line, "title-scene: quest-entry scheduler=%p fallback=%u",
                  reinterpret_cast<void*>(P(title + 0x398)), fallbackLogged ? 1u : 0u);
        ProfileRecord(line);
    }
}
