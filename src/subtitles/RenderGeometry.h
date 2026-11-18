#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <vector>

namespace RenderGeometry
{
struct Point
{
    double x = 0, y = 0;
    Point operator+(Point b) const { return {x + b.x, y + b.y}; }
    Point operator-(Point b) const { return {x - b.x, y - b.y}; }
    Point operator*(double scale) const { return {x * scale, y * scale}; }
};

inline double Dot(Point a, Point b) { return a.x * b.x + a.y * b.y; }
inline double Cross(Point a, Point b) { return a.x * b.y - a.y * b.x; }
inline double Length(Point p) { return std::hypot(p.x, p.y); }
inline Point Normal(Point p) { return {-p.y, p.x}; }

using Pins = std::array<double, 8>;

inline const Pins& IdentityPins()
{
    static const Pins corners{{0, 0, 1, 0, 1, 1, 0, 1}};
    return corners;
}

class Perspective
{
    Point origin, horizontal, vertical;
    double depthX = 0, depthY = 0, width = 0, height = 0;

public:
    bool Set(const Pins& pins, double w, double h)
    {
        if (!(w > 0 && h > 0)) return false;
        std::array<Point, 4> p;
        for (size_t i = 0; i < p.size(); ++i)
        {
            p[i] = {pins[2 * i], pins[2 * i + 1]};
            if (!std::isfinite(p[i].x) || !std::isfinite(p[i].y) ||
                std::abs(p[i].x) > 16 || std::abs(p[i].y) > 16)
                return false;
        }
        const double orientation = Cross(p[1] - p[0], p[2] - p[1]);
        if (std::abs(orientation) < 1e-10) return false;
        for (size_t i = 1; i < p.size(); ++i)
        {
            const auto a = p[(i + 1) % 4] - p[i];
            const auto b = p[(i + 2) % 4] - p[(i + 1) % 4];
            if (Cross(a, b) * std::copysign(1., orientation) < 1e-10) return false;
        }
        const auto a = p[1] - p[2], b = p[3] - p[2], difference = p[0] - p[1] + p[2] - p[3];
        const double determinant = Cross(a, b);
        depthX = Cross(difference, b) / determinant;
        depthY = Cross(a, difference) / determinant;
        if (1 + depthX < 1e-6 || 1 + depthY < 1e-6 || 1 + depthX + depthY < 1e-6)
            return false;
        origin = p[0];
        horizontal = p[1] * (1 + depthX) - p[0];
        vertical = p[3] * (1 + depthY) - p[0];
        width = w;
        height = h;
        return true;
    }

    bool Map(Point in, Point& out) const
    {
        const double u = in.x / width, v = in.y / height;
        const double denominator = 1 + u * depthX + v * depthY;
        if (!std::isfinite(denominator) || denominator < 1e-6) return false;
        const auto destination = (origin + horizontal * u + vertical * v) * (1 / denominator);
        out = {destination.x * width, destination.y * height};
        return std::abs(out.x) <= 2097152 && std::abs(out.y) <= 2097152;
    }

    bool Affine() const { return depthX == 0 && depthY == 0; }
};

class Curve
{
    std::vector<Point> points;
    std::vector<double> distances;

    bool Append(Point p)
    {
        if (points.size() >= 4096) return false;
        if (points.empty())
        {
            points.push_back(p);
            distances.push_back(0);
            return true;
        }
        const double segment = Length(p - points.back());
        if (segment > 1e-9)
        {
            points.push_back(p);
            distances.push_back(distances.back() + segment);
        }
        return true;
    }

    bool Cubic(Point a, Point b, Point c, Point d, unsigned depth = 0)
    {
        const double chord = Length(d - a);
        const double control = Length(b - a) + Length(c - b) + Length(d - c);
        if (control - chord <= .025 && (chord < .125 ||
                (std::max)(std::abs(Cross(d - a, b - a)), std::abs(Cross(d - a, c - a))) <= chord * .125))
            return Append(d);
        if (depth == 14) return false;
        const auto ab = (a+b)*.5, bc = (b+c)*.5, cd = (c+d)*.5;
        const auto left = (ab+bc)*.5, right = (bc+cd)*.5, center = (left+right)*.5;
        return Cubic(a, ab, left, center, depth + 1) && Cubic(center, right, cd, d, depth + 1);
    }

public:
    bool Parse(const wchar_t* source)
    {
        points.clear();
        distances.clear();
        if (std::wcslen(source) > 16384) return false;
        auto skip = [&] {
            while (*source == L' ' || *source == L'\t' || *source == L'\r' || *source == L'\n')
                ++source;
        };
        auto coord = [&](double& result) {
            skip();
            wchar_t* end;
            result = std::wcstod(source, &end);
            if (end == source || !std::isfinite(result) || std::abs(result) > 65536) return false;
            source = end;
            return true;
        };
        auto point = [&](Point& p) { return coord(p.x) && coord(p.y); };
        skip();
        if (*source != L'm') return false;
        ++source;
        Point first;
        if (!point(first) || !Append(first)) return false;
        wchar_t command = 0;
        std::vector<Point> spline;
        const auto finishSpline = [&](bool close) {
            if (spline.empty()) return false;
            if (close)
                spline.insert(spline.end(), {spline[0], spline[1], spline[2]});
            if (spline.size() < 4) return false;
            for (size_t i = 0; i + 3 < spline.size(); ++i)
            {
                const auto a = spline[i], b = spline[i+1], c = spline[i+2], d = spline[i+3];
                const auto from = (a + b*4 + c) * (1./6);
                const auto to = (b + c*4 + d) * (1./6);
                if (!Append(from) || !Cubic(from, (b*2 + c) * (1./3), (b + c*2) * (1./3), to))
                    return false;
            }
            spline.clear();
            return true;
        };
        while (true)
        {
            skip();
            if (!*source) break;
            if (*source >= L'a' && *source <= L'z')
            {
                const wchar_t next = *source++;
                if (!spline.empty() && next != L'p' && next != L'c' && !finishSpline(false)) return false;
                if (next == L'c')
                {
                    if (spline.size() < 3 || !finishSpline(true)) return false;
                    command = 0;
                    continue;
                }
                if (next != L'l' && next != L'b' && next != L's' && next != L'p') return false;
                if (next == L's') spline.push_back(points.back());
                if (next == L'p' && spline.empty()) return false;
                command = next;
            }
            Point a, b, end;
            if (!command || !point(a)) return false;
            if (command == L'l')
            {
                if (!Append(a)) return false;
            }
            else if (command == L'b')
            {
                if (!point(b) || !point(end) || !Cubic(points.back(), a, b, end)) return false;
            }
            else spline.push_back(a);
        }
        return (spline.empty() || finishSpline(false)) && points.size() >= 2 && distances.back() > 0;
    }

    double ArcLength() const { return distances.empty() ? 0 : distances.back(); }

    struct Sample { Point point, tangent; };
    Sample At(double distance) const
    {
        auto next = std::upper_bound(distances.begin(), distances.end(), distance);
        if (next == distances.begin()) ++next;
        if (next == distances.end()) --next;
        const size_t index = next - distances.begin();
        const double length = distances[index] - distances[index - 1];
        const auto tangent = (points[index] - points[index - 1]) * (1. / length);
        return {points[index - 1] + tangent * (distance - distances[index - 1]), tangent};
    }
};
}
