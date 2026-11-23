#pragma once

#include "NativeScroll.h"

namespace NativeChat
{
struct Appearance
{
    int bubbleColor = -1, bubbleAlpha = -1, borderColor = -1, borderAlpha = -1;
    int outlineColor = -1, outlineAlpha = -1;
    double border = 0, outline = -1;
};

struct Message
{
    CStringW name;
    Appearance skin;
    COLORREF colors[4]{};
    BYTE alpha[4]{};
    bool right = false, showName = true;
    int receipt = 0, readDelay = 0, top = 0, bottom = 0, start = 0;
};

struct Boundary { CStringW name; int side = 0; };

struct Scene
{
    int mode = 0, initial = 0, slide = 250, receipt = 0, readDelay = 0;
    int width = 0, textWidth = 0, viewport = 0, headerHeight = 0, pad = 0;
    int headerColor = -1, headerBackground = -1;
    bool hasTimes = false, gotTitle = false, showNames = true, receiptConfigured = false;
    CStringW title, main;
    Appearance skin;
    std::vector<int> times, reads;
    std::vector<Message> messages;
    std::vector<Boundary> boundaries;
    size_t nextBoundary = 0;
    unsigned current = 0, previousWord = 0;
    std::array<std::unique_ptr<std::array<double,12>>,2> presets;
    std::unique_ptr<STSStyle> beforePreset;
    Appearance beforeSkin;

    int AppearanceTime(size_t message) const
    {
        if (!hasTimes || int(message) < initial) return 0;
        const auto at = message - initial;
        return at < times.size() ? times[at] : INT_MAX;
    }

    std::vector<NativeScroll::Frame> Frame(int time, int y) const
    {
        int bottom = 0;
        for (size_t i = 0; i < messages.size(); ++i)
        {
            const int start = messages[i].start;
            if (start > time) continue;
            const int animated = hasTimes && int(i) >= initial && slide > 0 ?
                int((std::min)(1., double(time - start) / slide) * (messages[i].bottom - messages[i].top)) :
                messages[i].bottom - messages[i].top;
            bottom = (std::max)(bottom, messages[i].top + animated);
        }
        const int shift = (std::max)(0, bottom - headerHeight - viewport);
        std::vector<NativeScroll::Frame> result;
        for (size_t i = 0; i < messages.size(); ++i)
        {
            const auto& message = messages[i];
            const bool hidden = message.start > time;
            const bool enters = !hidden && hasTimes && int(i) >= initial && slide > 0;
            const int rise = enters ? int((1 - (std::min)(1., double(time - message.start) / slide)) *
                (message.bottom - message.top)) : 0;
            const int viewBottom = enters && rise ? (std::min)(headerHeight + viewport, message.bottom - shift) :
                headerHeight + viewport;
            result.push_back({rise-shift, (y + headerHeight + 4) >> 3,
                (y + viewBottom + 4) >> 3, true, hidden});
        }
        return result;
    }
};

inline CStringW LazyMessages(const CStringW& source, Scene& scene)
{
    CStringW result, speaker;
    int cursor = 0;
    while (cursor < source.GetLength() && scene.boundaries.size() < 128)
    {
        const auto c = source[cursor];
        if (c == L'{')
        {
            const int end = source.Find(L'}', cursor + 1);
            if (end < 0) break;
            result += source.Mid(cursor, end - cursor + 1);
            cursor = end + 1;
            continue;
        }
        if (c != L'|') { ++cursor; continue; }
        int end = cursor + 1, delimiter = -1;
        for (; end < source.GetLength(); ++end)
        {
            const auto ch = source[end];
            if (ch == L'{' || ch == L'<')
            {
                const int close = source.Find(ch == L'{' ? L'}' : L'>', end + 1);
                if (close >= 0) { end = close; continue; }
            }
            if (ch == L'\\' && (source[end + 1] == L'|' || source[end + 1] == L':'))
            {
                ++end;
                if (source[end] == L':' && source[end + 1] == L'\\') ++end;
                continue;
            }
            if (ch == L'|') break;
            if (delimiter < 0 && ch == L':' && source.Mid(end + 1, 2) == L"\\N")
                delimiter = end;
        }
        if (end == source.GetLength()) break;
        auto body = source.Mid(cursor + 1, end - cursor - 1);
        while (!body.IsEmpty() && body[0] == L'{')
        {
            const int close = body.Find(L'}');
            if (close < 0) break;
            result += body.Left(close + 1);
            body = body.Mid(close + 1);
            cursor += close + 1;
        }
        if (delimiter >= 0)
        {
            const int offset = delimiter - cursor - 1;
            if (offset >= 0 && offset + 3 <= body.GetLength())
            {
                speaker = body.Left(offset).Trim();
                speaker.Replace(L"\\:\\", L":");
                speaker.Replace(L"\\|", L"|");
                body = body.Mid(offset + 3);
            }
        }
        else if (body.Left(2) == L"\\N") body = body.Mid(2);
        scene.boundaries.push_back({speaker, 0});
        result += L"{\\msgmarker}";
        body.Replace(L"\\:\\", L":");
        result += body;
        cursor = end + 1;
    }
    return result;
}
}
