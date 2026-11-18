/*
 *	Copyright (C) 2003-2006 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "stdafx.h"
#include "RenderProfile.h"
#include <algorithm>
#include <cmath>
#include <math.h>
#include <time.h>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <stdexcept>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "RTS.h"

#if defined (_VSMOD) && defined(_LUA)
// path m012. Lua animation
#endif

struct TextDeviceContext
{
    HDC dc = nullptr;
    TextDeviceContext()
    {
        const HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        if (screen)
            ReleaseDC(nullptr, screen);
        if (!dc)
            throw std::runtime_error("Could not create the subtitle GDI context");
        SetGraphicsMode(dc, GM_ADVANCED);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, 0xffffff);
        SetMapMode(dc, MM_TEXT);
    }
    ~TextDeviceContext() { if (dc) DeleteDC(dc); }
    TextDeviceContext(const TextDeviceContext&) = delete;
    TextDeviceContext& operator=(const TextDeviceContext&) = delete;
};

static thread_local HDC g_hDC = nullptr;

class WorkerFilterScope
{
#ifdef _OPENMP
    int previous = omp_get_max_threads();
#endif
public:
    WorkerFilterScope()
    {
#ifdef _OPENMP
        omp_set_num_threads(1);
#endif
    }
    ~WorkerFilterScope()
    {
#ifdef _OPENMP
        omp_set_num_threads(previous);
#endif
    }
};

class TextDeviceScope
{
    HDC previous;
public:
    explicit TextDeviceScope(HDC dc) : previous(g_hDC) { g_hDC = dc; }
    ~TextDeviceScope() { g_hDC = previous; }
};

class SelectedTextFont
{
    HDC dc;
    HFONT previous;

public:
    explicit SelectedTextFont(HFONT font) : dc(g_hDC), previous(SelectFont(dc, font)) {}
    ~SelectedTextFont() { if (previous) SelectFont(dc, previous); }
    explicit operator bool() const { return previous != nullptr; }
    SelectedTextFont(const SelectedTextFont&) = delete;
    SelectedTextFont& operator=(const SelectedTextFont&) = delete;
};

static long revcolor(long c)
{
    return ((c & 0xff0000) >> 16) + (c & 0xff00) + ((c & 0xff) << 16);
}

//////////////////////////////////////////////////////////////////////////////////////////////

// CMyFont

static LOGFONT TextLogFont(STSStyle& style)
{
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    // This renderer already expresses font height in its GDI path units.
    // The generic style conversion queries screen DPI and computes a point
    // height that would immediately be overwritten here.
    lf.lfCharSet = style.charSet;
    _tcsncpy_s(lf.lfFaceName, LF_FACESIZE, CString(style.fontName), _TRUNCATE);
    lf.lfWeight = style.fontWeight;
    lf.lfItalic = style.fItalic ? -1 : 0;
    lf.lfUnderline = style.fUnderline ? -1 : 0;
    lf.lfStrikeOut = style.fStrikeOut ? -1 : 0;
    lf.lfHeight = (LONG)(style.fontSize + 0.5);
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
#ifdef _VSMOD // patch m007. symbol rotating
    lf.lfOrientation = (LONG)style.mod_fontOrient;
#endif
    return lf;
}

CMyFont::CMyFont(STSStyle& style) : CMyFont(TextLogFont(style))
{
}

CMyFont::CMyFont(const LOGFONT& font)
{
    LOGFONT lf = font;
    if(!CreateFontIndirect(&lf))
    {
        _tcscpy(lf.lfFaceName, _T("Arial"));
        CreateFontIndirect(&lf);
    }
}

struct TextGeometryKey
{
    LOGFONT font;
    CStringW text;

    bool operator==(const TextGeometryKey& other) const
    {
        return !memcmp(&font, &other.font, sizeof(font)) &&
            text.GetLength() == other.text.GetLength() &&
            !memcmp(text.GetString(), other.text.GetString(), text.GetLength() * sizeof(wchar_t));
    }
};

struct TextGeometryHash
{
    size_t operator()(const TextGeometryKey& key) const
    {
        size_t hash = 2166136261u;
        const auto* font = reinterpret_cast<const unsigned char*>(&key.font);
        for (size_t i = 0; i < sizeof(key.font); ++i)
            hash = (hash ^ font[i]) * 16777619u;
        for (int i = 0; i < key.text.GetLength(); ++i)
            hash = (hash ^ key.text[i]) * 16777619u;
        return hash;
    }
};

struct TextGeometry
{
    bool measured = false;
    CSize extent;
    int ascent = 0;
    int descent = 0;
    bool hasPath = false;
    std::vector<POINT> points;
    std::vector<BYTE> types;
};

struct TextGeometryCache
{
    std::unordered_map<TextGeometryKey, TextGeometry, TextGeometryHash> runs;
    CRegKey machineFonts, userFonts;
    FILETIME machineStamp{}, userStamp{};
    unsigned embeddedRevision = 0;
    size_t pathBytes = 0;
    bool saturated = false;
    using ClipKey = std::tuple<CStringW, int, int, double, double, bool>;
    std::map<ClipKey, std::shared_ptr<BYTE>> clipMasks;
    size_t clipBytes = 0;

    void Clear()
    {
        runs.clear();
        pathBytes = 0;
        saturated = false;
        clipMasks.clear();
        clipBytes = 0;
    }

    void SaveClip(const ClipKey& key, const std::shared_ptr<BYTE>& pixels, size_t bytes)
    {
        constexpr size_t limit = 16 * 1024 * 1024;
        if (bytes > limit)
            return;
        if (clipMasks.size() >= 8 || clipBytes + bytes > limit)
        {
            clipMasks.clear();
            clipBytes = 0;
        }
        if (clipMasks.emplace(key, pixels).second)
            clipBytes += bytes;
    }

    static bool FontStamp(HKEY root, CRegKey& key, FILETIME& stamp)
    {
        if (!key.m_hKey)
        {
            const LSTATUS result = key.Open(root,
                _T("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts"), KEY_QUERY_VALUE);
            if (result != ERROR_SUCCESS)
                return result == ERROR_FILE_NOT_FOUND;
        }
        const auto result = RegQueryInfoKey(key.m_hKey, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, &stamp);
        if (result != ERROR_SUCCESS)
            key.Close();
        return result == ERROR_SUCCESS;
    }

    void StartFrame()
    {
        PROFILE_RENDER_STAGE(Cache);
        const unsigned revision = SubtitleFontRevision();
        FILETIME currentMachine{}, currentUser{};
        if (!FontStamp(HKEY_LOCAL_MACHINE, machineFonts, currentMachine) ||
            !FontStamp(HKEY_CURRENT_USER, userFonts, currentUser) ||
            revision != embeddedRevision || CompareFileTime(&machineStamp, &currentMachine) ||
            CompareFileTime(&userStamp, &currentUser) || saturated)
            Clear();
        embeddedRevision = revision;
        machineStamp = currentMachine;
        userStamp = currentUser;
    }

    TextGeometry& Get(const LOGFONT& font, const CStringW& text)
    {
        PROFILE_RENDER_STAGE(Cache);
        TextGeometryKey key{font, text};
        const auto found = runs.find(key);
        if (found != runs.end())
            return found->second;
        if (runs.size() >= 512)
            Clear();
        return runs.emplace(std::move(key), TextGeometry{}).first->second;
    }

    void SavePath(TextGeometry& run, const POINT* points, const BYTE* types, size_t count)
    {
        PROFILE_RENDER_STAGE(Cache);
        constexpr size_t maxBytes = 4 * 1024 * 1024;
        if (count > maxBytes / (sizeof(POINT) + sizeof(BYTE)))
            return;
        const size_t bytes = count * (sizeof(POINT) + sizeof(BYTE));
        if (pathBytes + bytes > maxBytes)
        {
            saturated = true;
            return;
        }
        if (count)
        {
            run.points.assign(points, points + count);
            run.types.assign(types, types + count);
        }
        run.hasPath = true;
        pathBytes += bytes;
    }
};

// Words are short-lived. Bind the renderer's cache only while that renderer
// is laying out and painting a frame, including nested calls on this thread.
static thread_local TextGeometryCache* activeTextGeometry = nullptr;

class TextGeometryScope
{
    TextGeometryCache* previous;
public:
    explicit TextGeometryScope(TextGeometryCache* cache) : previous(activeTextGeometry)
    {
        activeTextGeometry = cache;
    }
    ~TextGeometryScope() { activeTextGeometry = previous; }
};

class EventWorkerPool
{
    std::vector<std::thread> threads;
    mutable std::mutex mutex;
    std::condition_variable ready[7], finished;
    const std::vector<std::function<void()>>* tasks = nullptr;
    std::atomic<size_t> next{0};
    size_t generation = 0;
    unsigned pending = 0;
    bool stopping = false;
    std::exception_ptr failure;
    unsigned workers = 1;
    unsigned activeWorkers = 1;
    bool needsTextContext = true;
#ifdef OBORODZUKI_PROFILE
    struct Sample
    {
        unsigned long long tasks = 0, busyNs = 0, wakeNs = 0, idleNs = 0, mutexNs = 0;
        RenderProfile stages;
        // Separate active counters without requiring over-aligned heap allocation.
        char padding[64];
    };
    Sample samples[8];
    EventWorkerStatistics stats;
    RenderProfile::Clock::time_point published;
    static unsigned long long Since(RenderProfile::Clock::time_point start)
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(RenderProfile::Clock::now() - start).count();
    }
#endif

    void RecordFailure()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failure)
            failure = std::current_exception();
    }

    void Drain(unsigned worker)
    {
        const WorkerFilterScope filterThreads;
        for (;;)
        {
            const size_t index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= tasks->size())
                return;
#ifdef OBORODZUKI_PROFILE
            const auto started = RenderProfile::Clock::now();
#endif
            try { (*tasks)[index](); }
            catch (...) { RecordFailure(); }
#ifdef OBORODZUKI_PROFILE
            ++samples[worker].tasks;
            samples[worker].busyNs += Since(started);
#endif
        }
    }

    void Worker(size_t observed, unsigned index)
    {
        std::unique_ptr<TextDeviceContext> dc;
        std::unique_ptr<TextGeometryCache> geometry;
        for (;;)
        {
            bool useTextContext;
            {
#ifdef OBORODZUKI_PROFILE
                const auto acquiring = RenderProfile::Clock::now();
#endif
                std::unique_lock<std::mutex> lock(mutex);
#ifdef OBORODZUKI_PROFILE
                samples[index].mutexNs += Since(acquiring);
                const auto waiting = RenderProfile::Clock::now();
#endif
                ready[index - 1].wait(lock, [&] {
                    return stopping || (generation != observed && index < activeWorkers);
                });
#ifdef OBORODZUKI_PROFILE
                samples[index].idleNs += Since(waiting);
#endif
                if (stopping)
                    return;
                observed = generation;
                useTextContext = needsTextContext;
            }
            try
            {
#ifdef OBORODZUKI_PROFILE
                samples[index].wakeNs += Since(published);
                render_profile() = RenderProfile{};
                samples[index].stages = RenderProfile{};
#endif
                if (useTextContext)
                {
                    if (!dc)
                        dc = std::make_unique<TextDeviceContext>();
                    if (!geometry)
                        geometry = std::make_unique<TextGeometryCache>();
                    geometry->StartFrame();
                    const TextDeviceScope deviceScope(dc->dc);
                    const TextGeometryScope geometryScope(geometry.get());
                    Drain(index);
                }
                else
                    Drain(index);
#ifdef OBORODZUKI_PROFILE
                samples[index].stages = render_profile();
#endif
            }
            catch (...) { RecordFailure(); }
            {
#ifdef OBORODZUKI_PROFILE
                const auto acquiring = RenderProfile::Clock::now();
#endif
                std::lock_guard<std::mutex> lock(mutex);
#ifdef OBORODZUKI_PROFILE
                samples[index].mutexNs += Since(acquiring);
#endif
                if (--pending == 0)
                    finished.notify_one();
            }
        }
    }

public:
    enum class Batch { Preparation, Capture, Blend };

    EventWorkerPool()
    {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        workers = (std::min)(4u, (std::max)(1u, unsigned(system.dwNumberOfProcessors)));
        wchar_t option[16]{};
        const DWORD length = GetEnvironmentVariableW(L"OBORODZUKI_WORKERS", option, 16);
        if (length && length < 16)
        {
            wchar_t* end = nullptr;
            const auto count = wcstoul(option, &end, 10);
            if (end != option && !*end && count >= 1 && count <= 8)
                workers = unsigned(count);
        }
    }

    ~EventWorkerPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        for (auto& wake : ready)
            wake.notify_one();
        for (auto& thread : threads)
            thread.join();
    }

    bool CanParallelize(size_t events) const { return workers > 1 && events >= 4; }

    void Run(const std::vector<std::function<void()>>& work, Batch batch = Batch::Preparation,
        unsigned blendLimit = 1)
    {
        const auto available = batch == Batch::Blend
            ? (std::min)(work.size(), size_t(blendLimit)) : work.size() / 2;
        const unsigned desiredWorkers = unsigned((std::min)(size_t(workers), available));
        if (desiredWorkers < 2)
        {
            for (const auto& task : work)
                task();
            return;
        }
        // Thread creation failure leaves the existing pool usable.
        while (threads.size() + 1 < desiredWorkers)
        {
            const size_t observed = generation;
            const unsigned index = unsigned(threads.size()) + 1;
            try { threads.emplace_back([this, observed, index] { Worker(observed, index); }); }
            catch (const std::system_error&) { break; }
        }
#ifdef OBORODZUKI_PROFILE
        const auto dispatched = RenderProfile::Clock::now();
#endif
        {
            std::lock_guard<std::mutex> lock(mutex);
#ifdef OBORODZUKI_PROFILE
            stats.mutexNs += Since(dispatched);
#endif
            tasks = &work;
            next.store(0, std::memory_order_relaxed);
            activeWorkers = (std::min)(desiredWorkers, unsigned(threads.size()) + 1);
            needsTextContext = batch != Batch::Blend;
            pending = activeWorkers - 1;
            failure = nullptr;
            ++generation;
#ifdef OBORODZUKI_PROFILE
            published = RenderProfile::Clock::now();
#endif
        }
        for (unsigned i = 0; i + 1 < activeWorkers; ++i)
            ready[i].notify_one();
#ifdef OBORODZUKI_PROFILE
        stats.dispatchNs += Since(dispatched);
        constexpr auto glyph = static_cast<unsigned>(RenderStage::GlyphPath);
        constexpr auto hit = static_cast<unsigned>(RenderStage::GlyphPathHit);
        const auto callerRequests = render_profile().calls[glyph];
        const auto callerHits = render_profile().calls[hit];
#endif
        Drain(0);
#ifdef OBORODZUKI_PROFILE
        const auto waiting = RenderProfile::Clock::now();
#endif
        std::unique_lock<std::mutex> lock(mutex);
#ifdef OBORODZUKI_PROFILE
        stats.mutexNs += Since(waiting);
#endif
        finished.wait(lock, [&] { return pending == 0; });
        tasks = nullptr;
#ifdef OBORODZUKI_PROFILE
        stats.waitNs += Since(waiting);
        if (batch == Batch::Blend)
        {
            stats.blendNs += Since(dispatched);
            ++stats.blendBatches;
        }
        else
            stats.preparationNs += Since(dispatched);
        ++stats.batches;
        stats.tasks += work.size();
        stats.glyphRequests[0] += render_profile().calls[glyph] - callerRequests;
        stats.glyphHits[0] += render_profile().calls[hit] - callerHits;
        for (unsigned worker = 1; worker < activeWorkers; ++worker)
        {
            stats.glyphRequests[worker] += samples[worker].stages.calls[glyph];
            stats.glyphHits[worker] += samples[worker].stages.calls[hit];
            for (unsigned stage = 0; stage < static_cast<unsigned>(RenderStage::Count); ++stage)
            {
                render_profile().ns[stage] += samples[worker].stages.ns[stage];
                render_profile().calls[stage] += samples[worker].stages.calls[stage];
            }
        }
#endif
        if (failure)
            std::rethrow_exception(failure);
    }

    EventWorkerStatistics Statistics() const
    {
#ifdef OBORODZUKI_PROFILE
        std::lock_guard<std::mutex> lock(mutex);
        auto result = stats;
        for (unsigned index = 0; index < 8; ++index)
        {
            const auto& sample = samples[index];
            result.busyNs += sample.busyNs;
            result.wakeNs += sample.wakeNs;
            result.tasksByWorker[index] = sample.tasks;
            result.busyByWorkerNs[index] = sample.busyNs;
            result.idleNs += sample.idleNs;
            result.mutexNs += sample.mutexNs;
        }
        return result;
#else
        return {};
#endif
    }
};

EventWorkerStatistics CRenderedTextSubtitle::WorkerStatistics() const
{
    return m_eventWorkers ? m_eventWorkers->Statistics() : EventWorkerStatistics{};
}

// CWord

#ifdef _VSMOD
struct CWord::NativeOutlineMasks
{
    struct Ring
    {
        const KfxPaint::OutlineLayer* layer;
        std::unique_ptr<Rasterizer> mask;
        std::shared_ptr<const KfxPaint::Sources> paint;
    };
    std::vector<Ring> rings;
    std::unique_ptr<Rasterizer> shadow;
};

bool CWord::HasNativeOutline() const
{
    if (!m_style.nativeOutline || m_style.borderStyle == 1)
        return false;
    for (const auto& layer : m_style.nativeOutline->layers)
    {
        if (layer.number ? layer.x > 0 || layer.y > 0 :
            (layer.blur >= 0 || layer.edgeBlur >= 0 || layer.waitingColor >= 0 || layer.waitingPaint) &&
                m_style.outlineWidthX + m_style.outlineWidthY > 0)
            return true;
    }
    return false;
}

bool CWord::CreateNativeOutline()
{
    const auto& style = *m_style.nativeOutline;
    auto masks = std::make_unique<NativeOutlineMasks>();
    std::vector<OutlineTarget> targets;
    // Keep the additional masks bounded even for pathological tag values.
    constexpr double maxRadius = 2048; // subpixels
    double rx = 0, ry = 0;
    int previousX = 0, previousY = 0;
    auto add = [&](double x, double y, const KfxPaint::OutlineLayer* layer) {
        rx = (std::min)(maxRadius, rx + x);
        ry = (std::min)(maxRadius, ry + y);
        const int roundedX = int(rx + 0.5);
        const int roundedY = int(ry + 0.5);
        if (roundedX == previousX && roundedY == previousY)
            return;
        previousX = roundedX;
        previousY = roundedY;
        auto mask = std::make_unique<Rasterizer>();
        targets.push_back({roundedX, roundedY, mask.get()});
        std::shared_ptr<const KfxPaint::Sources> paint;
        if (layer && layer->number && (layer->paint[0] || layer->paint[1]))
        {
            auto parts = std::make_shared<KfxPaint::Sources>();
            parts->parts[4] = layer->paint[0];
            parts->parts[5] = layer->paint[1];
            paint = std::move(parts);
        }
        masks->rings.push_back({layer, std::move(mask), std::move(paint)});
    };
    const auto* first = !style.layers.empty() && style.layers.front().number == 0 ? &style.layers.front() : nullptr;
    add(m_style.outlineWidthX, m_style.outlineWidthY, first);
    for (const auto& layer : style.layers)
        if (layer.number)
            add(layer.x * style.scaleX * m_style.objectScale / 100,
                layer.y * style.scaleY * m_style.objectScale / 100, &layer);
    if (targets.empty())
        return true;
    if (m_style.shadowDepthX != 0 || m_style.shadowDepthY != 0)
        masks->shadow = std::make_unique<Rasterizer>();
    if (!CreateOutlineRings(targets, masks->shadow.get()))
        return false;
    m_nativeMasks = std::move(masks);
    return true;
}

bool CWord::RasterizeNativeOutline(CPoint p)
{
    if (!m_nativeMasks)
        return true;
    for (const auto& ring : m_nativeMasks->rings)
    {
        const auto* layer = ring.layer;
        const double blur = layer && layer->blur >= 0 ? layer->blur * m_style.objectScale / 100 : -1;
        const int edge = layer && layer->edgeBlur >= 0 ? layer->edgeBlur : m_style.fBlur;
        if (!ring.mask->Rasterize(p.x, p.y, edge,
                blur >= 0 ? blur : m_style.fGaussianBlurX, blur >= 0 ? blur : m_style.fGaussianBlurY, true))
            return false;
    }
    return !m_nativeMasks->shadow || m_nativeMasks->shadow->Rasterize(p.x, p.y, m_style.fBlur,
        m_style.fGaussianBlurX, m_style.fGaussianBlurY, true);
}

CRect CWord::DrawNativeOutline(SubPicDesc& spd, CRect& clip, BYTE* mask, int x, int y, int time, int fade,
    MOD_MOVEVC& vectorClip)
{
    CRect bounds(0, 0, 0, 0);
    if (!m_nativeMasks)
        return bounds;
    // Each mask contains only its own ring. Traverse exterior to interior,
    // which also defines compositing order where independent blurs overlap.
    for (auto r = m_nativeMasks->rings.rbegin(); r != m_nativeMasks->rings.rend(); ++r)
    {
        const auto* layer = r->layer;
        const DWORD rgb = layer && layer->color >= 0 ? DWORD(layer->color) : revcolor(m_style.colors[2]);
        const int alpha = layer && layer->inverseAlpha >= 0 ? layer->inverseAlpha : m_style.alpha[2];
        const DWORD opacity = MulDiv(255 - alpha, 255 - fade, 255);
        MOD_GRADIENT extra;
        extra.paint = r->paint;
        extra.positionedOpacity = m_style.mod_grad.positionedOpacity;
        extra.fadalpha = fade;
        MOD_GRADIENT& paint = layer && layer->number ? extra : m_style.mod_grad;
        const auto draw = [&](CRect region, DWORD color, MOD_GRADIENT& shading) {
            if (region.IsRectEmpty()) return;
            const DWORD colors[4] = {color | (opacity << 24), DWORD(-1), 0, 0};
            bounds |= r->mask->Draw(spd, region, mask, x, y, colors, true, false, 2,
                shading, vectorClip, m_style.mod_blendMode);
        };
        const bool waiting = layer && (layer->waitingColor >= 0 || layer->waitingPaint) && m_ktype != 2 && m_ktype != 3 &&
            time < (m_ktype == 1 ? m_kend : m_kstart);
        if (!waiting)
        {
            draw(clip, rgb, paint);
            continue;
        }
        CRect active(clip), inactive(clip);
        if (m_ktype == 1 && m_kstart < time)
        {
            const double progress = double(time - m_kstart) / (m_kend - m_kstart);
            const double angle = std::remainder(m_style.fontAngleZ, 360.0);
            const bool reversed = std::abs(angle) > 90;
            const int edge = (x + int((reversed ? 1 - progress : progress) * m_width)) >> 3;
            if (reversed)
            {
                active.left = (std::max)(LONG(edge), active.left);
                inactive.right = (std::min)(LONG(edge), inactive.right);
            }
            else
            {
                active.right = (std::min)(LONG(edge), active.right);
                inactive.left = (std::max)(LONG(edge), inactive.left);
            }
            draw(active, rgb, paint);
        }
        auto waitingPaint = paint;
        auto parts = paint.paint ? std::make_shared<KfxPaint::Sources>(*paint.paint) :
            std::make_shared<KfxPaint::Sources>();
        parts->parts[4] = layer->waitingPaint;
        waitingPaint.paint = std::move(parts);
        waitingPaint.legacyParts &= ~(1u << 4);
        draw(inactive, layer->waitingColor >= 0 ? DWORD(layer->waitingColor) : rgb, waitingPaint);
    }
    return bounds;
}

CRect CWord::DrawNativeShadow(SubPicDesc& spd, CRect& clip, BYTE* mask, int x, int y, DWORD* colors,
    MOD_MOVEVC& vectorClip)
{
    auto* rasterizer = m_nativeMasks && m_nativeMasks->shadow ? m_nativeMasks->shadow.get() : this;
    return rasterizer->Draw(spd, clip, mask, x, y, colors, true, false, 3, m_style.mod_grad,
        vectorClip, m_style.mod_blendMode);
}

bool CWord::RasterizeDecorations(CPoint p, CPoint org)
{
    if (!m_undecoratedGlyphs)
        return true;
#ifdef _LUA
    m_undecoratedGlyphs->Paint(p, org, m_layer);
#else
    m_undecoratedGlyphs->Paint(p, org);
#endif
    if (!m_decorationMask)
    {
        auto mask = std::make_unique<Rasterizer>();
        if (!mask->CreateDecorationMask(*this, *m_undecoratedGlyphs))
        {
            m_undecoratedGlyphs.reset();
            return true;
        }
        m_decorationMask = std::move(mask);
    }
    return m_decorationMask->Rasterize(p.x, p.y, m_style.fBlur, m_style.fGaussianBlurX, m_style.fGaussianBlurY);
}

CRect CWord::DrawBodyPaint(SubPicDesc& spd, CRect& clip, BYTE* mask, int x, int y, const DWORD* colors,
    MOD_MOVEVC& vectorClip, bool reverse)
{
    auto& style = m_style.mod_grad;
    if (!m_decorationMask)
        return Draw(spd, clip, mask, x, y, colors, true, false, 0, style, vectorClip, m_style.mod_blendMode, reverse);

    auto bounds = m_undecoratedGlyphs->Draw(spd, clip, mask, x, y, colors, true, false, 0,
        style, vectorClip, m_style.mod_blendMode, reverse);
    const auto& deco = *m_style.decoration;
    auto gradient = style;
    auto sources = style.paint ? std::make_shared<KfxPaint::Sources>(*style.paint) :
        std::make_shared<KfxPaint::Sources>();
    DWORD palette[4];
    std::copy_n(colors, 4, palette);
    for (int i = 0; i < 2; ++i)
    {
        for (int component = 0; component < 2; ++component)
        {
            const int part = 2 * i + component;
            if (deco.paint[component] || (component ? deco.inverseAlpha : deco.color) >= 0)
            {
                sources->parts[part] = deco.paint[component];
                gradient.legacyParts &= ~(1u << part);
            }
        }
        if (deco.color >= 0)
            palette[2 * i] = (palette[2 * i] & 0xff000000u) | DWORD(deco.color);
        if (deco.inverseAlpha >= 0)
        {
            const DWORD opacity = MulDiv(255 - deco.inverseAlpha, 255 - style.fadalpha, 255);
            palette[2 * i] = (palette[2 * i] & 0xffffffu) | (opacity << 24);
        }
    }
    gradient.paint = std::move(sources);
    bounds |= m_decorationMask->Draw(spd, clip, mask, x, y, palette, true, false, 0,
        gradient, vectorClip, m_style.mod_blendMode, reverse);
    return bounds;
}
#endif

CWord::CWord(STSStyle& style, CStringW str, int ktype, int kstart, int kend, double scalex, double scaley)
    : m_style(style), m_str(str)
    , m_width(0), m_ascent(0), m_descent(0)
    , m_ktype(ktype), m_kstart(kstart), m_kend(kend)
    , m_scalex(scalex), m_scaley(scaley)
    , m_fDrawn(false), m_p(INT_MAX, INT_MAX)
    , m_fLineBreak(false), m_fWhiteSpaceChar(false)
    , m_pOpaqueBox(NULL), isOpaqueBox(false)
{
    if(str.IsEmpty())
    {
        m_fWhiteSpaceChar = m_fLineBreak = true;
    }
}

void CWord::MeasureSelectedFont()
{
    TEXTMETRIC metrics{};
    GetTextMetrics(g_hDC, &metrics);
    m_ascent = int(m_style.fontScaleY / 100 * ((metrics.tmAscent + 4) >> 3));
    m_descent = int(m_style.fontScaleY / 100 * ((metrics.tmDescent + 4) >> 3));
}

CWord::CWord(CWord& word)
    : CWord(word.m_style, word.m_str, word.m_ktype, word.m_kstart, word.m_kend, word.m_scalex, word.m_scaley)
{
    m_width = word.m_width;
    m_ascent = word.m_ascent;
    m_descent = word.m_descent;
    m_logicalLine = word.m_logicalLine;
    m_fLineBreak = word.m_fLineBreak;
    m_fWhiteSpaceChar = word.m_fWhiteSpaceChar;
    isOpaqueBox = word.isOpaqueBox;
#ifdef _VSMOD
    if (word.m_distortionDomain)
        m_distortionDomain = std::make_unique<CRect>(*word.m_distortionDomain);
#endif
}

CWord::~CWord()
{
    if(m_pOpaqueBox) delete m_pOpaqueBox;
}

bool CWord::Append(CWord* w)
{
    if(!(m_style == w->m_style)
       || m_fLineBreak || w->m_fLineBreak
       || w->m_kstart != w->m_kend || m_ktype != w->m_ktype) return(false);

    m_fWhiteSpaceChar = m_fWhiteSpaceChar && w->m_fWhiteSpaceChar;
    m_str += w->m_str;
    m_width += w->m_width;

    m_fDrawn = false;
    m_p = CPoint(INT_MAX, INT_MAX);

    return(true);
}

#if defined (_VSMOD) && defined(_LUA)
void CWord::Paint(CPoint p, CPoint org, int Layer)
#else
void CWord::Paint(CPoint p, CPoint org)
#endif
{
#if defined (_VSMOD) && defined(_LUA)
    m_layer = Layer;
#endif
    if(!m_str) return;

    if(!m_fDrawn)
    {
        if(!CreatePath()) return;

        CPoint morg = CPoint((org.x - p.x) * 8, (org.y - p.y) * 8);
#if defined (_VSMOD) && defined(_LUA)
        if(m_style.LuaBeforeTransformHandler.GetLength() > 0) CustomTransform(morg, m_style.LuaBeforeTransformHandler, Layer);
        if(m_style.LuaCustomTransformHandler.GetLength() > 0)
            CustomTransform(morg, m_style.LuaCustomTransformHandler, Layer);
        else
            Transform(morg);
        if(m_style.LuaAfterTransformHandler.GetLength() > 0) CustomTransform(morg, m_style.LuaAfterTransformHandler, Layer);
#else
        Transform(CPoint((org.x - p.x) * 8, (org.y - p.y) * 8));
#endif

#ifdef _VSMOD
        if(!ScanConvert(m_style.borderStyle == 3)) return;
#else
        if(!ScanConvert()) return;
#endif

#ifdef _VSMOD
        m_nativeMasks.reset();
        m_decorationMask.reset();
        m_undecoratedGlyphs.reset();
        if (m_style.decoration && m_style.decoration->Active() && (m_style.fUnderline || m_style.fStrikeOut))
            m_undecoratedGlyphs = CreateUndecoratedWord();
        if (HasNativeOutline())
        {
            if (!CreateNativeOutline()) return;
        }
        else
#endif
        if(m_style.borderStyle != 1 && (m_style.outlineWidthX + m_style.outlineWidthY > 0))
        {
            if(!CreateWidenedRegion((int)(m_style.outlineWidthX + 0.5), (int)(m_style.outlineWidthY + 0.5))) return;
        }
        else if(m_style.borderStyle == 1)
        {
            if(!CreateOpaqueBox()) return;
        }

        if(!Rasterize(p.x & 7, p.y & 7, m_style.fBlur, m_style.fGaussianBlurX, m_style.fGaussianBlurY)) return;
#ifdef _VSMOD
        if (!RasterizeNativeOutline(p) || !RasterizeDecorations(p, org)) return;
#endif
        m_fDrawn = true;
    }
    else if((m_p.x & 7) != (p.x & 7) || (m_p.y & 7) != (p.y & 7))
    {
        Rasterize(p.x & 7, p.y & 7, m_style.fBlur, m_style.fGaussianBlurX, m_style.fGaussianBlurY);
#ifdef _VSMOD
        if (!RasterizeNativeOutline(p) || !RasterizeDecorations(p, org)) return;
#endif
    }

    m_p = p;

    if(m_pOpaqueBox)
#if defined (_VSMOD) && defined(_LUA)
        m_pOpaqueBox->Paint(p, org, 5);
#else
        m_pOpaqueBox->Paint(p, org);
#endif
}

#ifdef _VSMOD
#include <emmintrin.h>

#ifdef _LUA
void CWord::CustomTransform(CPoint org, CString F, int Layer)
{
    PROFILE_RENDER_STAGE(Transform);
    CStringA Func(F);
    // Calculate size:
    int minx = INT_MAX, miny = INT_MAX, maxx = -INT_MAX, maxy = -INT_MAX;

    for(int i = 0; i < mPathPoints; i++)
    {
        if(minx > mpPathPoints[i].x) minx = mpPathPoints[i].x;
        if(miny > mpPathPoints[i].y) miny = mpPathPoints[i].y;
        if(maxx < mpPathPoints[i].x) maxx = mpPathPoints[i].x;
        if(maxy < mpPathPoints[i].y) maxy = mpPathPoints[i].y;
    }

    int xsz = max(maxx - minx, 0);
    int ysz = max(maxy - miny, 0);

    for(ptrdiff_t i = 0; i < mPathPoints; i++)
    {
        double x, y, z, xx, yy, zz;

        x = mpPathPoints[i].x;
        y = mpPathPoints[i].y;

        // Find function =D
        lua_getglobal(L, Func);

        // Create line table
        lua_newtable(L);

        // 1 - text, 3 - outline, 4 - shadow, 5 - opaque box, 6 - clip
        LuaAddNumberField(L, "layer", Layer);
        // ID
        LuaAddNumberField(L, "id", m_entry);

        // Geometry
        LuaAddNumberField(L, "minx", minx);
        LuaAddNumberField(L, "maxx", maxx);
        LuaAddNumberField(L, "miny", miny);
        LuaAddNumberField(L, "maxy", maxy);

        // Style
        LuaAddNumberField(L, "frx", m_style.fontAngleX);
        LuaAddNumberField(L, "fry", m_style.fontAngleY);
        LuaAddNumberField(L, "frz", m_style.fontAngleZ);

        LuaAddNumberField(L, "fscx", m_style.fontScaleX);
        LuaAddNumberField(L, "fscy", m_style.fontScaleY);

        LuaAddNumberField(L, "fax", m_style.fontShiftX);
        LuaAddNumberField(L, "fay", m_style.fontShiftY);

        // Size
        lua_newtable(L);
        LuaAddNumberField(L, "w", xsz);
        LuaAddNumberField(L, "h", ysz);
        lua_setfield(L, -2, "size");

        // Pos
        lua_newtable(L);
        LuaAddNumberField(L, "x", x);
        LuaAddNumberField(L, "y", y);
        LuaAddNumberField(L, "z", m_style.mod_z);
        lua_setfield(L, -2, "pos");

        // Org table
        lua_newtable(L);
        LuaAddNumberField(L, "x", org.x);
        LuaAddNumberField(L, "y", org.y);
        lua_setfield(L, -2, "org");

        // User table
        {
            CStringA index;
            index.Format("sub_%d", m_entry);
            lua_getglobal(L, index);
            if(lua_istable(L, -1))
                lua_setfield(L, -2, "user");
            else
                lua_pop(L, 1);
        }

        if (lua_pcall(L, 1, 1, 0) != 0)
        {
	        // error
	        CString ErrorText = L"Error: ";
	        CString LuaErrorText(lua_tostring(L, -1));
            lua_pop(L, 1);
	        LuaError(ErrorText + LuaErrorText);
        }
        else
        {
	        // Retrieve result
	        if (!lua_istable(L, -1))
		        LuaError(L"Line function must return a table");
	        else
	        {
                if(LuaIsNumber(L, L"x")) mpPathPoints[i].x = LuaGetFloat(L, L"x");
                if(LuaIsNumber(L, L"y")) mpPathPoints[i].y = LuaGetFloat(L, L"y");
	        }
            lua_pop(L, 1);
        }
    }
}
#endif
#endif

void CWord::Transform(CPoint org)
{
    PROFILE_RENDER_STAGE(Transform);
    double scalex = isOpaqueBox ? 1 : m_style.fontScaleX / 100;
    double scaley = isOpaqueBox ? 1 : m_style.fontScaleY / 100;
    const double xzoomf = m_scalex * 20000.0;
    const double yzoomf = m_scaley * 20000.0;

    double caz = cos((3.1415 / 180) * m_style.fontAngleZ);
    double saz = sin((3.1415 / 180) * m_style.fontAngleZ);
    double cax = cos((3.1415 / 180) * m_style.fontAngleX);
    double sax = sin((3.1415 / 180) * m_style.fontAngleX);
    double cay = cos((3.1415 / 180) * m_style.fontAngleY);
    double say = sin((3.1415 / 180) * m_style.fontAngleY);

#ifdef _VSMOD
    double xrnd = m_style.mod_rand.X * 100;
    double yrnd = m_style.mod_rand.Y * 100;
    double zrnd = m_style.mod_rand.Z * 100;

    bool ortho = m_style.mod_ortho;

    srand(m_style.mod_rand.Seed);
    // CPUID from VDub
    bool fSSE2 = !!(g_cpuid.m_flags & CCpuID::sse2);

    // SSE code
    // speed up ~1.5-1.7x
    if(fSSE2)
    {
        __m128 __xshift = _mm_set_ps1(m_style.fontShiftX);
        __m128 __yshift = _mm_set_ps1(m_style.fontShiftY);

        __m128 __xorg = _mm_set_ps1(org.x);
        __m128 __yorg = _mm_set_ps1(org.y);

        __m128 __xscale = _mm_set_ps1(scalex);
        __m128 __yscale = _mm_set_ps1(scaley);
        const __m128 __xzoomf = _mm_set_ps1((float)(m_scalex * 20000.0));
        const __m128 __yzoomf = _mm_set_ps1((float)(m_scaley * 20000.0));
        const __m128 __1000 = _mm_set_ps1(1000.0f);

        __m128 __xsz = _mm_setzero_ps();
        __m128 __ysz = _mm_setzero_ps();

        __m128 __dst1x, __dst1y, __dst213x, __dst213y, __dst3x, __dst3y;

        __m128 __miny;
        __m128 __minx = _mm_set_ps(INT_MAX, INT_MAX, 0, 0);
        __m128 __max = _mm_set_ps(-INT_MAX, -INT_MAX, 1, 1);

        bool is_dist = m_style.mod_distort.enabled;
        if(is_dist)
        {
            if (m_distortionDomain)
            {
                const auto& plane = *m_distortionDomain;
                __minx = _mm_set_ps(float(plane.left), float(plane.top), 0, 0);
                __max = _mm_set_ps(float(plane.right), float(plane.bottom), 0, 0);
            }
            else
                for(int i = 0; i < mPathPoints; i++)
                {
                    const auto point = _mm_set_ps(float(mpPathPoints[i].x), float(mpPathPoints[i].y), 0, 0);
                    __minx = _mm_min_ps(__minx, point);
                    __max = _mm_max_ps(__max, point);
                }

            __m128 __zero = _mm_setzero_ps();
            __max = _mm_sub_ps(__max, __minx); // xsz, ysz, 1, 1
            __max = _mm_max_ps(__max, __zero);

            __xsz = _mm_shuffle_ps(__max, __max, _MM_SHUFFLE(3,3,3,3));
            __ysz = _mm_shuffle_ps(__max, __max, _MM_SHUFFLE(2,2,2,2));

            __miny = _mm_shuffle_ps(__minx, __minx, _MM_SHUFFLE(2,2,2,2));
            __minx = _mm_shuffle_ps(__minx, __minx, _MM_SHUFFLE(3,3,3,3));

            __dst1x = _mm_set_ps1(m_style.mod_distort.pointsx[0]);
            __dst1y = _mm_set_ps1(m_style.mod_distort.pointsy[0]);
            __dst3x = _mm_set_ps1(m_style.mod_distort.pointsx[2]);
            __dst3y = _mm_set_ps1(m_style.mod_distort.pointsy[2]);
            __dst213x = _mm_set_ps1(m_style.mod_distort.pointsx[1]); // 2 - 1 - 3
            __dst213x = _mm_sub_ps(__dst213x, __dst1x);
            __dst213x = _mm_sub_ps(__dst213x, __dst3x);

            __dst213y = _mm_set_ps1(m_style.mod_distort.pointsy[1]);
            __dst213y = _mm_sub_ps(__dst213y, __dst1y);
            __dst213y = _mm_sub_ps(__dst213y, __dst3y); 
        }

        __m128 __caz = _mm_set_ps1(caz);
        __m128 __saz = _mm_set_ps1(saz);
        __m128 __cax = _mm_set_ps1(cax);
        __m128 __sax = _mm_set_ps1(sax);
        __m128 __cay = _mm_set_ps1(cay);
        __m128 __say = _mm_set_ps1(say);

        // this can be paralleled for openmp
        int mPathPointsD4 = mPathPoints / 4;
        int mPathPointsM4 = mPathPoints % 4;

        for(ptrdiff_t i = 0; i < mPathPointsD4 + 1; i++)
        {
            __m128 __pointx, __pointy;
            // we cann't use load .-.
            if(i == mPathPointsD4) // last cycle
            {
                switch(mPathPointsM4)
                {
                case 0: continue;
                case 1:
                    __pointx = _mm_set_ps(mpPathPoints[4 * i + 0].x, 0, 0, 0);
                    __pointy = _mm_set_ps(mpPathPoints[4 * i + 0].y, 0, 0, 0);
                    break;
                case 2:
                    __pointx = _mm_set_ps(mpPathPoints[4 * i + 0].x, mpPathPoints[4 * i + 1].x, 0, 0);
                    __pointy = _mm_set_ps(mpPathPoints[4 * i + 0].y, mpPathPoints[4 * i + 1].y, 0, 0);
                    break;
                case 3:
                    __pointx = _mm_set_ps(mpPathPoints[4 * i + 0].x, mpPathPoints[4 * i + 1].x, mpPathPoints[4 * i + 2].x, 0);
                    __pointy = _mm_set_ps(mpPathPoints[4 * i + 0].y, mpPathPoints[4 * i + 1].y, mpPathPoints[4 * i + 2].y, 0);
                    break;
                }
            }
            else
            {
                // 
                __pointx = _mm_set_ps(mpPathPoints[4 * i + 0].x, mpPathPoints[4 * i + 1].x, mpPathPoints[4 * i + 2].x, mpPathPoints[4 * i + 3].x);
                __pointy = _mm_set_ps(mpPathPoints[4 * i + 0].y, mpPathPoints[4 * i + 1].y, mpPathPoints[4 * i + 2].y, mpPathPoints[4 * i + 3].y);
            }
            __m128 __pointz = _mm_set_ps1(m_style.mod_z);

            // distort
            if(is_dist)
            {
                //P = P0 + (P1 - P0)u + (P3 - P0)v + (P0 + P2 - P1 - P3)uv
                __m128 __u = _mm_sub_ps(__pointx, __minx);
                __m128 __v = _mm_sub_ps(__pointy, __miny);
                __m128 __1_xsz = _mm_rcp_ps(__xsz);
                __m128 __1_ysz = _mm_rcp_ps(__ysz);
                __u = _mm_mul_ps(__u, __1_xsz);
                __v = _mm_mul_ps(__v, __1_ysz);

                // x
                __pointx = _mm_mul_ps(__dst213x, __u);
                __pointx = _mm_mul_ps(__pointx, __v);

                __m128 __tmpx = _mm_mul_ps(__dst3x, __v);
                __pointx = _mm_add_ps(__pointx, __tmpx);
                __tmpx = _mm_mul_ps(__dst1x, __u);
                __pointx = _mm_add_ps(__pointx, __tmpx);

                __pointx = _mm_mul_ps(__pointx, __xsz);
                __pointx = _mm_add_ps(__pointx, __minx);

                // y
                __pointy = _mm_mul_ps(__dst213y, __u);
                __pointy = _mm_mul_ps(__pointy, __v);

                __m128 __tmpy = _mm_mul_ps(__dst3y, __v);
                __pointy = _mm_add_ps(__pointy, __tmpy);
                __tmpy = _mm_mul_ps(__dst1y, __u);
                __pointy = _mm_add_ps(__pointy, __tmpy);

                __pointy = _mm_mul_ps(__pointy, __ysz);
                __pointy = _mm_add_ps(__pointy, __miny);
            }

            // randomize
            if(xrnd!=0 || yrnd!=0 || zrnd!=0)
            {
                __declspec(align(16)) float rx[4], ry[4], rz[4]; 
                for(int k=0;k<4;k++)
                {
                    rx[k] = xrnd > 0 ? (xrnd - rand() % (int)(xrnd * 2 + 1)) : 0;
                    ry[k] = yrnd > 0 ? (yrnd - rand() % (int)(yrnd * 2 + 1)) : 0;
                    rz[k] = zrnd > 0 ? (zrnd - rand() % (int)(zrnd * 2 + 1)) : 0;
                }
                __m128 __001 = _mm_set_ps1(0.01f);

                if(xrnd!=0)
                {
                    __m128 __rx = _mm_load_ps(rx);
                    __rx = _mm_mul_ps(__rx, __001);
                    __pointx = _mm_add_ps(__pointx, __rx);
                }

                if(yrnd!=0)
                {
                    __m128 __ry = _mm_load_ps(ry);
                    __ry = _mm_mul_ps(__ry, __001);
                    __pointy = _mm_add_ps(__pointy, __ry);
                }

                if(zrnd!=0)
                {
                    __m128 __rz = _mm_load_ps(rz);
                    __rz = _mm_mul_ps(__rz, __001);
                    __pointz = _mm_add_ps(__pointz, __rz);
                }
            }

            // scale and shift
            __m128 __tmpx;
            if(m_style.fontShiftX!=0)
            {
                __tmpx = _mm_mul_ps(__xshift, __pointy);
                __tmpx = _mm_add_ps(__tmpx, __pointx);
            }
            else
            {
                __tmpx = __pointx;
            }
            __tmpx = _mm_mul_ps(__tmpx, __xscale);
            __tmpx = _mm_sub_ps(__tmpx, __xorg);

            __m128 __tmpy;
            if(m_style.fontShiftY!=0)
            {
                __tmpy = _mm_mul_ps(__yshift, __pointx);
                __tmpy = _mm_add_ps(__tmpy, __pointy);
            }
            else
            {
                __tmpy = __pointy;
            }
            __tmpy = _mm_mul_ps(__tmpy, __yscale);
            __tmpy = _mm_sub_ps(__tmpy, __yorg);

            // rotate
            __m128 __xx = _mm_mul_ps(__tmpx, __caz);
            __m128 __yy = _mm_mul_ps(__tmpy, __saz);
            __pointx = _mm_add_ps(__xx, __yy);
            __xx = _mm_mul_ps(__tmpx, __saz);
            __yy = _mm_mul_ps(__tmpy, __caz);
            __pointy = _mm_sub_ps(__yy, __xx);

            __m128 __zz = _mm_mul_ps(__pointz, __sax);
            __yy = _mm_mul_ps(__pointy, __cax);
            __tmpy = __pointy;
            __pointy = _mm_add_ps(__yy, __zz);
            __zz = _mm_mul_ps(__pointz, __cax);
            __yy = _mm_mul_ps(__tmpy, __sax);
            __pointz = _mm_sub_ps(__yy, __zz);

            __xx = _mm_mul_ps(__pointx, __cay);
            __zz = _mm_mul_ps(__pointz, __say);
            __tmpx = __pointx;
            __pointx = _mm_add_ps(__xx, __zz);
            if (ortho)
            {// vpatch v001. Orthogonal 2D projection
                __pointz = _mm_set_ps1(0);
            }
            else
            {
                __xx = _mm_mul_ps(__tmpx, __say);
                __zz = _mm_mul_ps(__pointz, __cay);
                __pointz = _mm_sub_ps(__xx, __zz);
            }

            __m128 __tmpzz = _mm_add_ps(__pointz, __xzoomf); // zz + xzoomf

            __xx = _mm_mul_ps(__pointx, __xzoomf);       // xx * xzoomf
            __pointx = _mm_div_ps(__xx, _mm_max_ps(__tmpzz, __1000)); // x = (xx * xzoomf) / std::max((zz + xzoomf), 1000.0)

            __tmpzz = _mm_add_ps(__pointz, __yzoomf);       // zz + yzoomf

            __yy = _mm_mul_ps(__pointy, __yzoomf);       // yy * yzoomf
            __pointy = _mm_div_ps(__yy, _mm_max_ps(__tmpzz, __1000)); // y = yy * yzoomf / std::max((zz + yzoomf), 1000.0);

            __pointx = _mm_add_ps(__pointx, __xorg);
            __pointy = _mm_add_ps(__pointy, __yorg);

            __m128 __05 = _mm_set_ps1(0.5);

            __pointx = _mm_add_ps(__pointx, __05);
            __pointy = _mm_add_ps(__pointy, __05);

            if(i == mPathPointsD4) // last cycle
            {
                for(int k=0;k<mPathPointsM4;k++)
                {
                    mpPathPoints[i*4+k].x = static_cast<LONG>(__pointx.m128_f32[3-k]);
                    mpPathPoints[i*4+k].y = static_cast<LONG>(__pointy.m128_f32[3-k]);
                }
            }
            else
            {
                for(int k=0;k<4;k++)
                {
                    mpPathPoints[i*4+k].x = static_cast<LONG>(__pointx.m128_f32[3-k]);
                    mpPathPoints[i*4+k].y = static_cast<LONG>(__pointy.m128_f32[3-k]);
                }
            }
        }
    }
    else
    // C-code
    {
        // patch m008. distort
        int xsz, ysz;
        double dst1x, dst1y, dst2x, dst2y, dst3x, dst3y;
        int minx = INT_MAX, miny = INT_MAX, maxx = -INT_MAX, maxy = -INT_MAX;

        bool is_dist = m_style.mod_distort.enabled;
        if(is_dist)
        {
            if (m_distortionDomain)
            {
                const auto& plane = *m_distortionDomain;
                minx = plane.left;
                miny = plane.top;
                maxx = plane.right;
                maxy = plane.bottom;
            }
            else
                for(int i = 0; i < mPathPoints; i++)
                {
                    if(minx > mpPathPoints[i].x) minx = mpPathPoints[i].x;
                    if(miny > mpPathPoints[i].y) miny = mpPathPoints[i].y;
                    if(maxx < mpPathPoints[i].x) maxx = mpPathPoints[i].x;
                    if(maxy < mpPathPoints[i].y) maxy = mpPathPoints[i].y;
                }

            xsz = max(maxx - minx, 0);
            ysz = max(maxy - miny, 0);

            dst1x = m_style.mod_distort.pointsx[0];
            dst1y = m_style.mod_distort.pointsy[0];
            dst2x = m_style.mod_distort.pointsx[1];
            dst2y = m_style.mod_distort.pointsy[1];
            dst3x = m_style.mod_distort.pointsx[2];
            dst3y = m_style.mod_distort.pointsy[2];
        }
#endif

        for(ptrdiff_t i = 0; i < mPathPoints; i++)
        {
            double x, y, z, xx, yy, zz;

            x = mpPathPoints[i].x;
            y = mpPathPoints[i].y;
#ifdef _VSMOD
            // patch m002. Z-coord
            z = m_style.mod_z;

            double u, v;
            if((is_dist) && (xsz) && (ysz))
            {
                u = (x - minx) / xsz;
                v = (y - miny) / ysz;

                x = minx + (0 + (dst1x - 0) * u + (dst3x - 0) * v + (0 + dst2x - dst1x - dst3x) * u * v) * xsz;
                y = miny + (0 + (dst1y - 0) * u + (dst3y - 0) * v + (0 + dst2y - dst1y - dst3y) * u * v) * ysz;
                //P = P0 + (P1 - P0)u + (P3 - P0)v + (P0 + P2 - P1 - P3)uv
            }

            // patch m003. random text points
            x = xrnd > 0 ? (xrnd - rand() % (int)(xrnd * 2 + 1)) / 100.0 + x : x;
            y = yrnd > 0 ? (yrnd - rand() % (int)(yrnd * 2 + 1)) / 100.0 + y : y;
            z = zrnd > 0 ? (zrnd - rand() % (int)(zrnd * 2 + 1)) / 100.0 + z : z;
#else
            z = 0;
#endif
            double _x = x;
            x = scalex * (x + m_style.fontShiftX * y) - org.x;
            y = scaley * (y + m_style.fontShiftY * _x) - org.y;

            xx = x * caz + y * saz;
            yy = -(x * saz - y * caz);
            zz = z;

            x = xx;
            y = yy * cax + zz * sax;
            z = yy * sax - zz * cax;

            xx = x * cay + z * say;
            yy = y;
            if (ortho) { // vpatch v001. Orthogonal 2D projection
                zz = 0;
            }
            else {
                zz = x * say - z * cay;
            }

            x = xx * xzoomf / max(zz + xzoomf, 1000.0);
            y = yy * yzoomf / max(zz + yzoomf, 1000.0);

            mpPathPoints[i].x = (LONG)(x + org.x + 0.5);
            mpPathPoints[i].y = (LONG)(y + org.y + 0.5);
        }
    }
}

bool CWord::CreateOpaqueBox()
{
    if(m_pOpaqueBox) return(true);

    STSStyle style = m_style;
    style.borderStyle = 0;
    style.outlineWidthX = style.outlineWidthY = 0;
    style.colors[0] = m_style.colors[2];
    style.alpha[0] = m_style.alpha[2];

    int w = (int)(m_style.outlineWidthX + 0.5);
    int h = (int)(m_style.outlineWidthY + 0.5);

    CStringW str;
    str.Format(L"m %d %d l %d %d %d %d %d %d",
               -w, -h,
               m_width + w, -h,
               m_width + w, m_ascent + m_descent + h,
               -w, m_ascent + m_descent + h);

    m_pOpaqueBox = DNew CPolygon(style, str, 0, 0, 0, 1.0 / 8, 1.0 / 8, 0);
    m_pOpaqueBox->isOpaqueBox = true;

    return(!!m_pOpaqueBox);
}

// CText

CText::CText(STSStyle& style, CStringW str, int ktype, int kstart, int kend, double scalex, double scaley)
    : CWord(style, str, ktype, kstart, kend, scalex, scaley)
{
    PROFILE_RENDER_STAGE(TextMeasure);
    if (m_str == L" ")
        m_fWhiteSpaceChar = true;

    const bool measureCharacters = m_style.fontSpacing || (long)GetVersion() < 0;
    const auto logFont = TextLogFont(m_style);
    auto* geometry = !measureCharacters && activeTextGeometry && m_str.GetLength() <= 128
        ? &activeTextGeometry->Get(logFont, m_str) : nullptr;
#ifdef _VSMOD
    const double angle = m_style.mod_fontOrient * 3.1415926 / 1800;
    const double cosOrientation = abs(cos(angle)), sinOrientation = abs(sin(angle));
#endif
    const auto applyMetrics = [&](int ascent, int descent) {
        m_ascent = int(m_style.fontScaleY / 100 * ascent);
        m_descent = int(m_style.fontScaleY / 100 * descent);
    };
    const auto measureWidth = [&](const CSize& extent) {
#ifdef _VSMOD
        return extent.cx * cosOrientation + extent.cy * sinOrientation;
#else
        return double(extent.cx);
#endif
    };
    if (geometry && geometry->measured)
    {
        PROFILE_RENDER_STAGE(TextMeasureHit);
        applyMetrics(geometry->ascent, geometry->descent);
        m_width = int(m_style.fontScaleX / 100 * int(measureWidth(geometry->extent)) + 4) >> 3;
        return;
    }

    PROFILE_RENDER_STAGE(GdiMetrics);
    CMyFont font(logFont);
    const SelectedTextFont selected(font);
    TEXTMETRIC metrics{};
    const bool hasMetrics = !!GetTextMetrics(g_hDC, &metrics);
    const int ascent = (metrics.tmAscent + 4) >> 3;
    const int descent = (metrics.tmDescent + 4) >> 3;
    applyMetrics(ascent, descent);
    if (measureCharacters)
    {
        for (LPCWSTR s = m_str; *s; ++s)
        {
            CSize extent;
            if (!GetTextExtentPoint32W(g_hDC, s, 1, &extent))
            {
                ASSERT(0);
                return;
            }
#ifdef _VSMOD
            m_width += int(measureWidth(extent) + m_style.fontSpacing);
#else
            m_width += extent.cx + int(m_style.fontSpacing);
#endif
        }
    }
    else
    {
        CSize extent;
        if (!GetTextExtentPoint32W(g_hDC, m_str, wcslen(str), &extent))
        {
            ASSERT(0);
            return;
        }
        m_width = int(measureWidth(extent));
        if (geometry && hasMetrics && selected && font.GetSafeHandle())
        {
            geometry->extent = extent;
            geometry->ascent = ascent;
            geometry->descent = descent;
            geometry->measured = true;
        }
    }
    m_width = int(m_style.fontScaleX / 100 * m_width + 4) >> 3;
}

CWord* CText::Copy()
{
    CWord* T = DNew CText(*this);
#if defined(_VSMOD) && defined(_LUA)
    T->L = L;
    T->LuaLog = LuaLog;
    T->m_entry = m_entry;
    T->LuaRendererHandler = LuaRendererHandler;
#endif
    return T;
}

#ifdef _VSMOD
std::unique_ptr<CWord> CText::CreateUndecoratedWord()
{
#ifdef _LUA
    if (!LuaRendererHandler.IsEmpty() || !m_style.LuaBeforeTransformHandler.IsEmpty() ||
        !m_style.LuaCustomTransformHandler.IsEmpty() || !m_style.LuaAfterTransformHandler.IsEmpty())
        return {};
#endif
    auto copy = std::unique_ptr<CWord>(Copy());
    auto& style = copy->m_style;
    style.fUnderline = style.fStrikeOut = false;
    style.borderStyle = 0;
    style.outlineWidthX = style.outlineWidthY = 0;
    style.shadowDepthX = style.shadowDepthY = 0;
    style.nativeOutline.reset();
    style.decoration.reset();
    return copy;
}
#endif

bool CText::Append(CWord* w)
{
    return(dynamic_cast<CText*>(w) && CWord::Append(w));
}

bool CText::CreatePath()
{
    PROFILE_RENDER_STAGE(GlyphPath);
    const bool measureCharacters = m_style.fontSpacing || (long)GetVersion() < 0;
    const auto logFont = TextLogFont(m_style);
    auto* geometry = !measureCharacters && activeTextGeometry && m_str.GetLength() <= 128
        ? &activeTextGeometry->Get(logFont, m_str) : nullptr;
    if (geometry && geometry->hasPath)
    {
        PROFILE_RENDER_STAGE(GlyphPathHit);
        if (CopyPath(geometry->points.data(), geometry->types.data(), geometry->points.size()))
            return true;
    }

    PROFILE_RENDER_STAGE(GdiPath);
    CMyFont font(logFont);
    const SelectedTextFont selected(font);

    int width = 0;

    if(measureCharacters)
    {
        bool bFirstPath = true;

        for(LPCWSTR s = m_str; *s; s++)
        {
            CSize extent;
            if(!GetTextExtentPoint32W(g_hDC, s, 1, &extent))
            {
                ASSERT(0);
                return(false);
            }

            PartialBeginPath(g_hDC, bFirstPath);
            bFirstPath = false;
            TextOutW(g_hDC, 0, 0, s, 1);
            PartialEndPath(g_hDC, width, 0);

            width += extent.cx + (int)m_style.fontSpacing;
        }
    }
    else
    {
        CSize extent;
        if(!GetTextExtentPoint32W(g_hDC, m_str, m_str.GetLength(), &extent))
        {
            ASSERT(0);
            return(false);
        }

        const bool begun = BeginPath(g_hDC);
        const bool drawn = !!TextOutW(g_hDC, 0, 0, m_str, m_str.GetLength());
        const bool ended = EndPath(g_hDC);
        if (geometry && !geometry->hasPath && begun && drawn && ended && selected &&
            font.GetSafeHandle() && (!mPathPoints || (mpPathPoints && mpPathTypes)))
            activeTextGeometry->SavePath(*geometry, mpPathPoints, mpPathTypes, mPathPoints);
    }

    return(true);
}

// CPolygon

CPolygon::CPolygon(STSStyle& style, CStringW str, int ktype, int kstart, int kend, double scalex, double scaley, int baseline)
    : CWord(style, str, ktype, kstart, kend, scalex, scaley)
    , m_scalex(scalex), m_scaley(scaley), m_baseline(baseline)
{
    CMyFont font(m_style);
    HFONT oldFont = SelectFont(g_hDC, font);
    MeasureSelectedFont();
    SelectFont(g_hDC, oldFont);
    ParseStr();
}

CPolygon::~CPolygon()
{
}

CWord* CPolygon::Copy()
{
    CWord* T = DNew CPolygon(m_style, m_str, m_ktype, m_kstart, m_kend, m_scalex, m_scaley, m_baseline);
#if defined(_VSMOD) && defined(_LUA)
    T->L = L;
    T->LuaLog = LuaLog;
    T->m_entry = m_entry;
    T->m_layer = m_layer;
    T->LuaRendererHandler = LuaRendererHandler;
#endif
    return T;
}

bool CPolygon::Append(CWord* w)
{
    int width = m_width;

    CPolygon* p = dynamic_cast<CPolygon*>(w);
    if(!p) return(false);

    // TODO
    return(false);

    return(true);
}

bool CPolygon::GetLONG(CStringW& str, LONG& ret)
{
    double dblVal = 0;
    LPWSTR s = (LPWSTR)(LPCWSTR)str, e = s;
    dblVal = wcstod(str, &e);
    ret = std::round(dblVal);
    str = str.Mid(e - s);
    return(e > s);
}
bool CPolygon::Get6BitFixedPoint(CStringW& str, LONG& ret)
{
    LPWSTR s = (LPWSTR)(LPCWSTR)str, e = s;
    ret = wcstod(str, &e) * 64;
    str.Delete(0, e - s);
    return(e > s);
}
bool CPolygon::GetPOINT(CStringW& str, POINT& ret)
{
    return(Get6BitFixedPoint(str, ret.x) && Get6BitFixedPoint(str, ret.y));
}

bool CPolygon::ParseStr()
{
    if(m_pathTypesOrg.GetCount() > 0) return(true);

    CPoint p;
    int i, j, lastsplinestart = -1, firstmoveto = -1, lastmoveto = -1;

    CStringW str = m_str;
    str.SpanIncluding(L"mnlbspc 0123456789");
    str.Replace(L"m", L"*m");
    str.Replace(L"n", L"*n");
    str.Replace(L"l", L"*l");
    str.Replace(L"b", L"*b");
    str.Replace(L"s", L"*s");
    str.Replace(L"p", L"*p");
    str.Replace(L"c", L"*c");

    int k = 0;
    for(CStringW s = str.Tokenize(L"*", k); !s.IsEmpty(); s = str.Tokenize(L"*", k))
    {
        WCHAR c = s[0];
        s.TrimLeft(L"mnlbspc ");
        switch(c)
        {
        case 'm':
            lastmoveto = m_pathTypesOrg.GetCount();
            if(firstmoveto == -1) firstmoveto = lastmoveto;
            while(GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_MOVETO);
                m_pathPointsOrg.Add(p);
            }
            break;
        case 'n':
            while(GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_MOVETONC);
                m_pathPointsOrg.Add(p);
            }
            break;
        case 'l':
            if (m_pathPointsOrg.GetCount() < 1) break;
            while(GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_LINETO);
                m_pathPointsOrg.Add(p);
            }
            break;
        case 'b':
            j = m_pathTypesOrg.GetCount();
            if (j < 1) break;
            while(GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_BEZIERTO);
                m_pathPointsOrg.Add(p);
                j++;
            }
            j = m_pathTypesOrg.GetCount() - ((m_pathTypesOrg.GetCount() - j) % 3);
            m_pathTypesOrg.SetCount(j);
            m_pathPointsOrg.SetCount(j);
            break;
        case 's':
            if (m_pathPointsOrg.GetCount() < 1) break;
            j = lastsplinestart = m_pathTypesOrg.GetCount();
            i = 3;
            while(i-- && GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_BSPLINETO);
                m_pathPointsOrg.Add(p);
                j++;
            }
            if(m_pathTypesOrg.GetCount() - lastsplinestart < 3)
            {
                m_pathTypesOrg.SetCount(lastsplinestart);
                m_pathPointsOrg.SetCount(lastsplinestart);
                lastsplinestart = -1;
            }
            // no break here
        case 'p':
            if (m_pathPointsOrg.GetCount() < 3) break;
            while(GetPOINT(s, p))
            {
                m_pathTypesOrg.Add(PT_BSPLINEPATCHTO);
                m_pathPointsOrg.Add(p);
                j++;
            }
            break;
        case 'c':
            if(lastsplinestart > 0)
            {
                m_pathTypesOrg.Add(PT_BSPLINEPATCHTO);
                m_pathTypesOrg.Add(PT_BSPLINEPATCHTO);
                m_pathTypesOrg.Add(PT_BSPLINEPATCHTO);
                p = m_pathPointsOrg[lastsplinestart-1]; // we need p for temp storage, because operator [] will return a reference to CPoint and Add() may reallocate its internal buffer (this is true for MFC 7.0 but not for 6.0, hehe)
                m_pathPointsOrg.Add(p);
                p = m_pathPointsOrg[lastsplinestart];
                m_pathPointsOrg.Add(p);
                p = m_pathPointsOrg[lastsplinestart+1];
                m_pathPointsOrg.Add(p);
                lastsplinestart = -1;
            }
            break;
        default:
            break;
        }
    }

    if(lastmoveto == -1 || firstmoveto > 0)
    {
        m_pathTypesOrg.RemoveAll();
        m_pathPointsOrg.RemoveAll();
        return(false);
    }

    int minx = INT_MAX, miny = INT_MAX, maxx = -INT_MAX, maxy = -INT_MAX;

    for(i = 0; i < m_pathTypesOrg.GetCount(); i++)
    {
        m_pathPointsOrg[i].x = (int)(m_scalex * m_pathPointsOrg[i].x);
        m_pathPointsOrg[i].y = (int)(m_scaley * m_pathPointsOrg[i].y);
        if(minx > m_pathPointsOrg[i].x) minx = m_pathPointsOrg[i].x;
        if(miny > m_pathPointsOrg[i].y) miny = m_pathPointsOrg[i].y;
        if(maxx < m_pathPointsOrg[i].x) maxx = m_pathPointsOrg[i].x;
        if(maxy < m_pathPointsOrg[i].y) maxy = m_pathPointsOrg[i].y;
    }

    m_width = max(maxx - minx, 0);
    m_ascent = max(maxy - miny, 0);

    int baseline = (int)(64 * m_scaley * m_baseline);
    m_descent = baseline;
    m_ascent -= baseline;

    m_width = ((int)(m_style.fontScaleX / 100 * m_width) + 4) >> 3;
    m_ascent = ((int)(m_style.fontScaleY / 100 * m_ascent) + 4) >> 3;
    m_descent = ((int)(m_style.fontScaleY / 100 * m_descent) + 4) >> 3;

    return(true);
}

bool CPolygon::CreatePath()
{
    int len = m_pathTypesOrg.GetCount();
    if(len == 0) return(false);

    if(mPathPoints != len)
    {
        mpPathTypes = (BYTE*)realloc(mpPathTypes, len * sizeof(BYTE));
        mpPathPoints = (POINT*)realloc(mpPathPoints, len * sizeof(POINT));
        if(!mpPathTypes || !mpPathPoints) return(false);
        mPathPoints = len;
    }

    memcpy(mpPathTypes, m_pathTypesOrg.GetData(), len * sizeof(BYTE));
    memcpy(mpPathPoints, m_pathPointsOrg.GetData(), len * sizeof(POINT));

    return(true);
}

// CClipper
#ifdef _LUA
void CClipper::ParseLuaTable(STSStyle& style, CPoint & pos, CPoint & org)
{
    // Check "style" table:
    if(LuaIsTable(L, L"style"))
    {
        // Push table on top
        lua_getfield(L, -1, "style");
        
        // Blur
        if(LuaIsNumber(L, L"blur")) style.fGaussianBlurX = style.fGaussianBlurY = LuaGetFloat(L, L"blur");
        if(LuaIsNumber(L, L"be")) style.fBlur = LuaGetFloat(L, L"be");

        // Transformation
        if(LuaIsNumber(L, L"frx")) style.fontAngleX = LuaGetFloat(L, L"frx");
        if(LuaIsNumber(L, L"fry")) style.fontAngleY = LuaGetFloat(L, L"fry");
        if(LuaIsNumber(L, L"frz")) style.fontAngleZ = LuaGetFloat(L, L"frz");
        if(LuaIsNumber(L, L"fax")) style.fontShiftX = LuaGetFloat(L, L"fax");
        if(LuaIsNumber(L, L"fay")) style.fontShiftY = LuaGetFloat(L, L"fay");
        if(LuaIsNumber(L, L"fscx")) style.fontScaleX = LuaGetFloat(L, L"fscx");
        if(LuaIsNumber(L, L"fscy")) style.fontScaleY = LuaGetFloat(L, L"fscy");
        if(LuaIsNumber(L, L"fsc")) style.fontScaleX = style.fontScaleY = LuaGetFloat(L, L"fsc");

        // Alphas
        if(LuaIsNumber(L, L"a1")) style.alpha[0] = LuaGetInt(L, L"a1") & 0xFF;
        if(LuaIsNumber(L, L"a2")) style.alpha[1] = LuaGetInt(L, L"a2") & 0xFF;
        if(LuaIsNumber(L, L"a3")) style.alpha[2] = LuaGetInt(L, L"a3") & 0xFF;
        if(LuaIsNumber(L, L"a4")) style.alpha[3] = LuaGetInt(L, L"a4") & 0xFF;

        // Other
        if(LuaIsNumber(L, L"rndx")) style.mod_rand.X = LuaGetFloat(L, L"rndx");
        if(LuaIsNumber(L, L"rndy")) style.mod_rand.Y = LuaGetFloat(L, L"rndy");
        if(LuaIsNumber(L, L"rndz")) style.mod_rand.Z = LuaGetFloat(L, L"rndz");
        if(LuaIsNumber(L, L"rnds")) style.mod_rand.Seed = LuaGetFloat(L, L"rnds");
        if(LuaIsNumber(L, L"rnd"))
            style.mod_rand.X = style.mod_rand.Y = style.mod_rand.Z = LuaGetFloat(L, L"rnd");

        // Pop table
        lua_pop(L, 1);
    }

    //  User data
    // Check "user" table:
    if(LuaIsTable(L, L"user"))
    {
        CStringA index;
        index.Format("sub_%d", m_entry);

        // Push table on top
        lua_getfield(L, -1, "user");
        lua_setglobal(L, index);
    }

    // Check "pos" table:
    if(LuaIsTable(L, L"pos"))
    {
        // Push table on top
        lua_getfield(L, -1, "pos");
        if(LuaIsNumber(L, L"x") && LuaIsNumber(L, L"y"))
        {
            pos.x = LuaGetFloat(L, L"x") * 8;
            pos.y = LuaGetFloat(L, L"y") * 8;
        }
        // Pop table
        lua_pop(L, 1);
    }

    // Check "org" table:
    if(LuaIsTable(L, L"org"))
    {
        // Push table on top
        lua_getfield(L, -1, "org");
        if(LuaIsNumber(L, L"x") && LuaIsNumber(L, L"y"))
        {
            org.x = LuaGetFloat(L, L"x") * 8;
            org.y = LuaGetFloat(L, L"y") * 8;
        }
        // Pop table
        lua_pop(L, 1);
    }

    // Custom modify points before transform
    {
        CString LuaBeforeTransformHandler = CheckLuaHandler(L"beforetransform");
        CString LuaAfterTransformHandler = CheckLuaHandler(L"aftertransform");
        CString LuaCustomTransformHandler = CheckLuaHandler(L"customtransform");

        if(LuaBeforeTransformHandler.GetLength() > 0)
            style.LuaBeforeTransformHandler = LuaBeforeTransformHandler;
        if(LuaAfterTransformHandler.GetLength() > 0)
            style.LuaAfterTransformHandler = LuaAfterTransformHandler;
        if(LuaCustomTransformHandler.GetLength() > 0)
            style.LuaCustomTransformHandler = LuaCustomTransformHandler;
    }
}
#endif

#if defined (_VSMOD) && defined(_LUA)
    CClipper::CClipper(CStringW str, CSize size, double scalex, double scaley, bool inverse, CString LuaStyle, lua_State * L, std::wofstream * LuaLog, int entry)
#else
    CClipper::CClipper(CStringW str, CSize size, double scalex, double scaley, bool inverse)
#endif
    : CPolygon(STSStyle(), str, 0, 0, 0, scalex, scaley, 0)
{
    PROFILE_RENDER_STAGE(Mask);
    m_size.cx = m_size.cy = 0;
    m_pAlphaMask = NULL;

    if (size.cx < 0 || size.cy < 0)
        return;
    const size_t maskBytes = size_t(size.cx) * size.cy;
    bool cacheMask = activeTextGeometry && str.GetLength() <= 1024 &&
        std::isfinite(scalex) && std::isfinite(scaley) && maskBytes <= 16 * 1024 * 1024;
#if defined(_VSMOD) && defined(_LUA)
    cacheMask = cacheMask && LuaStyle.IsEmpty();
#endif
    const TextGeometryCache::ClipKey key(str, size.cx, size.cy, scalex, scaley, inverse);
    if (cacheMask)
    {
        const auto found = activeTextGeometry->clipMasks.find(key);
        if (found != activeTextGeometry->clipMasks.end())
        {
            m_maskStorage = found->second;
            m_pAlphaMask = m_maskStorage.get();
            m_size = size;
            m_inverse = inverse;
#if defined(_VSMOD) && defined(_LUA)
            this->L = L;
            this->LuaLog = LuaLog;
            this->LuaStyle = LuaStyle;
            m_entry = entry;
#endif
            return;
        }
    }
    m_maskStorage = std::shared_ptr<BYTE>(DNew BYTE[maskBytes], std::default_delete<BYTE[]>());
    m_pAlphaMask = m_maskStorage.get();

#if defined (_VSMOD) && defined(_LUA)
    this->L = L;
    this->LuaLog = LuaLog;
    this->LuaStyle = LuaStyle;

    CPoint org(0, 0);
    CPoint pos(0, 0);

    m_entry = entry;
    if(LuaStyle.GetLength() > 0)
    {
        CStringA Func(LuaStyle);
        if(LuaHasFunction(L, LuaStyle))
        {
            // Find function =D
            lua_getglobal(L, Func);

            // Create line table
            lua_newtable(L);
            LuaAddIntegerField(L, "id", m_entry);

            // Saved user data
            {
                CStringA index;
                index.Format("sub_%d", m_entry);
                lua_getglobal(L, index);
                if(lua_istable(L, -1))
                    lua_setfield(L, -2, "user");
                else
                    lua_pop(L, 1);
            }
            if (lua_pcall(L, 1, 1, 0) != 0)
            {
                // error
                CString ErrorText = L"Error: ";
                CString LuaErrorText(lua_tostring(L, -1));

                LuaError(ErrorText + LuaErrorText);
                lua_pop(L, 1);
            }
            else
            {
                // Retrieve result
                if (!lua_istable(L, -1))
                    LuaError(L"Clip function must return a table");
                else
                {
                    ParseLuaTable(m_style, pos, org);
                }
                lua_pop(L, 1);
            }
        }
    }
#endif

    m_size = size;
    m_inverse = inverse;

    memset(m_pAlphaMask, 0, size.cx * size.cy);

#if defined (_VSMOD) && defined(_LUA)
    Paint(pos, org, 6);
#else
    Paint(CPoint(0, 0), CPoint(0, 0));
#endif
    int w = mOverlayWidth, h = mOverlayHeight;

    int x = (mOffsetX + 4) >> 3, y = (mOffsetY + 4) >> 3;
    int xo = 0, yo = 0;

    if(x < 0)
    {
        xo = -x;
        w -= -x;
        x = 0;
    }
    if(y < 0)
    {
        yo = -y;
        h -= -y;
        y = 0;
    }
    if(x + w > m_size.cx) w = m_size.cx - x;
    if(y + h > m_size.cy) h = m_size.cy - y;

    if(w <= 0 || h <= 0) return;

    const BYTE* src = mpOverlayBuffer + 2 * (mOverlayWidth * yo + xo);
    BYTE* dst = m_pAlphaMask + m_size.cx * y + x;

    while(h--)
    {
        for(ptrdiff_t wt = 0; wt < w; ++wt)
            dst[wt] = src[wt*2];

        src += 2 * mOverlayWidth;
        dst += m_size.cx;
    }

    if(inverse)
    {
        BYTE* dst = m_pAlphaMask;
        for(ptrdiff_t i = size.cx * size.cy; i > 0; --i, ++dst)
            *dst = 0x40 - *dst; // mask is 6 bit
    }
    if (cacheMask)
        activeTextGeometry->SaveClip(key, m_maskStorage, maskBytes);
}

CClipper::~CClipper()
{
    m_pAlphaMask = NULL;
}

void CClipper::MakeMaskWritable()
{
    if (m_maskStorage && !m_maskStorage.unique())
    {
        const size_t bytes = size_t(m_size.cx) * m_size.cy;
        auto pixels = std::shared_ptr<BYTE>(DNew BYTE[bytes], std::default_delete<BYTE[]>());
        memcpy(pixels.get(), m_pAlphaMask, bytes);
        m_maskStorage = std::move(pixels);
        m_pAlphaMask = m_maskStorage.get();
    }
}

CWord* CClipper::Copy()
{
#if defined (_VSMOD) && defined(_LUA)
    return(DNew CClipper(m_str, m_size, m_scalex, m_scaley, m_inverse, LuaStyle, L, LuaLog, m_entry));
#else
    return(DNew CClipper(m_str, m_size, m_scalex, m_scaley, m_inverse));
#endif
}

bool CClipper::Append(CWord* w)
{
    return(false);
}

// CLine

CLine::~CLine()
{
    POSITION pos = GetHeadPosition();
    while(pos) delete GetNext(pos);
}

void CLine::Compact()
{
    POSITION pos = GetHeadPosition();
    while(pos)
    {
        CWord* w = GetNext(pos);
        if(!w->m_fWhiteSpaceChar) break;

        m_width -= w->m_width;
        delete w;
        RemoveHead();
    }

    pos = GetTailPosition();
    while(pos)
    {
        CWord* w = GetPrev(pos);
        if(!w->m_fWhiteSpaceChar) break;

        m_width -= w->m_width;
        delete w;
        RemoveTail();
    }

    if(IsEmpty()) return;

    CLine l;
    l.AddTailList(this);
    RemoveAll();

    CWord* last = NULL;

    pos = l.GetHeadPosition();
    while(pos)
    {
        CWord* w = l.GetNext(pos);

        if(!last || !last->Append(w))
            AddTail(last = w->Copy());
    }

    m_ascent = m_descent = m_borderX = m_borderY = 0;

    pos = GetHeadPosition();
    while(pos)
    {
        CWord* w = GetNext(pos);

        if(m_ascent < w->m_ascent) m_ascent = w->m_ascent;
        if(m_descent < w->m_descent) m_descent = w->m_descent;
        if(m_borderX < w->m_style.outlineWidthX) m_borderX = (int)(w->m_style.outlineWidthX + 0.5);
        if(m_borderY < w->m_style.outlineWidthY) m_borderY = (int)(w->m_style.outlineWidthY + 0.5);
    }
}

#ifdef _VSMOD // patch m006. moveable vector clip
CRect CLine::PaintShadow(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha, MOD_MOVEVC& mod_vc, REFERENCE_TIME rt)
#else
CRect CLine::PaintShadow(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha)
#endif
{
    CRect bbox(0, 0, 0, 0);

    POSITION pos = GetHeadPosition();
    while(pos)
    {
        CWord* w = GetNext(pos);

        if(w->m_fLineBreak) return(bbox); // should not happen since this class is just a line of text without any breaks

        if((w->m_style.shadowDepthX != 0 || w->m_style.shadowDepthY != 0) &&
            (w->m_ktype != 3 || time >= w->m_kstart))
        {
#ifdef _VSMOD
            // vpatch v002. Horizontal fontspacing
            int x = p.x - w->m_style.mod_horizontalSpace + (int)(w->m_style.shadowDepthX + 0.5);
            // patch m001. Vertical fontspacing
            int y = p.y - w->m_style.mod_verticalSpace + m_ascent - w->m_ascent + (int)(w->m_style.shadowDepthY + 0.5);
#else
            int x = p.x + (int)(w->m_style.shadowDepthX + 0.5);
            int y = p.y + m_ascent - w->m_ascent + (int)(w->m_style.shadowDepthY + 0.5);
#endif
            DWORD a = 0xff - w->m_style.alpha[3];
            if(alpha > 0) a = MulDiv(a, 0xff - alpha, 0xff);
            COLORREF shadow = revcolor(w->m_style.colors[3]) | (a << 24);
            DWORD sw[6] = {shadow, -1};

#ifdef _VSMOD // patch m011. jitter
            CPoint mod_jitter = w->m_style.mod_jitter.getOffset(rt);
            x += mod_jitter.x;
            y += mod_jitter.y;
            // patch m010. png background
            // subpixel positioning
            w->m_style.mod_grad.subpixx = x & 7;
            w->m_style.mod_grad.subpixy = y & 7;
            w->m_style.mod_grad.fadalpha = alpha;
#endif
#if defined (_VSMOD) && defined(_LUA)
            w->Paint(CPoint(x, y), org, 4);
#else
            w->Paint(CPoint(x, y), org);
#endif
            if(w->m_style.borderStyle == 0 || w->m_style.borderStyle == 3)
            {
#ifdef _VSMOD // patch m004. gradient colors
                if (w->HasNativeOutline() && !(w->m_ktype == 2 && time < w->m_kstart))
                    bbox |= w->DrawNativeShadow(spd, clipRect, pAlphaMask, x, y, sw, mod_vc);
                else
                {
                const auto& paint = w->m_style.mod_grad.paint;
                bbox |= w->Draw(spd, clipRect, pAlphaMask, x, y, sw,
                                w->m_ktype > 0 || w->m_style.alpha[0] < 0xff || (paint && paint->parts[1]),
                                (w->m_style.outlineWidthX + w->m_style.outlineWidthY > 0) && !(w->m_ktype == 2 && time < w->m_kstart), 3, w->m_style.mod_grad, mod_vc, w->m_style.mod_blendMode);
                }
#else
                bbox |= w->Draw(spd, clipRect, pAlphaMask, x, y, sw,
                                w->m_ktype > 0 || w->m_style.alpha[0] < 0xff,
                                (w->m_style.outlineWidthX + w->m_style.outlineWidthY > 0) && !(w->m_ktype == 2 && time < w->m_kstart));
#endif
            }
            else if(w->m_style.borderStyle == 1 && w->m_pOpaqueBox)
            {
#ifdef _VSMOD // patch m004. gradient colors
                bbox |= w->m_pOpaqueBox->Draw(spd, clipRect, pAlphaMask, x, y, sw, true, false, 3,
                    w->m_style.mod_grad, mod_vc, w->m_style.mod_blendMode, false, false);
#else
                bbox |= w->m_pOpaqueBox->Draw(spd, clipRect, pAlphaMask, x, y, sw, true, false);
#endif
            }
        }

        p.x += w->m_width;
    }

    return(bbox);
}

#ifdef _VSMOD // patch m006. moveable vector clip
CRect CLine::PaintOutline(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha, MOD_MOVEVC& mod_vc, REFERENCE_TIME rt)
#else
CRect CLine::PaintOutline(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha)
#endif
{
    CRect bbox(0, 0, 0, 0);

    POSITION pos = GetHeadPosition();
    while(pos)
    {
        CWord* w = GetNext(pos);

        if(w->m_fLineBreak) return(bbox); // should not happen since this class is just a line of text without any breaks

        if((w->m_style.outlineWidthX + w->m_style.outlineWidthY > 0
#ifdef _VSMOD
            || w->HasNativeOutline()
#endif
            ) && !((w->m_ktype == 2 || w->m_ktype == 3) && time < w->m_kstart))
        {
#ifdef _VSMOD // vpatch v002. Horizontal fontspacing
            int x = p.x - w->m_style.mod_horizontalSpace;
            // patch m001. Vertical fontspacing
            int y = p.y - w->m_style.mod_verticalSpace + m_ascent - w->m_ascent;
#else
            int x = p.x;
            int y = p.y + m_ascent - w->m_ascent;
#endif
            DWORD aoutline = w->m_style.alpha[2];
            if(alpha > 0) aoutline += MulDiv(alpha, 0xff - w->m_style.alpha[2], 0xff);
            COLORREF outline = revcolor(w->m_style.colors[2]) | ((0xff - aoutline) << 24);
            DWORD sw[6] = {outline, -1};

#ifdef _VSMOD // patch m011. jitter
            CPoint mod_jitter = w->m_style.mod_jitter.getOffset(rt);
            x += mod_jitter.x;
            y += mod_jitter.y;
            // patch m010. png background
            // subpixel positioning
            w->m_style.mod_grad.subpixx = x & 7;
            w->m_style.mod_grad.subpixy = y & 7;
            w->m_style.mod_grad.fadalpha = alpha;
#endif

#if defined (_VSMOD) && defined(_LUA)
            w->Paint(CPoint(x, y), org, 3);
#else
            w->Paint(CPoint(x, y), org);
#endif

            if(w->m_style.borderStyle != 1)
            {
#ifdef _VSMOD // patch m004. gradient colors
                if (w->HasNativeOutline())
                    bbox |= w->DrawNativeOutline(spd, clipRect, pAlphaMask, x, y, time, alpha, mod_vc);
                else
                {
                const auto& paint = w->m_style.mod_grad.paint;
                const bool opaqueFill = !w->m_style.alpha[0] && !w->m_style.alpha[1] && !alpha &&
                    !w->m_style.mod_grad.bodyIsGradAlpha && !(paint && (paint->parts[1] || paint->parts[3]));
                bbox |= w->Draw(spd, clipRect, pAlphaMask, x, y, sw, opaqueFill, true, 2, w->m_style.mod_grad, mod_vc, w->m_style.mod_blendMode);
                }
#else
                bbox |= w->Draw(spd, clipRect, pAlphaMask, x, y, sw, !w->m_style.alpha[0] && !w->m_style.alpha[1] && !alpha, true);
#endif
            }
            else if(w->m_style.borderStyle == 1 && w->m_pOpaqueBox)
            {
#ifdef _VSMOD // patch m004. gradient colors
                bbox |= w->m_pOpaqueBox->Draw(spd, clipRect, pAlphaMask, x, y, sw, true, false, 2,
                    w->m_style.mod_grad, mod_vc, w->m_style.mod_blendMode, false, false);
#else
                bbox |= w->m_pOpaqueBox->Draw(spd, clipRect, pAlphaMask, x, y, sw, true, false);
#endif
            }
        }

        p.x += w->m_width;
    }

    return(bbox);
}

#ifdef _VSMOD // patch m006. moveable vector clip
CRect CLine::PaintBody(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha, MOD_MOVEVC& mod_vc, REFERENCE_TIME rt)
#else
CRect CLine::PaintBody(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha)
#endif
{
    CRect bbox(0, 0, 0, 0);

    POSITION pos = GetHeadPosition();
    while(pos)
    {
        CWord* w = GetNext(pos);

        if(w->m_fLineBreak) return(bbox); // should not happen since this class is just a line of text without any breaks
#ifdef _VSMOD
        // vpatch v002. Horizontal fontspacing
        int x = p.x - w->m_style.mod_horizontalSpace;
        // patch m001. Vertical fontspacing
        int y = p.y - w->m_style.mod_verticalSpace + m_ascent - w->m_ascent;
#else
        int x = p.x;
        int y = p.y + m_ascent - w->m_ascent;
#endif
#ifdef _VSMOD
        if (w->m_ktype == 3 && time < w->m_kstart)
        {
            p.x += w->m_width;
            continue;
        }
#endif
        // colors

        DWORD aprimary = w->m_style.alpha[0];
        if(alpha > 0) aprimary += MulDiv(alpha, 0xff - w->m_style.alpha[0], 0xff);
        COLORREF primary = revcolor(w->m_style.colors[0]) | ((0xff - aprimary) << 24);

        DWORD asecondary = w->m_style.alpha[1];
        if(alpha > 0) asecondary += MulDiv(alpha, 0xff - w->m_style.alpha[1], 0xff);
        COLORREF secondary = revcolor(w->m_style.colors[1]) | ((0xff - asecondary) << 24);

        DWORD sw[6] = {primary, 0, secondary};
        bool reversePaint = false;

        // karaoke

        double t;

        if(w->m_ktype == 0 || w->m_ktype == 2 || w->m_ktype == 3)
        {
            t = time < w->m_kstart ? 0 : 1;
        }
        else if(w->m_ktype == 1)
        {
            if(time < w->m_kstart) t = 0;
            else if(time < w->m_kend)
            {
                t = 1.0 * (time - w->m_kstart) / (w->m_kend - w->m_kstart);

                double angle = fmod(w->m_style.fontAngleZ, 360.0);
                if(angle > 90 && angle < 270)
                {
                    reversePaint = true;
                    t = 1 - t;
                    COLORREF tmp = sw[0];
                    sw[0] = sw[2];
                    sw[2] = tmp;
                }
            }
            else t = 1.0;
        }

        if(t >= 1)
        {
            sw[1] = 0xFFFFFFF;
        }

        // move dividerpoint
        int bluradjust = 0;
        double fGaussianBlur = max(w->m_style.fGaussianBlurX, w->m_style.fGaussianBlurY);
        if(fGaussianBlur > 0)
            bluradjust += (int)(fGaussianBlur * 3 * 8 + 0.5) | 1;
        if(w->m_style.fBlur)
            bluradjust += 8;
        double tx = w->m_style.fontAngleZ;
        sw[4] = sw[2];
        sw[5] = 0x00ffffff;

#ifdef _VSMOD // patch m011. jitter
        CPoint mod_jitter = w->m_style.mod_jitter.getOffset(rt);
        x += mod_jitter.x;
        y += mod_jitter.y;
        // patch m010. png background
        // subpixel positioning
        w->m_style.mod_grad.subpixx = x & 7;
        w->m_style.mod_grad.subpixy = y & 7;
        w->m_style.mod_grad.fadalpha = alpha;
#endif

#if defined (_VSMOD) && defined(_LUA)
            w->Paint(CPoint(x, y), org, 1);
#else
            w->Paint(CPoint(x, y), org);
#endif

        sw[3] = (int)(w->m_style.outlineWidthX + t * w->getOverlayWidth() + t * bluradjust) >> 3;

#ifdef _VSMOD // patch m004. gradient colors
        bbox |= w->DrawBodyPaint(spd, clipRect, pAlphaMask, x, y, sw, mod_vc, reversePaint);
#else
        bbox |= w->Draw(spd, clipRect, pAlphaMask, x, y, sw, true, false);
#endif
        p.x += w->m_width;
    }

    return(bbox);
}


// CSubtitle

CSubtitle::CSubtitle()
{
    memset(m_effects, 0, sizeof(Effect*)*EF_NUMBEROFEFFECTS);
    m_pClipper = NULL;
    m_clipInverse = false;
    m_scalex = m_scaley = 1;
}

CSubtitle::~CSubtitle()
{
    Empty();
}

void CSubtitle::Empty()
{
    POSITION pos = GetHeadPosition();
    while(pos) delete GetNext(pos);

    pos = m_words.GetHeadPosition();
    while(pos) delete m_words.GetNext(pos);

    for(ptrdiff_t i = 0; i < EF_NUMBEROFEFFECTS; i++)
    {
        if(m_effects[i]) delete m_effects[i];
    }
    memset(m_effects, 0, sizeof(Effect*)*EF_NUMBEROFEFFECTS);

    if(m_pClipper) delete m_pClipper;
    m_pClipper = NULL;
}

int CSubtitle::GetFullWidth()
{
    int width = 0;

    POSITION pos = m_words.GetHeadPosition();
    while(pos) width += m_words.GetNext(pos)->m_width;

    return(width);
}

int CSubtitle::GetFullLineWidth(POSITION pos)
{
    int width = 0;

    while(pos)
    {
        CWord* w = m_words.GetNext(pos);
        if(w->m_fLineBreak) break;
        width += w->m_width;
    }

    return(width);
}

int CSubtitle::GetWrapWidth(POSITION pos, int maxwidth)
{
    if(m_wrapStyle == 0 || m_wrapStyle == 3)
    {
        if(maxwidth > 0)
        {
//			int fullwidth = GetFullWidth();
            int fullwidth = GetFullLineWidth(pos);

            int minwidth = fullwidth / ((abs(fullwidth) / maxwidth) + 1);

            int width = 0, wordwidth = 0;

            while(pos && width < minwidth)
            {
                CWord* w = m_words.GetNext(pos);
                wordwidth = w->m_width;
                if(abs(width + wordwidth) < abs(maxwidth)) width += wordwidth;
            }

            if(m_wrapStyle == 3 && width < fullwidth && fullwidth - width + wordwidth < maxwidth) width -= wordwidth;
            maxwidth = width;
        }
    }
    else if(m_wrapStyle == 1)
    {
//		maxwidth = maxwidth;
    }
    else if(m_wrapStyle == 2)
    {
        maxwidth = INT_MAX;
    }

    return(maxwidth);
}

CLine* CSubtitle::GetNextLine(POSITION& pos, int maxwidth)
{
    if(pos == NULL) return(NULL);

    CLine* ret = DNew CLine();
    if(!ret) return(NULL);
#ifdef _VSMOD
    if (!m_lineAlignment.empty())
    {
        const auto number = m_words.GetAt(pos)->m_logicalLine;
        auto at = m_lineAlignment.upper_bound(number);
        if (at != m_lineAlignment.begin())
        {
            --at;
            ret->m_horizontalAnchor = at->first == number ? at->second.first : at->second.inherited;
        }
    }
#endif

#if defined(_VSMOD) && defined(_LUA)

#endif

    ret->m_width = ret->m_ascent = ret->m_descent = ret->m_borderX = ret->m_borderY = 0;

    maxwidth = GetWrapWidth(pos, maxwidth);

    bool fEmptyLine = true;

    while(pos)
    {
        CWord* w = m_words.GetNext(pos);

        if(ret->m_ascent < w->m_ascent) ret->m_ascent = w->m_ascent;
        if(ret->m_descent < w->m_descent) ret->m_descent = w->m_descent;
        if(ret->m_borderX < w->m_style.outlineWidthX) ret->m_borderX = (int)(w->m_style.outlineWidthX + 0.5);
        if(ret->m_borderY < w->m_style.outlineWidthY) ret->m_borderY = (int)(w->m_style.outlineWidthY + 0.5);

        if(w->m_fLineBreak)
        {
            if(fEmptyLine)
            {
                ret->m_ascent /= 2;
                ret->m_descent /= 2;
                ret->m_borderX = ret->m_borderY = 0;
            }

            ret->Compact();

            return(ret);
        }

        fEmptyLine = false;

        bool fWSC = w->m_fWhiteSpaceChar;

        int width = w->m_width;
        POSITION pos2 = pos;
        while(pos2)
        {
            if(m_words.GetAt(pos2)->m_fWhiteSpaceChar != fWSC
               || m_words.GetAt(pos2)->m_fLineBreak) break;

            CWord* w2 = m_words.GetNext(pos2);
            width += w2->m_width;
        }

        if((ret->m_width += width) <= maxwidth || ret->IsEmpty())
        {
            ret->AddTail(w->Copy());

            while(pos != pos2)
            {
                ret->AddTail(m_words.GetNext(pos)->Copy());
            }

            pos = pos2;
        }
        else
        {
            if(pos) m_words.GetPrev(pos);
            else pos = m_words.GetTailPosition();

            ret->m_width -= width;

            break;
        }
    }

    ret->Compact();

    return(ret);
}

void CSubtitle::CreateClippers(CSize size)
{
    PROFILE_RENDER_STAGE(Mask);
    size.cx >>= 3;
    size.cy >>= 3;

    if(m_effects[EF_BANNER] && m_effects[EF_BANNER]->param[2])
    {
        int width = m_effects[EF_BANNER]->param[2];

        int w = size.cx, h = size.cy;

        if(!m_pClipper)
        {
            CStringW str;
            str.Format(L"m %d %d l %d %d %d %d %d %d", 0, 0, w, 0, w, h, 0, h);
#if defined (_VSMOD) && defined(_LUA)
            m_pClipper = DNew CClipper(str, size, 1, 1, false, L"", L, LuaLog, 0);
#else
            m_pClipper = DNew CClipper(str, size, 1, 1, false);
#endif
            if(!m_pClipper) return;
        }

        m_pClipper->MakeMaskWritable();
        int da = (64 << 8) / width;
        BYTE* am = m_pClipper->m_pAlphaMask;

        for(ptrdiff_t j = 0; j < h; j++, am += w)
        {
            int a = 0;
            int k = min(width, w);

            for(ptrdiff_t i = 0; i < k; i++, a += da)
                am[i] = (am[i] * a) >> 14;

            a = 0x40 << 8;
            k = w - width;

            if(k < 0)
            {
                a -= -k * da;
                k = 0;
            }

            for(ptrdiff_t i = k; i < w; i++, a -= da)
                am[i] = (am[i] * a) >> 14;
        }
    }
    else if(m_effects[EF_SCROLL] && m_effects[EF_SCROLL]->param[4])
    {
        int height = m_effects[EF_SCROLL]->param[4];

        int w = size.cx, h = size.cy;

        if(!m_pClipper)
        {
            CStringW str;
            str.Format(L"m %d %d l %d %d %d %d %d %d", 0, 0, w, 0, w, h, 0, h);
#if defined (_VSMOD) && defined(_LUA)
            m_pClipper = DNew CClipper(str, size, 1, 1, false, L"", L, LuaLog, 0);
#else
            m_pClipper = DNew CClipper(str, size, 1, 1, false);
#endif
            if(!m_pClipper) return;
        }

        m_pClipper->MakeMaskWritable();
        int da = (64 << 8) / height;
        int a = 0;
        int k = m_effects[EF_SCROLL]->param[0] >> 3;
        int l = k + height;
        if(k < 0)
        {
            a += -k * da;
            k = 0;
        }
        if(l > h)
        {
            l = h;
        }

        if(k < h)
        {
            BYTE* am = &m_pClipper->m_pAlphaMask[k*w];

            memset(m_pClipper->m_pAlphaMask, 0, am - m_pClipper->m_pAlphaMask);

            for(ptrdiff_t j = k; j < l; j++, a += da)
            {
                for(ptrdiff_t i = 0; i < w; i++, am++)
                    *am = ((*am) * a) >> 14;
            }
        }

        da = -(64 << 8) / height;
        a = 0x40 << 8;
        l = m_effects[EF_SCROLL]->param[1] >> 3;
        k = l - height;
        if(k < 0)
        {
            a += -k * da;
            k = 0;
        }
        if(l > h)
        {
            l = h;
        }

        if(k < h)
        {
            BYTE* am = &m_pClipper->m_pAlphaMask[k*w];

            int j = k;
            for(; j < l; j++, a += da)
            {
                for(ptrdiff_t i = 0; i < w; i++, am++)
                    *am = ((*am) * a) >> 14;
            }

            memset(am, 0, (h - j)*w);
        }
    }
}

void CSubtitle::MakeLines(CSize size, CRect marginRect)
{
    PROFILE_RENDER_STAGE(Layout);
    CSize spaceNeeded(0, 0);

    bool fFirstLine = true;

    m_topborder = m_bottomborder = 0;

    CLine* l = NULL;

    POSITION pos = m_words.GetHeadPosition();
    while(pos)
    {
        l = GetNextLine(pos, size.cx - marginRect.left - marginRect.right);
        if(!l) break;

        if(fFirstLine)
        {
            m_topborder = l->m_borderY;
            fFirstLine = false;
        }

        spaceNeeded.cx = max(l->m_width + l->m_borderX, spaceNeeded.cx);
        spaceNeeded.cy += l->m_ascent + l->m_descent;
#ifdef _VSMOD
        m_textWidth = (std::max)(m_textWidth, l->m_width);
#endif

        AddTail(l);
    }

    if(l) m_bottomborder = l->m_borderY;

    m_rect = CRect(
                 CPoint((m_scrAlignment % 3) == 1 ? marginRect.left
                        : (m_scrAlignment % 3) == 2 ? (marginRect.left + (size.cx - marginRect.right) - spaceNeeded.cx + 1) / 2
                        : (size.cx - marginRect.right - spaceNeeded.cx),
                        m_scrAlignment <= 3 ? (size.cy - marginRect.bottom - spaceNeeded.cy)
                        : m_scrAlignment <= 6 ? (marginRect.top + (size.cy - marginRect.bottom) - spaceNeeded.cy + 1) / 2
                        : marginRect.top),
                 spaceNeeded);
}

void CSubtitle::AppendWord(CWord* word)
{
#ifdef _VSMOD
    word->m_logicalLine = m_logicalLine;
    if (word->m_fLineBreak)
        ++m_logicalLine;
#endif
    m_words.AddTail(word);
}

int CSubtitle::LineLeft(const CLine& line, int anchor) const
{
    const auto offset = [](int width, int alignment) {
        return alignment == 1 ? 0 : alignment == 2 ? width / 2 : width;
    };
    const int normal = m_scrAlignment % 3;
#ifdef _VSMOD
    const int text = m_textAnchor < 0 ? normal : m_textAnchor;
    if (line.m_horizontalAnchor >= 0)
        return anchor - offset(m_textWidth, text) + offset(m_textWidth - line.m_width, line.m_horizontalAnchor);
    return anchor - offset(line.m_width, text);
#else
    return anchor - offset(line.m_width, normal);
#endif
}

#ifdef _VSMOD
namespace
{
class EventBoxPath : public CWord
{
    std::vector<POINT> points;
    std::vector<BYTE> types;

    bool CreatePath() override
    {
        return CopyPath(points.data(), types.data(), points.size());
    }

public:
    EventBoxPath(STSStyle& style, double sx, double sy, KfxPaint::Rect plane) : CWord(style, L".", 0, 0, 0, sx, sy)
    {
        isOpaqueBox = true;
        m_style.outlineWidthX = m_style.outlineWidthY = 0;
        m_style.shadowDepthX = m_style.shadowDepthY = 0;
        m_style.borderStyle = 0;
        m_style.nativeOutline.reset();
        m_style.mod_rand.clear();
        m_style.fBlur = 0;
        m_style.fGaussianBlurX = m_style.fGaussianBlurY = 0;
        if (m_style.mod_distort.enabled)
            m_distortionDomain = std::make_unique<CRect>(LONG(std::lround(plane.left * 8)),
                LONG(std::lround(plane.top * 8)), LONG(std::lround(plane.right * 8)), LONG(std::lround(plane.bottom * 8)));
#ifdef _LUA
        m_style.LuaBeforeTransformHandler.Empty();
        m_style.LuaCustomTransformHandler.Empty();
        m_style.LuaAfterTransformHandler.Empty();
#endif
    }

    CWord* Copy() override { return new EventBoxPath(*this); }

    void AddContour(KfxPaint::Rect rect, double rx, double ry, bool hole)
    {
        const auto start = points.size();
        const auto push = [&](double x, double y) {
            points.push_back({LONG(std::lround(x * 8)), LONG(std::lround(y * 8))});
            types.push_back(PT_LINETO);
        };
        rx = (std::min)(rx, (rect.right - rect.left) / 2);
        ry = (std::min)(ry, (rect.bottom - rect.top) / 2);
        if (rx <= 0 || ry <= 0)
        {
            push(rect.left, rect.top);
            push(rect.right, rect.top);
            push(rect.right, rect.bottom);
            push(rect.left, rect.bottom);
        }
        else
        {
            const double cx[] = {rect.right-rx, rect.right-rx, rect.left+rx, rect.left+rx};
            const double cy[] = {rect.top+ry, rect.bottom-ry, rect.bottom-ry, rect.top+ry};
            const int segments = (std::min)(32, 8 + int(std::sqrt((std::max)(rx, ry) / 8)));
            constexpr double quarter = 1.57079632679489661923;
            for (int corner = 0; corner < 4; ++corner)
                for (int step = 0; step <= segments; ++step)
                {
                    const double angle = (corner - 1 + double(step) / segments) * quarter;
                    push(cx[corner] + rx * std::cos(angle), cy[corner] + ry * std::sin(angle));
                }
        }
        if (hole)
            std::reverse(points.begin() + start, points.end());
        types[start] = PT_MOVETO;
    }
};
}

void CSubtitle::CreateEventBox(int borderStyle, int time, bool scaledBorder)
{
    m_eventBox.clear();
    if ((m_boxMode < 0 ? borderStyle != 2 : m_boxMode == 0) || IsEmpty())
        return;

    CWord* canonical = nullptr;
    double left = 0, right = 0;
    POSITION lines = GetHeadPosition();
    while (lines)
    {
        auto* line = GetNext(lines);
        const double x = LineLeft(*line, 0);
        left = (std::min)(left, x);
        right = (std::max)(right, x + line->m_width);
        if (canonical) continue;
        POSITION words = line->GetHeadPosition();
        while (words && !canonical)
        {
            auto* word = line->GetNext(words);
            if (!word->m_fWhiteSpaceChar && !word->m_fLineBreak && (word->m_ktype != 3 || time >= word->m_kstart))
                canonical = word;
        }
    }
    if (!canonical || right <= left || m_rect.Height() <= 0)
        return;

    auto& style = canonical->m_style;
    const double soft = style.softScale > 0 ? style.softScale : 100;
    const double sx = std::abs(style.fontScaleX) / soft * (scaledBorder ? m_scalex : 1) * 8;
    const double sy = std::abs(style.fontScaleY) / soft * (scaledBorder ? m_scaley : 1) * 8;
    const KfxPaint::BoxShape defaults;
    const auto& box = style.boxShape ? *style.boxShape : defaults;
    const auto bounded = [](double pixels) { return (std::min)(pixels, 8192.0); };
    const double edgeX = (std::max)(0., style.shadowDepthX) + style.outlineWidthX;
    const double edgeTop = (std::max)(0., style.shadowDepthY) + m_topborder;
    const double edgeBottom = (std::max)(0., style.shadowDepthY) + m_bottomborder;
    const KfxPaint::Rect rect{left - edgeX - bounded(box.padding[0] * sx), -edgeTop - bounded(box.padding[2] * sy),
        right + edgeX + bounded(box.padding[1] * sx), m_rect.Height() + edgeBottom + bounded(box.padding[3] * sy)};
    const double radiusX = (std::min)(bounded(box.radius * sx), (rect.right - rect.left) / 2);
    const double radiusY = (std::min)(bounded(box.radius * sy), (rect.bottom - rect.top) / 2);
    const auto expand = [&](double x, double y) {
        return KfxPaint::Rect{rect.left-x, rect.top-y, rect.right+x, rect.bottom+y};
    };
    auto fill = std::make_unique<EventBoxPath>(style, m_scalex, m_scaley, rect);
    fill->AddContour(rect, radiusX, radiusY, false);
    m_eventBox.push_back(std::move(fill));
    double x = 0, y = 0;
    if (style.boxRings)
        for (const auto& layer : style.boxRings->layers)
        {
            if (layer.x <= 0) continue;
            const double outerX = bounded(x + layer.x * sx);
            const double outerY = bounded(y + layer.x * sy);
            if (outerX == x && outerY == y) continue;
            auto ring = std::make_unique<EventBoxPath>(style, m_scalex, m_scaley, rect);
            ring->AddContour(expand(outerX, outerY), radiusX + outerX, radiusY + outerY, false);
            ring->AddContour(expand(x, y), radiusX + x, radiusY + y, true);
            auto& paint = ring->m_style;
            paint.mod_grad.clear();
            if (layer.color >= 0) paint.colors[3] = revcolor(layer.color);
            if (layer.inverseAlpha >= 0) paint.alpha[3] = BYTE(layer.inverseAlpha);
            if (layer.paint[0] || layer.paint[1])
            {
                auto sources = std::make_shared<KfxPaint::Sources>();
                sources->parts[6] = layer.paint[0];
                sources->parts[7] = layer.paint[1];
                paint.mod_grad.paint = std::move(sources);
            }
            m_eventBox.push_back(std::move(ring));
            x = outerX;
            y = outerY;
        }
}
#endif

// CScreenLayoutAllocator

void CScreenLayoutAllocator::Empty()
{
    m_subrects.RemoveAll();
}

void CScreenLayoutAllocator::AdvanceToSegment(int segment, const CAtlArray<int>& sa)
{
    PROFILE_RENDER_STAGE(Collision);
    POSITION pos = m_subrects.GetHeadPosition();
    while(pos)
    {
        POSITION prev = pos;

        SubRect& sr = m_subrects.GetNext(pos);

        bool fFound = false;

        if(abs(sr.segment - segment) <= 1) // using abs() makes it possible to play the subs backwards, too :)
        {
            for(ptrdiff_t i = 0; i < sa.GetCount() && !fFound; i++)
            {
                if(sa[i] == sr.entry)
                {
                    sr.segment = segment;
                    fFound = true;
                }
            }
        }

        if(!fFound) m_subrects.RemoveAt(prev);
    }
}

CRect CScreenLayoutAllocator::AllocRect(CSubtitle* s, int segment, int entry, int layer, int collisions)
{
    PROFILE_RENDER_STAGE(Collision);
    // TODO: handle collisions == 1 (reversed collisions)

    POSITION pos = m_subrects.GetHeadPosition();
    while(pos)
    {
        SubRect& sr = m_subrects.GetNext(pos);
        if(sr.segment == segment && sr.entry == entry)
        {
            return(sr.r + CRect(0, -s->m_topborder, 0, -s->m_bottomborder));
        }
    }

    CRect r = s->m_rect + CRect(0, s->m_topborder, 0, s->m_bottomborder);

    bool fSearchDown = s->m_scrAlignment > 3;

    bool fOK;

    do
    {
        fOK = true;

        pos = m_subrects.GetHeadPosition();
        while(pos)
        {
            SubRect& sr = m_subrects.GetNext(pos);

            if(layer == sr.layer && !(r & sr.r).IsRectEmpty())
            {
                if(fSearchDown)
                {
                    r.bottom = sr.r.bottom + r.Height();
                    r.top = sr.r.bottom;
                }
                else
                {
                    r.top = sr.r.top - r.Height();
                    r.bottom = sr.r.top;
                }

                fOK = false;
            }
        }
    }
    while(!fOK);

    SubRect sr;
    sr.r = r;
    sr.segment = segment;
    sr.entry = entry;
    sr.layer = layer;
    m_subrects.AddTail(sr);

    return(sr.r + CRect(0, -s->m_topborder, 0, -s->m_bottomborder));
}

// CRenderedTextSubtitle

struct OverrideTag {
    CStringW rawCommand;
    CStringW command;
    std::vector<CStringW> parameters;
    int rawCount;
    bool parenthesized = false;
};

using OverrideTags = std::vector<OverrideTag>;

struct OverrideTagCache {
    std::map<CStringW, std::shared_ptr<const OverrideTags>> blocks;
    size_t characters = 0;

    std::shared_ptr<const OverrideTags> Get(const CStringW& text);
#ifdef _VSMOD
    std::array<std::map<CStringW, AssNumeric::Program>, 2> programs;
    std::map<int, AssNumeric::Context> layout;

    const AssNumeric::Program& Expression(const CStringW& source, bool timing)
    {
        auto& cache = programs[timing];
        const auto existing = cache.find(source);
        if (existing != cache.end()) return existing->second;
        if (cache.size() >= 256) cache.clear();
        return cache.emplace(source, AssNumeric::Program::Compile(source, timing)).first->second;
    }
#endif
#if defined(_VSMOD) && defined(_LUA)
    bool defaultFunctionsKnown = false;
    std::set<CStringW> defaultFunctions;

    bool IsLuaFunction(CMyLua& owner, const CStringW& command)
    {
        if (owner.hasExternalLua)
            return owner.LuaHasFunction(owner.L, command);
        if (!defaultFunctionsKnown)
        {
            // Before any user code executes, the global function table comes
            // exclusively from luaL_openlibs. Snapshot its actual names once.
            const int top = lua_gettop(owner.L);
            lua_pushnil(owner.L);
            while (lua_next(owner.L, LUA_GLOBALSINDEX))
            {
                if (lua_type(owner.L, -2) == LUA_TSTRING && lua_isfunction(owner.L, -1))
                    defaultFunctions.insert(CStringW(CStringA(lua_tostring(owner.L, -2))));
                lua_pop(owner.L, 1);
            }
            lua_settop(owner.L, top);
            defaultFunctionsKnown = true;
        }
        return defaultFunctions.find(command) != defaultFunctions.end();
    }
#endif
};


CRenderedTextSubtitle::CRenderedTextSubtitle(CCritSec* pLock, STSStyle *styleOverride, bool doOverride)
    : ISubPicProviderImpl(pLock), m_doOverrideStyle(doOverride), m_pStyleOverride(styleOverride)
{
    m_size = CSize(0, 0);

}

CRenderedTextSubtitle::~CRenderedTextSubtitle()
{
    m_eventWorkers.reset();
    Deinit();

}

void CRenderedTextSubtitle::Copy(CSimpleTextSubtitle& sts)
{
    __super::Copy(sts);

    m_size = CSize(0, 0);

    if(CRenderedTextSubtitle* pRTS = dynamic_cast<CRenderedTextSubtitle*>(&sts))
    {
        m_size = pRTS->m_size;
    }
}

void CRenderedTextSubtitle::Empty()
{
    Deinit();

    __super::Empty();
}

void CRenderedTextSubtitle::OnChanged()
{
    __super::OnChanged();
    if (m_textGeometry)
        m_textGeometry->Clear();
#ifdef _VSMOD
    if (m_overrideTags)
        m_overrideTags->layout.clear();
#endif

    POSITION pos = m_subtitleCache.GetStartPosition();
    while(pos)
    {
        int i;
        CSubtitle* s;
        m_subtitleCache.GetNextAssoc(pos, i, s);
        delete s;
    }

    m_subtitleCache.RemoveAll();

    m_sla.Empty();
}

bool CRenderedTextSubtitle::Init(CSize size, CRect vidrect)
{
    Deinit();

    m_size = CSize(size.cx * 8, size.cy * 8);
    m_vidrect = CRect(vidrect.left * 8, vidrect.top * 8, vidrect.right * 8, vidrect.bottom * 8);

    m_sla.Empty();

    return(true);
}

void CRenderedTextSubtitle::Deinit()
{
    m_overrideTags.reset();
    m_textGeometry.reset();
    m_textDC.reset();
    POSITION pos = m_subtitleCache.GetStartPosition();
    while(pos)
    {
        int i;
        CSubtitle* s;
        m_subtitleCache.GetNextAssoc(pos, i, s);
        delete s;
    }

    m_subtitleCache.RemoveAll();

    m_sla.Empty();

    m_size = CSize(0, 0);
    m_vidrect.SetRectEmpty();
}

void CRenderedTextSubtitle::ParseEffect(CSubtitle* sub, CString str)
{
    str.Trim();
    if(!sub || str.IsEmpty()) return;

    const TCHAR* s = _tcschr(str, ';');
    if(!s)
    {
        s = (LPTSTR)(LPCTSTR)str;
        s += str.GetLength() - 1;
    }
    s++;
    CString effect = str.Left(s - str);

    if(!effect.CompareNoCase(_T("Banner;")))
    {
        int delay, lefttoright = 0, fadeawaywidth = 0;
        if(_stscanf(s, _T("%d;%d;%d"), &delay, &lefttoright, &fadeawaywidth) < 1) return;

        Effect* e = DNew Effect;
        if(!e) return;

        sub->m_effects[e->type = EF_BANNER] = e;
        e->param[0] = (int)(max(1.0 * delay / sub->m_scalex, 1));
        e->param[1] = lefttoright;
        e->param[2] = (int)(sub->m_scalex * fadeawaywidth);

        sub->m_wrapStyle = 2;
    }
    else if(!effect.CompareNoCase(_T("Scroll up;")) || !effect.CompareNoCase(_T("Scroll down;")))
    {
        int top, bottom, delay, fadeawayheight = 0;
        if(_stscanf(s, _T("%d;%d;%d;%d"), &top, &bottom, &delay, &fadeawayheight) < 3) return;

        if(top > bottom)
        {
            int tmp = top;
            top = bottom;
            bottom = tmp;
        }

        Effect* e = DNew Effect;
        if(!e) return;

        sub->m_effects[e->type = EF_SCROLL] = e;
        e->param[0] = (int)(sub->m_scaley * top * 8);
        e->param[1] = (int)(sub->m_scaley * bottom * 8);
        e->param[2] = (int)(max(1.0 * delay / sub->m_scaley, 1));
        e->param[3] = (effect.GetLength() == 12);
        e->param[4] = (int)(sub->m_scaley * fadeawayheight);
    }
}

void CRenderedTextSubtitle::ParseString(CSubtitle* sub, CStringW str, STSStyle& style)
{
    PROFILE_RENDER_STAGE(Layout);
    if(!sub) return;

    str.Replace(L"\\N", L"\n");
    str.Replace(L"\\n", (sub->m_wrapStyle < 2 || sub->m_wrapStyle == 3) ? L" " : L"\n");
    str.Replace(L"\\h", L"\x00A0");

    for(size_t i = 0, j = 0, len = str.GetLength(); j <= len; j++)
    {
        WCHAR c = str[j];

        if(c != '\n' && c != ' ' && c != '\x00A0' && c != 0)
            continue;

        if(i < j)
        {
            if(CWord* w = DNew CText(style, str.Mid(i, j - i), m_ktype, m_kstart, m_kend, sub->m_scalex, sub->m_scaley))
            {
#if defined (_VSMOD) && defined(_LUA)
                w->L = L;
                w->LuaLog = LuaLog;
                w->m_entry = m_entry;
                w->LuaRendererHandler = style.LuaRendererHandler;
#endif
                sub->AppendWord(w);
                m_kstart = m_kend;
            }
        }

        if(c == '\n')
        {
            if(CWord* w = DNew CText(style, CStringW(), m_ktype, m_kstart, m_kend, sub->m_scalex, sub->m_scaley))
            {
#if defined (_VSMOD) && defined(_LUA)
                w->L = L;
                w->LuaLog = LuaLog;
                w->m_entry = m_entry;
                w->LuaRendererHandler = style.LuaRendererHandler;
#endif
                sub->AppendWord(w);
                m_kstart = m_kend;
            }
        }
        else if(c == ' ' || c == '\x00A0')
        {
            if(CWord* w = DNew CText(style, CStringW(c), m_ktype, m_kstart, m_kend, sub->m_scalex, sub->m_scaley))
            {
#if defined (_VSMOD) && defined(_LUA)
                w->L = L;
                w->LuaLog = LuaLog;
                w->m_entry = m_entry;
                w->LuaRendererHandler = style.LuaRendererHandler;
#endif
                sub->AppendWord(w);
                m_kstart = m_kend;
            }
        }

        i = j + 1;
    }

    return;
}

void CRenderedTextSubtitle::ParsePolygon(CSubtitle* sub, CStringW str, STSStyle& style)
{
    PROFILE_RENDER_STAGE(Layout);
    if(!sub || !str.GetLength() || !m_nPolygon) return;

    if(CWord* w = DNew CPolygon(style, str, m_ktype, m_kstart, m_kend, sub->m_scalex / (1 << (m_nPolygon - 1)), sub->m_scaley / (1 << (m_nPolygon - 1)), m_polygonBaselineOffset))
    {
#if defined (_VSMOD) && defined(_LUA)
        w->L = L;
        w->LuaLog = LuaLog;
        w->m_entry = m_entry;
        w->LuaRendererHandler = style.LuaRendererHandler;
#endif
        sub->AppendWord(w);
        m_kstart = m_kend;
    }
}

#ifdef _LUA
void CRenderedTextSubtitle::ParseLuaTable(CSubtitle* sub, STSStyle& style)
{
    // Check "style" table:
    if(LuaIsTable(L, L"style"))
    {
        // Push table on top
        lua_getfield(L, -1, "style");
        
        // Font style
        if(LuaIsNumber(L, L"fs")) style.fontSize = LuaGetFloat(L, L"fs");
        if(LuaIsBool(L, L"bold")) style.fontWeight = (LuaGetBool(L, L"bold") ? FW_BOLD : FW_NORMAL);
        if(LuaIsBool(L, L"italic")) style.fItalic = LuaGetBool(L, L"italic");
        if(LuaIsBool(L, L"underline")) style.fUnderline = LuaGetBool(L, L"underline");
        if(LuaIsBool(L, L"strikeout")) style.fStrikeOut = LuaGetBool(L, L"strikeout");

        // Blur
        if(LuaIsNumber(L, L"blur")) style.fGaussianBlurX = style.fGaussianBlurY = LuaGetFloat(L, L"blur");
        if(LuaIsNumber(L, L"be")) style.fBlur = LuaGetFloat(L, L"be");

        // Transformation
        if(LuaIsNumber(L, L"frx")) style.fontAngleX = LuaGetFloat(L, L"frx");
        if(LuaIsNumber(L, L"fry")) style.fontAngleY = LuaGetFloat(L, L"fry");
        if(LuaIsNumber(L, L"frz")) style.fontAngleZ = LuaGetFloat(L, L"frz");
        if(LuaIsNumber(L, L"fax")) style.fontShiftX = LuaGetFloat(L, L"fax");
        if(LuaIsNumber(L, L"fay")) style.fontShiftY = LuaGetFloat(L, L"fay");
        if(LuaIsNumber(L, L"fscx")) style.fontScaleX = LuaGetFloat(L, L"fscx");
        if(LuaIsNumber(L, L"fscy")) style.fontScaleY = LuaGetFloat(L, L"fscy");
        if(LuaIsNumber(L, L"fsc")) style.fontScaleX = style.fontScaleY = LuaGetFloat(L, L"fsc");

        // Shadow and border
        if(LuaIsNumber(L, L"shadx")) style.shadowDepthX = LuaGetFloat(L, L"shadx");
        if(LuaIsNumber(L, L"shady")) style.shadowDepthY = LuaGetFloat(L, L"shady");
        if(LuaIsNumber(L, L"shad")) style.shadowDepthX = style.shadowDepthY = LuaGetFloat(L, L"shad");
        if(LuaIsNumber(L, L"bordx")) style.outlineWidthX = LuaGetFloat(L, L"bordx");
        if(LuaIsNumber(L, L"bordy")) style.outlineWidthY = LuaGetFloat(L, L"bordy");
        if(LuaIsNumber(L, L"bord")) style.outlineWidthX = style.outlineWidthY = LuaGetFloat(L, L"bord");

        // Alphas
        if(LuaIsNumber(L, L"a1")) style.alpha[0] = LuaGetInt(L, L"a1") & 0xFF;
        if(LuaIsNumber(L, L"a2")) style.alpha[1] = LuaGetInt(L, L"a2") & 0xFF;
        if(LuaIsNumber(L, L"a3")) style.alpha[2] = LuaGetInt(L, L"a3") & 0xFF;
        if(LuaIsNumber(L, L"a4")) style.alpha[3] = LuaGetInt(L, L"a4") & 0xFF;

        // Colors
        if(LuaIsNumber(L, L"c1")) style.colors[0] = LuaGetInt(L, L"c1") & 0xFFFFFF;
        if(LuaIsNumber(L, L"c2")) style.colors[1] = LuaGetInt(L, L"c2") & 0xFFFFFF;
        if(LuaIsNumber(L, L"c3")) style.colors[2] = LuaGetInt(L, L"c3") & 0xFFFFFF;
        if(LuaIsNumber(L, L"c4")) style.colors[3] = LuaGetInt(L, L"c4") & 0xFFFFFF;

        // Other
        if(LuaIsNumber(L, L"rndx")) style.mod_rand.X = LuaGetFloat(L, L"rndx");
        if(LuaIsNumber(L, L"rndy")) style.mod_rand.Y = LuaGetFloat(L, L"rndy");
        if(LuaIsNumber(L, L"rndz")) style.mod_rand.Z = LuaGetFloat(L, L"rndz");
        if(LuaIsNumber(L, L"rnds")) style.mod_rand.Seed = LuaGetFloat(L, L"rnds");
        if(LuaIsNumber(L, L"rnd"))
            style.mod_rand.X = style.mod_rand.Y = style.mod_rand.Z = LuaGetFloat(L, L"rnd");
        if(LuaIsNumber(L, L"fsp")) style.fontSpacing = LuaGetFloat(L, L"fsp");
        if(LuaIsNumber(L, L"fvsp")) style.mod_verticalSpace = LuaGetFloat(L, L"fvsp");
        if(LuaIsNumber(L, L"frs")) style.mod_fontOrient = LuaGetFloat(L, L"frs");

        // Pop table
        lua_pop(L, 1);
    }

    // Position
    // Check "pos" table:
    if(LuaIsTable(L, L"pos"))
    {
        // Push table on top
        lua_getfield(L, -1, "pos");
        if(LuaIsNumber(L, L"x") && LuaIsNumber(L, L"y"))
        {
            int X = LuaGetFloat(L, L"x") * sub->m_scalex * 8;
            int Y = LuaGetFloat(L, L"y") * sub->m_scaley * 8;

            if(Effect* e = DNew Effect)
            {
                e->param[0] = 0; // usual move
                e->param[1] = e->param[3] = X;
                e->param[2] = e->param[4] = Y;
                e->t[0] = e->t[1] = 0;

                sub->m_effects[EF_MOVE] = e;
            }
        }
        // Pop table
        lua_pop(L, 1);
    }
    // Check "org" table:
    if(LuaIsTable(L, L"org"))
    {
        // Push table on top
        lua_getfield(L, -1, "org");
        if(LuaIsNumber(L, L"x") && LuaIsNumber(L, L"y"))
        {
            int X = LuaGetFloat(L, L"x") * sub->m_scalex * 8;
            int Y = LuaGetFloat(L, L"y") * sub->m_scaley * 8;

            if(Effect* e = DNew Effect)
            {
                e->param[0] = 0; // usual move
                e->param[1] = e->param[3] = X;
                e->param[2] = e->param[4] = Y;
                e->t[0] = e->t[1] = 0;

                sub->m_effects[EF_ORG] = e;
            }
        }
        // Pop table
        lua_pop(L, 1);
    }
    // Vector clip position
    // Check "vcpos" table:
    if(LuaIsTable(L, L"vcpos"))
    {
        // Push table on top
        lua_getfield(L, -1, "vcpos");
        if(LuaIsNumber(L, L"x") && LuaIsNumber(L, L"y"))
        {
            int X = LuaGetFloat(L, L"x") * sub->m_scalex;
            int Y = LuaGetFloat(L, L"y") * sub->m_scaley;

            if(Effect* e = DNew Effect)
            {
                e->param[0] = 0; // usual move
                e->param[1] = e->param[3] = X;
                e->param[2] = e->param[4] = Y;
                e->t[0] = e->t[1] = 0;

                sub->m_effects[EF_VECTCLP] = e;
            }
        }
        // Pop table
        lua_pop(L, 1);
    }

    //  User data
    // Check "user" table:
    if(LuaIsTable(L, L"user"))
    {
        CStringA index;
        index.Format("sub_%d", m_entry);

        // Push table on top
        lua_getfield(L, -1, "user");
        lua_setglobal(L, index);
    }

    // Custom modify points before transform
    {
        CString LuaBeforeTransformHandler = CheckLuaHandler(L"beforetransform");
        CString LuaAfterTransformHandler = CheckLuaHandler(L"aftertransform");
        CString LuaCustomTransformHandler = CheckLuaHandler(L"customtransform");
        CString LuaClipStyleHandler = CheckLuaHandler(L"clipstyle");
        CString LuaRendererHandler = CheckLuaHandler(L"renderer");

        if(LuaBeforeTransformHandler.GetLength() > 0)
            style.LuaBeforeTransformHandler = LuaBeforeTransformHandler;
        if(LuaAfterTransformHandler.GetLength() > 0)
            style.LuaAfterTransformHandler = LuaAfterTransformHandler;
        if(LuaCustomTransformHandler.GetLength() > 0)
            style.LuaCustomTransformHandler = LuaCustomTransformHandler;
        if(LuaClipStyleHandler.GetLength() > 0)
            style.LuaClipStyleHandler = LuaClipStyleHandler;
        if(LuaRendererHandler.GetLength() > 0)
            style.LuaRendererHandler = LuaRendererHandler;
    }
}
#endif

#ifdef _VSMOD
namespace
{
struct PaintTag
{
    int part;
    bool slide;
    bool positioned;
    bool positionedAlpha;
    bool decoration;
    CStringW argument;
};

enum class OutlineProperty { Width, WidthX, WidthY, Color, Alpha, Blur, EdgeBlur, Corners,
    AlphaCorners, Gradient, AlphaGradient, Positioned, Slide, AlphaSlide, Waiting,
    WaitingCorners, WaitingGradient, WaitingSlide };

struct OutlineTag
{
    unsigned layer = 0;
    bool box = false;
    OutlineProperty property = OutlineProperty::Width;
    CStringW argument;
};

using AssNumeric::ReadDecimalDigit;

bool ReadPaintTag(const CStringW& name, PaintTag& tag)
{
    const wchar_t* p = name;
    int channel = *p >= L'1' && *p <= L'5' ? *p++ - L'0' : 0;
    if (*p >= 128 && channel == 0)
    {
        const auto* next = p;
        const int digit = ReadDecimalDigit(next);
        if (digit >= 1 && digit <= 5)
        {
            channel = digit;
            p = next;
        }
    }
    if (*p != L'g' && *p != L'v' && *p != L'p') return false;
    tag.positioned = !wcsncmp(p, L"pgrd", 4);
    tag.decoration = channel == 5;
    tag.positionedAlpha = tag.positioned && !channel && !wcsncmp(p + 4, L"alpha", 5);
    if (tag.positionedAlpha)
    {
        p += 9;
        tag.slide = *p == L's';
        if (tag.slide) ++p;
        tag.part = KfxPaint::PositionedAlphaSlidePart;
        tag.argument = p;
        tag.argument.Trim();
        return true;
    }
    const bool color = !wcsncmp(p, L"grd", 3) || tag.positioned;
    const bool alpha = !wcsncmp(p, L"gra", 3);
    if (color || (alpha && channel))
    {
        p += tag.positioned ? 4 : 3;
        tag.slide = *p == L's';
        if (tag.slide) ++p;
        tag.part = !channel && tag.slide && !tag.positioned ? -1 : ((channel ? channel : 1) - 1) * 2 + int(alpha);
        if (tag.decoration) tag.part = KfxPaint::DecorationSlideStart + int(alpha);
    }
    else if (!wcsncmp(p, L"vcs", 3) || !wcsncmp(p, L"vas", 3))
    {
        if (tag.decoration) return false;
        tag.slide = true;
        tag.part = ((channel ? channel : 1) - 1) * 2 + int(p[1] == L'a');
        p += 3;
    }
    else return false;
    tag.argument = p;
    tag.argument.Trim();
    return true;
}

bool ReadDecorationScalar(const CStringW& name, bool& inverse, CStringW& argument)
{
    const wchar_t* p = name;
    if ((*p != L'5' && *p < 128) || ReadDecimalDigit(p) != 5 || (*p != L'c' && *p != L'a'))
        return false;
    inverse = *p++ == L'a';
    argument = p;
    argument.Trim();
    return true;
}

bool ReadOutlineTag(const CStringW& name, OutlineTag& tag)
{
    const wchar_t* p = name;
    int number = 1;
    const bool numbered = wcsncmp(p, L"bb", 2) != 0;
    if (numbered)
    {
        if ((*p < L'1' || *p > L'9') && *p < 128)
            return false;
        const int first = ReadDecimalDigit(p);
        if (first < 1 || first > 9)
            return false;
        number = first;
        const auto* next = p;
        if (first == 1 && ReadDecimalDigit(next) == 0)
        {
            number = 10;
            p = next;
        }
    }
    using P = OutlineProperty;
    if (number == 3 && *p == L's')
    {
        tag.layer = 0;
        if (!wcsncmp(p, L"sgrds", 5) || !wcsncmp(p, L"svcs", 4))
        {
            tag.property = P::WaitingSlide;
            p += p[1] == L'g' ? 5 : 4;
        }
        else if (!wcsncmp(p, L"sgrd", 4))
        {
            tag.property = P::WaitingGradient;
            p += 4;
        }
        else if (!wcsncmp(p, L"svc", 3))
        {
            tag.property = P::WaitingCorners;
            p += 3;
        }
        else if (!wcsncmp(p, L"sc", 2))
        {
            tag.property = P::Waiting;
            p += 2;
        }
        else return false;
        tag.argument = p;
        tag.argument.Trim();
        return true;
    }
    if (*p++ != L'b')
        return false;
    // Native bblur and bbe start with the same two letters as box rings.
    const bool filter = numbered && (!wcsncmp(p, L"blur", 4) || !wcsncmp(p, L"be", 2));
    tag.box = *p == L'b' && !filter;
    if (tag.box) ++p;
    static constexpr struct { const wchar_t* name; P property; } names[] = {
        {L"pgrds", P::Slide}, {L"grds", P::Slide}, {L"gas", P::AlphaSlide}, {L"vcs", P::Slide}, {L"vas", P::AlphaSlide},
        {L"pgrd", P::Positioned},
        {L"grd", P::Gradient}, {L"ga", P::AlphaGradient}, {L"blur", P::Blur},
        {L"be", P::EdgeBlur}, {L"vc", P::Corners}, {L"va", P::AlphaCorners}, {L"sc", P::Waiting},
        {L"sx", P::WidthX}, {L"sy", P::WidthY}, {L"s", P::Width}, {L"c", P::Color}, {L"a", P::Alpha},
    };
    for (const auto& entry : names)
    {
        const auto length = wcslen(entry.name);
        if (wcsncmp(p, entry.name, length))
            continue;
        const auto* argument = p + length;
        if (*argument >= L'a' && *argument <= L'z' && wcscmp(argument, L"off"))
            return false;
        if (tag.box && (entry.property == P::WidthX || entry.property == P::WidthY ||
            entry.property == P::Blur || entry.property == P::EdgeBlur || entry.property == P::Waiting ||
            *entry.name == L'p'))
            return false;
        tag.layer = number - 1;
        tag.property = entry.property;
        tag.argument = argument;
        tag.argument.Trim();
        return true;
    }
    return false;
}

bool EventBoxTag(const CStringW& name)
{
    const wchar_t* p = name;
    return !wcsncmp(p, L"box", 3) || !wcsncmp(p, L"bs", 2);
}

std::vector<CStringW> PaintArguments(const CStringW& text)
{
    std::vector<CStringW> args;
    if (text.IsEmpty())
        return args;
    int start = 0;
    do
    {
        int end = text.Find(L',', start);
        if (end < 0) end = text.GetLength();
        args.push_back(text.Mid(start, end - start).Trim());
        start = end + 1;
    } while (start <= text.GetLength() && args.size() <= 2 * KfxPaint::MaxStops + 3);
    return args;
}

bool ReadPaintNumber(const CStringW& text, double& value)
{
    if (text.IsEmpty() || text.FindOneOf(L"xXpP") >= 0)
        return false;
    wchar_t* end;
    value = wcstod(text, &end);
    return end == text.GetString() + text.GetLength() && std::isfinite(value);
}

bool ExtendedNumber(const CStringW& text)
{
    const wchar_t* p = text;
    if (*p == L'~' || *p == L'(' || wcspbrk(p, L"$*/%"))
        return true;
    for (const wchar_t* start = p; *p; ++p)
        if (*p >= 128 || (p > start && (*p == L'+' || *p == L'-') && p[-1] != L'e' && p[-1] != L'E'))
            return true;
    return false;
}

CStringW NormalizeNumericDigits(const CStringW& input)
{
    return CStringW(AssNumeric::NormalizeDigits(input).c_str());
}

bool NeedsLayoutVariables(const CStringW& event)
{
    int position = 0;
    while ((position = event.Find(L'{', position)) >= 0)
    {
        const int stop = event.Find(L'}', ++position);
        if (stop < 0) return false;
        while ((position = event.Find(L'$', position)) >= 0 && position < stop)
        {
            const wchar_t* name = event.GetString() + ++position;
            for (const auto* variable : {L"width", L"height", L"left", L"right", L"center", L"top", L"bottom", L"middle"})
                if (!wcsncmp(name, variable, wcslen(variable)))
                    return true;
        }
        position = stop + 1;
    }
    return false;
}

bool ReadPaintValue(const CStringW& text, bool alpha, KfxPaint::Value& result)
{
    if (!alpha)
    {
        if (!text.CompareNoCase(L"$white") || !text.CompareNoCase(L"$shiro") ||
            !text.CompareNoCase(L"$siro"))
        {
            result = 0xFFFFFF;
            return true;
        }
        if (!text.CompareNoCase(L"$black") || !text.CompareNoCase(L"$kuro"))
        {
            result = 0;
            return true;
        }
    }
    const wchar_t* p = text;
    const bool decimal = alpha && *p == L'$';
    if (decimal) ++p;
    else
    {
        while (*p == L'&' || *p == L'H' || *p == L'h') ++p;
    }
    const auto* first = p;
    unsigned value = 0;
    const unsigned limit = alpha ? 255 : 0xFFFFFF;
    while (*p && *p != L'&')
    {
        unsigned digit;
        if (decimal)
        {
            const int decoded = ReadDecimalDigit(p);
            if (decoded < 0) return false;
            digit = decoded;
        }
        else
        {
            const auto c = *p++;
            if (c >= L'0' && c <= L'9') digit = c - L'0';
            else if (c >= L'A' && c <= L'F') digit = 10 + c - L'A';
            else if (c >= L'a' && c <= L'f') digit = 10 + c - L'a';
            else return false;
        }
        value = value * (decimal ? 10 : 16) + digit;
        if (value > limit) return false;
    }
    if (p == first || (*p && (decimal || p[1])))
        return false;
    result = alpha ? value : KfxPaint::Value(revcolor(value));
    return true;
}

template<class ReadNumber>
bool ReadPaintField(const std::vector<CStringW>& args, bool alpha, KfxPaint::Paint& paint, size_t offset,
    ReadNumber read)
{
    if (args.size() < offset + 3 || !(args.size() % 2) ||
        args.size() > offset + 2 * KfxPaint::MaxStops - 1 || !read(args[offset], paint.angle))
        return false;
    paint.angle = std::fmod(paint.angle, 360.0);
    paint.kind = KfxPaint::Paint::Kind::Field;
    double position = 0;
    for (size_t i = offset + 1; i + 1 < args.size(); i += 2)
    {
        KfxPaint::Value value;
        if (!ReadPaintValue(args[i], alpha, value)) return false;
        paint.stops.push_back({position, value});
        if (i + 2 == args.size()) break;
        CStringW percent = args[i + 1];
        double next;
        if (percent.IsEmpty() || percent[percent.GetLength() - 1] != L'%' ||
            !read(percent.Left(percent.GetLength() - 1).Trim(), next) ||
            next < 0 || next > 100 || next / 100 < position)
            return false;
        position = next / 100;
    }
    KfxPaint::Value last;
    if (!ReadPaintValue(args.back(), alpha, last)) return false;
    paint.stops.push_back({1, last});
    return true;
}

KfxPaint::Paint PaintCorners(const STSStyle& style, int channel, bool alpha)
{
    KfxPaint::Paint paint;
    paint.kind = KfxPaint::Paint::Kind::Corners;
    for (int i = 0; i < 4; ++i)
        paint.stops.push_back({0, alpha ? KfxPaint::Value(style.mod_grad.alpha[channel][i]) :
                                        KfxPaint::Value(style.mod_grad.color[channel][i])});
    return paint;
}

void SetPaintPart(STSStyle& style, int part, KfxPaint::Source source)
{
    auto state = style.mod_grad.paint ? std::make_shared<KfxPaint::Sources>(*style.mod_grad.paint) :
                                       std::make_shared<KfxPaint::Sources>();
    state->parts[part] = std::move(source);
    style.mod_grad.paint = std::move(state);
}

KfxPaint::Source ResolvePaintSource(CSubtitle& subtitle, KfxPaint::Source old, KfxPaint::Paint target,
    KfxPaint::Value solid, bool alpha, double progress, double width, bool keepCorners = false,
    bool animate = false)
{
    using namespace KfxPaint;
    if (progress <= 0)
        return old;
    const Paint* endpoint = old && old->kind == Paint::Kind::Slide ? old->to.get() : old.get();
    if (animate && endpoint && endpoint->kind == Paint::Kind::Field && target.kind == Paint::Kind::Field &&
        endpoint->positioned != target.positioned)
        return old;
    if (target.positioned) target.outside = solid;

    if (progress < 1 && width >= 0)
    {
        if (!old)
        {
            Paint fallback;
            fallback.solid = solid;
            old = std::make_shared<const Paint>(std::move(fallback));
        }
        if (old->depth == MaxSlides) return old;
        Paint slide;
        slide.kind = Paint::Kind::Slide;
        slide.angle = target.kind == Paint::Kind::Field ? target.angle : old->angle;
        slide.width = width;
        slide.progress = progress;
        slide.positioned = target.positioned || old->positioned;
        if (slide.positioned)
            slide.rect = target.positioned && old->positioned ? Unite(target.rect, old->rect) :
                target.positioned ? target.rect : old->rect;
        slide.depth = old->depth + 1;
        slide.from = old;
        slide.to = std::make_shared<const Paint>(std::move(target));
        target = std::move(slide);
    }
    else if (progress < 1)
    {
        if (old && old->kind == Paint::Kind::Slide)
            old = old->to;
        const Paint* field = old && old->kind == Paint::Kind::Field ? old.get() : nullptr;
        if (target.kind == Paint::Kind::Field)
            target = Interpolate(field, target, solid, progress, alpha);
        else if (target.kind == Paint::Kind::Solid && field)
        {
            const auto value = target.solid;
            target = *field;
            target.outside = Mix(field->outside, value, progress, alpha, false);
            for (auto& stop : target.stops)
                stop.value = Mix(stop.value, value, progress, alpha, false);
        }
    }

    if (target.kind == Paint::Kind::Solid || (target.kind == Paint::Kind::Corners && !keepCorners))
        return {};
    target.segment = target.kind == Paint::Kind::Field && old && old->segment &&
        progress < 1 ? old->segment : ++subtitle.m_paintSegments;
    return std::make_shared<const Paint>(std::move(target));
}

void ApplyPaintPart(CSubtitle& subtitle, STSStyle& style, int part, KfxPaint::Paint target,
                    double progress, double width, bool animate = false)
{
    using namespace KfxPaint;
    if (progress <= 0)
        return;
    const bool alpha = (part & 1) != 0;
    const int channel = part / 2;
    Source old = style.mod_grad.paint ? style.mod_grad.paint->parts[part] : Source{};
    const Value solid = alpha ? Value(style.alpha[channel]) : Value(revcolor(style.colors[channel]));
    if (!old && progress < 1 && width >= 0 && (style.mod_grad.legacyParts & (1u << part)))
        old = std::make_shared<const Paint>(PaintCorners(style, channel, alpha));
    SetPaintPart(style, part, ResolvePaintSource(subtitle, std::move(old), std::move(target),
        solid, alpha, progress, width, false, animate));
}

KfxPaint::OutlineLayer& EditOutlineLayer(KfxPaint::OutlineStyle& state, unsigned number)
{
    const auto pos = std::lower_bound(state.layers.begin(), state.layers.end(), number,
        [](const KfxPaint::OutlineLayer& layer, unsigned n) { return layer.number < n; });
    if (pos != state.layers.end() && pos->number == number)
        return *pos;
    KfxPaint::OutlineLayer layer;
    layer.number = number;
    return *state.layers.insert(pos, std::move(layer));
}
}
#endif

static std::shared_ptr<const OverrideTags> TokenizeOverrideTags(const CStringW& text)
{
    CStringW str = text;
#ifdef _VSMOD
    if (str.Find(L"\\~") >= 0)
        str.Replace(L"\\~", L"~");
#endif
    auto result = std::make_shared<OverrideTags>();
    for(int i = 0, j; (j = str.Find('\\', i)) >= 0; i = j)
    {
        CStringW cmd;
        for(WCHAR c = str[++j]; c && c != '(' && c != '\\'; cmd += c, c = str[++j]);
        cmd.Trim();
        if(cmd.IsEmpty()) continue;
#ifdef _VSMOD
        if (cmd[0] >= 128)
        {
            const wchar_t* start = cmd;
            const auto* next = start;
            const int number = ReadDecimalDigit(next);
            if (number >= 0)
                cmd = CStringW(wchar_t(L'0' + number)) + CStringW(next);
        }
#endif

        std::vector<CStringW> params;
        bool parenthesized = false;

        if(str[j] == '(')
        {
            CStringW param;
            // complex tags search
            int br = 1; // 1 bracket
            for(WCHAR c = str[++j]; c && br > 0; param += c, c = str[++j])
            {
                if(c == '(') br++;
                if(c == ')') br--;
                if(br == 0) break;
            }
            param.Trim();
            parenthesized = str[j] == L')';

#ifdef _VSMOD
            PaintTag paint{};
            OutlineTag outline;
            if (ReadPaintTag(cmd, paint) || ReadOutlineTag(cmd, outline) || EventBoxTag(cmd) || cmd == L"fad")
            {
                auto fields = PaintArguments(param);
                if (cmd == L"fad" && fields.size() != 4)
                    fields.clear();
                else
                {
                    if (str[j] == L')')
                    {
                        const int count = int(fields.size());
                        result->push_back({cmd, cmd, std::move(fields), count, true});
                    }
                    continue;
                }
            }
#endif
            while(!param.IsEmpty())
            {
                int i = param.Find(','), j = param.Find('\\');

                if(i >= 0 && (j < 0 || i < j))
                {
                    CStringW s = param.Left(i).Trim();
                    if(!s.IsEmpty()) params.push_back(s);
                    param = i + 1 < param.GetLength() ? param.Mid(i + 1) : L"";
                }
                else
                {
                    param.Trim();
                    if(!param.IsEmpty()) params.push_back(param);
                    param.Empty();
                }
            }
        }

        const CStringW rawCommand = cmd;
        const int rawCount = static_cast<int>(params.size());
#ifdef _VSMOD
        OutlineTag outline;
        PaintTag paint{};
        if (EventBoxTag(cmd) || ReadOutlineTag(cmd, outline) ||
            (ReadPaintTag(cmd, paint) && paint.positioned))
        {
            result->push_back({cmd, cmd, std::move(params), rawCount});
            continue;
        }
#endif
        if(!cmd.Find(L"1c") || !cmd.Find(L"2c") || !cmd.Find(L"3c") || !cmd.Find(L"4c"))
            params.push_back(cmd.Mid(2).Trim(L"&H")), cmd = cmd.Left(2);
        else if(!cmd.Find(L"1a") || !cmd.Find(L"2a") || !cmd.Find(L"3a") || !cmd.Find(L"4a"))
            params.push_back(cmd.Mid(2).Trim(L"&H")), cmd = cmd.Left(2);
#ifdef _VSMOD // patch m010. png background
        else if(!cmd.Find(L"1img") || !cmd.Find(L"2img") || !cmd.Find(L"3img") || !cmd.Find(L"4img"))
            ;//params.push_back(cmd.Mid(4)), cmd = cmd.Left(4);
#endif
#ifdef _VSMOD // patch m004. gradient colors
        else if(!cmd.Find(L"1vc") || !cmd.Find(L"2vc") || !cmd.Find(L"3vc") || !cmd.Find(L"4vc"))
            ;//params.push_back(cmd.Mid(3)), cmd = cmd.Left(3);
        else if(!cmd.Find(L"1va") || !cmd.Find(L"2va") || !cmd.Find(L"3va") || !cmd.Find(L"4va"))
            ;//params.push_back(cmd.Mid(3)), cmd = cmd.Left(3);
#endif
        else if(!cmd.Find(L"alpha"))
            params.push_back(cmd.Mid(5).Trim(L"&H")), cmd = cmd.Left(5);
        else if(!cmd.Find(L"an"))
            params.push_back(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"a"))
            params.push_back(cmd.Mid(1)), cmd = cmd.Left(1);
        else if(!cmd.Find(L"blur"))
            params.push_back(cmd.Mid(4)), cmd = cmd.Left(4);
        else if(!cmd.Find(L"bord"))
            params.push_back(cmd.Mid(4)), cmd = cmd.Left(4);
        else if(!cmd.Find(L"be"))
            params.push_back(cmd.Mid(2)), cmd = cmd.Left(2);
#ifdef _VSMOD
        else if (!cmd.Find(L"blend")) // vpatch v003. blending mode
            params.push_back(cmd.Mid(5)), cmd = cmd.Left(5);
#endif
        else if(!cmd.Find(L"b"))
            params.push_back(cmd.Mid(1)), cmd = cmd.Left(1);
#ifdef _VSMOD
        else if(cmd == L"clippos")
        {
            // Translation uses the arguments already read from the tuple.
        }
#endif
        else if(!cmd.Find(L"clip"))
            ;
        else if(!cmd.Find(L"c"))
            params.push_back(cmd.Mid(1).Trim(L"&H")), cmd = cmd.Left(1);
#ifdef _VSMOD // patch m008. distort
        else if(!cmd.Find(L"distort"))
            ;
#endif
        else if(!cmd.Find(L"fade"))
            ;
        else if(!cmd.Find(L"fe"))
            params.push_back(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"fn"))
            params.push_back(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"frx") || !cmd.Find(L"fry") || !cmd.Find(L"frz"))
            params.push_back(cmd.Mid(3)), cmd = cmd.Left(3);
#ifdef _VSMOD // patch m007. symbol rotating
        else if(!cmd.Find(L"frs"))
            params.push_back(cmd.Mid(3)), cmd = cmd.Left(3);
#endif
        else if(!cmd.Find(L"fax") || !cmd.Find(L"fay"))
            params.push_back(cmd.Mid(3)), cmd = cmd.Left(3);
        else if(!cmd.Find(L"fr"))
            params.push_back(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"fscx") || !cmd.Find(L"fscy"))
            params.push_back(cmd.Mid(4)), cmd = cmd.Left(4);
        else if(!cmd.Find(L"fsc"))
            params.push_back(cmd.Mid(3)), cmd = cmd.Left(3);
        else if(!cmd.Find(L"fsp"))
            params.push_back(cmd.Mid(3)), cmd = cmd.Left(3);
#ifdef _VSMOD// patch m001. Vertical fontspacing
        else if(!cmd.Find(L"fsvp"))
            params.push_back(cmd.Mid(4)), cmd = cmd.Left(4);
        else if (!cmd.Find(L"fshp"))// vpatch v002. Horizontal fontspacing
            params.push_back(cmd.Mid(4)), cmd = cmd.Left(4);
#endif
        else if(!cmd.Find(L"fs"))
            params.push_back(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"iclip"))
            ;
        else if(!cmd.Find(L"i"))
            params.push_back(cmd.Mid(1)), cmd = cmd.Left(1);
#ifdef _VSMOD // patch m011. jitter
        else if(!cmd.Find(L"jitter"))
            ;
#endif
        else if(!cmd.Find(L"kt") || !cmd.Find(L"kf") || !cmd.Find(L"ko")
#ifdef _VSMOD
                || !cmd.Find(L"kO")
#endif
                )
            params.push_back(cmd.Mid(2)), cmd = cmd.Left(2);
        else if(!cmd.Find(L"k") || !cmd.Find(L"K"))
            params.push_back(cmd.Mid(1)), cmd = cmd.Left(1);
#ifdef _VSMOD // patch m012. add lua animation
        else if(!cmd.Find(L"lua"))
            ;
#endif
#ifdef _VSMOD // patch m005. add some move types
        else if(!cmd.Find(L"mover"))	// radial move
            ;
        else if(!cmd.Find(L"moves3"))	// square spline
            ;
        else if(!cmd.Find(L"moves4"))	// cubic spline
            ;
#endif
#ifdef _VSMOD // patch m006. moveable vector clip
        else if(!cmd.Find(L"movevc"))
            ;
#endif
        else if(!cmd.Find(L"move"))
            ;
        else if(!cmd.Find(L"org"))
            ;
        else if(!cmd.Find(L"pbo"))
            params.push_back(cmd.Mid(3)), cmd = cmd.Left(3);
        else if(!cmd.Find(L"pos"))
            ;
        else if(!cmd.Find(L"p"))
            params.push_back(cmd.Mid(1)), cmd = cmd.Left(1);
        else if(!cmd.Find(L"q"))
            params.push_back(cmd.Mid(1)), cmd = cmd.Left(1);
#ifdef _VSMOD // patch m003. random text points
        else if(!cmd.Find(L"rndx") || !cmd.Find(L"rndy") || !cmd.Find(L"rndz") || !cmd.Find(L"rnds"))
            params.push_back(cmd.Mid(4)), cmd = cmd.Left(4);
        else if(!cmd.Find(L"rnd"))
            params.push_back(cmd.Mid(3)), cmd = cmd.Left(3);
#endif
        else if(!cmd.Find(L"r"))
            params.push_back(cmd.Mid(1)), cmd = cmd.Left(1);
        else if(!cmd.Find(L"shad"))
            params.push_back(cmd.Mid(4)), cmd = cmd.Left(4);
#ifdef _VSMOD
        else if(!cmd.Find(L"scale"))
            params.push_back(cmd.Mid(5)), cmd = cmd.Left(5);
#endif
        else if(!cmd.Find(L"s"))
            params.push_back(cmd.Mid(1)), cmd = cmd.Left(1);
#ifdef _VSMOD
        else if(!cmd.Find(L"tan"))
            params.push_back(cmd.Mid(3)), cmd = cmd.Left(3);
        else if(!cmd.Find(L"ta"))
            params.push_back(cmd.Mid(2)), cmd = cmd.Left(2);
#endif
        else if(!cmd.Find(L"t"))
            ;
        else if(!cmd.Find(L"u"))
            params.push_back(cmd.Mid(1)), cmd = cmd.Left(1);
        else if(!cmd.Find(L"xbord"))
            params.push_back(cmd.Mid(5)), cmd = cmd.Left(5);
        else if(!cmd.Find(L"xshad"))
            params.push_back(cmd.Mid(5)), cmd = cmd.Left(5);
        else if(!cmd.Find(L"ybord"))
            params.push_back(cmd.Mid(5)), cmd = cmd.Left(5);
        else if(!cmd.Find(L"yshad"))
            params.push_back(cmd.Mid(5)), cmd = cmd.Left(5);
        else if (!cmd.Find(L"xblur"))
            params.push_back(cmd.Mid(5)), cmd = cmd.Left(5);
        else if (!cmd.Find(L"yblur"))
            params.push_back(cmd.Mid(5)), cmd = cmd.Left(5);
#ifdef _VSMOD // patch m002. Z-coord
        else if(!cmd.Find(L"z"))
            params.push_back(cmd.Mid(1)), cmd = cmd.Left(1);
        else if(!cmd.Find(L"ortho")) // vpatch v001. ortho
            params.push_back(cmd.Mid(5)), cmd = cmd.Left(5);
#endif
        result->push_back({rawCommand, cmd, std::move(params), rawCount, parenthesized});
    }
    return result;
}

std::shared_ptr<const OverrideTags> OverrideTagCache::Get(const CStringW& text)
{
    const size_t length = text.GetLength();
    if (length > 1024)
        return TokenizeOverrideTags(text);

    const auto found = blocks.find(text);
    if (found != blocks.end())
        return found->second;

    auto tags = TokenizeOverrideTags(text);
    if (blocks.size() >= 1024 || characters + length > 128 * 1024)
    {
        blocks.clear();
        characters = 0;
    }
    blocks.emplace(text, tags);
    characters += length;
    return tags;
}

bool CRenderedTextSubtitle::ParseSSATag(CSubtitle* sub, CStringW str, STSStyle& style, STSStyle& org, bool fAnimate,
    const KfxPaint::SlideSettings* inheritedSlides)
{
    PROFILE_RENDER_STAGE(Overrides);
    if (!sub)
        return false;

    if (!m_overrideTags)
        m_overrideTags = std::make_unique<OverrideTagCache>();
    // A nested transform can rotate the cache while this block is being applied.
    const auto tags = m_overrideTags->Get(str);
#ifdef _VSMOD
    std::unique_ptr<KfxPaint::SlideSettings> localSlides;
    const auto slides = [&]() {
        return fAnimate ? (localSlides ? localSlides.get() : inheritedSlides) : sub->m_slideSettings.get();
    };
    const auto editSlides = [&]() -> KfxPaint::SlideSettings& {
        auto& owner = fAnimate ? localSlides : sub->m_slideSettings;
        if (!owner)
        {
            const auto* previous = slides();
            owner = previous ? std::make_unique<KfxPaint::SlideSettings>(*previous) :
                std::make_unique<KfxPaint::SlideSettings>();
        }
        return *owner;
    };
    const auto paintProgress = [&]() { return fAnimate ? KfxPaint::Unit(CalcAnimation(1, 0, true)) : 1.0; };
    const auto readOperand = [&](CStringW input, bool signedRelative, bool timing, AssNumeric::Operand& operand) {
        input.Trim();
        if (input.IsEmpty()) return false;
        bool negate = false;
        if (input[0] == L'~')
        {
            if (input.GetLength() < 3 || (input[1] != L'+' && input[1] != L'-'))
                return false;
            operand.relative = true;
            negate = input[1] == L'-';
            input = input.Mid(2).Trim();
        }
        else operand.relative = signedRelative && (input[0] == L'+' || input[0] == L'-');
        if (ExtendedNumber(input))
        {
            const auto normalized = NormalizeNumericDigits(input);
            if (!ReadPaintNumber(normalized, operand.value))
            {
                if (sub->m_measuringLayout || normalized.GetLength() > 256)
                    return false;
                AssNumeric::Context context{};
                if (sub->m_layoutVariables)
                    context = *sub->m_layoutVariables;
                else
                    std::fill(context.begin() + AssNumeric::Width, context.end(),
                        std::numeric_limits<double>::quiet_NaN());
                context[AssNumeric::End] = context[AssNumeric::Duration] = m_delay;
                context[AssNumeric::Mid] = m_delay / 2.0;
                if (!m_overrideTags->Expression(normalized, timing).Evaluate(context, operand.value))
                    return false;
            }
        }
        else if (!ReadPaintNumber(input, operand.value))
            return false;
        if (negate) operand.value = -operand.value;
        return std::abs(operand.value) <= (timing ? double(INT_MAX) + 1 : 1000000.0);
    };
    const auto readNumber = [&](const CStringW& text, double& result) {
        AssNumeric::Operand value;
        if (!readOperand(text, false, false, value) || value.relative)
            return false;
        result = value.value;
        return true;
    };
    const auto readField = [&](const std::vector<CStringW>& args, bool inverse, bool positioned, KfxPaint::Paint& field) {
        if (!ReadPaintField(args, inverse, field, positioned ? 4 : 0, readNumber))
            return false;
        if (positioned)
        {
            double coordinates[4];
            for (int i = 0; i < 4; ++i)
                if (!readNumber(args[i], coordinates[i]))
                    return false;
            const double left = sub->m_relativeTo == 1 ? m_vidrect.left / 8.0 : 0;
            const double top = sub->m_relativeTo == 1 ? m_vidrect.top / 8.0 : 0;
            field.positioned = true;
            field.rect = {coordinates[0] * sub->m_scalex + left, coordinates[1] * sub->m_scaley + top,
                coordinates[2] * sub->m_scalex + left, coordinates[3] * sub->m_scaley + top};
        }
        return true;
    };
    const auto copyOutline = [&](const std::shared_ptr<const KfxPaint::OutlineStyle>& source) {
        auto state = source ? std::make_shared<KfxPaint::OutlineStyle>(*source) :
            std::make_shared<KfxPaint::OutlineStyle>();
        state->scaleX = (m_fScaledBAS ? sub->m_scalex : 1) * 8;
        state->scaleY = (m_fScaledBAS ? sub->m_scaley : 1) * 8;
        return state;
    };
    const auto editOutlineAlpha = [&](unsigned alpha, double progress) {
        if (!style.nativeOutline || progress <= 0)
            return;
        auto state = copyOutline(style.nativeOutline);
        for (auto& layer : state->layers)
        {
            if (!layer.number) continue;
            const auto from = layer.inverseAlpha >= 0 ? unsigned(layer.inverseAlpha) : unsigned(style.alpha[2]);
            KfxPaint::Paint solid;
            solid.solid = alpha;
            const int part = KfxPaint::NativeSlideStart + int(layer.number - 1) * 2 + 1;
            layer.paint[1] = ResolvePaintSource(*sub, layer.paint[1], std::move(solid), from,
                true, progress, fAnimate && slides() && layer.paint[1] ? slides()->Width(part) : -1, true);
            layer.inverseAlpha = KfxPaint::Mix(from, alpha, progress, true, false);
        }
        style.nativeOutline = std::move(state);
    };
#endif
    for (const auto& tag : *tags)
    {
        const auto& cmd = tag.command;
        const auto& params = tag.parameters;
#ifdef _LUA
        // Direct function call!
        if(!sub->m_measuringLayout && m_overrideTags->IsLuaFunction(*this, tag.rawCommand))
        {
            hasExternalLua = true;
            // Find function =D
            const CStringA Func(tag.rawCommand);
            lua_getglobal(L, Func);

            // Create line table
            lua_newtable(L);
            LuaAddIntegerField(L, "time", m_time);
            LuaAddIntegerField(L, "start", m_start);
            LuaAddIntegerField(L, "end", m_end);
            LuaAddIntegerField(L, "length", m_delay);
            LuaAddIntegerField(L, "id", m_entry);
            if(fAnimate)
                LuaAddNumberField(L, "animate", CalcAnimation(1.0, 0.0, true));

            // Saved user data
            {
                CStringA index;
                index.Format("sub_%d", m_entry);
                lua_getglobal(L, index);
                if(lua_istable(L, -1))
                    lua_setfield(L, -2, "user");
                else
                    lua_pop(L, 1);
            }
            // Push arguments
            for(int arg = 0; arg < tag.rawCount; arg++)
            {
                CStringA Param(params[arg]);
                lua_pushstring(L, Param);
            }
            if (lua_pcall(L, tag.rawCount + 1, 1, 0) != 0)
            {
                // error
                CString ErrorText = L"Error: ";
                CString LuaErrorText(lua_tostring(L, -1));
                lua_pop(L, 1);
                LuaError(ErrorText + LuaErrorText);
            }
            else
            {
                // Retrieve result
                if (!lua_istable(L, -1))
                    LuaError(L"Line function must return a table");
                else
                {
                    sub->m_fAnimated = true;
                    ParseLuaTable(sub, style);
                }
                lua_pop(L, 1);
            }
            continue;
        }
#endif
        CStringW p = params.size() > 0 ? params[0] : L"";

#ifdef _VSMOD
        const wchar_t first = p.IsEmpty() ? 0 : p[0];
        const bool unicodeNumber = (first >= 128 || first == L'+' || first == L'-' ||
            (first >= L'0' && first <= L'9')) && AssNumeric::HasNonAscii(p);
        if (unicodeNumber && (cmd == L"an" || cmd == L"a" || cmd == L"kt" || cmd == L"k" || cmd == L"kO" ||
            cmd == L"K" || cmd == L"kf" || cmd == L"ko" || cmd == L"p" || cmd == L"q"))
            p = NormalizeNumericDigits(p);
        if (first == L'~' || unicodeNumber || (first && tag.parenthesized && ExtendedNumber(p)))
        {
            AssNumeric::Operand operand;
            bool handled = false;
            const auto scalar = [&](double& current, double units = 1., bool nonnegative = false,
                double limit = (std::numeric_limits<double>::max)()) {
                if (!readOperand(p, false, false, operand)) return;
                const double target = operand.Resolve(current / units) * units;
                const double value = CalcAnimation(target, current, fAnimate);
                if (std::isfinite(value))
                    current = (std::min)(limit, nonnegative ? (std::max)(0., value) : value);
            };
            if (cmd == L"fs")
            {
                scalar(style.fontSize, 1., false, 4096.);
                if (style.fontSize <= 0) style.fontSize = org.fontSize;
            }
            else if (cmd == L"fscx") scalar(style.fontScaleX, 1., true);
            else if (cmd == L"fscy") scalar(style.fontScaleY, 1., true);
            else if (cmd == L"fsp") scalar(style.fontSpacing);
            else if (cmd == L"fsvp") scalar(style.mod_verticalSpace, 8.);
            else if (cmd == L"fshp") scalar(style.mod_horizontalSpace, 8.);
            else if (cmd == L"fax") scalar(style.fontShiftX);
            else if (cmd == L"fay") scalar(style.fontShiftY);
            else if (cmd == L"fr" || cmd == L"frz") scalar(style.fontAngleZ);
            else if (cmd == L"frx") scalar(style.fontAngleX);
            else if (cmd == L"fry") scalar(style.fontAngleY);
            else if (cmd == L"z") scalar(style.mod_z, 80.);
            else if (cmd == L"frs" || cmd == L"pbo")
            {
                auto& current = cmd == L"frs" ? style.mod_fontOrient : m_polygonBaselineOffset;
                double value = current;
                scalar(value, cmd == L"frs" ? 10. : 1.);
                current = int(value);
            }
            else if (cmd == L"rnd" || cmd == L"rndx" || cmd == L"rndy" || cmd == L"rndz")
            {
                auto apply = [&](int& current) {
                    double value = current;
                    scalar(value, 8.);
                    current = int(value);
                };
                if (cmd == L"rnd" || cmd == L"rndx") apply(style.mod_rand.X);
                if (cmd == L"rnd" || cmd == L"rndy") apply(style.mod_rand.Y);
                if (cmd == L"rnd" || cmd == L"rndz") apply(style.mod_rand.Z);
            }
            else if (cmd == L"bord" || cmd == L"xbord" || cmd == L"ybord")
            {
                if (cmd != L"ybord") scalar(style.outlineWidthX, 1., true, 256.);
                if (cmd != L"xbord") scalar(style.outlineWidthY, 1., true, 256.);
            }
            else if (cmd == L"shad" || cmd == L"xshad" || cmd == L"yshad")
            {
                if (cmd != L"yshad") scalar(style.shadowDepthX);
                if (cmd != L"xshad") scalar(style.shadowDepthY);
            }
            else if (cmd == L"blur" || cmd == L"xblur" || cmd == L"yblur")
            {
                if (cmd != L"yblur") scalar(style.fGaussianBlurX, 1., true, 100.);
                if (cmd != L"xblur") scalar(style.fGaussianBlurY, 1., true, 100.);
            }
            else if (cmd == L"be")
            {
                double value = style.fBlur;
                scalar(value, 1., true);
                style.fBlur = int((std::min)(127., value));
            }
            else handled = true;
            if (!handled) continue;
        }

        if (!p.IsEmpty() && p[0] == L'$')
        {
            const bool numbered = cmd.GetLength() == 2 && cmd[0] >= L'1' && cmd[0] <= L'4';
            const bool color = cmd == L"c" || (numbered && cmd[1] == L'c');
            const bool inverse = cmd == L"alpha" || (numbered && cmd[1] == L'a');
            if (color || inverse)
            {
                KfxPaint::Value value;
                if (!ReadPaintValue(p, inverse, value)) continue;
                p.Format(L"%lX", static_cast<unsigned long>(inverse ? value : revcolor(value)));
            }
        }

        bool decorationAlpha;
        CStringW decorationArgument;
        if (cmd.GetLength() >= 2 && (cmd[0] == L'5' || cmd[0] >= 128) &&
            ReadDecorationScalar(cmd, decorationAlpha, decorationArgument))
        {
            using namespace KfxPaint;
            if (params.size() > 1 || (!params.empty() && !decorationArgument.IsEmpty())) continue;
            const auto& argument = params.empty() ? decorationArgument : p;
            const auto progress = paintProgress();
            if (progress <= 0) continue;
            const bool reset = argument.IsEmpty();
            Value value = 0;
            if (!reset && !ReadPaintValue(argument, decorationAlpha, value)) continue;
            auto deco = style.decoration ? std::make_shared<DecorationPaint>(*style.decoration) :
                std::make_shared<DecorationPaint>();
            auto& solid = decorationAlpha ? deco->inverseAlpha : deco->color;
            const Value from = solid >= 0 ? Value(solid) : decorationAlpha ? style.alpha[0] : revcolor(style.colors[0]);
            auto& source = deco->paint[int(decorationAlpha)];
            Paint target;
            target.solid = value;
            source = reset ? Source{} : ResolvePaintSource(*sub, source, std::move(target), from, decorationAlpha,
                progress, fAnimate && slides() && source ? slides()->Width(DecorationSlideStart + int(decorationAlpha)) : -1);
            solid = reset ? -1 : int(Mix(from, value, progress, decorationAlpha, false));
            style.decoration = std::move(deco);
            continue;
        }

        if (EventBoxTag(cmd))
        {
            const wchar_t* name = cmd;
            if (!wcsncmp(name, L"bs", 2))
            {
                if (!fAnimate && sub->m_borderStyleOverride < 0 && params.empty())
                {
                    if (!wcscmp(name + 2, L"1")) sub->m_borderStyleOverride = 0;
                    if (!wcscmp(name + 2, L"3")) sub->m_borderStyleOverride = 1;
                    if (!wcscmp(name + 2, L"4")) sub->m_borderStyleOverride = 2;
                    if (!wcscmp(name + 2, L"5")) sub->m_borderStyleOverride = 3;
                }
                continue;
            }
            name += 3;
            if (params.empty() && (*name == L'0' || *name == L'1') && !name[1])
            {
                if (!fAnimate || paintProgress() == 1) sub->m_boxMode = *name - L'0';
                continue;
            }
            auto box = style.boxShape ? std::make_shared<KfxPaint::BoxShape>(*style.boxShape) :
                std::make_shared<KfxPaint::BoxShape>();
            if (*name == L'r')
            {
                ++name;
                AssNumeric::Operand radius;
                if ((params.empty() && (!*name || readOperand(CStringW(name), false, false, radius))) ||
                    (!*name && params.size() == 1 && readOperand(p, false, false, radius)))
                {
                    box->radius = (std::max)(0., CalcAnimation(radius.Resolve(box->radius), box->radius, fAnimate));
                    style.boxShape = std::move(box);
                }
            }
            else if (*name == L'p')
            {
                ++name;
                const int first = *name == L'y' ? 2 : 0;
                const int count = *name == L'x' || *name == L'y' ? 2 : 4;
                if (count == 2) ++name;
                const bool tuple = !*name && params.size() == size_t(count);
                AssNumeric::Operand scalar;
                if (!tuple && (!params.empty() || !readOperand(CStringW(name), true, false, scalar)))
                    continue;
                bool valid = true;
                for (int i = 0; i < count; ++i)
                {
                    auto value = scalar;
                    if (tuple)
                    {
                        if (!readOperand(params[i], true, false, value))
                        {
                            valid = false;
                            break;
                        }
                    }
                    auto& padding = box->padding[first + i];
                    padding = (std::max)(0., CalcAnimation(value.Resolve(padding), padding, fAnimate));
                }
                if (valid) style.boxShape = std::move(box);
            }
            continue;
        }

        OutlineTag outline;
        if (ReadOutlineTag(cmd, outline))
        {
            using P = OutlineProperty;
            using namespace KfxPaint;
            const auto property = outline.property;
            const bool base = !outline.box && outline.layer == 0;
            const int channel = outline.box ? 3 : 2;
            auto& ringState = outline.box ? style.boxRings : style.nativeOutline;
            const bool inverse = property == P::Alpha || property == P::AlphaCorners ||
                property == P::AlphaGradient || property == P::AlphaSlide;
            const bool waitingField = property == P::WaitingGradient || property == P::WaitingCorners;
            const int part = waitingField || property == P::WaitingSlide ? WaitingSlidePart :
                (outline.box ? BoxSlideStart + 2 * int(outline.layer) :
                base ? 4 : NativeSlideStart + 2 * int(outline.layer - 1)) + int(inverse);
            const double progress = paintProgress();
            if (progress <= 0 || (!params.empty() && !outline.argument.IsEmpty()))
                continue;
            const auto& argument = params.empty() ? outline.argument : p;
            if (property == P::Slide || property == P::AlphaSlide || property == P::WaitingSlide)
            {
                if (params.size() > 1) continue;
                double value = 0;
                if (argument == L"off") value = -2;
                else if (!argument.IsEmpty())
                {
                    AssNumeric::Operand width;
                    if (!readOperand(argument, false, false, width)) continue;
                    value = width.Resolve(slides() ? (std::max)(0., slides()->Width(part)) : 0.);
                    if (value < 0) continue;
                    if (!value) value = -1;
                }
                auto& state = editSlides();
                (property == P::WaitingSlide ? state.waiting : outline.box ? state.box[part - BoxSlideStart] :
                    base ? state.channels[part] : state.outline[part - NativeSlideStart]) = value;
                continue;
            }
            const bool positioned = property == P::Positioned;
            const bool gradient = property == P::Gradient || property == P::AlphaGradient ||
                property == P::WaitingGradient || positioned;
            const bool corners = property == P::Corners || property == P::AlphaCorners || property == P::WaitingCorners;
            const double width = fAnimate && slides() ? slides()->Width(part) : -1;
            if (gradient || corners)
            {
                Paint target;
                const bool reset = params.empty() && (positioned ? tag.parenthesized && !fAnimate :
                    argument.IsEmpty() || argument == L"0");
                if (!reset && gradient && !readField(params, inverse, positioned, target))
                    continue;
                if (!reset && corners)
                {
                    if (params.size() != 4) continue;
                    target.kind = Paint::Kind::Corners;
                    for (const auto& a : params)
                    {
                        Value value;
                        if (!ReadPaintValue(a, inverse, value)) break;
                        target.stops.push_back({0, value});
                    }
                    if (target.stops.size() != 4) continue;
                }
                if (waitingField)
                {
                    auto state = copyOutline(ringState);
                    auto& layer = EditOutlineLayer(*state, 0);
                    const Value solid = layer.waitingColor >= 0 ? Value(layer.waitingColor) : revcolor(style.colors[2]);
                    layer.waitingPaint = reset ? Source{} : ResolvePaintSource(*sub,
                        layer.waitingPaint, std::move(target), solid, false, progress, width, true, fAnimate);
                    ringState = std::move(state);
                }
                else if (base)
                {
                    const Value solid = inverse ? style.alpha[2] : revcolor(style.colors[2]);
                    Source old = style.mod_grad.paint ? style.mod_grad.paint->parts[part] : Source{};
                    if (positioned && reset && (!old || !old->positioned)) continue;
                    if (!old && width >= 0 && (style.mod_grad.legacyParts & (1u << part)))
                        old = std::make_shared<const Paint>(PaintCorners(style, 2, inverse));
                    SetPaintPart(style, part, reset ? Source{} : ResolvePaintSource(*sub,
                        std::move(old), std::move(target), solid, inverse, progress, width, true, fAnimate));
                    style.mod_grad.legacyParts &= ~(1u << part);
                }
                else
                {
                    auto state = copyOutline(ringState);
                    auto& layer = EditOutlineLayer(*state, outline.layer);
                    const int component = int(inverse);
                    if (positioned && reset && (!layer.paint[component] || !layer.paint[component]->positioned))
                        continue;
                    const int fallback = inverse ? layer.inverseAlpha : layer.color;
                    const Value solid = fallback >= 0 ? Value(fallback) :
                        inverse ? style.alpha[channel] : revcolor(style.colors[channel]);
                    layer.paint[component] = reset ? Source{} : ResolvePaintSource(*sub,
                        layer.paint[component], std::move(target), solid, inverse, progress, width, true, fAnimate);
                    ringState = std::move(state);
                }
                continue;
            }
            if (params.size() > 1) continue;
            if (property == P::Color || property == P::Alpha || property == P::Waiting)
            {
                const bool reset = argument.IsEmpty();
                Value value = inverse ? org.alpha[channel] : revcolor(org.colors[channel]);
                if (!reset && !ReadPaintValue(argument, inverse, value)) continue;
                if (base && property != P::Waiting)
                {
                    const Value from = inverse ? style.alpha[2] : revcolor(style.colors[2]);
                    Paint target;
                    target.solid = value;
                    ApplyPaintPart(*sub, style, part, std::move(target), progress, width);
                    style.mod_grad.legacyParts &= ~(1u << part);
                    const Value mixed = Mix(from, value, progress, inverse, false);
                    if (inverse) style.alpha[2] = BYTE(mixed);
                    else style.colors[2] = revcolor(mixed);
                }
                else
                {
                    auto state = copyOutline(ringState);
                    auto& layer = EditOutlineLayer(*state, outline.layer);
                    auto& dest = property == P::Waiting ? layer.waitingColor : inverse ? layer.inverseAlpha : layer.color;
                    const Value from = dest >= 0 ? Value(dest) : inverse ? style.alpha[channel] : revcolor(style.colors[channel]);
                    if (property == P::Waiting)
                    {
                        Paint target;
                        target.solid = value;
                        const double waitingWidth = fAnimate && slides() ? slides()->Width(WaitingSlidePart) : -1;
                        layer.waitingPaint = reset ? Source{} : ResolvePaintSource(*sub, layer.waitingPaint,
                            std::move(target), from, false, progress, layer.waitingPaint ? waitingWidth : -1, true);
                    }
                    else
                    {
                        Paint target;
                        target.solid = value;
                        auto& source = layer.paint[int(inverse)];
                        source = reset ? Source{} : ResolvePaintSource(*sub, source, std::move(target),
                            from, inverse, progress, source ? width : -1, true);
                    }
                    dest = reset ? -1 : int(Mix(from, value, progress, inverse, false));
                    ringState = std::move(state);
                }
                continue;
            }

            const bool reset = argument.IsEmpty();
            AssNumeric::Operand value;
            if (!reset && !readOperand(argument, true, false, value)) continue;
            if (property == P::Blur || property == P::EdgeBlur)
            {
                auto state = copyOutline(ringState);
                auto& layer = EditOutlineLayer(*state, outline.layer);
                if (property == P::Blur)
                {
                    const double current = layer.blur >= 0 ? layer.blur : style.fGaussianBlurX;
                    layer.blur = reset ? -1 : CalcAnimation((std::max)(0., (std::min)(value.Resolve(current), 100.)),
                        current, fAnimate);
                }
                else
                {
                    const double current = layer.edgeBlur >= 0 ? layer.edgeBlur : style.fBlur;
                    layer.edgeBlur = reset ? -1 : int(CalcAnimation((std::max)(0., (std::min)(value.Resolve(current), 127.)),
                        current, fAnimate) + 0.5);
                }
                ringState = std::move(state);
                continue;
            }
            const bool applyX = property != P::WidthY;
            const bool applyY = property != P::WidthX;
            const auto animateSize = [&](double current, double fallback) {
                return (std::max)(0., CalcAnimation(reset ? fallback : value.Resolve(current), current, fAnimate));
            };
            if (base)
            {
                if (applyX) style.outlineWidthX = animateSize(style.outlineWidthX, org.outlineWidthX);
                if (applyY) style.outlineWidthY = animateSize(style.outlineWidthY, org.outlineWidthY);
            }
            else
            {
                auto state = copyOutline(ringState);
                auto& layer = EditOutlineLayer(*state, outline.layer);
                if (applyX) layer.x = animateSize(layer.x, 0.);
                if (applyY) layer.y = animateSize(layer.y, 0.);
                if (!outline.box && (layer.x > 0 || layer.y > 0))
                {
                    if (layer.color < 0) layer.color = revcolor(style.colors[2]);
                    if (layer.inverseAlpha < 0) layer.inverseAlpha = style.alpha[2];
                }
                ringState = std::move(state);
            }
            continue;
        }

        PaintTag paint{};
        if (ReadPaintTag(cmd, paint))
        {
            if (paint.slide)
            {
                if (params.size() > 1 || (!params.empty() && !paint.argument.IsEmpty())) continue;
                const CStringW& argument = params.empty() ? paint.argument : p;
                double setting = 0;
                if (argument == L"off") setting = -2;
                else if (!argument.IsEmpty())
                {
                    AssNumeric::Operand width;
                    if (!readOperand(argument, false, false, width)) continue;
                    const auto* state = slides();
                    const double current = !state ? 0. : paint.part < 0 ?
                        state->global : (std::max)(0., state->Width(paint.part));
                    setting = width.Resolve((std::max)(0., current));
                    if (setting < 0) continue;
                    if (setting == 0) setting = -1;
                }
                auto& state = editSlides();
                (paint.positionedAlpha ? state.positionedAlpha :
                    paint.decoration ? state.decoration[paint.part - KfxPaint::DecorationSlideStart] :
                    paint.part < 0 ? state.global : state.channels[paint.part]) = setting;
            }
            else if (paint.positionedAlpha)
            {
                if (!tag.parenthesized || !paint.argument.IsEmpty()) continue;
                auto& source = style.mod_grad.positionedOpacity;
                if (params.empty())
                {
                    if (!fAnimate) source.reset();
                    continue;
                }
                constexpr unsigned maxScopes = 32;
                if (sub->m_positionedAlphaScopes >= maxScopes) continue;
                KfxPaint::Paint field;
                if (!readField(params, true, true, field)) continue;
                const double progress = paintProgress();
                if (progress <= 0) continue;
                source = ResolvePaintSource(*sub, source, std::move(field), 0, true, progress,
                    fAnimate && slides() ? slides()->Width(paint.part) : -1, false, fAnimate);
                ++sub->m_positionedAlphaScopes;
            }
            else if (paint.decoration)
            {
                using namespace KfxPaint;
                const bool inverse = paint.part == DecorationSlideStart + 1;
                const auto progress = paintProgress();
                if (progress <= 0) continue;
                const bool reset = params.empty() && (paint.positioned ? tag.parenthesized && !fAnimate :
                    paint.argument.IsEmpty() || paint.argument == L"0");
                Paint field;
                if (!reset && (!paint.argument.IsEmpty() || !readField(params, inverse, paint.positioned, field)))
                    continue;
                auto deco = style.decoration ? std::make_shared<DecorationPaint>(*style.decoration) :
                    std::make_shared<DecorationPaint>();
                auto& source = deco->paint[int(inverse)];
                if (paint.positioned && reset && (!source || !source->positioned)) continue;
                const int own = inverse ? deco->inverseAlpha : deco->color;
                const Value solid = own >= 0 ? Value(own) : inverse ? style.alpha[0] : revcolor(style.colors[0]);
                source = reset ? Source{} : ResolvePaintSource(*sub, source, std::move(field), solid, inverse, progress,
                    fAnimate && slides() ? slides()->Width(paint.part) : -1, false, fAnimate);
                style.decoration = std::move(deco);
            }
            else if (params.empty() && (paint.argument.IsEmpty() || paint.argument == L"0"))
            {
                if (!fAnimate && (!paint.positioned || (tag.parenthesized && paint.argument.IsEmpty() &&
                    style.mod_grad.paint && style.mod_grad.paint->parts[paint.part] &&
                    style.mod_grad.paint->parts[paint.part]->positioned)))
                {
                    style.mod_grad.legacyParts &= ~(1u << paint.part);
                    SetPaintPart(style, paint.part, {});
                }
            }
            else if (paint.argument.IsEmpty())
            {
                KfxPaint::Paint field;
                if (readField(params, (paint.part & 1) != 0, paint.positioned, field))
                {
                    const auto progress = paintProgress();
                    ApplyPaintPart(*sub, style, paint.part, std::move(field), progress,
                                   fAnimate && slides() ? slides()->Width(paint.part) : -1, fAnimate);
                    if (progress > 0)
                    {
                        style.mod_grad.legacyParts &= ~(1u << paint.part);
                        if (!(paint.part & 1) && style.mod_grad.mode[paint.part / 2] == 2)
                            style.mod_grad.mode[paint.part / 2] = 0;
                    }
                }
            }
            continue;
        }

        // Feed ordinary paint tags into a transform only when extended paint
        // or an explicit slide setting already participates in that channel.
        const bool numbered = cmd.GetLength() >= 2 && cmd[0] >= L'1' && cmd[0] <= L'4';
        if (style.mod_grad.paint || slides())
        {
            const bool scalarColor = cmd == L"c" || (numbered && cmd.GetLength() == 2 && cmd[1] == L'c');
            const bool allAlpha = cmd == L"alpha";
            const bool scalarAlpha = allAlpha || (numbered && cmd.GetLength() == 2 && cmd[1] == L'a');
            if (scalarColor || scalarAlpha)
            {
                const int first = numbered ? cmd[0] - L'1' : 0;
                const auto progress = paintProgress();
                for (int channel = first; channel < (allAlpha ? 4 : first + 1); ++channel)
                {
                    const int part = channel * 2 + int(scalarAlpha);
                    const double width = fAnimate && slides() ? slides()->Width(part) : -1;
                    const auto& state = style.mod_grad.paint;
                    if ((!state || !state->parts[part]) &&
                        (width < 0 || !(style.mod_grad.legacyParts & (1u << part))))
                        continue;
                    KfxPaint::Paint solid;
                    solid.solid = scalarAlpha ? KfxPaint::Value(p.IsEmpty() ? org.alpha[channel] : wcstol(p, nullptr, 16) & 255) :
                        KfxPaint::Value(revcolor(p.IsEmpty() ? org.colors[channel] : wcstol(p, nullptr, 16) & 0xFFFFFF));
                    ApplyPaintPart(*sub, style, part, std::move(solid), progress, width);
                }
                if (progress <= 0)
                    continue;
            }
            if (numbered && cmd.GetLength() == 3 && cmd[1] == L'v' &&
                (cmd[2] == L'a' || cmd[2] == L'c') && params.size() >= 4)
            {
                const bool inverseAlpha = cmd[2] == L'a';
                const int part = (cmd[0] - L'1') * 2 + int(inverseAlpha);
                const auto progress = paintProgress();
                if (progress <= 0) continue;
                const double width = fAnimate && slides() ? slides()->Width(part) : -1;
                if (width >= 0 && progress < 1)
                {
                    KfxPaint::Paint corners;
                    corners.kind = KfxPaint::Paint::Kind::Corners;
                    for (int i = 0; i < 4; ++i)
                    {
                        KfxPaint::Value value;
                        if (!ReadPaintValue(params[i], inverseAlpha, value)) break;
                        corners.stops.push_back({0, value});
                    }
                    if (corners.stops.size() == 4)
                    {
                        ApplyPaintPart(*sub, style, part, std::move(corners), progress, width);
                        continue;
                    }
                }
                else if (style.mod_grad.paint && style.mod_grad.paint->parts[part])
                    SetPaintPart(style, part, {});
            }
        }
#endif

        if(cmd == "1c" || cmd == L"2c" || cmd == L"3c" || cmd == L"4c")
        {
            int i = cmd[0] - '1';

            DWORD c = wcstol(p, NULL, 16);
            style.colors[i] = !p.IsEmpty()
                              ? (((int)CalcAnimation(c & 0xff, style.colors[i] & 0xff, fAnimate)) & 0xff
                                 | ((int)CalcAnimation(c & 0xff00, style.colors[i] & 0xff00, fAnimate)) & 0xff00
                                 | ((int)CalcAnimation(c & 0xff0000, style.colors[i] & 0xff0000, fAnimate)) & 0xff0000)
                              : org.colors[i];
#ifdef _VSMOD // patch m004. gradient colors
            style.mod_grad.colors[i] = style.colors[i];
            style.mod_grad.legacyParts &= ~(1u << (2 * i));
            if(!fAnimate)
            {
                style.mod_grad.mode[i] = style.mod_grad.paint && (style.mod_grad.legacyParts & (2u << (2 * i))) ? 1 : 0;
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.color[i][j] = !p.IsEmpty()
                                                 ? revcolor(c & 0xffffff)
                                                 : org.mod_grad.color[i][j];
                }
            }
            else if(style.mod_grad.mode[i] != 0)
            {
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.color[i][j] = !p.IsEmpty()
                                                 ? (((int)CalcAnimation(c & 0xff, style.mod_grad.color[i][j] & 0xff, fAnimate)) & 0xff
                                                    | ((int)CalcAnimation(c & 0xff00, style.mod_grad.color[i][j] & 0xff00, fAnimate)) & 0xff00
                                                    | ((int)CalcAnimation(c & 0xff0000, style.mod_grad.color[i][j] & 0xff0000, fAnimate)) & 0xff0000)
                                                 : org.colors[i];
                }
            }
#endif
        }
        else if(cmd == L"1a" || cmd == L"2a" || cmd == L"3a" || cmd == L"4a")
        {
            DWORD al = wcstol(p, NULL, 16) & 0xff;
            int i = cmd[0] - '1';
#ifdef _VSMOD
            if (i == 2 && style.nativeOutline)
                editOutlineAlpha(p.IsEmpty() ? org.alpha[2] : al, paintProgress());
#endif

            style.alpha[i] = !p.IsEmpty()
                             ? (BYTE)CalcAnimation(al, style.alpha[i], fAnimate)
                             : org.alpha[i];
#ifdef _VSMOD // patch m004. gradient colors
            style.mod_grad.alphas[i] = style.alpha[i];
            style.mod_grad.legacyParts &= ~(2u << (2 * i));
            style.mod_grad.b_images[i].alpha = 255 - style.alpha[i];
            if(!fAnimate)
            {
                //style.mod_grad.mode[i] = 0;
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.alpha[i][j] = !p.IsEmpty()
                                                 ? al
                                                 : org.mod_grad.alpha[i][j];
                }
            }
            else if(style.mod_grad.mode[i] != 0)
            {
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.alpha[i][j] = !p.IsEmpty()
                                                 ? (((int)CalcAnimation(al, style.mod_grad.alpha[i][j], fAnimate)))
                                                 : org.alpha[i];
                }
            }
#endif
        }
#ifdef _VSMOD // patch m010. png background
        else if(cmd == L"1img" || cmd == L"2img" || cmd == L"3img" || cmd == L"4img")
        {
            int i = cmd[0] - '1';

            if(params.size() >= 1)// file[,xoffset,yoffset[,angle]]
            {
                if(!fAnimate)
                {
                    CString fpath = !m_resPath.IsEmpty() ? m_resPath : m_path.Left(m_path.ReverseFind('\\') + 1);

                    bool t_init = false;
                    // buffer
                    for(ptrdiff_t k = 0, j = mod_images.GetCount(); k < j; k++)
                    {
                        MOD_PNGIMAGE t_temp = mod_images[k];
                        if(t_temp.filename == params[0]) // found buffered image
                        {
                            style.mod_grad.b_images[i] = t_temp;
                            t_init = true;
                            break;
                        }
                        if(t_temp.filename == fpath + params[0]) // found buffered image
                        {
                            style.mod_grad.b_images[i] = t_temp;
                            t_init = true;
                            break;
                        }
                    }
                    if(t_init)
                    {
                        style.mod_grad.mode[i] = 2;
                    }
                    else
                    {
                        // not found
                        MOD_PNGIMAGE t_temp;
                        if(t_temp.initImage(params[0])) // absolute path or default directory
                        {
                            style.mod_grad.mode[i] = 2;
                            style.mod_grad.b_images[i] = t_temp;
                            mod_images.Add(t_temp);
                        }
                        else
                        if(t_temp.initImage(fpath + params[0])) // path + relative path
                        {
                            style.mod_grad.mode[i] = 2;
                            style.mod_grad.b_images[i] = t_temp;
                            mod_images.Add(t_temp);
                        }
                    }
                }
                if(params.size() >= 3)
                {
                    style.mod_grad.b_images[i].xoffset = !p.IsEmpty()
                                                         ? CalcAnimation(wcstol(params[1], NULL, 10), style.mod_grad.b_images[i].xoffset, fAnimate)
                                                         : org.mod_grad.b_images[i].xoffset;
                    style.mod_grad.b_images[i].yoffset = !p.IsEmpty()
                                                         ? CalcAnimation(wcstol(params[2], NULL, 10), style.mod_grad.b_images[i].yoffset, fAnimate)
                                                         : org.mod_grad.b_images[i].yoffset;
                }
            }
        }
#endif
#ifdef _VSMOD // patch m004. gradient colors
        else if(cmd == L"1vc" || cmd == L"2vc" || cmd == L"3vc" || cmd == L"4vc")
        {
            int i = cmd[0] - '1';

            if(params.size() >= 4)
            {
                style.mod_grad.legacyParts |= 1u << (2 * i);
                DWORD c;
                for(int j = 0; j < 4; j++)
                {
                    c = wcstol(CStringW(params[j]).Trim(L"&H"), NULL, 16);
                    style.mod_grad.color[i][j] = !p.IsEmpty()
                                                 ? (((int)CalcAnimation((c & 0xff0000) >> 16, style.mod_grad.color[i][j] & 0xff, fAnimate)) & 0xff
                                                    | ((int)CalcAnimation(c & 0xff00, style.mod_grad.color[i][j] & 0xff00, fAnimate)) & 0xff00
                                                    | ((int)CalcAnimation((c & 0xff) << 16, style.mod_grad.color[i][j] & 0xff0000, fAnimate)) & 0xff0000)
                                                 : org.mod_grad.color[i][j];
                }
                if (style.mod_grad.mode[i] == 0)
                {
                    for (int j = 0; j < 4; j++)
                    {
                        style.mod_grad.alpha[i][j] = style.alpha[i];
                    }
                }
                //if (!fAnimate)
                style.mod_grad.mode[i] = 1;

                if (i == 0 || i == 1)
                    style.mod_grad.mode[0] = style.mod_grad.mode[1] = 1;
            }
        }
        else if(cmd == L"1va" || cmd == L"2va" || cmd == L"3va" || cmd == L"4va")
        {
            int i = cmd[0] - '1';

            if(params.size() >= 4)
            {
                style.mod_grad.legacyParts |= 2u << (2 * i);
                int a;
                for(int j = 0; j < 4; j++)
                {
                    a = wcstol(CStringW(params[j]).Trim(L"&H"), NULL, 16);
                    style.mod_grad.alpha[i][j] = !p.IsEmpty()
                                                 ? (int)CalcAnimation(a, style.mod_grad.alpha[i][j], fAnimate) : org.mod_grad.alpha[i][j];
                }
                if(style.mod_grad.mode[i] == 0)
                {
                    for(int j = 0; j < 4; j++)
                    {
                        style.mod_grad.color[i][j] = revcolor(style.colors[i]);
                    }
                }
                //if (!fAnimate)
                style.mod_grad.mode[i] = 1;

                if (i == 0 || i == 1) {
                    style.mod_grad.mode[0] = style.mod_grad.mode[1] = 1;
                    style.mod_grad.bodyIsGradAlpha = true;
                }
            }
        }
#endif
        else if(cmd == L"alpha")
        {
#ifdef _VSMOD
            if (style.nativeOutline)
                editOutlineAlpha(p.IsEmpty() ? org.alpha[2] : wcstol(p, nullptr, 16) & 255, paintProgress());
#endif
            for(ptrdiff_t i = 0; i < 4; i++)
            {
                DWORD al = wcstol(p, NULL, 16) & 0xff;
                style.alpha[i] = !p.IsEmpty()
                                 ? (BYTE)CalcAnimation(al, style.alpha[i], fAnimate)
                                 : org.alpha[i];
#ifdef _VSMOD // patch m004. gradient colors
                style.mod_grad.alphas[i] = style.alpha[i];
                style.mod_grad.legacyParts &= ~(2u << (2 * i));
                if(!fAnimate)
                {
                    //style.mod_grad.mode[i] = 0;
                    for(int j = 0; j < 4; j++)
                    {
                        style.mod_grad.alpha[i][j] = !p.IsEmpty()
                                                     ? al
                                                     : org.mod_grad.alpha[i][j];
                        style.mod_grad.b_images[i].alpha = 255 - al;
                    }
                }
                else if(style.mod_grad.mode[i] != 0)
                {
                    for(int j = 0; j < 4; j++)
                    {
                        style.mod_grad.alpha[i][j] = !p.IsEmpty()
                                                     ? (((int)CalcAnimation(al, style.mod_grad.alpha[i][j], fAnimate)))
                                                     : org.alpha[i];
                        style.mod_grad.b_images[i].alpha = 255 - style.alpha[i];
                    }
                }
#endif
            }
        }
        else if(cmd == L"an")
        {
            int n = wcstol(p, NULL, 10);
            if(sub->m_scrAlignment < 0)
                sub->m_scrAlignment = (n > 0 && n < 10) ? n : org.scrAlignment;
        }
        else if(cmd == L"a")
        {
            int n = wcstol(p, NULL, 10);
            if(sub->m_scrAlignment < 0)
                sub->m_scrAlignment = (n > 0 && n < 12) ? ((((n - 1) & 3) + 1) + ((n & 4) ? 6 : 0) + ((n & 8) ? 3 : 0)) : org.scrAlignment;
        }
        else if(cmd == L"blur")
        {
            double dst = wcstod(p, NULL);
            double nx = CalcAnimation(dst, style.fGaussianBlurX, fAnimate);
            style.fGaussianBlurX = !p.IsEmpty()
                                  ? (nx < 0 ? 0 : nx)
                                      : org.fGaussianBlurX;
            double ny = CalcAnimation(dst, style.fGaussianBlurY, fAnimate);
            style.fGaussianBlurY = !p.IsEmpty()
                                    ? (ny < 0 ? 0 : ny)
                                      : org.fGaussianBlurY;
        }
        else if(cmd == L"bord")
        {
            double dst = wcstod(p, NULL);
            double nx = CalcAnimation(dst, style.outlineWidthX, fAnimate);
            style.outlineWidthX = !p.IsEmpty()
                                  ? (nx < 0 ? 0 : nx)
                                      : org.outlineWidthX;
            double ny = CalcAnimation(dst, style.outlineWidthY, fAnimate);
            style.outlineWidthY = !p.IsEmpty()
                                  ? (ny < 0 ? 0 : ny)
                                      : org.outlineWidthY;
        }
        else if(cmd == L"be")
        {
            int n = (int)(CalcAnimation(wcstol(p, NULL, 10), style.fBlur, fAnimate) + 0.5);
            style.fBlur = !p.IsEmpty()
                          ? n
                          : org.fBlur;
        }
        else if(cmd == L"b")
        {
            int n = wcstol(p, NULL, 10);
            style.fontWeight = !p.IsEmpty()
                               ? (n == 0 ? FW_NORMAL : n == 1 ? FW_BOLD : n >= 100 ? n : org.fontWeight)
                                   : org.fontWeight;
        }
#ifdef _VSMOD
        else if(cmd == L"clippos")
        {
            AssNumeric::Operand x, y;
            if (params.size() == 2 && tag.parenthesized &&
                readOperand(params[0], false, false, x) && readOperand(params[1], false, false, y))
            {
                sub->m_clipOffsetX = CalcAnimation(x.Resolve(sub->m_clipOffsetX), sub->m_clipOffsetX, fAnimate);
                sub->m_clipOffsetY = CalcAnimation(y.Resolve(sub->m_clipOffsetY, -1), sub->m_clipOffsetY, fAnimate);
            }
        }
#endif
        else if(cmd == L"clip" || cmd == L"iclip")
        {
            bool invert = (cmd == L"iclip");

            if(params.size() == 1 && !sub->m_pClipper)
            {
#if defined (_VSMOD) && defined(_LUA)
                sub->m_pClipper = DNew CClipper(params[0], CSize(m_size.cx >> 3, m_size.cy >> 3), sub->m_scalex, sub->m_scaley, invert, style.LuaClipStyleHandler, L, LuaLog, m_entry);
#else
                sub->m_pClipper = DNew CClipper(params[0], CSize(m_size.cx >> 3, m_size.cy >> 3), sub->m_scalex, sub->m_scaley, invert);
#endif           
            }
            else if(params.size() == 2 && !sub->m_pClipper)
            {
                int scale = max(wcstol(p, NULL, 10), 1);
#if defined (_VSMOD) && defined(_LUA)
                sub->m_pClipper = DNew CClipper(params[1], CSize(m_size.cx >> 3, m_size.cy >> 3), sub->m_scalex / (1 << (scale - 1)), sub->m_scaley / (1 << (scale - 1)), invert, style.LuaClipStyleHandler, L, LuaLog, m_entry);
#else
                sub->m_pClipper = DNew CClipper(params[1], CSize(m_size.cx >> 3, m_size.cy >> 3), sub->m_scalex / (1 << (scale - 1)), sub->m_scaley / (1 << (scale - 1)), invert);
#endif       
            }
            else if(params.size() == 4)
            {
                CRect r;

                CPoint o(0, 0);

                if(sub->m_relativeTo == 1) // TODO: this should also apply to the other two clippings above
                {
                    o.x = m_vidrect.left >> 3;
                    o.y = m_vidrect.top >> 3;
                }
#ifdef _VSMOD
                sub->m_hasRectClip = true;
                if (std::any_of(params.begin(), params.end(), ExtendedNumber))
                {
                    const LONG previous[] = {sub->m_clip.left, sub->m_clip.top, sub->m_clip.right, sub->m_clip.bottom};
                    double resolved[4];
                    bool valid = true;
                    for (int i = 0; i < 4; ++i)
                    {
                        AssNumeric::Operand operand;
                        if (!readOperand(params[i], false, false, operand))
                        {
                            valid = false;
                            break;
                        }
                        const double scale = i % 2 ? sub->m_scaley : sub->m_scalex;
                        const double offset = i % 2 ? o.y : o.x;
                        const double dst = operand.relative ? previous[i] + operand.value * scale :
                            operand.value * scale + offset;
                        resolved[i] = CalcAnimation(dst, previous[i], fAnimate);
                    }
                    if (valid)
                    {
                        sub->m_clipInverse = invert;
                        sub->m_clip.SetRect(int(resolved[0]), int(resolved[1]), int(resolved[2]), int(resolved[3]));
                    }
                    continue;
                }
#endif
                sub->m_clipInverse = invert;
                r.SetRect(
                    wcstol(params[0], NULL, 10),
                    wcstol(params[1], NULL, 10),
                    wcstol(params[2], NULL, 10),
                    wcstol(params[3], NULL, 10));

                sub->m_clip.SetRect(
                    (int)CalcAnimation(sub->m_scalex * r.left + o.x, sub->m_clip.left, fAnimate),
                    (int)CalcAnimation(sub->m_scaley * r.top + o.y, sub->m_clip.top, fAnimate),
                    (int)CalcAnimation(sub->m_scalex * r.right + o.x, sub->m_clip.right, fAnimate),
                    (int)CalcAnimation(sub->m_scaley * r.bottom + o.y, sub->m_clip.bottom, fAnimate));
            }
        }
        else if(cmd == L"c")
        {
            DWORD c = wcstol(p, NULL, 16);
            style.colors[0] = !p.IsEmpty()
                              ? (((int)CalcAnimation(c & 0xff, style.colors[0] & 0xff, fAnimate)) & 0xff
                                 | ((int)CalcAnimation(c & 0xff00, style.colors[0] & 0xff00, fAnimate)) & 0xff00
                                 | ((int)CalcAnimation(c & 0xff0000, style.colors[0] & 0xff0000, fAnimate)) & 0xff0000)
                              : org.colors[0];
#ifdef _VSMOD // patch m004. gradient colors
            style.mod_grad.colors[0] = style.colors[0];
            style.mod_grad.legacyParts &= ~1u;
            if(!fAnimate)
            {
                style.mod_grad.mode[0] = style.mod_grad.paint && (style.mod_grad.legacyParts & 2u) ? 1 : 0;
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.color[0][j] = !p.IsEmpty()
                                                 ? ((int)c & 0xff
                                                    | (int)c & 0xff00
                                                    | (int)c & 0xff0000)
                                                 : org.mod_grad.color[0][j];
                }
            }
            else if(style.mod_grad.mode[0] != 0)
            {
                for(int j = 0; j < 4; j++)
                {
                    style.mod_grad.color[0][j] = !p.IsEmpty()
                                                 ? (((int)CalcAnimation(c & 0xff, style.mod_grad.color[0][j] & 0xff, fAnimate)) & 0xff
                                                    | ((int)CalcAnimation(c & 0xff00, style.mod_grad.color[0][j] & 0xff00, fAnimate)) & 0xff00
                                                    | ((int)CalcAnimation(c & 0xff0000, style.mod_grad.color[0][j] & 0xff0000, fAnimate)) & 0xff0000)
                                                 : org.colors[0];
                }
            }
#endif
        }
#ifdef _VSMOD // patch m008. distort
        else if(cmd == L"distort")
        {
            if(params.size() >= 6)
            {
                DWORD c;
                for(int j = 0; j < 3; j++)
                {
                    style.mod_distort.pointsx[j] = !p.IsEmpty()
                                                   ? (CalcAnimation(wcstod(params[j*2], NULL), style.mod_distort.pointsx[j], fAnimate))
                                                   : org.mod_distort.pointsx[j];
                    style.mod_distort.pointsy[j] = !p.IsEmpty()
                                                   ? (CalcAnimation(wcstod(params[j*2+1], NULL), style.mod_distort.pointsy[j], fAnimate))
                                                   : org.mod_distort.pointsy[j];
                }
                style.mod_distort.enabled = true;
            }
        }
#endif
        else if(cmd == L"fade" || cmd == L"fad")
        {
            if(params.size() == 7 && !sub->m_effects[EF_FADE])// {\fade(a1=param[0], a2=param[1], a3=param[2], t1=t[0], t2=t[1], t3=t[2], t4=t[3])
            {
                if(Effect* e = DNew Effect)
                {
                    for(ptrdiff_t i = 0; i < 3; i++)
                        e->param[i] = wcstol(params[i], NULL, 10);
                    for(ptrdiff_t i = 0; i < 4; i++)
                        e->t[i] = wcstol(params[3+i], NULL, 10);

                    sub->m_effects[EF_FADE] = e;
                }
            }
            else if((params.size() == 2 || (cmd == L"fad" && params.size() == 4)) && !sub->m_effects[EF_FADE])
            {
                if(Effect* e = DNew Effect)
                {
                    e->param[0] = e->param[2] = 0xff;
                    e->param[1] = 0x00;
                    for(ptrdiff_t i = 1; i < 3; i++)
                        e->t[i] = wcstol(params[i-1], NULL, 10);
                    e->t[0] = e->t[3] = -1; // will be substituted with "start" and "end"
#ifdef _VSMOD
                    if (params.size() == 4)
                    {
                        for (int side = 0; side < 2; ++side)
                        {
                            CStringW argument = params[side + 2];
                            const int length = argument.GetLength();
                            const bool combine = length >= 2 && !argument.Right(2).CompareNoCase(L"+a");
                            if (combine) argument = argument.Left(length - 2).Trim();
                            KfxPaint::Value color;
                            if (ReadPaintValue(argument, false, color))
                            {
                                e->fadeColors[side] = color;
                                e->hasFadeColor[side] = true;
                                if (!combine) e->param[side * 2] = 0;
                            }
                        }
                    }
#endif

                    sub->m_effects[EF_FADE] = e;
                }
            }
        }
        else if(cmd == L"fax")
        {
            style.fontShiftX = !p.IsEmpty()
                               ? CalcAnimation(wcstod(p, NULL), style.fontShiftX, fAnimate)
                               : org.fontShiftX;
        }
        else if(cmd == L"fay")
        {
            style.fontShiftY = !p.IsEmpty()
                               ? CalcAnimation(wcstod(p, NULL), style.fontShiftY, fAnimate)
                               : org.fontShiftY;
        }
        else if(cmd == L"fe")
        {
            int n = wcstol(p, NULL, 10);
            style.charSet = !p.IsEmpty()
                            ? n
                            : org.charSet;
        }
        else if(cmd == L"fn")
        {
            style.fontName = (!p.IsEmpty() && p != '0')
                             ? CString(p).Trim()
                             : org.fontName;
        }
#ifdef _VSMOD // patch m007. symbol rotating
        else if(cmd == L"frs")
        {
            double dst = wcstod(p, NULL) * 10;

            style.mod_fontOrient = !p.IsEmpty()
                                   ? CalcAnimation(dst, style.mod_fontOrient, fAnimate)
                                   : org.mod_fontOrient;
        }
#endif
        else if(cmd == L"frx")
        {
            style.fontAngleX = !p.IsEmpty()
                               ? CalcAnimation(wcstod(p, NULL), style.fontAngleX, fAnimate)
                               : org.fontAngleX;
        }
        else if(cmd == L"fry")
        {
            style.fontAngleY = !p.IsEmpty()
                               ? CalcAnimation(wcstod(p, NULL), style.fontAngleY, fAnimate)
                               : org.fontAngleY;
        }
        else if(cmd == L"frz" || cmd == L"fr")
        {
            style.fontAngleZ = !p.IsEmpty()
                               ? CalcAnimation(wcstod(p, NULL), style.fontAngleZ, fAnimate)
                               : org.fontAngleZ;
        }
        else if(cmd == L"fscx")
        {
            double n = CalcAnimation(wcstol(p, NULL, 10), style.fontScaleX, fAnimate);
            style.fontScaleX = !p.IsEmpty()
                               ? ((n < 0) ? 0 : n)
                                   : org.fontScaleX;
        }
        else if(cmd == L"fscy")
        {
            double n = CalcAnimation(wcstol(p, NULL, 10), style.fontScaleY, fAnimate);
            style.fontScaleY = !p.IsEmpty()
                               ? ((n < 0) ? 0 : n)
                                   : org.fontScaleY;
        }
        else if(cmd == L"fsc")
        {
#ifdef _VSMOD
            AssNumeric::Operand operand;
            if (p.IsEmpty()) style.softScale = 100;
            else if (readOperand(p, true, false, operand))
                style.softScale = (std::max)(0., (std::min)(10000., CalcAnimation(
                    operand.Resolve(style.softScale), style.softScale, fAnimate)));
#else
            style.fontScaleX = org.fontScaleX;
            style.fontScaleY = org.fontScaleY;
#endif
        }
#ifdef _VSMOD
        else if(cmd == L"scale")
        {
            AssNumeric::Operand operand;
            if (p.IsEmpty()) style.objectScale = 100;
            else if (readOperand(p, true, false, operand))
                style.objectScale = (std::max)(0., (std::min)(10000., CalcAnimation(
                    operand.Resolve(style.objectScale), style.objectScale, fAnimate)));
        }
#endif
        else if(cmd == L"fsp")
        {
            style.fontSpacing = !p.IsEmpty()
                                ? CalcAnimation(wcstod(p, NULL), style.fontSpacing, fAnimate)
                                : org.fontSpacing;
        }
#ifdef _VSMOD // patch m001. Vertical fontspacing
        else if(cmd == L"fsvp")
        {
            double dst = wcstod(p, NULL) * 8;
            double nx = CalcAnimation(dst, style.mod_verticalSpace, fAnimate);
            style.mod_verticalSpace = !p.IsEmpty() ? nx : org.mod_verticalSpace;
        }
        // vpatch v002. Horizontal fontspacing
        else if (cmd == L"fshp")
        {
            double dst = wcstod(p, NULL) * 8;
            double nx = CalcAnimation(dst, style.mod_horizontalSpace, fAnimate);
            style.mod_horizontalSpace = !p.IsEmpty() ? nx : org.mod_horizontalSpace;
        }
#endif
        else if(cmd == L"fs")
        {
            if(!p.IsEmpty())
            {
                if(p[0] == '-' || p[0] == '+')
                {
                    double n = CalcAnimation(style.fontSize + style.fontSize * wcstol(p, NULL, 10) / 10, style.fontSize, fAnimate);
                    style.fontSize = (n > 0) ? n : org.fontSize;
                }
                else
                {
                    double n = CalcAnimation(wcstol(p, NULL, 10), style.fontSize, fAnimate);
                    style.fontSize = (n > 0) ? n : org.fontSize;
                }
            }
            else
            {
                style.fontSize = org.fontSize;
            }
        }
        else if(cmd == L"i")
        {
            int n = wcstol(p, NULL, 10);
            style.fItalic = !p.IsEmpty()
                            ? (n == 0 ? false : n == 1 ? true : org.fItalic)
                                : org.fItalic;
        }
#ifdef _VSMOD // patch m011. jitter
        else if(cmd == L"jitter") // {\jitter(left,right,up,down,period,[seed])}
        {
            if((params.size() >= 4))
            {
                int left = (int)abs(wcstol(params[0], NULL, 10)) * 8;
                int right = (int)abs(wcstol(params[1], NULL, 10)) * 8;
                int up = (int)abs(wcstol(params[2], NULL, 10)) * 8;
                int down = (int)abs(wcstol(params[3], NULL, 10)) * 8;
                style.mod_jitter.offset.top = CalcAnimation(up, style.mod_jitter.offset.top, fAnimate);
                style.mod_jitter.offset.bottom = CalcAnimation(down, style.mod_jitter.offset.bottom, fAnimate);
                style.mod_jitter.offset.left = CalcAnimation(left, style.mod_jitter.offset.left, fAnimate);
                style.mod_jitter.offset.right = CalcAnimation(right, style.mod_jitter.offset.right, fAnimate);
                style.mod_jitter.enabled = true;

                if(params.size() >= 5)
                {
                    int period = wcstol(params[4], NULL, 10) * 10000;
                    style.mod_jitter.period = CalcAnimation(period, style.mod_jitter.period, fAnimate);
                    if(params.size() >= 6)
                    {
                        style.mod_jitter.seed = wcstol(params[5], NULL, 10);
                    }
                }
            }
        }
#endif
        else if(cmd == L"kt")
        {
            m_kstart = !p.IsEmpty()
                       ? wcstol(p, NULL, 10) * 10
                       : 0;
            m_kend = m_kstart;
        }
        else if(cmd == L"kf" || cmd == L"K")
        {
            m_ktype = 1;
            m_kstart = m_kend;
            m_kend += !p.IsEmpty()
                      ? wcstol(p, NULL, 10) * 10
                      : 1000;
        }
#ifdef _VSMOD
        else if(cmd == L"kO")
        {
            sub->m_revealKaraoke = true;
            m_ktype = 3;
            m_kstart = m_kend;
            m_kend += !p.IsEmpty() ? wcstol(p, NULL, 10) * 10 : 1000;
        }
#endif
        else if(cmd == L"ko")
        {
            m_ktype = 2;
            m_kstart = m_kend;
            m_kend += !p.IsEmpty()
                      ? wcstol(p, NULL, 10) * 10
                      : 1000;
        }
        else if(cmd == L"k")
        {
            m_ktype = 0;
            m_kstart = m_kend;
            m_kend += !p.IsEmpty()
                      ? wcstol(p, NULL, 10) * 10
                      : 1000;
        }
#ifdef _VSMOD // patch m012. add lua animation
#ifdef _LUA
        else if(cmd == L"lua")
        {
            if (sub->m_measuringLayout) continue;
            if(params.size() > 0)
            {
                if(LuaHasFunction(L, params[0]))
                {
                    hasExternalLua = true;
                    CStringA Func(params[0]);
                    lua_getglobal(L, Func);
                    // Create line table
                    lua_newtable(L);
                    LuaAddIntegerField(L, "time", m_time);
                    LuaAddIntegerField(L, "start", m_start);
                    LuaAddIntegerField(L, "end", m_end);
                    LuaAddIntegerField(L, "length", m_delay);
                    LuaAddIntegerField(L, "id", m_entry);
                    if(fAnimate)
                        LuaAddNumberField(L, "animate", CalcAnimation(1.0, 0.0, true));
                    // Saved user data
                    {
                        CStringA index;
                        index.Format("sub_%d", m_entry);
                        lua_getglobal(L, index);
                        if(lua_istable(L, -1))
                            lua_setfield(L, -2, "user");
                        else
                            lua_pop(L, 1);
                    }
                    // Push arguments
                    for(int arg = 1; arg < params.size(); arg++)
                    {
                        CStringA Param(params[arg]);
                        lua_pushstring(L, Param); 
                    }
                    if (lua_pcall(L, params.size(), 1, 0) != 0)
                    {
                        // error
                        CString ErrorText = L"Error: ";
                        CString LuaErrorText(lua_tostring(L, -1));
                        lua_pop(L, 1);
                        LuaError(ErrorText + LuaErrorText);
                    }
                    else
                    {
                        // Retrieve result
                        if (!lua_istable(L, -1))
                            LuaError(L"Line function must return a table");
                        else
                        {
                            sub->m_fAnimated = true;
                            ParseLuaTable(sub, style);
                        }
                        lua_pop(L, 1);
                    }
                }
            }
        }
#endif
#endif
#ifdef _VSMOD // patch m005. add some move types
        else if(cmd == L"mover") // {\mover(x1,x2,x2,y2,alp1,alp2,r1,r2,t1,t2)}
        {
            if((params.size() == 8 || params.size() == 10) && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = new Effect)
                {
                    e->param[0] = 1; // Radial move
                    e->param[1] = (int)(sub->m_scalex * wcstod(params[0], NULL) * 8); // x1
                    e->param[2] = (int)(sub->m_scaley * wcstod(params[1], NULL) * 8); // y1
                    e->param[3] = (int)(sub->m_scalex * wcstod(params[2], NULL) * 8); // x2
                    e->param[4] = (int)(sub->m_scaley * wcstod(params[3], NULL) * 8); // y2
                    e->param[5] = (int)(wcstod(params[4], NULL) * 10000); // alp1
                    e->param[6] = (int)(wcstod(params[5], NULL) * 10000); // alp2
                    e->param[7] = (int)(sub->m_scalex * wcstod(params[6], NULL) * 8); // r1
                    e->param[8] = (int)(sub->m_scaley * wcstod(params[7], NULL) * 8); // r2

                    e->t[0] = e->t[1] = -1;

                    if(params.size() == 10)
                    {
                        for(int i = 0; i < 2; i++)
                            e->t[i] = wcstol(params[8+i], NULL, 10);
                    }
                    sub->m_effects[EF_MOVE] = e;
                }
            }
        }
        else if(cmd == L"moves3") // {\moves3(x1,x2,x2,y2,x3,y3[,t1,t2])}
        {
            if((params.size() == 6 || params.size() == 8) && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = new Effect)
                {
                    e->param[0] = 2; // square spline
                    e->param[1] = (int)(sub->m_scalex * wcstod(params[0], NULL) * 8); // x1
                    e->param[2] = (int)(sub->m_scaley * wcstod(params[1], NULL) * 8); // y1
                    e->param[3] = (int)(sub->m_scalex * wcstod(params[2], NULL) * 8); // x2
                    e->param[4] = (int)(sub->m_scaley * wcstod(params[3], NULL) * 8); // y2
                    e->param[5] = (int)(sub->m_scalex * wcstod(params[4], NULL) * 8); // x3
                    e->param[6] = (int)(sub->m_scaley * wcstod(params[5], NULL) * 8); // y3
                    e->t[0] = e->t[1] = -1;

                    if(params.size() == 8)
                    {
                        for(int i = 0; i < 2; i++)
                            e->t[i] = wcstol(params[6+i], NULL, 10);
                    }
                    sub->m_effects[EF_MOVE] = e;
                }
            }
        }
        else if(cmd == L"moves4") // {\moves4(x1,x2,x2,y2,x3,y3,x4,y4[,t1,t2])}
        {
            if((params.size() == 8 || params.size() == 10) && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = new Effect)
                {
                    e->param[0] = 3; // cubic spline
                    e->param[1] = (int)(sub->m_scalex * wcstod(params[0], NULL) * 8); // x1
                    e->param[2] = (int)(sub->m_scaley * wcstod(params[1], NULL) * 8); // y1
                    e->param[3] = (int)(sub->m_scalex * wcstod(params[2], NULL) * 8); // x2
                    e->param[4] = (int)(sub->m_scaley * wcstod(params[3], NULL) * 8); // y2
                    e->param[5] = (int)(sub->m_scalex * wcstod(params[4], NULL) * 8); // x3
                    e->param[6] = (int)(sub->m_scaley * wcstod(params[5], NULL) * 8); // y3
                    e->param[7] = (int)(sub->m_scalex * wcstod(params[6], NULL) * 8); // x4
                    e->param[8] = (int)(sub->m_scaley * wcstod(params[7], NULL) * 8); // y4
                    e->t[0] = e->t[1] = -1;

                    if(params.size() == 10)
                    {
                        for(int i = 0; i < 2; i++)
                            e->t[i] = wcstol(params[8+i], NULL, 10);
                    }
                    sub->m_effects[EF_MOVE] = e;
                }
            }
        }
#endif
#ifdef _VSMOD // patch m006. moveable vector clip
        else if(cmd == L"movevc")
        {
            if((params.size() == 2 || params.size() == 4 || params.size() == 6) && !sub->m_effects[EF_VECTCLP])
            {
                if(Effect* e = new Effect)
                {
                    e->param[0] = e->param[2] = (int)(sub->m_scalex * wcstod(params[0], NULL));
                    e->param[1] = e->param[3] = (int)(sub->m_scaley * wcstod(params[1], NULL));
                    e->t[0] = e->t[1] = -1;

                    if(params.size() >= 4)
                    {
                        e->param[2] = (int)(sub->m_scalex * wcstod(params[2], NULL));
                        e->param[3] = (int)(sub->m_scaley * wcstod(params[3], NULL));
                    }
                    if(params.size() == 6)
                    {
                        e->t[0] = (int)(sub->m_scalex * wcstod(params[4], NULL));
                        e->t[1] = (int)(sub->m_scaley * wcstod(params[5], NULL));
                    }
                    sub->m_effects[EF_VECTCLP] = e;
                }
            }
        }
#endif
        else if(cmd == L"move") // {\move(x1=param[0], y1=param[1], x2=param[2], y2=param[3][, t1=t[0], t2=t[1]])}
        {
#ifdef _VSMOD
            if (sub->m_motion && (sub->m_motion->shifted || sub->m_motion->hasMove))
                continue;
            if ((params.size() == 4 || params.size() == 6) && !sub->m_effects[EF_MOVE] &&
                std::any_of(params.begin(), params.end(), ExtendedNumber))
            {
                AssMotion::Move move{};
                AssNumeric::Operand* args[] = {&move.x1, &move.y1, &move.x2, &move.y2};
                bool valid = true;
                for (int i = 0; i < 4; ++i)
                {
                    valid = readOperand(params[i], false, false, *args[i]);
                    if (!valid) break;
                    args[i]->value *= i % 2 ? sub->m_scaley * (args[i]->relative ? -8 : 8) : sub->m_scalex * 8;
                }
                if (!valid) continue;
                move.end = m_delay;
                if (params.size() == 6)
                {
                    AssNumeric::Operand start, end;
                    if (!readOperand(params[4], false, true, start) || !readOperand(params[5], false, true, end) ||
                        start.relative || end.relative || start.value < INT_MIN || end.value < INT_MIN ||
                        start.value > INT_MAX || end.value > INT_MAX)
                        continue;
                    move.start = int(start.value);
                    move.end = int(end.value);
                    if (move.start > move.end) std::swap(move.start, move.end);
                    if (move.end <= 0)
                    {
                        move.start = 0;
                        move.end = m_delay;
                    }
                }
                if (!sub->m_motion) sub->m_motion = std::make_unique<AssMotion::Track>();
                sub->m_motion->hasMove = true;
                sub->m_motion->move = move;
                sub->m_fAnimated = true;
                continue;
            }
#endif
            if((params.size() == 4 || params.size() == 6) && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = DNew Effect)
                {
#ifdef _VSMOD // patch m005. add some move types
                    e->param[0] = 0; // обычный мов
                    e->param[1] = (int)(sub->m_scalex * wcstod(params[0], NULL) * 8);
                    e->param[2] = (int)(sub->m_scaley * wcstod(params[1], NULL) * 8);
                    e->param[3] = (int)(sub->m_scalex * wcstod(params[2], NULL) * 8);
                    e->param[4] = (int)(sub->m_scaley * wcstod(params[3], NULL) * 8);
#else
                    e->param[0] = (int)(sub->m_scalex * wcstod(params[0], NULL) * 8);
                    e->param[1] = (int)(sub->m_scaley * wcstod(params[1], NULL) * 8);
                    e->param[2] = (int)(sub->m_scalex * wcstod(params[2], NULL) * 8);
                    e->param[3] = (int)(sub->m_scaley * wcstod(params[3], NULL) * 8);
#endif
                    e->t[0] = e->t[1] = -1;

                    if(params.size() == 6)
                    {
                        for(ptrdiff_t i = 0; i < 2; i++)
                            e->t[i] = wcstol(params[4+i], NULL, 10);
                    }

                    sub->m_effects[EF_MOVE] = e;
                }
            }
        }
        else if(cmd == L"org") // {\org(x=param[0], y=param[1])}
        {
#ifdef _VSMOD // patch f003. moving \org for some karaoke effects. part 1
            if (params.size() == 2 && (ExtendedNumber(params[0]) || ExtendedNumber(params[1])))
            {
                std::array<AssNumeric::Operand, 2> next;
                if (!readOperand(params[0], false, false, next[0]) || !readOperand(params[1], false, false, next[1]))
                    continue;
                if ((sub->m_origin || sub->m_effects[EF_ORG]) && !next[0].relative && !next[1].relative)
                    continue;
                next[0].value *= sub->m_scalex * 8;
                next[1].value *= sub->m_scaley * (next[1].relative ? -8 : 8);
                if (!sub->m_origin)
                    sub->m_origin = std::make_unique<std::array<AssNumeric::Operand, 2>>(next);
                else
                    for (int i = 0; i < 2; ++i)
                    {
                        auto& previous = (*sub->m_origin)[i];
                        if (next[i].relative) previous.value += next[i].value;
                        else previous = next[i];
                    }
                continue;
            }
            if (sub->m_origin) continue;
            if((params.size() == 2 || params.size() == 4 || params.size() == 6) && !sub->m_effects[EF_ORG])
            {
                if(Effect* e = new Effect)
                {
                    e->param[0] = e->param[2] = (int)(sub->m_scalex * wcstod(params[0], NULL) * 8);
                    e->param[1] = e->param[3] = (int)(sub->m_scaley * wcstod(params[1], NULL) * 8);
                    e->t[0] = e->t[1] = -1;

                    if(params.size() >= 4)
                    {
                        e->param[2] = (int)(sub->m_scalex * wcstod(params[2], NULL) * 8);
                        e->param[3] = (int)(sub->m_scaley * wcstod(params[3], NULL) * 8);
                    }
                    if(params.size() == 6)
                    {
                        e->t[0] = (int)(sub->m_scalex * wcstod(params[4], NULL) * 8);
                        e->t[1] = (int)(sub->m_scaley * wcstod(params[5], NULL) * 8);
                    }
                    sub->m_effects[EF_ORG] = e;
                }
            }
#else
            if(params.size() == 2 && !sub->m_effects[EF_ORG])
            {
                if(Effect* e = DNew Effect)
                {
                    e->param[0] = (int)(sub->m_scalex * wcstod(params[0], NULL) * 8);
                    e->param[1] = (int)(sub->m_scaley * wcstod(params[1], NULL) * 8);

                    sub->m_effects[EF_ORG] = e;
                }
            }
#endif
        }
        else if(cmd == L"pbo")
        {
            m_polygonBaselineOffset = wcstol(p, NULL, 10);
        }
        else if(cmd == L"pos")
        {
#ifdef _VSMOD
            if (params.size() == 2 && (fAnimate || ExtendedNumber(params[0]) || ExtendedNumber(params[1])))
            {
                AssNumeric::Operand x, y;
                if (!readOperand(params[0], false, false, x) || !readOperand(params[1], false, false, y))
                    continue;
                x.value *= sub->m_scalex * 8;
                y.value *= sub->m_scaley * (y.relative ? -8 : 8);
                if (fAnimate || x.relative || y.relative)
                {
                    if (!sub->m_motion) sub->m_motion = std::make_unique<AssMotion::Track>();
                    if (fAnimate)
                    {
                        const int end = m_animEnd || m_exactAnimationEnd ? m_animEnd : m_delay;
                        if (sub->m_motion->intents.size() < AssMotion::Track::Limit &&
                            std::isfinite(m_animAccel) && m_animAccel >= 0)
                            sub->m_motion->intents.push_back({x, y, m_animStart,
                                (std::max)(m_animStart, end), m_animAccel});
                    }
                    else sub->m_motion->Translate(x, y);
                    continue;
                }
                if (!sub->m_effects[EF_MOVE] && (!sub->m_motion || (!sub->m_motion->shifted && !sub->m_motion->hasMove)))
                {
                    auto* e = new Effect;
                    e->param[0] = 0;
                    e->param[1] = e->param[3] = int(x.value);
                    e->param[2] = e->param[4] = int(y.value);
                    e->t[0] = e->t[1] = 0;
                    sub->m_effects[EF_MOVE] = e;
                }
                continue;
            }
            if (sub->m_motion && (sub->m_motion->shifted || sub->m_motion->hasMove))
                continue;
#endif
            if(params.size() == 2 && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = DNew Effect)
                {
#ifdef _VSMOD // patch m005. add some move types
                    e->param[0] = 0; // usual move
                    e->param[1] = e->param[3] = (int)(sub->m_scalex * wcstod(params[0], NULL) * 8);
                    e->param[2] = e->param[4] = (int)(sub->m_scaley * wcstod(params[1], NULL) * 8);
#else
                    e->param[0] = e->param[2] = (int)(sub->m_scalex * wcstod(params[0], NULL) * 8);
                    e->param[1] = e->param[3] = (int)(sub->m_scaley * wcstod(params[1], NULL) * 8);
#endif
                    e->t[0] = e->t[1] = 0;

                    sub->m_effects[EF_MOVE] = e;
                }
            }
#ifdef _VSMOD // patch m002. Z-coord
            else if(params.size() == 3 && !sub->m_effects[EF_MOVE])
            {
                if(Effect* e = DNew Effect)
                {
                    e->param[0] = e->param[2] = (int)(sub->m_scalex * wcstod(params[0], NULL) * 8);
                    e->param[1] = e->param[3] = (int)(sub->m_scaley * wcstod(params[1], NULL) * 8);
                    e->t[0] = e->t[1] = 0;

                    sub->m_effects[EF_MOVE] = e;
                    style.mod_z = wcstod(params[2], NULL) * 80;
                }
            }
#endif
        }
        else if(cmd == L"p")
        {
            int n = wcstol(p, NULL, 10);
            m_nPolygon = (n <= 0 ? 0 : n);
        }
        else if(cmd == L"q")
        {
            int n = wcstol(p, NULL, 10);
            sub->m_wrapStyle = !p.IsEmpty() && (0 <= n && n <= 3)
                               ? n
                               : m_defaultWrapStyle;
        }
#ifdef _VSMOD // patch m003. random text points
        else if(cmd == L"rnds")
        {
            double dst = wcstol(p, NULL, 16);
            double nx = CalcAnimation(dst, style.mod_rand.Seed, fAnimate);
            style.mod_rand.Seed = !p.IsEmpty() ? nx : org.mod_rand.Seed;
        }
        else if(cmd == L"rndx")
        {
            double dst = wcstod(p, NULL) * 8;
            double nx = CalcAnimation(dst, style.mod_rand.X, fAnimate);
            style.mod_rand.X = !p.IsEmpty() ? nx : org.mod_rand.X;
        }
        else if(cmd == L"rndy")
        {
            double dst = wcstod(p, NULL) * 8;
            double nx = CalcAnimation(dst, style.mod_rand.Y, fAnimate);
            style.mod_rand.Y = !p.IsEmpty() ? nx : org.mod_rand.Y;
        }
        else if(cmd == L"rndz")
        {
            double dst = wcstod(p, NULL) * 8;
            double nx = CalcAnimation(dst, style.mod_rand.Z, fAnimate);
            style.mod_rand.Z = !p.IsEmpty() ? nx : org.mod_rand.Z;
        }
        else if(cmd == L"rnd")
        {
            double dst = wcstod(p, NULL) * 8;
            style.mod_rand.X = !p.IsEmpty() ? CalcAnimation(dst, style.mod_rand.X, fAnimate) : org.mod_rand.X;
            style.mod_rand.Y = !p.IsEmpty() ? CalcAnimation(dst, style.mod_rand.Y, fAnimate) : org.mod_rand.Y;
            style.mod_rand.Z = !p.IsEmpty() ? CalcAnimation(dst, style.mod_rand.Z, fAnimate) : org.mod_rand.Z;
        }
#endif
        else if(cmd == L"r")
        {
#ifdef _VSMOD
            if (!fAnimate) sub->m_boxMode = -1;
#endif
#ifdef _VSMOD
            if (slides()) editSlides() = {};
#endif
            STSStyle* val;
            style = (!p.IsEmpty() && m_styles.Lookup(CString(p), val) && val) ? *val : org;
        }
        else if(cmd == L"shad")
        {
            double dst = wcstod(p, NULL);
            double nx = CalcAnimation(dst, style.shadowDepthX, fAnimate);
            style.shadowDepthX = !p.IsEmpty()
                                 ? (nx < 0 ? 0 : nx)
                                     : org.shadowDepthX;
            double ny = CalcAnimation(dst, style.shadowDepthY, fAnimate);
            style.shadowDepthY = !p.IsEmpty()
                                 ? (ny < 0 ? 0 : ny)
                                     : org.shadowDepthY;
        }
        else if(cmd == L"s")
        {
            int n = wcstol(p, NULL, 10);
            style.fStrikeOut = !p.IsEmpty()
                               ? (n == 0 ? false : n == 1 ? true : org.fStrikeOut)
                                   : org.fStrikeOut;
        }
#ifdef _VSMOD
        else if (cmd == L"tan" || cmd == L"ta")
        {
            if (fAnimate || p.GetLength() != 1) continue;
            const wchar_t* number = p;
            const int alignment = ReadDecimalDigit(number);
            if (alignment < 1 || alignment > 9) continue;
            const int horizontal = alignment % 3;
            if (cmd == L"tan")
                sub->m_textAnchor = horizontal;
            else
            {
                const auto inserted = sub->m_lineAlignment.emplace(sub->m_logicalLine,
                    CSubtitle::LineAlignment{horizontal, horizontal});
                inserted.first->second.inherited = horizontal;
            }
        }
#endif
        else if(cmd == L"t") // \t([<t1>,<t2>,][<accel>,]<style modifiers>)
        {
#ifdef _VSMOD
            if (sub->m_measuringLayout) continue;
#endif
            p.Empty();

            m_animStart = m_animEnd = 0;
            m_animAccel = 1;
            m_exactAnimationEnd = false;

            if(params.size() == 1)
            {
                p = params[0];
            }
            else if(params.size() == 2)
            {
                m_animAccel = wcstod(params[0], NULL);
                p = params[1];
            }
            else if(params.size() == 3)
            {
                m_animStart = (int)wcstod(params[0], NULL);
                m_animEnd = (int)wcstod(params[1], NULL);
                p = params[2];
            }
            else if(params.size() == 4)
            {
                m_animStart = wcstol(params[0], NULL, 10);
                m_animEnd = wcstol(params[1], NULL, 10);
                m_animAccel = wcstod(params[2], NULL);
                p = params[3];
            }

#ifdef _VSMOD
            bool valid = true;
            if (params.size() >= 2 && params.size() <= 4)
                for (size_t i = 0; i + 1 < params.size(); ++i)
                {
                    if (!ExtendedNumber(params[i])) continue;
                    const bool time = params.size() >= 3 && i < 2;
                    AssNumeric::Operand operand;
                    if (!readOperand(params[i], false, time, operand) || operand.relative ||
                        (time && (operand.value < INT_MIN || operand.value > INT_MAX)) ||
                        (!time && operand.value < 0))
                    {
                        valid = false;
                        break;
                    }
                    if (!time) m_animAccel = operand.value;
                    else if (i == 0) m_animStart = int(operand.value);
                    else
                    {
                        m_animEnd = int(operand.value);
                        m_exactAnimationEnd = true;
                    }
                }
            if (!valid) continue;
            ParseSSATag(sub, p, style, org, true, slides());
#else
            ParseSSATag(sub, p, style, org, true);
#endif

            sub->m_fAnimated = true;
        }
        else if(cmd == L"u")
        {
            int n = wcstol(p, NULL, 10);
            style.fUnderline = !p.IsEmpty()
                               ? (n == 0 ? false : n == 1 ? true : org.fUnderline)
                                   : org.fUnderline;
        }
        else if(cmd == L"xbord")
        {
            double dst = wcstod(p, NULL);
            double nx = CalcAnimation(dst, style.outlineWidthX, fAnimate);
            style.outlineWidthX = !p.IsEmpty()
                                  ? (nx < 0 ? 0 : nx)
                                      : org.outlineWidthX;
        }
        else if(cmd == L"xshad")
        {
            double dst = wcstod(p, NULL);
            double nx = CalcAnimation(dst, style.shadowDepthX, fAnimate);
            style.shadowDepthX = !p.IsEmpty()
                                 ? nx
                                 : org.shadowDepthX;
        }
        else if(cmd == L"ybord")
        {
            double dst = wcstod(p, NULL);
            double ny = CalcAnimation(dst, style.outlineWidthY, fAnimate);
            style.outlineWidthY = !p.IsEmpty()
                                  ? (ny < 0 ? 0 : ny)
                                      : org.outlineWidthY;
        }
        else if(cmd == L"yshad")
        {
            double dst = wcstod(p, NULL);
            double ny = CalcAnimation(dst, style.shadowDepthY, fAnimate);
            style.shadowDepthY = !p.IsEmpty()
                                 ? ny
                                 : org.shadowDepthY;
        }
        else if (cmd == L"xblur")
        {
        double dst = wcstod(p, NULL);
        double nx = CalcAnimation(dst, style.fGaussianBlurX, fAnimate);
        style.fGaussianBlurX = !p.IsEmpty()
            ? (nx < 0 ? 0 : nx)
            : org.fGaussianBlurX;
        }
        else if (cmd == L"yblur")
        {
        double dst = wcstod(p, NULL);
        double nx = CalcAnimation(dst, style.fGaussianBlurY, fAnimate);
        style.fGaussianBlurY = !p.IsEmpty()
            ? (nx < 0 ? 0 : nx)
            : org.fGaussianBlurY;
        }
#ifdef _VSMOD // patch m002. Z-coord
        else if(cmd == L"z")
        {
            double dst = wcstod(p, NULL) * 80;
            double nx = CalcAnimation(dst, style.mod_z, fAnimate);
            style.mod_z = !p.IsEmpty() ? nx : org.mod_z;
        }
        else if (cmd == L"ortho")// vpatch v001. Orthogonal projection
        {
            int n = wcstol(p, NULL, 10);
            style.mod_ortho = !p.IsEmpty()
                ? (n == 0 ? false : n == 1 ? true : org.mod_ortho)
                : org.mod_ortho;
        }
        else if (cmd == L"blend") // vpatch v003. blending mode
        {
            MOD_BLEND mode = org.mod_blendMode;
            if (p.GetLength()>2) {
                if (p == L"over") mode = BLEND_OVERLAY;
                else if (p == L"add") mode = BLEND_ADD;
                else if (p == L"sub") mode = BLEND_SUBSTRACT;
                else if (p == L"mult") mode = BLEND_MULTIPLY;
                else if (p == L"scr") mode = BLEND_SCREEN;
                else if (p == L"diff") mode = BLEND_DIFFERENCE;
                else if (p == L"rsub") mode = BLEND_SUBSTRACT_REVERSE;
                else if (p == L"isub") mode = BLEND_SUBSTRACT_INVERSE;
            }
            else {
                mode = (MOD_BLEND)wcstol(p, NULL, 10);
            }
            style.mod_blendMode = !p.IsEmpty()
                ? mode
                : org.mod_blendMode;
        }
#endif
    }

    return true; // Preserve comments inside override blocks.
}

bool CRenderedTextSubtitle::ParseHtmlTag(CSubtitle* sub, CStringW str, STSStyle& style, STSStyle& org)
{
    PROFILE_RENDER_STAGE(Overrides);
    if(str.Find(L"!--") == 0)
        return(true);

    bool fClosing = str[0] == '/';
    str.Trim(L" /");

    int i = str.Find(' ');
    if(i < 0) i = str.GetLength();

    CStringW tag = str.Left(i).MakeLower();
    str = str.Mid(i).Trim();

    CAtlArray<CStringW> attribs, params;
    while((i = str.Find('=')) > 0)
    {
        attribs.Add(str.Left(i).Trim().MakeLower());
        str = str.Mid(i + 1);
        for(i = 0; _istspace(str[i]); i++);
        str = str.Mid(i);
        if(str[0] == '\"')
        {
            str = str.Mid(1);
            i = str.Find('\"');
        }
        else i = str.Find(' ');
        if(i < 0) i = str.GetLength();
        params.Add(str.Left(i).Trim().MakeLower());
        str = str.Mid(i + 1);
    }

    if(tag == L"text")
        ;
    else if(tag == L"b" || tag == L"strong")
        style.fontWeight = !fClosing ? FW_BOLD : org.fontWeight;
    else if(tag == L"i" || tag == L"em")
        style.fItalic = !fClosing ? true : org.fItalic;
    else if(tag == L"u")
        style.fUnderline = !fClosing ? true : org.fUnderline;
    else if(tag == L"s" || tag == L"strike" || tag == L"del")
        style.fStrikeOut = !fClosing ? true : org.fStrikeOut;
    else if(tag == L"font")
    {
        if(!fClosing)
        {
            for(i = 0; i < attribs.GetCount(); i++)
            {
                if(params[i].IsEmpty()) continue;

                int nColor = -1;

                if(attribs[i] == L"face")
                {
                    style.fontName = params[i];
                }
                else if(attribs[i] == L"size")
                {
                    if(params[i][0] == '+')
                        style.fontSize += wcstol(params[i], NULL, 10);
                    else if(params[i][0] == '-')
                        style.fontSize -= wcstol(params[i], NULL, 10);
                    else
                        style.fontSize = wcstol(params[i], NULL, 10);
                }
                else if(attribs[i] == L"color")
                {
                    nColor = 0;
                }
                else if(attribs[i] == L"outline-color")
                {
                    nColor = 2;
                }
                else if(attribs[i] == L"outline-level")
                {
                    style.outlineWidthX = style.outlineWidthY = wcstol(params[i], NULL, 10);
                }
                else if(attribs[i] == L"shadow-color")
                {
                    nColor = 3;
                }
                else if(attribs[i] == L"shadow-level")
                {
                    style.shadowDepthX = style.shadowDepthY = wcstol(params[i], NULL, 10);
                }

                if(nColor >= 0 && nColor < 4)
                {
                    CString key = WToT(params[i]).TrimLeft('#');
                    DWORD val;
                    if(g_colors.Lookup(key, val))
                        style.colors[nColor] = val;
                    else if((style.colors[nColor] = _tcstol(key, NULL, 16)) == 0)
                        style.colors[nColor] = 0x00ffffff;  // default is white
                    style.colors[nColor] = ((style.colors[nColor] >> 16) & 0xff) | ((style.colors[nColor] & 0xff) << 16) | (style.colors[nColor] & 0x00ff00);
                }
            }
        }
        else
        {
            style.fontName = org.fontName;
            style.fontSize = org.fontSize;
            memcpy(style.colors, org.colors, sizeof(style.colors));
        }
    }
    else if(tag == L"k" && attribs.GetCount() == 1 && attribs[0] == L"t")
    {
        m_ktype = 1;
        m_kstart = m_kend;
        m_kend += wcstol(params[0], NULL, 10);
    }
    else
        return(false);

    return(true);
}

double CRenderedTextSubtitle::CalcAnimation(double dst, double src, bool fAnimate)
{
    PROFILE_RENDER_STAGE(Animation);
    int s = m_animStart ? m_animStart : 0;
    int e = m_animEnd || m_exactAnimationEnd ? m_animEnd : m_delay;

    if(fAnimate && fabs(dst - src) >= 0.0001)
    {
        if(m_time < s) dst = src;
        else if(s <= m_time && m_time < e)
        {
            const double fraction = (double(m_time) - s) / (double(e) - s);
            const double t = m_animAccel == 1.0 ? fraction : pow(fraction, m_animAccel);
            dst = (1 - t) * src + t * dst;
        }
//		else dst = dst;
    }

    return(dst);
}

CSubtitle* CRenderedTextSubtitle::GetSubtitle(int entry, bool measuringLayout)
{
    PROFILE_RENDER_STAGE(Subtitle);
    CSubtitle* previous = nullptr;
    if (!measuringLayout && m_subtitleCache.Lookup(entry, previous) && !previous->m_fAnimated)
        return previous;

    std::unique_ptr<CSubtitle> owned(DNew CSubtitle());
    CSubtitle* sub = owned.get();
    if(!sub) return(NULL);

#if defined (_VSMOD) && defined(_LUA)
    sub->L = L;
    sub->LuaLog = LuaLog;
#endif

    CStringW str = GetStrW(entry, true);
#ifdef _VSMOD
    sub->m_measuringLayout = measuringLayout;
    if (!measuringLayout && str.Find(L'$') >= 0 && NeedsLayoutVariables(str)
#ifdef _LUA
        && !hasExternalLua
#endif
        )
    {
        if (!m_overrideTags)
            m_overrideTags = std::make_unique<OverrideTagCache>();
        auto& cache = m_overrideTags->layout;
        const auto existing = cache.find(entry);
        if (!m_doOverrideStyle && existing != cache.end())
            sub->m_layoutVariables = std::make_unique<AssNumeric::Context>(existing->second);
        else
        {
            std::unique_ptr<CSubtitle> baseline(GetSubtitle(entry, true));
            if (baseline && !baseline->IsEmpty() && baseline->m_scalex > 0 && baseline->m_scaley > 0)
            {
                CRect rect = baseline->m_rect;
                if (auto* motion = baseline->m_effects[EF_MOVE])
                    if (!motion->param[0])
                    {
                        const auto alignment = baseline->m_scrAlignment;
                        const auto x = alignment % 3 == 1 ? rect.left :
                            alignment % 3 == 2 ? rect.CenterPoint().x : rect.right;
                        const auto y = alignment <= 3 ? rect.bottom :
                            alignment <= 6 ? rect.CenterPoint().y : rect.top;
                        rect.OffsetRect(motion->param[1] - x, motion->param[2] - y);
                    }
                AssNumeric::Context bounds{};
                const int baseX = baseline->m_scrAlignment % 3 == 1 ? rect.left :
                    baseline->m_scrAlignment % 3 == 2 ? rect.CenterPoint().x : rect.right;
                int left = INT_MAX, right = INT_MIN;
                POSITION lines = baseline->GetHeadPosition();
                while (lines)
                {
                    const auto* line = baseline->GetNext(lines);
                    const int x = baseline->LineLeft(*line, baseX);
                    left = (std::min)(left, x);
                    right = (std::max)(right, x + line->m_width);
                }
                bounds[AssNumeric::Left] = left / (baseline->m_scalex * 8);
                bounds[AssNumeric::Top] = rect.top / (baseline->m_scaley * 8);
                bounds[AssNumeric::Right] = right / (baseline->m_scalex * 8);
                bounds[AssNumeric::Bottom] = rect.bottom / (baseline->m_scaley * 8);
                bounds[AssNumeric::Width] = bounds[AssNumeric::Right] - bounds[AssNumeric::Left];
                bounds[AssNumeric::Height] = bounds[AssNumeric::Bottom] - bounds[AssNumeric::Top];
                bounds[AssNumeric::Center] = (bounds[AssNumeric::Left] + bounds[AssNumeric::Right]) / 2;
                bounds[AssNumeric::Middle] = (bounds[AssNumeric::Top] + bounds[AssNumeric::Bottom]) / 2;
                sub->m_layoutVariables = std::make_unique<AssNumeric::Context>(bounds);
                if (!m_doOverrideStyle)
                {
                    if (cache.size() >= 256) cache.clear();
                    cache.emplace(entry, bounds);
                }
            }
        }
    }
#endif

    STSStyle stss, orgstss;
    if(m_doOverrideStyle && m_pStyleOverride != NULL)
    {
        // this RTS has been signaled to ignore embedded styles, use the built-in one
        stss = *m_pStyleOverride;
    }
    else
    {
        // find the appropriate embedded style
        GetStyle(entry, stss);
    }
    if(m_ePARCompensationType == EPCTUpscale)
    {
        if(stss.fontScaleX / stss.fontScaleY == 1.0 && m_dPARCompensation != 1.0)
        {
            if(m_dPARCompensation < 1.0)
                stss.fontScaleY /= m_dPARCompensation;
            else
                stss.fontScaleX *= m_dPARCompensation;
        }
    }
    else if(m_ePARCompensationType == EPCTDownscale)
    {
        if(stss.fontScaleX / stss.fontScaleY == 1.0 && m_dPARCompensation != 1.0)
        {
            if(m_dPARCompensation < 1.0)
                stss.fontScaleX *= m_dPARCompensation;
            else
                stss.fontScaleY /= m_dPARCompensation;
        }
    }
    else if(m_ePARCompensationType == EPCTAccurateSize)
    {
        if(stss.fontScaleX / stss.fontScaleY == 1.0 && m_dPARCompensation != 1.0)
        {
            stss.fontScaleX *= m_dPARCompensation;
        }
    }

    orgstss = stss;

    sub->m_clip.SetRect(0, 0, m_size.cx >> 3, m_size.cy >> 3);
    sub->m_scrAlignment = -stss.scrAlignment;
    sub->m_wrapStyle = m_defaultWrapStyle;
    sub->m_fAnimated = false;
    sub->m_relativeTo = stss.relativeTo;
    // this whole conditional is a work-around for what happens in STS.cpp:
    // in CSimpleTextSubtitle::Open, we have m_dstScreenSize = CSize(384, 288)
    // now, files containing embedded subtitles (and with styles) set m_dstScreenSize to a correct value
    // but where no style is given, those defaults are taken - 384, 288
    if(m_doOverrideStyle && m_pStyleOverride != NULL)
    {
        // so mind the default values, stated here to increase comprehension
        sub->m_scalex = (stss.relativeTo == 1 ? m_vidrect.Width() : m_size.cx) / (384 * 8);
        sub->m_scaley = (stss.relativeTo == 1 ? m_vidrect.Height() : m_size.cy) / (288 * 8);
    }
    else
    {
        sub->m_scalex = m_dstScreenSize.cx > 0 ? 1.0 * (stss.relativeTo == 1 ? m_vidrect.Width() : m_size.cx) / (m_dstScreenSize.cx * 8) : 1.0;
        sub->m_scaley = m_dstScreenSize.cy > 0 ? 1.0 * (stss.relativeTo == 1 ? m_vidrect.Height() : m_size.cy) / (m_dstScreenSize.cy * 8) : 1.0;
    }

#if defined(_VSMOD) && defined(_LUA)
    m_entry = entry;
#endif
    m_animStart = m_animEnd = 0;
    m_animAccel = 1;
    m_exactAnimationEnd = false;
    m_ktype = m_kstart = m_kend = 0;
    m_nPolygon = 0;
    m_polygonBaselineOffset = 0;
#ifdef _VSMOD // patch m004. gradient colors
    // allow init gradient without \$vc \$va
    for (int i = 0; i < 4; i++)
    {
        COLORREF revClr = revcolor(stss.colors[i]);

        for (int j = 0; j < 4; j++)
        {
            stss.mod_grad.alpha[i][j] = stss.alpha[i];
            stss.mod_grad.color[i][j] = revClr;
        }
    }
#endif
    ParseEffect(sub, GetAt(entry).effect);

    while(!str.IsEmpty())
    {
        bool fParsed = false;

        int i;

        if(str[0] == '{' && (i = str.Find(L'}')) > 0)
        {
            if(fParsed = ParseSSATag(sub, str.Mid(1, i - 1), stss, orgstss))
                str = str.Mid(i + 1);
        }
        else if(str[0] == '<' && (i = str.Find(L'>')) > 0)
        {
            if(fParsed = ParseHtmlTag(sub, str.Mid(1, i - 1), stss, orgstss))
                str = str.Mid(i + 1);
        }

        if(fParsed)
        {
            i = str.FindOneOf(L"{<");
            if(i < 0) i = str.GetLength();
            if(i == 0) continue;
        }
        else
        {
            i = str.Mid(1).FindOneOf(L"{<");
            if(i < 0) i = str.GetLength() - 1;
            i++;
        }

        STSStyle tmp;

        tmp = stss;

        tmp.fontSize = sub->m_scaley * tmp.fontSize * 64;
        tmp.fontSpacing = sub->m_scalex * tmp.fontSpacing * 64;
        tmp.outlineWidthX *= (m_fScaledBAS ? sub->m_scalex : 1) * 8;
        tmp.outlineWidthY *= (m_fScaledBAS ? sub->m_scaley : 1) * 8;
        tmp.shadowDepthX *= (m_fScaledBAS ? sub->m_scalex : 1) * 8;
        tmp.shadowDepthY *= (m_fScaledBAS ? sub->m_scaley : 1) * 8;
#ifdef _VSMOD
        if (tmp.softScale != 100 || tmp.objectScale != 100)
        {
            const double object = tmp.objectScale / 100;
            const double glyph = object * tmp.softScale / 100;
            tmp.fontScaleX *= glyph;
            tmp.fontScaleY *= glyph;
            tmp.outlineWidthX *= object;
            tmp.outlineWidthY *= object;
            tmp.shadowDepthX *= object;
            tmp.shadowDepthY *= object;
            tmp.fGaussianBlurX *= object;
            tmp.fGaussianBlurY *= object;
            tmp.mod_verticalSpace *= object;
            tmp.mod_horizontalSpace *= object;
            tmp.mod_rand.X *= object;
            tmp.mod_rand.Y *= object;
            tmp.mod_rand.Z *= object;
            if (tmp.mod_jitter.enabled)
            {
                auto& offset = tmp.mod_jitter.offset;
                offset.SetRect(int(offset.left * object), int(offset.top * object),
                    int(offset.right * object), int(offset.bottom * object));
            }
        }
#endif

        if(m_nPolygon)
        {
            ParsePolygon(sub, str.Left(i), tmp);
        }
        else
        {
            ParseString(sub, str.Left(i), tmp);
        }

        str = str.Mid(i);
    }

    // just a "work-around" solution... in most cases nobody will want to use \org together with moving but without rotating the subs
    if(sub->m_effects[EF_ORG] && (sub->m_effects[EF_MOVE] || sub->m_effects[EF_BANNER] || sub->m_effects[EF_SCROLL]))
        sub->m_fAnimated = true;

    sub->m_scrAlignment = abs(sub->m_scrAlignment);

    STSEntry stse = GetAt(entry);
    CRect marginRect = stse.marginRect;
    if(marginRect.left == 0) marginRect.left = orgstss.marginRect.left;
    if(marginRect.top == 0) marginRect.top = orgstss.marginRect.top;
    if(marginRect.right == 0) marginRect.right = orgstss.marginRect.right;
    if(marginRect.bottom == 0) marginRect.bottom = orgstss.marginRect.bottom;
    marginRect.left = (int)(sub->m_scalex * marginRect.left * 8);
    marginRect.top = (int)(sub->m_scaley * marginRect.top * 8);
    marginRect.right = (int)(sub->m_scalex * marginRect.right * 8);
    marginRect.bottom = (int)(sub->m_scaley * marginRect.bottom * 8);

    if(stss.relativeTo == 1)
    {
        marginRect.left += m_vidrect.left;
        marginRect.top += m_vidrect.top;
        marginRect.right += m_size.cx - m_vidrect.right;
        marginRect.bottom += m_size.cy - m_vidrect.bottom;
    }

    if (!measuringLayout)
        sub->CreateClippers(m_size);

#ifdef _VSMOD
    const int borderStyle = sub->m_borderStyleOverride < 0 ? stss.borderStyle : sub->m_borderStyleOverride;
    const bool hasBox = sub->m_boxMode < 0 ? borderStyle == 2 : sub->m_boxMode != 0;
    if (hasBox || sub->m_borderStyleOverride >= 0 || sub->m_boxMode == 0)
    {
        POSITION words = sub->m_words.GetHeadPosition();
        while (words)
        {
            auto& style = sub->m_words.GetNext(words)->m_style;
            if (hasBox)
                style.borderStyle = 2;
            else if (sub->m_borderStyleOverride >= 0)
                style.borderStyle = sub->m_borderStyleOverride == 2 ? 0 : sub->m_borderStyleOverride;
            else if (style.borderStyle == 2)
                style.borderStyle = 0;
        }
    }
#endif
    sub->MakeLines(m_size, marginRect);
    if (measuringLayout)
        return owned.release();
#ifdef _VSMOD
    if (hasBox)
    {
        if (sub->m_revealKaraoke)
            sub->m_fAnimated = true;
        sub->CreateEventBox(borderStyle, m_time, m_fScaledBAS);
    }
#endif

    m_subtitleCache[entry] = sub;
    delete previous;
    return owned.release();
}

//

STDMETHODIMP CRenderedTextSubtitle::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    CheckPointer(ppv, E_POINTER);
    *ppv = NULL;

    return
        QI(IPersist)
        QI(ISubStream)
        QI(ISubPicProvider)
        __super::NonDelegatingQueryInterface(riid, ppv);
}

// ISubPicProvider

STDMETHODIMP_(POSITION) CRenderedTextSubtitle::GetStartPosition(REFERENCE_TIME rt, double fps)
{
    int iSegment = -1;
    SearchSubs((int)(rt / 10000), fps, &iSegment, NULL);

    if(iSegment < 0) iSegment = 0;

    return(GetNext((POSITION)iSegment));
}

STDMETHODIMP_(POSITION) CRenderedTextSubtitle::GetNext(POSITION pos)
{
    int iSegment = (int)pos;

    const STSSegment* stss;
    while((stss = GetSegment(iSegment)) && stss->subs.GetCount() == 0)
        iSegment++;

    return(stss ? (POSITION)(iSegment + 1) : NULL);
}

STDMETHODIMP_(REFERENCE_TIME) CRenderedTextSubtitle::GetStart(POSITION pos, double fps)
{
    return(10000i64 * TranslateSegmentStart((int)pos - 1, fps));
}

STDMETHODIMP_(REFERENCE_TIME) CRenderedTextSubtitle::GetStop(POSITION pos, double fps)
{
    return(10000i64 * TranslateSegmentEnd((int)pos - 1, fps));
}

STDMETHODIMP_(bool) CRenderedTextSubtitle::IsAnimated(POSITION pos)
{
    return(true);
}

struct LSub
{
    int idx, layer, readorder;
};

static int lscomp(const void* ls1, const void* ls2)
{
    const auto& a = *static_cast<const LSub*>(ls1);
    const auto& b = *static_cast<const LSub*>(ls2);
    if (a.layer != b.layer)
        return a.layer < b.layer ? -1 : 1;
    return (a.readorder > b.readorder) - (a.readorder < b.readorder);
}

struct PreparedWord
{
    CWord* word;
    CPoint position;
    int layer;
};

static std::function<void()> PrepareEventGeometry(CSubtitle* subtitle, CPoint origin,
    CPoint transformOrigin, CRect rectangle, int time, REFERENCE_TIME timestamp)
{
    std::vector<PreparedWord> words;
    int y = rectangle.top;
    POSITION linePosition = subtitle->GetHeadPosition();
    while (linePosition)
    {
        CLine* line = subtitle->GetNext(linePosition);
        int x = subtitle->LineLeft(*line, origin.x);
        POSITION wordPosition = line->GetHeadPosition();
        while (wordPosition)
        {
            CWord* word = line->GetNext(wordPosition);
            if (word->m_fLineBreak)
                break;
            const auto& style = word->m_style;
            CPoint position(x, y + line->m_ascent - word->m_ascent);
            int layer = 1;
#ifdef _VSMOD
            position.x -= style.mod_horizontalSpace;
            position.y -= style.mod_verticalSpace;
            position += word->m_style.mod_jitter.getOffset(timestamp);
#endif
            // Match the first Paint invocation, because its origin determines
            // the path transform and its subpixel phase determines rasterization.
            if (style.shadowDepthX != 0 || style.shadowDepthY != 0)
            {
                position.x += int(style.shadowDepthX + 0.5);
                position.y += int(style.shadowDepthY + 0.5);
                layer = 4;
            }
            else if (style.outlineWidthX + style.outlineWidthY > 0 &&
                !(word->m_ktype == 2 && time < word->m_kstart))
                layer = 3;
            if (word->NeedsPreparation(position))
                words.push_back({word, position, layer});
            x += word->m_width;
        }
        y += line->m_ascent + line->m_descent;
    }
    if (words.empty())
        return {};
    return [words = std::move(words), transformOrigin] {
        for (const auto& prepared : words)
        {
#if defined(_VSMOD) && defined(_LUA)
            prepared.word->Paint(prepared.position, transformOrigin, prepared.layer);
#else
            prepared.word->Paint(prepared.position, transformOrigin);
#endif
        }
    };
}

struct FramePaint
{
    std::function<CRect()> paint;
    int entry;
};

#if defined(_VSMOD) && defined(_LUA)
static bool HasLuaPixelHandler(CSubtitle& subtitle)
{
    POSITION line = subtitle.GetHeadPosition();
    while (line)
    {
        auto* words = subtitle.GetNext(line);
        POSITION word = words->GetHeadPosition();
        while (word)
            if (!words->GetNext(word)->LuaRendererHandler.IsEmpty())
                return true;
    }
    return false;
}
#endif

static CRect PaintFrameBands(EventWorkerPool& pool, const std::vector<FramePaint>& events, int height)
{
    std::vector<OverlayDrawList> overlays(events.size());
    std::vector<std::function<void()>> work;
    work.reserve(events.size());
    for (size_t i = 0; i < events.size(); ++i)
    {
        work.emplace_back([&, i] {
            const OverlayCaptureScope capture(overlays[i]);
            events[i].paint();
        });
    }
    pool.Run(work, EventWorkerPool::Batch::Capture);

    struct Band
    {
        int top = 0, bottom = 0;
        uint64_t pixels = 0;
        std::vector<const OverlayDraw*> draws;
#ifdef _VSMOD
        bool positionedAlpha = false;
#endif
    };
    // Bound the number of jobs for large surfaces. Smaller bands distribute
    // overlapping effects independently of how many distinct events draw them.
    const int bandHeight = (std::max)(32, (height - 1) / 48 + 1);
    std::vector<Band> bands;
    CRect bounds(0, 0, 0, 0);
    uint64_t totalPixels = 0;
    {
        PROFILE_RENDER_STAGE(BandPlan);
        bands.resize((height - 1) / bandHeight + 1);
        for (const auto& event : overlays)
        {
            for (const auto& draw : event.draws)
            {
                bounds |= draw.bounds;
                totalPixels += uint64_t(draw.bounds.Width()) * draw.bounds.Height();
                for (int i = draw.bounds.top / bandHeight; i <= (draw.bounds.bottom - 1) / bandHeight; ++i)
                {
                    auto& band = bands[i];
                    band.top = i * bandHeight;
                    band.bottom = band.top + (std::min)(bandHeight, height - band.top);
                    const int rows = (std::min)(int(draw.bounds.bottom), band.bottom) -
                        (std::max)(int(draw.bounds.top), band.top);
                    band.pixels += uint64_t(rows) * draw.bounds.Width();
                    band.draws.push_back(&draw);
#ifdef _VSMOD
                    if (draw.gradient.positionedOpacity) band.positionedAlpha = true;
#endif
                }
            }
        }
    }

    constexpr uint64_t pixelsPerWorker = 64 * 1024;
    if (totalPixels < 2 * pixelsPerWorker)
    {
        for (const auto& event : overlays)
            event.Blend();
        return bounds;
    }

    // Each band visits commands in the original event and paint order. Only
    // the scheduling order of whole, disjoint bands may change.
    {
        PROFILE_RENDER_STAGE(BandPlan);
        std::sort(bands.begin(), bands.end(), [](const Band& a, const Band& b) { return a.pixels > b.pixels; });
        work.clear();
        for (const auto& band : bands)
        {
            if (band.draws.empty())
                break;
            work.emplace_back([job = &band] {
#ifdef _VSMOD
                if (job->positionedAlpha)
                {
                    BlendCapturedBands(job->draws, job->top, job->bottom);
                    return;
                }
#endif
                for (const auto* draw : job->draws)
                    draw->Blend(job->top, job->bottom);
            });
        }
    }
    pool.Run(work, EventWorkerPool::Batch::Blend,
        unsigned((std::min)(totalPixels / pixelsPerWorker, uint64_t(bands.size()))));
    return bounds;
}

STDMETHODIMP CRenderedTextSubtitle::Render(SubPicDesc& spd, REFERENCE_TIME rt, double fps, RECT& bbox)
try
{
    PROFILE_RENDER_STAGE(Frame);
    CRect bbox2(0, 0, 0, 0);

    if(m_size != CSize(spd.w * 8, spd.h * 8) || m_vidrect != CRect(spd.vidrect.left * 8, spd.vidrect.top * 8, spd.vidrect.right * 8, spd.vidrect.bottom * 8))
        Init(CSize(spd.w, spd.h), spd.vidrect);

    int t = (int)(rt / 10000);

    int segment;
//    TRACE(_T("search started: %d"), t);
    const STSSegment* stss;
    {
        PROFILE_RENDER_STAGE(ActiveEvents);
        stss = SearchSubs(t, fps, &segment);
    }
//    TRACE(_T("search complete: %d"), t);
    if(!stss) return S_FALSE;

    if (!m_textGeometry)
        m_textGeometry = std::make_unique<TextGeometryCache>();
    if (!m_textDC)
        m_textDC = std::make_unique<TextDeviceContext>();
    const TextDeviceScope deviceScope(m_textDC->dc);
    m_textGeometry->StartFrame();
    const TextGeometryScope geometryScope(m_textGeometry.get());

    // clear any cached subs that current position is not in its bounds
    {
        PROFILE_RENDER_STAGE(Cache);
        POSITION pos = m_subtitleCache.GetStartPosition();
        while(pos)
        {
            int key;
            CSubtitle* value;
            m_subtitleCache.GetNextAssoc(pos, key, value);

            STSEntry& stse = GetAt(key);
            if (stse.end <= (t) || stse.start > (t))
            {
                delete value;
                m_subtitleCache.RemoveKey(key);
                pos = m_subtitleCache.GetStartPosition();
            }
        }
    }

    m_sla.AdvanceToSegment(segment, stss->subs);

    CAtlArray<LSub> subs;

    {
        PROFILE_RENDER_STAGE(ActiveEvents);
        for(ptrdiff_t i = 0, j = stss->subs.GetCount(); i < j; i++)
        {
            const int index = stss->subs[i];
            const auto& entry = GetAt(index);
            subs.Add({index, entry.layer, entry.readorder});
        }
        qsort(subs.GetData(), subs.GetCount(), sizeof(LSub), lscomp);
    }

    if (!m_eventWorkers)
        m_eventWorkers = std::make_unique<EventWorkerPool>();
    bool parallel = m_eventWorkers->CanParallelize(subs.GetCount());
#if defined(_VSMOD) && defined(_LUA)
    // User scripts may observe callback order and shared Lua globals.
    parallel = parallel && !hasExternalLua;
    if (m_doOverrideStyle && m_pStyleOverride)
    {
        const auto& style = *m_pStyleOverride;
        parallel = parallel && style.LuaCustomTransformHandler.IsEmpty() &&
            style.LuaBeforeTransformHandler.IsEmpty() && style.LuaAfterTransformHandler.IsEmpty() &&
            style.LuaRendererHandler.IsEmpty();
    }
#endif
    const auto rowBytes = int64_t(spd.w) * int64_t(sizeof(DWORD));
    const bool captureOverlays = parallel && subs.GetCount() >= 8 && spd.bpp == 32 &&
        spd.w > 0 && spd.h > 0 && spd.bits &&
        (int64_t(spd.pitch) >= rowBytes || int64_t(spd.pitch) <= -rowBytes);
    std::vector<std::function<void()>> preparations;
    std::vector<FramePaint> compositions;
    if (parallel)
    {
        if (!captureOverlays)
            preparations.reserve(subs.GetCount());
        compositions.reserve(subs.GetCount());
    }
    const auto clearLuaData = [this](int entry) {
#if defined (_VSMOD) && defined(_LUA)
        CStringA index;
        index.Format("sub_%d", entry);
        lua_pushnil(L);
        lua_setglobal(L, index);
#endif
    };
    const auto finishEvents = [&](bool captureOverlays) {
        if (captureOverlays)
        {
            bbox2 |= PaintFrameBands(*m_eventWorkers, compositions, spd.h);
            for (const auto& event : compositions)
                clearLuaData(event.entry);
        }
        else
        {
            m_eventWorkers->Run(preparations);
            for (const auto& event : compositions)
            {
                bbox2 |= event.paint();
                clearLuaData(event.entry);
            }
        }
        preparations.clear();
        compositions.clear();
    };

    for(ptrdiff_t i = 0, j = subs.GetCount(); i < j; i++)
    {
        int entry = subs[i].idx;

        STSEntry stse = GetAt(entry);

        {
            int start = TranslateStart(entry, fps);
            m_time = t - start;
            m_delay = TranslateEnd(entry, fps) - start;

#if defined(_VSMOD) && defined(_LUA)
            m_start = start;
            m_end = start + m_delay;
#endif
        }

        CSubtitle* s = GetSubtitle(entry);
        if(!s) continue;
#if defined(_VSMOD) && defined(_LUA)
        if (parallel && hasExternalLua)
        {
            finishEvents(false);
            parallel = false;
        }
#endif

        CRect clipRect = s->m_clip;
        CRect r = s->m_rect;
        CSize spaceNeeded = r.Size();

        // apply the effects

        bool fPosOverride = false, fOrgOverride = false;

        int alpha = 0x00;
#ifdef _VSMOD
        KfxPaint::Fade paintFade;
#endif

        CPoint org2;

        BYTE* pAlphaMask = s->m_pClipper ? s->m_pClipper->m_pAlphaMask : NULL;

#ifdef _VSMOD // patch m006. moveable vector clip
        const CPoint clipOffset(int(s->m_clipOffsetX * s->m_scalex), int(s->m_clipOffsetY * s->m_scaley));
        if (s->m_hasRectClip)
            clipRect.OffsetRect(clipOffset);
        MOD_MOVEVC mod_vc;
        mod_vc.spd = CSize(spd.w, spd.h);
        //mod_vc.alphamask = pAlphaMask;
        mod_vc.size = s->m_pClipper ? s->m_pClipper->m_size : CSize(0, 0);
#endif

        for(int k = 0; k < EF_NUMBEROFEFFECTS; k++)
        {
            if(!s->m_effects[k]) continue;

            PROFILE_RENDER_STAGE(Animation);
            switch(k)
            {
            case EF_MOVE: // {\move(x1=param[0], y1=param[1], x2=param[2], y2=param[3], t1=t[0], t2=t[1])}
            {
                CPoint p;
#ifdef _VSMOD // patch m005. add some move types
                if(s->m_effects[k]->param[0] == 0)
                {
                    CPoint p1(s->m_effects[k]->param[1], s->m_effects[k]->param[2]);
                    CPoint p2(s->m_effects[k]->param[3], s->m_effects[k]->param[4]);
#else
                CPoint p1(s->m_effects[k]->param[0], s->m_effects[k]->param[1]);
                CPoint p2(s->m_effects[k]->param[2], s->m_effects[k]->param[3]);
#endif
                    int t1 = s->m_effects[k]->t[0];
                    int t2 = s->m_effects[k]->t[1];

                    if(t2 < t1)
                    {
                        int t = t1;
                        t1 = t2;
                        t2 = t;
                    }

                    if(t1 <= 0 && t2 <= 0)
                    {
                        t1 = 0;
                        t2 = m_delay;
                    }

                    if(m_time <= t1) p = p1;
                    else if(p1 == p2) p = p1;
                    else if(t1 < m_time && m_time < t2)
                    {
                        double t = 1.0 * (m_time - t1) / (t2 - t1);
                        p.x = (int)((1 - t) * p1.x + t * p2.x);
                        p.y = (int)((1 - t) * p1.y + t * p2.y);
                    }
                    else p = p2;
#ifdef _VSMOD // patch m005. add some move types
                }
                else if(s->m_effects[k]->param[0] == 1) // radial move
                {
                    CPoint p1(s->m_effects[k]->param[1], s->m_effects[k]->param[2]); // p0
                    CPoint p2(s->m_effects[k]->param[3], s->m_effects[k]->param[4]); // p1
                    CPoint r0(s->m_effects[k]->param[7], s->m_effects[k]->param[8]);
                    double alp1 = s->m_effects[k]->param[5] * PI / 1800000; // alp1
                    double alp2 = s->m_effects[k]->param[6] * PI / 1800000; // alp2
                    int t1 = s->m_effects[k]->t[0];
                    int t2 = s->m_effects[k]->t[1];

                    CPoint pr1(p1.x + cos(alp1) * r0.x, p1.y - sin(alp1) * r0.x);
                    CPoint pr2(p2.x + cos(alp2) * r0.y, p2.y - sin(alp2) * r0.y);

                    if(t2 < t1)
                    {
                        int t = t1;
                        t1 = t2;
                        t2 = t;
                    }

                    if(t1 <= 0 && t2 <= 0)
                    {
                        t1 = 0;
                        t2 = m_delay;
                    }

                    if(m_time <= t1) p = pr1;
                    //else if (p1 == p2) p = pr1; // jfs: avoid rounding error problems sometimes causing subtitles with \pos to jump around a bit
                    else if((t1 < m_time) && (m_time < t2))
                    {
                        double t = (double)(m_time - t1) / (t2 - t1);
                        double alp = ((1 - t) * alp1 + t * alp2);
                        double rt = ((1 - t) * r0.x + t * r0.y);

                        p.x = (int)((1 - t) * p1.x + t * p2.x); // origin point
                        p.y = (int)((1 - t) * p1.y + t * p2.y);
                        p.x += (int)(cos(alp) * rt);
                        p.y -= (int)(sin(alp) * rt);
                    }
                    else p = pr2;
                }
                else if(s->m_effects[k]->param[0] == 2) // square spline
                {
                    CPoint p1(s->m_effects[k]->param[1], s->m_effects[k]->param[2]);
                    CPoint p2(s->m_effects[k]->param[3], s->m_effects[k]->param[4]);
                    CPoint p3(s->m_effects[k]->param[5], s->m_effects[k]->param[6]);

                    int t1 = s->m_effects[k]->t[0];
                    int t2 = s->m_effects[k]->t[1];

                    if(t2 < t1)
                    {
                        int t = t1;
                        t1 = t2;
                        t2 = t;
                    }

                    if(t1 <= 0 && t2 <= 0)
                    {
                        t1 = 0;
                        t2 = m_delay;
                    }

                    if(m_time <= t1) p = p1;
                    else if(p1 == p2) p = p1;  // jfs: avoid rounding error problems sometimes causing subtitles with \pos to jump around a bit
                    else if(t1 < m_time && m_time < t2)
                    {
                        double t = (double)(m_time - t1) / (t2 - t1);
                        p.x = (int)((1 - t) * (1 - t) * p1.x + 2 * t * (1 - t) * p2.x + t * t * p3.x);
                        p.y = (int)((1 - t) * (1 - t) * p1.y + 2 * t * (1 - t) * p2.y + t * t * p3.y);
                    }
                    else p = p3;
                }
                else if(s->m_effects[k]->param[0] == 3) // cubic spline
                {
                    CPoint p1(s->m_effects[k]->param[1], s->m_effects[k]->param[2]);
                    CPoint p2(s->m_effects[k]->param[3], s->m_effects[k]->param[4]);
                    CPoint p3(s->m_effects[k]->param[5], s->m_effects[k]->param[6]);
                    CPoint p4(s->m_effects[k]->param[7], s->m_effects[k]->param[8]);

                    int t1 = s->m_effects[k]->t[0];
                    int t2 = s->m_effects[k]->t[1];

                    if(t2 < t1)
                    {
                        int t = t1;
                        t1 = t2;
                        t2 = t;
                    }

                    if(t1 <= 0 && t2 <= 0)
                    {
                        t1 = 0;
                        t2 = m_delay;
                    }

                    if(m_time <= t1) p = p1;
                    else if(p1 == p2) p = p1;  // jfs: avoid rounding error problems sometimes causing subtitles with \pos to jump around a bit
                    else if(t1 < m_time && m_time < t2)
                    {
                        double t = (double)(m_time - t1) / (t2 - t1);
                        p.x = (int)((1 - t) * (1 - t) * (1 - t) * p1.x + 3 * t * (1 - t) * (1 - t) * p2.x + 3 * t * t * (1 - t) * p3.x + t * t * t * p4.x);
                        p.y = (int)((1 - t) * (1 - t) * (1 - t) * p1.y + 3 * t * (1 - t) * (1 - t) * p2.y + 3 * t * t * (1 - t) * p3.y + t * t * t * p4.y);
                    }
                    else p = p4;
                }
#endif
                r = CRect(
                        CPoint((s->m_scrAlignment % 3) == 1 ? p.x : (s->m_scrAlignment % 3) == 0 ? p.x - spaceNeeded.cx : p.x - (spaceNeeded.cx + 1) / 2,
                               s->m_scrAlignment <= 3 ? p.y - spaceNeeded.cy : s->m_scrAlignment <= 6 ? p.y - (spaceNeeded.cy + 1) / 2 : p.y),
                        spaceNeeded);

                if(s->m_relativeTo == 1)
                    r.OffsetRect(m_vidrect.TopLeft());
                fPosOverride = true;
            }
            break;
            case EF_ORG: // {\org(x=param[0], y=param[1])}
            {
#ifdef _VSMOD // patch f003. moving \org for some karaoke effects
                CPoint orgA(s->m_effects[k]->param[0], s->m_effects[k]->param[1]);
                CPoint orgB(s->m_effects[k]->param[2], s->m_effects[k]->param[3]);
                int to1 = s->m_effects[k]->t[0];
                int to2 = s->m_effects[k]->t[1];

                if(to2 < to1)
                {
                    int to = to1;
                    to1 = to2;
                    to2 = to;
                }

                if(to1 <= 0 && to2 <= 0)
                {
                    to1 = 0;
                    to2 = m_delay;
                }

                if(m_time <= to1) org2 = orgA;
                else if(to1 == to2) org2 = orgA;  // jfs: avoid rounding error problems sometimes causing subtitles with \pos to jump around a bit
                else if(to1 < m_time && m_time < to2)
                {
                    double t = 1.0 * (m_time - to1) / (to2 - to1);
                    org2.x = (int)((1 - t) * orgA.x + t * orgB.x);
                    org2.y = (int)((1 - t) * orgA.y + t * orgB.y);
                }
                else org2 = orgB;
#else
                org2 = CPoint(s->m_effects[k]->param[0], s->m_effects[k]->param[1]);
#endif
                fOrgOverride = true;
            }
            break;
            case EF_FADE: // {\fade(a1=param[0], a2=param[1], a3=param[2], t1=t[0], t2=t[1], t3=t[2], t4=t[3]) or {\fad(t1=t[1], t2=t[2])
            {
                const auto* effect = s->m_effects[k];
                int t1 = s->m_effects[k]->t[0];
                int t2 = s->m_effects[k]->t[1];
                int t3 = s->m_effects[k]->t[2];
                int t4 = s->m_effects[k]->t[3];

                if(t1 == -1 && t4 == -1)
                {
                    t1 = 0;
                    t3 = m_delay - t3;
                    t4 = m_delay;
                }

                if(m_time < t1) alpha = s->m_effects[k]->param[0];
                else if(m_time >= t1 && m_time < t2)
                {
                    double t = 1.0 * (m_time - t1) / (t2 - t1);
                    alpha = (int)(s->m_effects[k]->param[0] * (1 - t) + s->m_effects[k]->param[1] * t);
                }
                else if(m_time >= t2 && m_time < t3) alpha = s->m_effects[k]->param[1];
                else if(m_time >= t3 && m_time < t4)
                {
                    double t = 1.0 * (m_time - t3) / (t4 - t3);
                    alpha = (int)(s->m_effects[k]->param[1] * (1 - t) + s->m_effects[k]->param[2] * t);
                }
                else if(m_time >= t4) alpha = s->m_effects[k]->param[2];
#ifdef _VSMOD
                if (effect->hasFadeColor[0] || effect->hasFadeColor[1])
                {
                    const bool start = m_time < t2;
                    const int side = start ? 0 : 1;
                    if (effect->hasFadeColor[side])
                    {
                        paintFade.color = effect->fadeColors[side];
                        if (start)
                            paintFade.amount = m_time < t1 ? 1.0 : 1.0 - double(m_time - t1) / (t2 - t1);
                        else if (m_time >= t3)
                            paintFade.amount = m_time >= t4 ? 1.0 : double(m_time - t3) / (t4 - t3);
                    }
                }
#endif
            }
            break;
            case EF_BANNER: // Banner;delay=param[0][;leftoright=param[1];fadeawaywidth=param[2]]
            {
                int left = s->m_relativeTo == 1 ? m_vidrect.left : 0,
                    right = s->m_relativeTo == 1 ? m_vidrect.right : m_size.cx;

                r.left = !!s->m_effects[k]->param[1]
                         ? (left/*marginRect.left*/ - spaceNeeded.cx) + (int)(m_time * 8.0 / s->m_effects[k]->param[0])
                         : (right /*- marginRect.right*/) - (int)(m_time * 8.0 / s->m_effects[k]->param[0]);

                r.right = r.left + spaceNeeded.cx;

                clipRect &= CRect(left >> 3, clipRect.top, right >> 3, clipRect.bottom);

                fPosOverride = true;
            }
            break;
            case EF_SCROLL: // Scroll up/down(toptobottom=param[3]);top=param[0];bottom=param[1];delay=param[2][;fadeawayheight=param[4]]
            {
                r.top = !!s->m_effects[k]->param[3]
                        ? s->m_effects[k]->param[0] + (int)(m_time * 8.0 / s->m_effects[k]->param[2]) - spaceNeeded.cy
                        : s->m_effects[k]->param[1] - (int)(m_time * 8.0 / s->m_effects[k]->param[2]);

                r.bottom = r.top + spaceNeeded.cy;

                CRect cr(0, (s->m_effects[k]->param[0] + 4) >> 3, spd.w, (s->m_effects[k]->param[1] + 4) >> 3);

                if(s->m_relativeTo == 1)
                    r.top += m_vidrect.top,
                             r.bottom += m_vidrect.top,
                                         cr.top += m_vidrect.top >> 3,
                                                   cr.bottom += m_vidrect.top >> 3;

                clipRect &= cr;

                fPosOverride = true;
            }
            // Scroll has no vector-clip timing fields. Falling through reads
            // uninitialized Effect::t values and enables an unintended mask move.
            break;
#ifdef _VSMOD // patch m006. moveable vector clip
            case EF_VECTCLP:
            {
                CPoint vcpos1(s->m_effects[k]->param[0], s->m_effects[k]->param[1]);
                CPoint vcpos2(s->m_effects[k]->param[2], s->m_effects[k]->param[3]);
                int to1 = s->m_effects[k]->t[0];
                int to2 = s->m_effects[k]->t[1];

                if(to2 < to1)
                {
                    int to = to1;
                    to1 = to2;
                    to2 = to;
                }

                if(to1 <= 0 && to2 <= 0)
                {
                    to1 = 0;
                    to2 = m_delay;
                }

                if(m_time <= to1) mod_vc.pos = vcpos1;
                else if(to1 == to2) mod_vc.pos = vcpos1;  // jfs: avoid rounding error problems sometimes causing subtitles with \pos to jump around a bit
                else if(to1 < m_time && m_time < to2)
                {
                    double t = 1.0 * (m_time - to1) / (to2 - to1);
                    mod_vc.pos.x = (int)((1 - t) * vcpos1.x + t * vcpos2.x);
                    mod_vc.pos.y = (int)((1 - t) * vcpos1.y + t * vcpos2.y);
                }
                else mod_vc.pos = vcpos2;
                mod_vc.enable = true;
                mod_vc.isInverse = s->m_pClipper ? s->m_pClipper->m_inverse : false;
            }
#endif
            break;
            default:
                break;
            }
        }

#ifdef _VSMOD
        if (s->m_pClipper && clipOffset != CPoint(0, 0))
        {
            mod_vc.enable = true;
            mod_vc.pos += clipOffset;
            mod_vc.isInverse = s->m_pClipper->m_inverse;
        }
        if (s->m_motion)
        {
            const CPoint origin = s->m_relativeTo == 1 ? m_vidrect.TopLeft() : CPoint(0, 0);
            const CPoint current(
                s->m_scrAlignment % 3 == 1 ? r.left : s->m_scrAlignment % 3 == 2 ? r.CenterPoint().x : r.right,
                s->m_scrAlignment <= 3 ? r.bottom : s->m_scrAlignment <= 6 ? r.CenterPoint().y : r.top);
            const auto moved = s->m_motion->At({double(current.x - origin.x), double(current.y - origin.y)}, m_time);
            const auto bounded = [](double value) { return int((std::max)(-10000000., (std::min)(value, 10000000.))); };
            r.OffsetRect(bounded(moved.x) + origin.x - current.x, bounded(moved.y) + origin.y - current.y);
            fPosOverride = true;
        }
#endif
        if(!fPosOverride && !fOrgOverride && !s->m_fAnimated
#ifdef _VSMOD
            && !s->m_origin
#endif
            )
            r = m_sla.AllocRect(s, segment, entry, stse.layer, m_collisions);

        CPoint org;
        org.x = (s->m_scrAlignment % 3) == 1 ? r.left : (s->m_scrAlignment % 3) == 2 ? r.CenterPoint().x : r.right;
        org.y = s->m_scrAlignment <= 3 ? r.bottom : s->m_scrAlignment <= 6 ? r.CenterPoint().y : r.top;

        if(!fOrgOverride) org2 = org;
#ifdef _VSMOD
        if (s->m_origin)
        {
            const CPoint frameOrigin = s->m_relativeTo == 1 ? m_vidrect.TopLeft() : CPoint(0, 0);
            org2.x = LONG((*s->m_origin)[0].Resolve(org2.x - frameOrigin.x)) + frameOrigin.x;
            org2.y = LONG((*s->m_origin)[1].Resolve(org2.y - frameOrigin.y)) + frameOrigin.y;
        }
#endif

        const int eventTime = m_time;
        if (parallel && !captureOverlays)
        {
            auto prepare = PrepareEventGeometry(s, org, org2, r, eventTime, rt);
            if (prepare)
                preparations.push_back(std::move(prepare));
        }
        auto paint = [=, &spd]() mutable {
        PROFILE_RENDER_STAGE(EventPaint);
        CRect bbox2(0, 0, 0, 0);
        CPoint p, p2(0, r.top);

        POSITION pos;

        p = p2;

        // Rectangles for inverse clip
        CRect iclipRect[4];
        iclipRect[0] = CRect(0, 0, spd.w, clipRect.top);
        iclipRect[1] = CRect(0, clipRect.top, clipRect.left, clipRect.bottom);
        iclipRect[2] = CRect(clipRect.right, clipRect.top, spd.w, clipRect.bottom);
        iclipRect[3] = CRect(0, clipRect.bottom, spd.w, spd.h);

#ifdef _VSMOD
        if (!s->m_eventBox.empty())
        {
            const CPoint origin(org.x, p2.y);
            for (auto i = s->m_eventBox.rbegin(); i != s->m_eventBox.rend(); ++i)
            {
                auto& box = **i;
                auto& style = box.m_style;
#ifdef _LUA
                box.Paint(origin, org2, 4);
#else
                box.Paint(origin, org2);
#endif
                const DWORD opacity = MulDiv(255 - style.alpha[3], 255 - alpha, 255);
                const DWORD colors[4] = {DWORD(revcolor(style.colors[3])) | (opacity << 24), DWORD(-1), 0, 0};
                style.mod_grad.fadalpha = alpha;
                style.mod_grad.subpixx = origin.x & 7;
                style.mod_grad.subpixy = origin.y & 7;
                const auto drawBox = [&](CRect& clip) {
                    bbox2 |= box.Draw(spd, clip, pAlphaMask, origin.x, origin.y, colors, true, false, 3,
                        style.mod_grad, mod_vc, style.mod_blendMode, false, false);
                };
                if (s->m_clipInverse)
                    for (auto& clip : iclipRect) drawBox(clip);
                else drawBox(clipRect);
            }
        }
#endif
        pos = s->GetHeadPosition();
        while(pos)
        {
            CLine* l = s->GetNext(pos);

            p.x = s->LineLeft(*l, org.x);

#ifdef _VSMOD // patch m006. moveable vector clip
            if(s->m_clipInverse)
            {
                bbox2 |= l->PaintShadow(spd, iclipRect[0], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
                bbox2 |= l->PaintShadow(spd, iclipRect[1], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
                bbox2 |= l->PaintShadow(spd, iclipRect[2], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
                bbox2 |= l->PaintShadow(spd, iclipRect[3], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
            }
            else
            {
                bbox2 |= l->PaintShadow(spd, clipRect, pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
            }
#else
            if(s->m_clipInverse)
            {
                bbox2 |= l->PaintShadow(spd, iclipRect[0], pAlphaMask, p, org2, eventTime, alpha);
                bbox2 |= l->PaintShadow(spd, iclipRect[1], pAlphaMask, p, org2, eventTime, alpha);
                bbox2 |= l->PaintShadow(spd, iclipRect[2], pAlphaMask, p, org2, eventTime, alpha);
                bbox2 |= l->PaintShadow(spd, iclipRect[3], pAlphaMask, p, org2, eventTime, alpha);
            }
            else
            {
                bbox2 |= l->PaintShadow(spd, clipRect, pAlphaMask, p, org2, eventTime, alpha);
            }
#endif
            p.y += l->m_ascent + l->m_descent;
        }

        p = p2;

        pos = s->GetHeadPosition();
        while(pos)
        {
            CLine* l = s->GetNext(pos);

            p.x = s->LineLeft(*l, org.x);

#ifdef _VSMOD // patch m006. moveable vector clip
            if(s->m_clipInverse)
            {
                bbox2 |= l->PaintOutline(spd, iclipRect[0], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
                bbox2 |= l->PaintOutline(spd, iclipRect[1], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
                bbox2 |= l->PaintOutline(spd, iclipRect[2], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
                bbox2 |= l->PaintOutline(spd, iclipRect[3], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
            }
            else
            {
                bbox2 |= l->PaintOutline(spd, clipRect, pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
            }
#else
            if(s->m_clipInverse)
            {
                bbox2 |= l->PaintOutline(spd, iclipRect[0], pAlphaMask, p, org2, eventTime, alpha);
                bbox2 |= l->PaintOutline(spd, iclipRect[1], pAlphaMask, p, org2, eventTime, alpha);
                bbox2 |= l->PaintOutline(spd, iclipRect[2], pAlphaMask, p, org2, eventTime, alpha);
                bbox2 |= l->PaintOutline(spd, iclipRect[3], pAlphaMask, p, org2, eventTime, alpha);
            }
            else
            {
                bbox2 |= l->PaintOutline(spd, clipRect, pAlphaMask, p, org2, eventTime, alpha);
            }
#endif
            p.y += l->m_ascent + l->m_descent;
        }

        p = p2;

        pos = s->GetHeadPosition();
        while(pos)
        {
            CLine* l = s->GetNext(pos);

            p.x = s->LineLeft(*l, org.x);

#ifdef _VSMOD // patch m006. moveable vector clip
            if(s->m_clipInverse)
            {
                bbox2 |= l->PaintBody(spd, iclipRect[0], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
                bbox2 |= l->PaintBody(spd, iclipRect[1], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
                bbox2 |= l->PaintBody(spd, iclipRect[2], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
                bbox2 |= l->PaintBody(spd, iclipRect[3], pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
            }
            else
            {
                bbox2 |= l->PaintBody(spd, clipRect, pAlphaMask, p, org2, eventTime, alpha, mod_vc, rt);
            }
#else
            if(s->m_clipInverse)
            {
                bbox2 |= l->PaintBody(spd, iclipRect[0], pAlphaMask, p, org2, eventTime, alpha);
                bbox2 |= l->PaintBody(spd, iclipRect[1], pAlphaMask, p, org2, eventTime, alpha);
                bbox2 |= l->PaintBody(spd, iclipRect[2], pAlphaMask, p, org2, eventTime, alpha);
                bbox2 |= l->PaintBody(spd, iclipRect[3], pAlphaMask, p, org2, eventTime, alpha);
            }
            else
            {
                bbox2 |= l->PaintBody(spd, clipRect, pAlphaMask, p, org2, eventTime, alpha);
            }
#endif
            p.y += l->m_ascent + l->m_descent;
        }
        return bbox2;
        };
#ifdef _VSMOD
        if (s->m_paintSegments || paintFade.amount > 0)
        {
            bool collectSegments = s->m_paintSegments != 0;
#ifdef _LUA
            // A Lua pixel callback can read the destination during the draw.
            if (collectSegments && (hasExternalLua || m_doOverrideStyle))
                collectSegments = !HasLuaPixelHandler(*s);
#endif
            auto paintExtended = [=]() mutable {
                const OverlayPaintScope context(paintFade, s->m_scalex, s->m_scaley);
                if (!collectSegments || IsCapturingOverlays())
                    return paint();
                OverlayDrawList overlays;
                CRect bounds;
                {
                    const OverlayCaptureScope capture(overlays);
                    bounds = paint();
                }
                overlays.Blend();
                return bounds;
            };
            if (parallel)
                compositions.push_back({std::move(paintExtended), entry});
            else
            {
                bbox2 |= paintExtended();
                clearLuaData(entry);
            }
            continue;
        }
#endif
        if (parallel)
            compositions.push_back({std::move(paint), entry});
        else
        {
            bbox2 |= paint();
            clearLuaData(entry);
        }
    }
    if (parallel)
        finishEvents(captureOverlays);

    bbox = bbox2;

    return (subs.GetCount() && !bbox2.IsRectEmpty()) ? S_OK : S_FALSE;
}
catch (const std::bad_alloc&)
{
    Deinit();
    return E_OUTOFMEMORY;
}
catch (...)
{
    Deinit();
    return E_FAIL;
}

// IPersist

STDMETHODIMP CRenderedTextSubtitle::GetClassID(CLSID* pClassID)
{
    return pClassID ? *pClassID = __uuidof(this), S_OK : E_POINTER;
}

// ISubStream

STDMETHODIMP_(int) CRenderedTextSubtitle::GetStreamCount()
{
    return(1);
}

STDMETHODIMP CRenderedTextSubtitle::GetStreamInfo(int iStream, WCHAR** ppName, LCID* pLCID)
{
    USES_CONVERSION;
    if(iStream != 0) return E_INVALIDARG;

    if(ppName)
    {
        if(!(*ppName = (WCHAR*)CoTaskMemAlloc((m_name.GetLength() + 1) * sizeof(WCHAR))))
            return E_OUTOFMEMORY;

        wcscpy(*ppName, CStringW(m_name));

        if(pLCID)
        {
            *pLCID = ISO6391ToLcid(W2A(*ppName));
            if(*pLCID == 0)
                *pLCID = ISO6392ToLcid(W2A(*ppName));
        }
    }

    return S_OK;
}

STDMETHODIMP_(int) CRenderedTextSubtitle::GetStream()
{
    return(0);
}

STDMETHODIMP CRenderedTextSubtitle::SetStream(int iStream)
{
    return iStream == 0 ? S_OK : E_FAIL;
}

STDMETHODIMP CRenderedTextSubtitle::Reload()
{
    CFileStatus s;
    if(!CFile::GetStatus(m_path, s)) return E_FAIL;
    return !m_path.IsEmpty() && Open(m_path, DEFAULT_CHARSET) ? S_OK : E_FAIL;
}
