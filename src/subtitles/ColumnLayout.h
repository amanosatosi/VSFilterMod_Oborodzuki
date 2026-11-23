#pragma once

#include <memory>
#include <vector>

namespace SemanticColumns
{
enum Field : unsigned
{
    Family = 1u << 0, Size = 1u << 1, Weight = 1u << 2, Italic = 1u << 3,
    Underline = 1u << 4, Strike = 1u << 5, BorderX = 1u << 6, BorderY = 1u << 7,
    ShadowX = 1u << 8, ShadowY = 1u << 9, Gaussian = 1u << 10, Edge = 1u << 11,
    Color = 1u << 12, Alpha = 1u << 16, Decoration = 1u << 20, All = (1u << 21) - 1
};

inline unsigned DefaultFields(const CStringW& tag)
{
    if (tag == L"fn") return Family;
    if (tag == L"fs") return Size;
    if (tag == L"b") return Weight;
    if (tag == L"i") return Italic;
    if (tag == L"u") return Underline;
    if (tag == L"s") return Strike;
    if (tag == L"bord") return BorderX | BorderY;
    if (tag == L"xbord") return BorderX;
    if (tag == L"ybord") return BorderY;
    if (tag == L"shad") return ShadowX | ShadowY;
    if (tag == L"xshad") return ShadowX;
    if (tag == L"yshad") return ShadowY;
    if (tag == L"be") return Edge;
    if (tag == L"blur") return Gaussian;
    if (tag == L"c") return Color;
    if (tag == L"alpha") return 15 * Alpha;
    if (tag.GetLength() != 2 || tag[0] < L'1' || tag[0] > L'5') return 0;
    if (tag[0] == L'5') return tag[1] == L'c' || tag[1] == L'a' ? Decoration : 0;
    return (tag[1] == L'c' ? Color : tag[1] == L'a' ? Alpha : 0u) << (tag[0] - L'1');
}

inline void CopyFields(STSStyle& target, const STSStyle& source, unsigned fields)
{
    if (fields & Family) target.fontName = source.fontName;
    if (fields & Size) target.fontSize = source.fontSize;
    if (fields & Weight) target.fontWeight = source.fontWeight;
    if (fields & Italic) target.fItalic = source.fItalic;
    if (fields & Underline) target.fUnderline = source.fUnderline;
    if (fields & Strike) target.fStrikeOut = source.fStrikeOut;
    if (fields & BorderX) target.outlineWidthX = source.outlineWidthX;
    if (fields & BorderY) target.outlineWidthY = source.outlineWidthY;
    if (fields & ShadowX) target.shadowDepthX = source.shadowDepthX;
    if (fields & ShadowY) target.shadowDepthY = source.shadowDepthY;
    if (fields & Edge) target.fBlur = source.fBlur;
    if (fields & Gaussian)
    {
        target.fGaussianBlurX = source.fGaussianBlurX;
        target.fGaussianBlurY = source.fGaussianBlurY;
    }
    for (unsigned channel = 0; channel < 4; ++channel)
    {
        if (fields & (Color << channel)) target.colors[channel] = source.colors[channel];
        if (fields & (Alpha << channel)) target.alpha[channel] = source.alpha[channel];
    }
    if (fields & Decoration) target.decoration = source.decoration;
}

struct Column
{
    std::unique_ptr<STSStyle> defaults;
    unsigned fields = 0;
    int alignment = 1;
    double spacing = 1;
    bool closed = false, hasAlignment = false;
};

struct Block
{
    STSStyle base;
    std::vector<Column> columns;
    unsigned number, current = 0;

    Block(STSStyle& style, unsigned id) : base(style), columns(1), number(id) {}
    Column& Current() { return columns[current]; }
    void EndCell() { Current().closed |= Current().fields || Current().hasAlignment; }
    void NextCell(STSStyle& style, bool newRow)
    {
        EndCell();
        current = newRow ? 0 : (std::min)(127u, current + 1);
        if (current >= columns.size()) columns.resize(current + 1);
        CopyFields(style, base, All);
        const auto& next = Current();
        if (next.closed && next.defaults) CopyFields(style, *next.defaults, next.fields);
    }

    STSStyle* SpliceDefault(STSStyle& style, unsigned fields)
    {
        auto& cell = Current();
        if (!fields || cell.closed) return nullptr;
        if (!cell.defaults) cell.defaults = std::make_unique<STSStyle>(style);
        cell.fields |= fields;
        return cell.defaults.get();
    }
};
}
