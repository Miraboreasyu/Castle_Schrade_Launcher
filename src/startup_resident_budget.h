// Included after startup_profile.h. Extra work runs on the original startup
// thread before the normal aLogo PREPARE update; no new hook or worker thread.
constexpr uintptr_t kResidentUpdate = 0x1B7BFE0;
constexpr unsigned kResidentExtraCalls = 7;
constexpr int64_t kResidentBudgetUs = 2000;
constexpr unsigned char kResidentUpdateExpected[] = {
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,
};
using ResidentStepFn = uint64_t (*)(uintptr_t, uint32_t);
using ResidentClockFn = int64_t (*)();

struct ResidentBuildSlice {
    unsigned calls = 0;
    int64_t elapsed = 0;
};

int64_t ResidentCounter() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

ResidentBuildSlice RunResidentBuildSlice(uintptr_t resident, ResidentStepFn step,
                                         ResidentClockFn now, int64_t budget) {
    ResidentBuildSlice result;
    const int64_t start = now();
    // State4 contains only ordered index construction and finalization. Stop
    // before state5: later states submit/wait on real resource tickets.
    while (result.calls < kResidentExtraCalls && Rd32(resident + 0x38) == 4) {
        step(resident, 3);
        ++result.calls;
        result.elapsed = now() - start;
        if (result.elapsed >= budget) break;
    }
    return result;
}

struct ResidentBudgetState {
    bool checked = false;
    bool available = false;
    bool finished = false;
    uintptr_t owner = 0;
    uintptr_t resident = 0;
    int64_t frequency = 0;
    int64_t started = 0;
    int64_t work = 0;
    int64_t maxSlice = 0;
    unsigned updates = 0;
    unsigned calls = 0;
} g_residentBudget;

void AdvanceResidentBuild(uintptr_t logo, uint32_t logoState) {
    if (!g_cfg.enabled || g_cfg.title_screen_only || g_startupLogoDone ||
        g_titleContinued || g_residentBudget.finished) return;
    const uintptr_t resident = P(g_exeBase + 0x5011F00);
    if (!resident || !Readable(resident + 0x38, 4)) return;
    auto& s = g_residentBudget;
    if (s.owner && (s.owner != logo || s.resident != resident)) {
        s.finished = true;
        ProfileRecord("resident-budget: owner changed; stopping extra work");
        return;
    }
    const uint32_t state = Rd32(resident + 0x38);
    if (s.owner && state != 4) {
        char line[240];
        sprintf_s(line, "resident-budget: finished state=%u wall-ms=%.3f extra-calls=%u updates=%u extra-work-ms=%.3f max-slice-ms=%.3f",
                  state, (ResidentCounter() - s.started) * 1000.0 / s.frequency,
                  s.calls, s.updates, s.work * 1000.0 / s.frequency,
                  s.maxSlice * 1000.0 / s.frequency);
        ProfileRecord(line);
        s.finished = true;
        return;
    }
    if (logoState != 3 || state != 4) return;
    if (!s.checked) {
        s.checked = true;
        LARGE_INTEGER frequency{};
        s.available = Readable(g_exeBase + kResidentUpdate, sizeof(kResidentUpdateExpected)) &&
            memcmp(reinterpret_cast<const void*>(g_exeBase + kResidentUpdate),
                   kResidentUpdateExpected, sizeof(kResidentUpdateExpected)) == 0 &&
            QueryPerformanceFrequency(&frequency) && frequency.QuadPart >= 1000;
        s.frequency = frequency.QuadPart;
        if (!s.available) {
            ProfileRecord("resident-budget: unavailable code/clock; retaining native budget");
            return;
        }
    }
    if (!s.available) return;
    if (!s.owner) {
        s.owner = logo;
        s.resident = resident;
        s.started = ResidentCounter();
        ProfileRecord("resident-budget: begin extra-limit=7 soft-budget-us=2000");
    }
    const auto result = RunResidentBuildSlice(resident,
        reinterpret_cast<ResidentStepFn>(g_exeBase + kResidentUpdate),
        &ResidentCounter, s.frequency * kResidentBudgetUs / 1000000);
    ++s.updates;
    s.calls += result.calls;
    s.work += result.elapsed;
    if (result.elapsed > s.maxSlice) s.maxSlice = result.elapsed;
    if (Rd32(resident + 0x38) != 4) {
        // Record completion now, without including the next frame's idle gap.
        AdvanceResidentBuild(logo, logoState);
    }
}
