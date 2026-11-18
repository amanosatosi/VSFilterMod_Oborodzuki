#include "stdafx.h"
#include "ShapedText.h"
#include <dwrite_1.h>
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace ShapedText
{
struct Context::Impl
{
    HMODULE library = nullptr;
    CComPtr<IDWriteFactory> factory;

    Impl()
    {
        WCHAR system[MAX_PATH];
        const UINT length = GetSystemDirectoryW(system, MAX_PATH);
        if (!length || length >= MAX_PATH - 12) return;
        wcscat_s(system, L"\\dwrite.dll");
        library = LoadLibraryW(system);
        if (!library) return;
        const auto create = reinterpret_cast<decltype(&DWriteCreateFactory)>(GetProcAddress(library, "DWriteCreateFactory"));
        if (create)
        {
            IUnknown* object = nullptr;
            if (SUCCEEDED(create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &object)))
                factory.Attach(static_cast<IDWriteFactory*>(object));
        }
    }

    ~Impl()
    {
        factory.Release();
        if (library) FreeLibrary(library);
    }
};

namespace
{
class GeometrySink : public IDWriteGeometrySink
{
    Outline& out;
    float x, y;
    bool failed = false;

    void Add(D2D1_POINT_2F point, BYTE type)
    {
        if (failed) return;
        const double vx = (point.x + x) * 64., vy = (point.y + y) * 64.;
        if (!std::isfinite(vx) || !std::isfinite(vy) || std::abs(vx) > 16000000 || std::abs(vy) > 16000000 ||
            out.points.size() >= 16384)
        {
            failed = true;
            return;
        }
        try
        {
            out.points.push_back({LONG(std::lround(vx)), LONG(std::lround(vy))});
            out.types.push_back(type);
        }
        catch (...)
        {
            failed = true;
        }
    }

public:
    GeometrySink(Outline& outline, float dx, float dy) : out(outline), x(dx), y(dy) {}
    STDMETHODIMP QueryInterface(REFIID id, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IDWriteGeometrySink)) return E_NOINTERFACE;
        *object = static_cast<IDWriteGeometrySink*>(this);
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP_(void) SetFillMode(D2D1_FILL_MODE) override {}
    STDMETHODIMP_(void) SetSegmentFlags(D2D1_PATH_SEGMENT) override {}
    STDMETHODIMP_(void) BeginFigure(D2D1_POINT_2F point, D2D1_FIGURE_BEGIN) override { Add(point, PT_MOVETO); }
    STDMETHODIMP_(void) AddLines(const D2D1_POINT_2F* points, UINT32 count) override
    {
        for (UINT32 i = 0; i < count; ++i) Add(points[i], PT_LINETO);
    }
    STDMETHODIMP_(void) AddBeziers(const D2D1_BEZIER_SEGMENT* curves, UINT32 count) override
    {
        for (UINT32 i = 0; i < count; ++i)
        {
            Add(curves[i].point1, PT_BEZIERTO);
            Add(curves[i].point2, PT_BEZIERTO);
            Add(curves[i].point3, PT_BEZIERTO);
        }
    }
    STDMETHODIMP_(void) EndFigure(D2D1_FIGURE_END) override {}
    STDMETHODIMP Close() override { return failed ? E_FAIL : S_OK; }
};

class LayoutSink : public IDWriteTextRenderer
{
    Run& output;
    const std::vector<bool>& wrap;
    bool verticalVariants;
    size_t points = 0;

public:
    LayoutSink(Run& output, const std::vector<bool>& wrap, bool vertical)
        : output(output), wrap(wrap), verticalVariants(vertical) {}

    STDMETHODIMP QueryInterface(REFIID id, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (id == __uuidof(IDWritePixelSnapping))
            *object = static_cast<IDWritePixelSnapping*>(this);
        else if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteTextRenderer))
            *object = static_cast<IDWriteTextRenderer*>(this);
        else return E_NOINTERFACE;
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP IsPixelSnappingDisabled(void*, BOOL* out) override { *out = TRUE; return S_OK; }
    STDMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* out) override
    {
        *out = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    STDMETHODIMP GetPixelsPerDip(void*, FLOAT* out) override { *out = 1; return S_OK; }

    STDMETHODIMP DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION* description, IUnknown*) override
    {
        if (!description || !description->clusterMap || !run->glyphCount) return S_OK;
        if (run->glyphCount > 8192 || description->stringLength > 8192) return E_FAIL;
        try
        {
            struct LogicalCluster { unsigned begin, end, glyph; };
            std::vector<LogicalCluster> clusters;
            for (unsigned i = 0; i < description->stringLength;)
            {
                const auto start = i;
                const unsigned glyph = description->clusterMap[i++];
                while (i < description->stringLength && description->clusterMap[i] == glyph) ++i;
                if (glyph >= run->glyphCount) return E_FAIL;
                clusters.push_back({start, i, glyph});
            }
            std::sort(clusters.begin(), clusters.end(), [](const LogicalCluster& a, const LogicalCluster& b) {
                return a.glyph < b.glyph;
            });
            std::vector<float> offsets(run->glyphCount + 1, 0);
            for (unsigned i = 0; i < run->glyphCount; ++i)
                offsets[i + 1] = offsets[i] + run->glyphAdvances[i];
            CComPtr<IDWriteFontFace1> variants;
            if (verticalVariants)
                run->fontFace->QueryInterface(__uuidof(IDWriteFontFace1), reinterpret_cast<void**>(&variants));
            const bool rtl = (run->bidiLevel & 1) != 0;
            for (size_t i = 0; i < clusters.size(); ++i)
            {
                const auto& range = clusters[i];
                const unsigned first = range.glyph;
                const unsigned end = i + 1 == clusters.size() ? run->glyphCount : clusters[i + 1].glyph;
                if (first == end) continue;
                Cluster cluster;
                cluster.start = description->textPosition + range.begin;
                cluster.length = range.end - range.begin;
                cluster.advance = offsets[end] - offsets[first];
                cluster.left = rtl ? x - offsets[end] : x + offsets[first];
                cluster.whitespace = true;
                for (unsigned c = range.begin; c < range.end; ++c)
                    cluster.whitespace &= iswspace(description->string[c]) != 0 || description->string[c] == 0xa0;
                const size_t boundary = cluster.start + cluster.length;
                cluster.breakAfter = boundary < wrap.size() && wrap[boundary];
                if (!cluster.whitespace)
                {
                    for (unsigned g = first; g < end; ++g)
                    {
                        UINT16 id = run->glyphIndices[g];
                        const wchar_t c = description->string[range.begin];
                        if (variants && c >= 0x2e80 && variants->HasVerticalGlyphVariants())
                        {
                            UINT16 vertical = id;
                            if (SUCCEEDED(variants->GetVerticalGlyphVariants(1, &id, &vertical))) id = vertical;
                        }
                        cluster.glyphs.emplace_back();
                        auto& outline = cluster.glyphs.back();
                        GeometrySink sink(outline, rtl ? offsets[end] - offsets[g] : offsets[g] - offsets[first], y);
                        const auto* glyphOffset = run->glyphOffsets ? run->glyphOffsets + g : nullptr;
                        if (FAILED(run->fontFace->GetGlyphRunOutline(run->fontEmSize, &id, run->glyphAdvances + g,
                                glyphOffset, 1, FALSE, rtl, &sink)) || FAILED(sink.Close()))
                            return E_FAIL;
                        points += outline.points.size();
                        if (points > 262144) return E_FAIL;
                    }
                }
                output.clusters.push_back(std::move(cluster));
            }
        }
        catch (...)
        {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }
    STDMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    STDMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    STDMETHODIMP DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }
};
}

Context::Context() : native(std::make_unique<Impl>()) {}
Context::~Context() = default;

std::shared_ptr<const Run> Context::Shape(const wchar_t* text, unsigned length, const wchar_t* family,
    float em, int weight, bool italic, bool verticalVariants)
{
    if (!length || length > 8192 || em <= 0 || em > 4096 || !std::isfinite(em)) return {};
    if (!native->factory) return {};
    CComPtr<IDWriteTextFormat> format;
    const auto face = *family == L'@' ? family + 1 : family;
    if (FAILED(native->factory->CreateTextFormat(face, nullptr, DWRITE_FONT_WEIGHT((std::max)(1, (std::min)(999, weight))),
            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, em, L"", &format)))
        return {};
    if (FAILED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP))) return {};
    CComPtr<IDWriteTextLayout> layout;
    if (FAILED(native->factory->CreateTextLayout(text, length, format, 1000000, 1000000, &layout))) return {};
    DWRITE_LINE_METRICS line{};
    UINT32 lines = 0;
    if (FAILED(layout->GetLineMetrics(&line, 1, &lines)) || lines != 1) return {};
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return {};
    std::vector<DWRITE_CLUSTER_METRICS> clusters(length);
    UINT32 count = 0;
    if (FAILED(layout->GetClusterMetrics(clusters.data(), length, &count))) return {};
    std::vector<bool> wrap(length + 1);
    size_t position = 0;
    for (UINT32 i = 0; i < count; ++i)
    {
        position += clusters[i].length;
        if (position <= length) wrap[position] = clusters[i].canWrapLineAfter;
    }
    auto result = std::make_shared<Run>();
    result->width = metrics.widthIncludingTrailingWhitespace;
    result->ascent = line.baseline;
    result->descent = line.height - line.baseline;
    LayoutSink sink(*result, wrap, verticalVariants);
    if (FAILED(layout->Draw(nullptr, &sink, 0, 0))) return {};
    std::stable_sort(result->clusters.begin(), result->clusters.end(), [](const Cluster& a, const Cluster& b) {
        return a.left < b.left;
    });
    return result;
}
}
