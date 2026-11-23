#pragma once

#include <array>

namespace Furigana
{
struct Options
{
    double scaleX = 50, scaleY = 50, spacing = 0;
    double offsetX = 0, offsetY = 0;
    bool enabled = true, manual = false, autoGap = true, outward = false;
    int placement = 0, style = 0;
};

struct Fields
{
    std::array<CStringW, 3> text;
    int length = 0;
};

inline bool HasText(CStringW text)
{
    for (int i = 0; i < text.GetLength(); ++i)
    {
        if (text[i] == L'{')
        {
            const int end = text.Find(L'}', i + 1);
            if (end < 0) return false;
            i = end;
        }
        else if (text[i] != L' ' && text[i] != L'\t')
            return true;
    }
    return false;
}

inline int NextOverride(const CStringW& text, int start)
{
    for (int i = start; i < text.GetLength(); ++i)
    {
        if (text[i] == L'{') return i;
        if (text[i] != L'<') continue;
        int slash = i;
        while (slash && text[slash - 1] == L'\\') --slash;
        if ((i - slash) % 2 == 0) return i;
    }
    return text.GetLength();
}

inline void Unescape(CStringW& literal)
{
    CStringW output;
    for (int i = 0; i < literal.GetLength(); ++i)
    {
        if (literal[i] == L'\\' && i + 1 < literal.GetLength())
        {
            const auto escaped = literal[i + 1];
            if (escaped == L'<' || escaped == L'>' || escaped == L'|' || escaped == L'\\')
            {
                output.AppendChar(escaped);
                ++i;
                if (escaped == L'|' && i + 1 < literal.GetLength() && literal[i + 1] == L'\\') ++i;
                continue;
            }
        }
        output.AppendChar(literal[i]);
    }
    literal = std::move(output);
}

inline Fields Read(const CStringW& source)
{
    Fields fields;
    int side = 0;
    for (int i = 1; i < source.GetLength() && i < 16384; ++i)
    {
        const wchar_t c = source[i];
        if (c == L'{')
        {
            const int end = source.Find(L'}', i + 1);
            if (end < 0) return {};
            fields.text[side].Append(source.Mid(i, end - i + 1));
            i = end;
        }
        else if (c == L'\\' && i + 1 < source.GetLength())
        {
            const auto next = source[i + 1];
            if (next == L'N' || next == L'n') return {};
            if (next == L'|' || next == L'<' || next == L'>' || next == L'\\')
            {
                fields.text[side].AppendChar(next);
                ++i;
                if (next == L'|' && i + 1 < source.GetLength() && source[i + 1] == L'\\') ++i;
            }
            else fields.text[side].AppendChar(c);
        }
        else if (c == L'\n' || c == L'<' || c == L'\r')
            return {};
        else if (c == L'|')
        {
            if (++side >= int(fields.text.size())) return {};
        }
        else if (c == L'>')
        {
            if (side && HasText(fields.text[0]) && (HasText(fields.text[1]) || HasText(fields.text[2])))
                fields.length = i + 1;
            return fields;
        }
        else
            fields.text[side].AppendChar(c);
    }
    return {};
}
}
