#pragma once

#include <memory>
#include <vector>

namespace ShapedText
{
struct Outline
{
    std::vector<POINT> points;
    std::vector<BYTE> types;
};

struct Cluster
{
    unsigned start = 0, length = 0;
    float left = 0, advance = 0;
    bool whitespace = false, breakAfter = false;
    std::vector<Outline> glyphs;
};

struct Run
{
    float ascent = 0, descent = 0, width = 0;
    std::vector<Cluster> clusters;
};

// The returned outline and ownership data have no DirectWrite references.
// They may be read by any raster worker after shaping completes.
class Context
{
    struct Impl;
    std::unique_ptr<Impl> native;
public:
    Context();
    ~Context();
    std::shared_ptr<const Run> Shape(const wchar_t* text, unsigned length, const wchar_t* family,
        float em, int weight, bool italic, bool verticalVariants);
};
}
