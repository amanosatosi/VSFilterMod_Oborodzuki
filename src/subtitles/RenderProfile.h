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
#include <windows.h>

// Attribute time to the innermost instrumented stage, so stage totals
// do not double-count nested work (e.g. glyph paths during layout).
struct RenderProfile {
    unsigned long long ticks[static_cast<unsigned>(RenderStage::Count)]{};
    unsigned long long calls[static_cast<unsigned>(RenderStage::Count)]{};
    LONGLONG previous = 0;
    int active = -1;

    void charge()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (active >= 0)
            ticks[active] += now.QuadPart - previous;
        previous = now.QuadPart;
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
        profile.charge();
        profile.active = static_cast<int>(stage);
        ++profile.calls[profile.active];
    }

    ~RenderStageScope()
    {
        profile.charge();
        profile.active = parent;
    }
};

#define PROFILE_RENDER_STAGE(stage) RenderStageScope profile_scope_##stage(RenderStage::stage)
#else
#define PROFILE_RENDER_STAGE(stage)
#endif
