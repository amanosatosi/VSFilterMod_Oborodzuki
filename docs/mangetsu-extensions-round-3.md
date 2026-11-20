# Mangetsu layout, geometry and pattern paint

These overrides run in VSFilterMod Oborodzuki through its normal subtitle
hosts. The behavioral reference is Mangetsu revision
[`60e06ef0d4535c5403f446fd46b996df4d0d2560`](https://github.com/amanosatosi/libassmod/tree/60e06ef0d4535c5403f446fd46b996df4d0d2560).
The comparison baseline is the validated Round 2 checkpoint
[`ddaec61`](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/commit/ddaec6149e323f8d0d690e60af0e7ea8a6ad1acc).

## Native annotation and layout

| Overrides | Behavior |
| --- | --- |
| `<base\|reading>`, `<base\|reading\|reverse>`, `<base\|\|reverse>` | Attach one or two independently shaped readings to the base. One or two unescaped separators outside override blocks identify an annotation. |
| `\furi0`, `\furi1` | Disable or enable annotation parsing. Parsing starts enabled. |
| `\furis`, `\furisx`, `\furisy`, `\furifsp` | Set reading size as a percentage of base size or set reading-only spacing. Both axes default to 50%. |
| `\furipos(x,y)`, `\furiap0`, `\furiap1`, `\furiplaceauto0`, `\furiplaceauto1` | Offset a reading in screen axes, control the additional automatic gap, or place readings outward when two visual horizontal lines exist. |
| `\furichangepos0`, `\furichangepos1`, `\furichangepos2`, `\furistyle0`–`2` | Select compact, reserved or strict separation and choose free overhang or size-to-fit reading placement. |
| `\vert0`, `\vert1`, `\vtype0`–`3`, `\vdir0`–`2`, `\vsp`, `\vcolsp` | Select native vertical flow, mixed-script or explicit spacing policy, column direction and spacing. Set these before the first text character. |
| `\col0`, `\col1`, `\colan`, `\colsp` | Align event-local cells separated by a pipe. Explicit `\N` starts another row. Empty cells retain their column. |
| `\ct(path)`, `\ctan`, `\ctx`, `\cty` | Lay shaped text along an ASS drawing path by arc length and animate the offsets with ordinary `\t`. |

Horizontal readings go above and below their base. In native vertical text the
ordinary reading goes to the physical right and the reverse reading to the
physical left. Direction changes move subsequent columns while preserving
those sides. A bare `\furipos` returns to automatic clearance. `\r` resets the
group controls. `\|\` inside a candidate produces an ordinary pipe. Internal
karaoke tags in the readings use the event clock, with the first nonempty
reading assigning timing to base regions. Ordinary outer karaoke applies when
there is no internal timing.

Column mode measures each cell's shaped advances and ink. The first styled
cell for an index supplies its visual defaults. Later rows can use local
overrides and the next row returns to the stored defaults. Alignment follows
the horizontal part of the ASS numpad. Spacing is at least one script pixel.
Karaoke tags inside the table leave the timing and paint unchanged.

Curved text uses a valid path from the event and the existing `\ta` and
`\tan` line alignment settings. Invalid paths use regular layout. Native
vertical text, semantic columns, annotations, banner and scroll each take
precedence over the curve. `\t` supports offset motion.

```ass
{\an8\furis70\furichangepos2}<satou|ဆာတို့|佐藤>-<san|စံ>\N<suzuki|စူဇူကီ|鈴木>-<kun|ကွန်း>
{\vert1\vdir1}<漢字|かんじ|カンジ>\Nမြန်မာနိုင်ငံ
{\col1}HP|{\colan5}:|{\colan6}504\NMP|:|80\NAttack|:|1200{\col0}
{\an5\pos(960,540)\ct(m -360 0 b -220 -130 220 -130 360 0)\t(0,1000,\ctx80)}မြန်မာစာ သင်္ချာ
```

## Geometry

| Override | Behavior |
| --- | --- |
| `\mpersp(u0,v0,u1,v1,u2,v2,u3,v3)` | Apply event-wide projective corner pins in top-left, top-right, bottom-right, bottom-left order. |
| `\mpersp()` | Restore identity. Style resets preserve the active corner pins. |
| `\distort(u1,v1,u2,v2,u3,v3)` | Use the VSFilterMod three-point bilinear warp. |
| `\distort(u1,v1,u2,v2,u3,v3,u0,v0)` | Add a top-left pin and allow same-style runs to share a multiline outline domain. Both forms animate with `\t` and support explicit relative operands. |
| `\wtan1`–`\wtan9` | Place the whole warped text block using its outline bounds and the ASS numpad. |

Perspective uses normalized pins relative to the event's current layout
rectangle. It runs after the authored outline transforms. Invalid or crossed
quads and projections that exceed the path or mask budget use the ordinary
geometry for that frame. The event retains its pre-projection alignment and
collision rectangle. Screen-space clipping and shadow displacement use their
existing coordinates. A switch back to six distortion arguments restores
line-local grouping and interpolates the top-left pin toward zero.

```ass
{\an5\pos(960,540)\mpersp(0,.1,1,0,.85,1,.1,1)}Projective text
{\an5\pos(960,540)\wtan8\distort(1,-.1,1.2,1.1,-.1,1,.1,.05)}FIRST\NLONGER SECOND LINE
{\an5\pos(960,540)\bord4\2bs3\1grd(45,$white,$black)\fad(300,300,$white,$black+a)\mpersp(0,0,1,.05,.9,1,.1,1)}<漢字|かんじ>
```

## Pattern paint

| Overrides | Behavior |
| --- | --- |
| `\cyc`, `\1cyc`, `\2cyc`, `\scyc`, `\3cyc`, `\Nbcyc` | Start a 1–32 color palette on the corresponding fill, outline or native outline ring. Mode 1 advances per shaped cluster. Mode 2 advances per output glyph. |
| `\polc`, `\pols`, `\polsp` and their 1–3 prefixed forms | Set dot color, diameter and center spacing. Omitted spacing is 2.5 times the diameter. |
| `\Nbpc`, `\Nbps`, `\Nbsp` | Set dot color, diameter and spacing on native outline ring N. |
| `\zpol`, `\zpol1`, `\zpol0` | Enable or disable propagation from primary fill. A target's explicit setting takes precedence for each property. |

Spaces and line breaks leave the palette index unchanged. A new palette
starts at its first color. Later RGB source changes replace the affected
palette. Alpha, clipping and polka-dot sampling remain independent of the
palette. Readings inherit the first associated base cluster's color.
Palette declarations inside transforms are ignored. Dot size, spacing and
color interpolate through `\t`. Dots cover the surviving mask pixels and
share the existing extended RGB fade and blending modes. Drawings, shadows
and background boxes are outside pattern paint. N ranges from 1 through 10.

```ass
{\cyc(1,&H0000FF&,&H00FF00&,&HFF0000&)}ABCDEF
{\bord4\2bs6\polc&HFF80C0&\pols8\polsp20\zpol1\2bpc&HFFFFFF&}Polka
```

## Rendering and compatibility

The native layout path uses the Windows DirectWrite glyph runs, cluster maps,
font fallback and outline paths. Oborodzuki retains the resulting immutable
geometry in a bounded per-renderer cache. The native features share the
existing outline, blur, clip, fade and row-band compositor. Plain ASS text
continues through the established GDI path. The dynamic DirectWrite loading
preserves renderer startup on hosts where it is unavailable.

- DirectWrite and Mangetsu's HarfBuzz pipeline can choose different fallback
  glyphs, cluster boundaries and ink extents. The tests compare layout and
  paint behavior across renderers and require exact worker-count agreement
  within Oborodzuki.
- Native shaping uses the DirectWrite system font collection. Fonts registered
  privately through the host's GDI path can resolve differently in native
  layouts.
- Outline bounds are measured before final authored transforms. Region
  boundaries for timed base annotations use axis-aligned clips. Strong
  3D or projective transforms can move those boundaries relative to the ink.
  Curved `\kf` follows the local path tangent and authored Z rotation. Its
  split still uses a screen-space line under 3D or perspective.
- Glyph-mode cycling advances for every output glyph in an annotation base.
  A multi-glyph base cluster currently paints with its first glyph's color.
  Ordinary glyph-mode text paints each output glyph separately.
- Perspective falls back with scrolling, banners, outline randomization or
  Lua outline hooks. Legacy `\fn@FontName` layout remains available with
  native vertical mode disabled.
- The annotation parser has bounded input and timing budgets. Layout and
  projection use finite-coordinate and mask-area limits. Native chat,
  multiline scrolling and metadata transport are outside this feature pack.

## Verification

The Windows workflow exercises real pixels and geometry for Japanese and
Myanmar annotations, column defaults, vertical flow, curved text, projective
animation, multiline bilinear distortion, palette cycling and dot paint.
Focused fixtures include reverse playback and exact RGB hashes across 1, 2,
4, 6 and 8 workers. The baseline fixture remains
`.github/ci/op.fx.4testing.ass` at 1920×1080 and 60 fps. Each acceptance pass
renders all 5,400 consecutive frames. Both renderer revisions run on the same
runner in alternating order at 1, 2 and 4 workers. Six and eight workers
are additional correctness checks on the four-logical-processor runner.

[Focused Round 3 checks](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44354773008) ·
[Round 3 acceptance and profiles](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44356341316) ·
[Validated Round 2 checkpoint](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44317693536)

### Instrumented feature workloads

The following 120-frame workloads contain 12 simultaneous animated text
events. The control uses ordinary text with the same motion, stroke, shadows
and script. The native layout mix adds annotations, vertical text, curved
text and perspective on separate events. Pattern paint adds per-glyph
cycling and animated dots. These are instrumented measurements.

| Workload | Median ms at 1 / 2 / 4 workers | p95 ms at 1 / 2 / 4 workers | 4-worker CPU s | 4-worker peak MiB |
| --- | ---: | ---: | ---: | ---: |
| Layout control | 10.124 / 5.527 / 4.426 | 10.432 / 5.723 / 4.714 | 1.875 | 30.0 |
| Native layout mix | 16.966 / 9.293 / 7.769 | 17.629 / 10.664 / 8.306 | 3.328 | 32.9 |
| Pattern paint | 20.697 / 11.232 / 9.912 | 21.816 / 12.662 / 10.352 | 4.500 | 34.3 |

The warmed native layout mix reused all 2,160 shaping requests per pass.
At one worker, the profiler attributed 1.398 s to outline widening,
0.206 s to scan conversion, 0.111 s to compositing, 0.064 s to projection
and 0.013 s to native layout. Pattern paint widened 21,600 glyph masks
and spent 1.433 s widening outlines and 0.576 s compositing them. The
control widened 1,440 masks and spent 0.859 s widening and 0.052 s
compositing. These are exclusive stage totals.

The separate 600-frame ordinary KFX profile made zero native shape,
layout or projection calls at 1, 2 and 4 workers. Each setting recorded
23,280 hits in the established text-measure cache.
[Profile artifact](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44356341316/artifacts/14452717436)
