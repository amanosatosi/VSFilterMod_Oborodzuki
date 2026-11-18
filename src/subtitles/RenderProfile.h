#pragma once

enum class RenderStage {
    Frame,
    Subtitle,
    TextMeasure,
    GlyphPath,
    Transform,
    ScanConvert,
    WidenOutline,
    Rasterize,
    Blur,
    Composite,
    ActiveEvents,
    Overrides,
    Animation,
    Layout,
    Collision,
    Mask,
    Cache,
    GdiMetrics,
    GdiPath,
    TextMeasureHit,
    GlyphPathHit,
    EventPaint,
    BandPlan,
    Count
};

constexpr const char* RenderStageNames[] = {
    "frame_other", "subtitle_other", "text_measure_other", "glyph_path_other",
    "transform", "scan_convert", "widen_outline", "rasterize", "blur", "composite",
    "active_events", "overrides", "animation", "layout", "collision", "mask", "cache",
    "gdi_metrics", "gdi_path", "text_measure_hit", "glyph_path_hit", "event_paint", "band_plan"
};
static_assert(sizeof(RenderStageNames) / sizeof(*RenderStageNames) == static_cast<unsigned>(RenderStage::Count),
    "Missing render stage name");

struct EventWorkerStatistics
{
    unsigned long long batches = 0, tasks = 0, busyNs = 0, wakeNs = 0, waitNs = 0, dispatchNs = 0;
    unsigned long long preparationNs = 0;
    unsigned long long tasksByWorker[8]{}, busyByWorkerNs[8]{};
    unsigned long long idleNs = 0, mutexNs = 0;
    unsigned long long blendNs = 0, blendBatches = 0;
    unsigned long long glyphRequests[8]{}, glyphHits[8]{};
};

#ifdef OBORODZUKI_PROFILE
#include <chrono>
#include <cstdint>

// Attribute time to the innermost instrumented stage, so stage totals
// do not double-count nested work (e.g. glyph paths during layout).
struct RenderProfile {
    using Clock = std::chrono::steady_clock;
    uint64_t ns[static_cast<unsigned>(RenderStage::Count)]{};
    uint64_t calls[static_cast<unsigned>(RenderStage::Count)]{};
    Clock::time_point previous{};
    int active = -1;

    void charge(Clock::time_point now)
    {
        if (active >= 0)
            ns[active] += std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous).count();
        previous = now;
    }
};

inline RenderProfile& render_profile()
{
    static thread_local RenderProfile profile;
    return profile;
}

class RenderStageScope {
    RenderProfile& profile;
    int parent;
public:
    explicit RenderStageScope(RenderStage stage) : profile(render_profile()), parent(profile.active)
    {
        profile.charge(RenderProfile::Clock::now());
        profile.active = static_cast<int>(stage);
        ++profile.calls[profile.active];
    }

    ~RenderStageScope()
    {
        profile.charge(RenderProfile::Clock::now());
        profile.active = parent;
    }
};

#define PROFILE_RENDER_STAGE(stage) RenderStageScope profile_scope_##stage(RenderStage::stage)
#else
#define PROFILE_RENDER_STAGE(stage)
#endif
