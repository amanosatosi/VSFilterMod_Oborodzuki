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
    Count
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
