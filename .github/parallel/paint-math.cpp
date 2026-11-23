#include "../../src/subtitles/PaintExtensions.h"
#include <cassert>

using namespace KfxPaint;

static Paint Field(double angle, std::initializer_list<Stop> stops)
{
    Paint paint;
    paint.kind = Paint::Kind::Field;
    paint.angle = angle;
    paint.stops = stops;
    return paint;
}

int main()
{
    const auto hard = Field(0, {{0, 0xff0000}, {.5, 0x00ff00}, {.5, 0x0000ff}, {1, 0xffffff}});
    assert(SampleStops(hard, .5, false, true) == 0x00ff00);
    assert(SampleStops(hard, .500001, false, true) == 0x0000ff);

    const auto source = Field(350, {{0, 0}, {.25, 200}, {1, 100}});
    const auto target = Field(10, {{0, 100}, {.75, 40}, {1, 0}});
    const auto middle = Interpolate(&source, target, 0, .5, true);
    assert(middle.angle == 0);
    assert(middle.stops.size() == 4);
    assert(middle.stops[0].value == 50);
    assert(middle.stops[1].position == .25 && middle.stops[1].value == 140);
    assert(middle.stops[2].position == .75 && middle.stops[2].value == 86);
    assert(middle.stops[3].value == 50);
    assert(Mix(0, 255, .5, true, true) == 128);
    assert(Mix(0, 255, .5, true, false) == 127);

    const Rect bounds{10, 20, 210, 120};
    Paint wipe;
    wipe.kind = Paint::Kind::Slide;
    wipe.from = std::make_shared<const Paint>(Field(0, {{0, 0}, {1, 200}}));
    wipe.to = std::make_shared<const Paint>(Field(90, {{0, 200}, {1, 0}}));
    wipe.angle = 90;
    wipe.width = 0;
    wipe.progress = .5;
    wipe.depth = 1;
    Sampler sampler;
    sampler.Prepare(&wipe, bounds, 1, 1, true);
    assert(sampler.At(60, 45, 0, 0) == 150);  // Destination uses the vertical field.
    assert(sampler.At(60, 95, 0, 0) == 50);   // Source keeps its horizontal field.
}
