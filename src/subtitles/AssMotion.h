#pragma once

#include <algorithm>
#include <array>
#include <vector>
#include "NumericExpression.h"

namespace AssMotion
{
struct Point { double x, y; };
struct Intent
{
    AssNumeric::Operand x, y;
    int start, end;
    double acceleration;
};

struct Move
{
    AssNumeric::Operand x1, y1, x2, y2;
    int start, end;

    Point At(Point base, int time) const
    {
        const Point first{x1.Resolve(base.x), y1.Resolve(base.y)};
        const Point last{x2.Resolve(base.x), y2.Resolve(base.y)};
        if (time <= start) return first;
        if (time >= end) return last;
        const double t = (double(time) - start) / (double(end) - start);
        return {first.x + (last.x - first.x) * t, first.y + (last.y - first.y) * t};
    }
};

struct Track
{
    static constexpr size_t Limit = 64;
    std::vector<Intent> intents;
    AssNumeric::Operand x{0, true}, y{0, true};
    bool shifted = false;
    bool hasMove = false;
    Move move{};

    void Translate(AssNumeric::Operand dx, AssNumeric::Operand dy)
    {
        const auto move = [](AssNumeric::Operand& current, AssNumeric::Operand offset, bool target) {
            if (!offset.relative) current = offset;
            else if (!target || !current.relative) current.value += offset.value;
        };
        move(x, dx, false);
        move(y, dy, false);
        shifted = true;
        for (auto& motion : intents)
        {
            move(motion.x, dx, true);
            move(motion.y, dy, true);
        }
    }

    Point At(Point base, int time) const
    {
        if (hasMove) base = move.At(base, time);
        Point pos{x.Resolve(base.x), y.Resolve(base.y)};
        if (intents.empty()) return pos;
        std::array<int, 2 * Limit + 1> boundaries;
        size_t count = 0;
        boundaries[count++] = time;
        for (const auto& motion : intents)
        {
            if (motion.start <= time) boundaries[count++] = motion.start;
            if (motion.end <= time) boundaries[count++] = motion.end;
        }
        std::sort(boundaries.begin(), boundaries.begin() + count);
        count = std::unique(boundaries.begin(), boundaries.begin() + count) - boundaries.begin();
        std::array<Point, Limit> targets;
        for (size_t b = 0; b < count; ++b)
        {
            const int start = boundaries[b];
            for (size_t i = 0; i < intents.size(); ++i)
            {
                const auto& motion = intents[i];
                if (motion.start != start) continue;
                targets[i] = {motion.x.Resolve(pos.x), motion.y.Resolve(pos.y)};
                if (motion.end == start) pos = targets[i];
            }
            if (b + 1 == count) break;
            const Point origin = pos;
            for (size_t i = 0; i < intents.size(); ++i)
            {
                const auto& motion = intents[i];
                if (motion.start > start || motion.end <= start) continue;
                const double linear = (double(boundaries[b + 1]) - start) / (double(motion.end) - start);
                const double progress = motion.acceleration == 1 ? linear : std::pow(linear, motion.acceleration);
                pos.x += (targets[i].x - origin.x) * progress;
                pos.y += (targets[i].y - origin.y) * progress;
            }
        }
        return pos;
    }
};
}
