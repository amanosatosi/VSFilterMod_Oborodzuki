#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>
#include "RenderProfile.h"

namespace KfxPaint
{
constexpr size_t MaxStops = 64;
constexpr unsigned MaxSlides = 32;
using Value = std::uint32_t;

inline double Unit(double x)
{
    return (std::max)(0.0, (std::min)(x, 1.0));
}

// Values use the overlay mixer's RGB byte order or a single ASS inverse-alpha byte.
inline Value Mix(Value from, Value to, double t, bool alpha, bool round)
{
    if (t <= 0) return from;
    if (t >= 1) return to;
    Value result = 0;
    for (int shift = 0; shift < (alpha ? 8 : 24); shift += 8)
    {
        const double value = ((from >> shift) & 255) * (1 - t) + ((to >> shift) & 255) * t;
        result |= Value(value + (round ? 0.5 : 0.0)) << shift;
    }
    return result;
}

struct Stop
{
    double position;
    Value value;
};

struct Rect
{
    double left, top, right, bottom;
};

inline Rect Normalize(Rect rect)
{
    if (rect.left > rect.right) std::swap(rect.left, rect.right);
    if (rect.top > rect.bottom) std::swap(rect.top, rect.bottom);
    return rect;
}

inline Rect Unite(Rect a, Rect b)
{
    a = Normalize(a);
    b = Normalize(b);
    return {(std::min)(a.left, b.left), (std::min)(a.top, b.top),
            (std::max)(a.right, b.right), (std::max)(a.bottom, b.bottom)};
}

struct Paint;
using Source = std::shared_ptr<const Paint>;
struct Paint
{
    enum class Kind { Solid, Field, Corners, Slide };
    Kind kind = Kind::Solid;
    unsigned segment = 0;
    bool positioned = false;
    Rect rect{};
    Value outside = 0;
    double angle = 0;
    Value solid = 0;
    std::vector<Stop> stops;
    Source from, to;
    unsigned depth = 0;
    double width = 0, progress = 0;
};

struct DotState;

struct PaintBoundary
{
    double x = 0, y = 0, offset = 0;
    bool Active() const { return x != 0 || y != 0; }
};

struct Sources
{
    // Two independent slots per target: RGB, then inverse alpha.
    std::array<Source, 8> parts{};
    std::shared_ptr<const DotState> dots;
    int dotTarget = -1;
};

constexpr unsigned MaxOutlineLayers = 10;

constexpr unsigned PatternTargets = 2 + MaxOutlineLayers;

struct Dots
{
    int color = -1;
    double diameter = -1, spacing = -1;
};

struct DotState
{
    std::array<Dots, PatternTargets> layers;
    bool propagate = false;
    Dots Layer(unsigned target) const
    {
        auto result = layers[target];
        if (propagate && target)
        {
            if (result.color < 0) result.color = layers[0].color;
            if (result.diameter < 0) result.diameter = layers[0].diameter;
            if (result.spacing < 0) result.spacing = layers[0].spacing;
        }
        return result;
    }
};

using CycleIndices = std::array<int, PatternTargets>;

struct Cycles
{
    struct Palette
    {
        std::vector<Value> colors;
        unsigned mode = 0, next = 0;
    };
    std::array<Palette, PatternTargets> layers;
    unsigned active = 0;

    void Set(unsigned target, unsigned mode, std::vector<Value> colors)
    {
        layers[target] = {std::move(colors), mode, 0};
        if (mode) active |= 1u << target;
        else active &= ~(1u << target);
    }
    bool PerGlyph() const
    {
        for (const auto& pattern : layers) if (pattern.mode == 2) return true;
        return false;
    }
    CycleIndices Take(size_t glyphs, bool whitespace)
    {
        CycleIndices indices;
        indices.fill(-1);
        if (whitespace || !glyphs) return indices;
        for (unsigned i = 0; i < layers.size(); ++i)
        {
            auto& palette = layers[i];
            if (!palette.mode) continue;
            indices[i] = int(palette.next);
            palette.next = unsigned((palette.next + (palette.mode == 2 ? glyphs : 1)) % palette.colors.size());
        }
        return indices;
    }
    CycleIndices Colors(const CycleIndices& indices, size_t glyph) const
    {
        CycleIndices values;
        values.fill(-1);
        for (unsigned i = 0; i < layers.size(); ++i)
            if (indices[i] >= 0)
            {
                const auto& palette = layers[i];
                const size_t offset = palette.mode == 2 ? glyph : 0;
                values[i] = int(palette.colors[(indices[i] + offset) % palette.colors.size()]);
            }
        return values;
    }
};

struct OutlineLayer
{
    unsigned number = 0;
    double x = 0, y = 0;
    double blur = -1;
    int edgeBlur = -1;
    int color = -1, inverseAlpha = -1;
    int waitingColor = -1;
    Source waitingPaint;
    std::array<Source, 2> paint;
};

struct OutlineStyle
{
    std::vector<OutlineLayer> layers;
    double scaleX = 1, scaleY = 1;
};

struct BoxShape
{
    std::array<double, 4> padding{}; // left, right, top, bottom
    double radius = 0;
};

struct DecorationPaint
{
    std::array<Source, 2> paint;
    int color = -1, inverseAlpha = -1;
    bool Active() const { return color >= 0 || inverseAlpha >= 0 || paint[0] || paint[1]; }
};

constexpr int NativeSlideStart = 8;
constexpr int BoxSlideStart = NativeSlideStart + 2 * (MaxOutlineLayers - 1);
constexpr int WaitingSlidePart = BoxSlideStart + 2 * MaxOutlineLayers;
constexpr int PositionedAlphaSlidePart = WaitingSlidePart + 1;
constexpr int DecorationSlideStart = PositionedAlphaSlidePart + 1;

struct SlideSettings
{
    std::array<double, 8> channels{};
    std::array<double, 2 * (MaxOutlineLayers - 1)> outline{};
    std::array<double, 2 * MaxOutlineLayers> box{};
    double waiting = 0;
    double positionedAlpha = 0;
    std::array<double, 2> decoration{};
    double global = 0;

    double Width(int part) const
    {
        const double channel = part >= DecorationSlideStart ? decoration[part - DecorationSlideStart] :
            part == PositionedAlphaSlidePart ? positionedAlpha :
            part == WaitingSlidePart ? waiting :
            part < NativeSlideStart ? channels[part] :
            part < BoxSlideStart ? outline[part - NativeSlideStart] : box[part - BoxSlideStart];
        const double setting = channel ? channel : global;
        return setting == -1 ? 0 : setting > 0 ? setting : -1;
    }
};

struct Fade
{
    Value color = 0;
    double amount = 0;
};

inline Value SampleStops(const Paint& paint, double t, bool alpha, bool round)
{
    PROFILE_RENDER_STAGE(PaintStops);
    const auto& stops = paint.stops;
    if (t <= stops.front().position)
        return stops.front().value;
    const auto next = std::lower_bound(stops.begin() + 1, stops.end(), t,
        [](const Stop& stop, double position) { return stop.position < position; });
    if (next == stops.end())
        return stops.back().value;
    const auto& prior = *(next - 1);
    return Mix(prior.value, next->value, (t - prior.position) / (next->position - prior.position), alpha, round);
}

inline Paint Interpolate(const Paint* from, const Paint& to, Value solid, double t, bool alpha)
{
    Paint result;
    result.kind = Paint::Kind::Field;
    result.angle = to.angle;
    result.positioned = to.positioned;
    result.rect = to.rect;
    result.outside = to.outside;
    if (from)
    {
        if (to.positioned)
        {
            const auto at = [t](double a, double b) { return a * (1 - t) + b * t; };
            result.rect = {at(from->rect.left, to.rect.left), at(from->rect.top, to.rect.top),
                at(from->rect.right, to.rect.right), at(from->rect.bottom, to.rect.bottom)};
        }
        const auto delta = std::fmod(to.angle - from->angle, 360.0);
        result.angle = std::fmod(from->angle +
            t * (delta > 180 ? delta - 360 : delta < -180 ? delta + 360 : delta), 360.0);
        if (result.angle < 0) result.angle += 360;
    }
    auto add = [&](double position) {
        if (!result.stops.empty() && std::abs(result.stops.back().position - position) < 0.000001)
            return;
        if (result.stops.size() == MaxStops)
        {
            result.stops.back().position = 1;
            return;
        }
        result.stops.push_back({position, 0});
    };
    size_t i = 0, j = 0;
    while ((from && i < from->stops.size()) || j < to.stops.size())
    {
        if (from && i < from->stops.size() &&
            (j == to.stops.size() || from->stops[i].position < to.stops[j].position))
            add(from->stops[i++].position);
        else
            add(to.stops[j++].position);
    }
    add(1);
    for (auto& stop : result.stops)
    {
        const Value first = from ? SampleStops(*from, stop.position, alpha, false) : solid;
        const Value last = SampleStops(to, stop.position, alpha, false);
        stop.value = Mix(first, last, t, alpha, false);
    }
    return result;
}

struct Projection
{
    double dx = 0, dy = 0, low = 0, span = 0, inverseSpan = 0;

    Projection() = default;
    Projection(Rect bounds, double angle)
    {
        const double radians = angle * (3.14159265358979323846 / 180.0);
        dx = std::cos(radians);
        dy = std::sin(radians);
        low = (dx >= 0 ? bounds.left : bounds.right) * dx +
              (dy >= 0 ? bounds.top : bounds.bottom) * dy;
        span = (bounds.right - bounds.left) * std::abs(dx) +
               (bounds.bottom - bounds.top) * std::abs(dy);
        inverseSpan = span > 0 ? 1 / span : 0;
    }
    double At(double x, double y) const { return x * dx + y * dy - low; }
};

class Sampler
{
    static Rect Position(const Paint& paint, Rect occupied, double offsetY)
    {
        if (!paint.positioned) return occupied;
        auto rect = Normalize(paint.rect);
        rect.top += offsetY;
        rect.bottom += offsetY;
        return rect;
    }
    struct Endpoint
    {
        const Paint* paint = nullptr;
        Projection axis;
        Rect bounds{};

        Endpoint() = default;
        Endpoint(const Paint* source, Rect occupied, double offsetY) : paint(source),
            bounds(Position(*source, occupied, offsetY))
        {
            if (source->kind == Paint::Kind::Field)
                axis = Projection(bounds, source->angle);
        }

        Value At(double x, double y, double u, double v, bool alpha) const
        {
            if (paint->kind == Paint::Kind::Solid)
                return paint->solid;
            if (paint->positioned &&
                (bounds.left >= bounds.right || bounds.top >= bounds.bottom ||
                 x < bounds.left || x > bounds.right || y < bounds.top || y > bounds.bottom))
                return paint->outside;
            if (paint->kind == Paint::Kind::Corners)
            {
                // The legacy vector endpoint lives in bitmap coordinates.
                Value value = 0;
                for (int shift = 0; shift < (alpha ? 8 : 24); shift += 8)
                {
                    const auto component = [&](int i) { return (paint->stops[i].value >> shift) & 255; };
                    value |= Value((component(0) * (1-u) + component(1) * u) * (1-v) +
                                   (component(2) * (1-u) + component(3) * u) * v) << shift;
                }
                return value;
            }
            double t;
            {
                PROFILE_RENDER_STAGE(PaintProjection);
                t = Unit(axis.At(x, y) * axis.inverseSpan);
            }
            return SampleStops(*paint, t, alpha, true);
        }
    };
    struct Wipe
    {
        Endpoint target;
        Projection axis;
        double width, edge;
    };
    Endpoint start;
    std::vector<Wipe> wipes;
    bool alpha = false;
    bool corners = false;

public:
    void Prepare(const Paint* paint, Rect bounds, double sx, double sy, bool inverseAlpha, double offsetY = 0)
    {
        PROFILE_RENDER_STAGE(PaintProjection);
        alpha = inverseAlpha;
        for (; paint->kind == Paint::Kind::Slide; paint = paint->from.get())
        {
            const Projection axis(Position(*paint, bounds, offsetY), paint->angle);
            const double scale = sx == sy ? sx : (sx > 0 && sy > 0 ?
                1 / std::hypot(axis.dx / sx, axis.dy / sy) : 0);
            const double limit = (std::numeric_limits<double>::max)() / 2;
            const double width = (std::min)(limit, paint->width * scale);
            corners |= paint->to->kind == Paint::Kind::Corners;
            wipes.push_back({Endpoint(paint->to.get(), bounds, offsetY), axis, width,
                paint->progress * axis.span + paint->progress * width});
        }
        start = Endpoint(paint, bounds, offsetY);
        corners |= paint->kind == Paint::Kind::Corners;
    }

    bool Active() const { return start.paint != nullptr; }
    bool UsesCorners() const { return corners; }

    Value At(double x, double y, double u, double v) const
    {
        if (wipes.empty())
            return start.At(x, y, u, v, alpha);
        std::array<double, MaxSlides> weights;
        size_t first = 0;
        {
            PROFILE_RENDER_STAGE(PaintProjection);
            for (; first < wipes.size(); ++first)
            {
                const auto& wipe = wipes[first];
                const double distance = wipe.edge - wipe.axis.At(x, y);
                weights[first] = distance <= 0 ? 0 : distance >= wipe.width ? 1 : distance / wipe.width;
                if (weights[first] == 1)
                    break;
            }
        }
        const auto& base = first < wipes.size() ? wipes[first].target : start;
        Value value = base.At(x, y, u, v, alpha);
        while (first > 0)
        {
            --first;
            if (weights[first] > 0)
                value = Mix(value, wipes[first].target.At(x, y, u, v, alpha), weights[first], alpha, true);
        }
        return value;
    }
};
}
