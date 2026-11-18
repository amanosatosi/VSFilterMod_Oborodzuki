#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

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

struct Paint;
using Source = std::shared_ptr<const Paint>;
struct Paint
{
    enum class Kind { Solid, Field, Corners, Slide };
    Kind kind = Kind::Solid;
    unsigned segment = 0;
    double angle = 0;
    Value solid = 0;
    std::vector<Stop> stops;
    Source from, to;
    unsigned depth = 0;
    double width = 0, progress = 0;
};

struct Sources
{
    // Two independent slots per target: RGB, then inverse alpha.
    std::array<Source, 8> parts{};
};

struct SlideSettings
{
    std::array<double, 8> channels{};
    double global = 0;

    double Width(int part) const
    {
        const double setting = channels[part] ? channels[part] : global;
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
    if (from)
    {
        const auto delta = std::fmod(to.angle - from->angle, 360.0);
        result.angle = from->angle + t * (delta > 180 ? delta - 360 : delta < -180 ? delta + 360 : delta);
    }
    auto add = [&](double position) {
        if (!result.stops.empty() && std::abs(result.stops.back().position - position) < 0.000001)
            return;
        if (result.stops.size() == MaxStops)
        {
            result.stops.back().position = 1;
            return;
        }
        const Value first = from ? SampleStops(*from, position, alpha, false) : solid;
        const Value last = SampleStops(to, position, alpha, false);
        result.stops.push_back({position, Mix(first, last, t, alpha, false)});
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
    return result;
}

struct Rect
{
    double left, top, right, bottom;
};

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
    struct Endpoint
    {
        const Paint* paint = nullptr;
        Projection axis;

        Endpoint() = default;
        Endpoint(const Paint* source, Rect bounds) : paint(source),
            axis(source->kind == Paint::Kind::Field ? Projection(bounds, source->angle) : Projection{}) {}

        Value At(double x, double y, double u, double v, bool alpha) const
        {
            if (paint->kind == Paint::Kind::Solid)
                return paint->solid;
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
            return SampleStops(*paint, Unit(axis.At(x, y) * axis.inverseSpan), alpha, true);
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

public:
    void Prepare(const Paint* paint, Rect bounds, double sx, double sy, bool inverseAlpha)
    {
        alpha = inverseAlpha;
        for (; paint->kind == Paint::Kind::Slide; paint = paint->from.get())
        {
            const Projection axis(bounds, paint->angle);
            const double scale = sx == sy ? sx : (sx > 0 && sy > 0 ?
                1 / std::hypot(axis.dx / sx, axis.dy / sy) : 0);
            const double limit = (std::numeric_limits<double>::max)() / 2;
            const double width = (std::min)(limit, paint->width * scale);
            wipes.push_back({Endpoint(paint->to.get(), bounds), axis, width,
                paint->progress * axis.span + paint->progress * width});
        }
        start = Endpoint(paint, bounds);
    }

    bool Active() const { return start.paint != nullptr; }

    Value At(double x, double y, double u, double v) const
    {
        std::array<double, MaxSlides> weights;
        size_t first = 0;
        for (; first < wipes.size(); ++first)
        {
            const auto& wipe = wipes[first];
            const double distance = wipe.edge - wipe.axis.At(x, y);
            weights[first] = distance <= 0 ? 0 : distance >= wipe.width ? 1 : distance / wipe.width;
            if (weights[first] == 1)
                break;
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
