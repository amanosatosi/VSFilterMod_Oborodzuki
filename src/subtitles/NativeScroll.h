#pragma once

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

namespace NativeScroll
{
inline bool Integer(CStringW text, int& value)
{
    text.Trim();
    const wchar_t* cursor = text;
    if (*cursor == L'+' || *cursor == L'-') ++cursor;
    if (*cursor < L'0' || *cursor > L'9') return false;
    for (; *cursor; ++cursor) if (*cursor < L'0' || *cursor > L'9') return false;
    const auto number = std::wcstoll(text, nullptr, 10);
    if (number < INT_MIN || number > INT_MAX) return false;
    value = int(number);
    return true;
}

struct Cue { int start, duration, rows; double distance = 0; };
struct Frame { int offset = 0, top = 0, bottom = 0; bool clipped = false, hidden = false; };

struct Context
{
    std::vector<Cue> cues;
    int direction = -1, visibleRows = 0, top = 0, bottom = 0;
    bool measured = false;

    bool Parse(const std::vector<CStringW>& args, int duration)
    {
        size_t i = 0;
        if (args.empty() || args.size() > 2049) return false;
        if (args[0] == L"ue") ++i;
        else if (args[0] == L"shita" || args[0] == L"sita") { direction = 1; ++i; }
        if (args.size() == i || (args.size() - i) % 2) return false;
        for (; i < args.size(); i += 2)
        {
            Cue cue{0, duration, 0};
            const int sep = args[i].Find(L'|');
            const auto start = sep < 0 ? args[i] : args[i].Left(sep);
            if (!Integer(start, cue.start) || cue.start < 0 ||
                (sep >= 0 && (!Integer(args[i].Mid(sep + 1), cue.duration) || cue.duration < 0)) ||
                !Integer(args[i + 1], cue.rows)) return false;
            cues.push_back(cue);
        }
        return true;
    }

    void Resolve(const std::vector<std::pair<int,int>>& rows)
    {
        if (rows.empty()) return;
        measured = true;
        top = rows.front().first;
        if (visibleRows) bottom = rows[(std::min)(size_t(visibleRows), rows.size()) - 1].second;
        std::vector<double> prefix(1, 0);
        for (const auto& row : rows)
            prefix.push_back(prefix.back() + (std::max)(0., double(row.second) - row.first));
        int cursor = 0;
        for (auto& cue : cues)
        {
            const int next = int((std::max)(0LL, (std::min)(static_cast<long long>(rows.size()),
                static_cast<long long>(cursor) + cue.rows)));
            cue.distance = direction * (prefix[next] - prefix[cursor]);
            cursor = next;
        }
    }

    Frame At(int time, int eventTop) const
    {
        double y = 0;
        for (const auto& cue : cues)
            if (time >= cue.start)
                y += cue.distance * (cue.duration ? (std::min)(1., double(time - cue.start) / cue.duration) : 1.);
        return {int(std::lround((std::max)(-4000000., (std::min)(4000000., y)))),
            (eventTop + top + 4) >> 3, (eventTop + bottom + 4) >> 3, measured && visibleRows != 0};
    }
};
}
