# KFX paint extensions

VSFilterMod Oborodzuki supports attached RGB and inverse-alpha gradients,
sliding paint transforms, and RGB fades on ordinary inline ASS dialogue.

| Paint surface | RGB | Inverse alpha |
|---|---|---|
| Primary fill | \grd, \1grd | \1gra |
| Secondary karaoke fill | \2grd | \2gra |
| First outline | \3grd | \3gra |
| Shadow | \4grd | \4gra |

Use an angle in degrees, first and last values, and up to 62 additional
position/value pairs. Positions use percentages in nondecreasing order.
Repeated positions create a sharp change. RGB values use ASS BGR notation.
The shortcuts $white, $shiro, $siro, $black, and $kuro are case-insensitive.
Alpha accepts ASS hexadecimal bytes or decimal bytes prefixed by $.
00 is opaque and FF is transparent.

~~~ass
{\grd(30,&H0000FF&,45%,&H00FF00&,45%,&HFF0000&,$white)}Repeated stop
{\1grd(0,$white,$black)\1gra(90,&H00&,$255)\3grd(0,$black,$white)}Independent RGB and alpha
{\grd(350,&H0000FF&,&HFF0000&)\t(0,1000,2,\1grd(10,&HFF0000&,60%,$white,&H0000FF&))}Animated angle and stops
~~~

Angles use the final screen axes. 0° points right and 90° points down.
The final rendered bounds of each paint segment supply the gradient domain.
Clipping and karaoke select coverage within that domain. A paint segment can
span font changes and line breaks. Separate inline gradient declarations
start separate segments.

Ordinary transforms use ASS timing and acceleration. Color and alpha are
interpolated independently. Angles take the shortest arc, and different stop
lists are evaluated at their sorted union. A solid endpoint contributes the
same value at each stop. Empty gradient parentheses and inline 0 clear the
matching source. Reset tags and solid replacements affect their own paint
component. A valid legacy \1vc–\4vc or \1va–\4va declaration replaces the
matching extended source while retaining the established corner sampler.
The normal style reset \r restores the style.

## Sliding paint

\grds sets the default slide width in script-resolution pixels. \1grds–\4grds
and \1gras–\4gras override it for each color or alpha source. The same settings
are available through \1vcs–\4vcs and \1vas–\4vas, with \vcs and \vas for
the first channel. Width 0 selects a sharp wipe. The suffix off disables
sliding at that specificity. A bare setting or empty parentheses clears it,
so a channel can inherit the global setting again. Settings within a transform
are scoped to that transform. Sliding is off by default.

~~~ass
{\grds20\1grd(0,&H0000FF&,&H00FF00&)\t(0,1000,\1grd(90,&HFF0000&,$white))}Sliding gradient
{\grds20\1grds0\1gra(0,&H30&,&H00&)\1grasoff\t(0,1000,\1grd(45,$white,$black))}Sharp RGB wipe
{\1grd(45,$white,$black)\1grds12\t(0,1000,\1c&H0000FF&)}Wipe to solid
~~~


The global `\grdsd` sets the wipe direction independently of its width and
the orientation of either gradient. It accepts finite signed decimal degrees.
`0` moves right, `90` down, `180` left, and `270` up. Equivalent angles wrap
around, so `-90` moves up. A bare `\grdsd` restores the active style or actor
default and otherwise uses the automatic gradient-derived direction.
The tag takes its number directly. A parenthesized argument has no effect.

Each wipe keeps the direction selected at its construction. Inside `\t`
the setting applies to subsequent tags in that transform, and the direction
itself stays fixed. Set a width with `\grds` or a compatible channel-specific
setting to enable a wipe. `\grds0` still gives a sharp edge.

```ass
{\grds20\grdsd180\1grd(45,&H0000FF&,&H00FF00&)\t(0,1000,\1grd(45,&HFF0000&,&HFFFFFF&))}Right-to-left wipe
{\grds20\grdsd180\grdsd\1grd(45,&H0000FF&,&H00FF00&)\t(0,1000,\1grd(45,&HFF0000&,&HFFFFFF&))}Inherited or automatic direction
```

Each endpoint retains its paint field. Automatic direction follows the
destination angle or the source angle for a solid destination. The moving
transition band starts outside the surface and ends beyond it, preserving
the exact endpoints even for large widths. Ordinary solid-to-solid
transforms retain standard ASS interpolation. The limits are 64 stops per
field and 32 simultaneously resolved slide steps per component.

## Extended color fades

~~~ass
{\fad(300,300,&HFFFFFF&,&H000000&)}Fade from white and toward black
{\fad(300,300,,&H000000&)}Classic alpha fade-in and black RGB fade-out
{\fad(300,300,&HFFFFFF&,)}White RGB fade-in and classic alpha fade-out
{\fad(300,300,&HFFFFFF&+a,&H000000&+a)}RGB and alpha on both sides
~~~

RGB fades mix the final color of the sampled fill, karaoke fill, outline, or
shadow before compositing. An empty or invalid color uses classic alpha
fading on that side. +a combines RGB and alpha. Two-argument \fad and
seven-argument \fade keep their legacy behavior.

## Rendering and scope

Extended paint is immutable once assigned to a word. Each event collects
full-mask segment bounds before clipping. The existing compositor then
samples the same coordinates in any row band and uses the normal coverage,
clipping, ordering, and blend operations. Lua pixel callbacks keep ownership
of their custom draw operations and execute on the calling thread.

This pack covers the four paint surfaces in the table. Positioned gradients,
arithmetic expressions, style and actor metadata, extra borders and boxes,
a fifth decoration channel, and other layout extensions are planned for
later packs.

The semantic reference is Mangetsu 60e06ef. [Gradient targets and slides](https://github.com/amanosatosi/libassmod/blob/60e06ef0d4535c5403f446fd46b996df4d0d2560/docs/gradient-support.md)
[RGBA sampling](https://github.com/amanosatosi/libassmod/blob/60e06ef0d4535c5403f446fd46b996df4d0d2560/docs/rgba-rendering.md)
[Color fades](https://github.com/amanosatosi/libassmod/blob/60e06ef0d4535c5403f446fd46b996df4d0d2560/docs/fad-color-fade.md)

## Validated Round 1 checkpoint

The renderer in `4c0c034` rendered all 5,400 frames of the unchanged 1080p60
KFX script at 1, 2, 4, 6 and 8 workers with zero RGB differences against the
validated `20c6174` renderer. The two implementations ran alternating trials at
1, 2 and 4 workers on the same Windows runner with two physical cores and four
logical processors. Values below average the two trials. Six and eight workers
were tested for correctness. [Full acceptance run](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44218018689)

| Workers | Mean ms before → after | Median ms before → after | p95 ms before → after | FPS before → after | CPU s before → after | Peak MiB before → after | Efficiency before → after |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 9.384 → 9.340 | 8.943 → 8.894 | 20.394 → 20.238 | 106.5 → 107.1 | 51.52 → 50.41 | 32.9 → 32.9 | 100% → 100% |
| 2 | 5.441 → 5.445 | 5.106 → 5.102 | 11.154 → 11.224 | 183.8 → 183.7 | 51.69 → 50.86 | 35.6 → 34.8 | 86.2% → 85.8% |
| 4 | 4.725 → 4.724 | 4.451 → 4.442 | 9.440 → 9.463 | 211.7 → 211.7 | 75.91 → 76.06 | 38.4 → 38.8 | 49.7% → 49.4% |

Neither four-worker candidate trial had a frame over 16.67 ms. The full
workflow artifact includes per-frame timings and hashes, p99, worst frames
and slow-frame counts. Windows x86/x64 smoke tests and the standard renderer
benchmark regression checks also passed. [Windows builds](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44218018725)
[Standard renderer checks](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44218018873)

The `f81f31a` checkpoint has identical renderer source and verifies requested
worker settings in the focused paint benchmark. Selected vector-drawing pixels
matched analytic expectations and the pinned Mangetsu samples. Oborodzuki hashes
matched at all five worker settings and after backward seeks. [Focused verification](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44218421620)

On the 20-event vector-clipped paint workload, the median of three mean
latencies was 53.223, 29.071 and 23.374 ms with the new effects at 1, 2 and 4
workers. The solid-paint control measured 30.888, 17.391 and 14.347 ms.
Instrumented runs attributed most of the paint-specific cost to per-pixel
projection and stop sampling. Vector-mask sample counts stayed the same in
both variants. The unchanged 600-frame KFX profiling window recorded zero
extended paint bounds or stop evaluations and 23,280 text-metric cache hits
out of 23,420 requests at each worker count.
