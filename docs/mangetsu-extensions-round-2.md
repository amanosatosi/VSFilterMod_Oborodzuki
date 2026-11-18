# Mangetsu extensions in Oborodzuki

This branch extends the [Round 1 paint system](kfx-paint-extensions.md). Its
semantic reference is Mangetsu `60e06ef0d4535c5403f446fd46b996df4d0d2560`.
All tags below can be written in ordinary ASS dialogue overrides.

## Native outline rings

Use `N` from 1 through 10 in the following families.

| Property | Tags |
| --- | --- |
| Thickness | `\Nbs`, `\Nbsx`, `\Nbsy` |
| RGB and inverse alpha | `\Nbc`, `\Nba` |
| Gaussian and edge blur | `\Nbblur`, `\Nbbe` |
| Four-corner paint | `\Nbvc`, `\Nbva` |
| Attached stops | `\Nbgrd`, `\Nbga` |
| Positioned RGB | `\Nbpgrd` |
| Slide settings | `\Nbgrds`, `\Nbgas`, `\Nbvcs`, `\Nbvas`, `\Nbpgrds` |
| Waiting karaoke RGB | `\Nbsc` |

`\bord5\2bs3\3bs2` produces outer extents of 5, 8 and 10 script pixels
before scaling. The rasterizer subtracts each inner silhouette before
applying ring-specific filtering. `\3a` controls every native ring.
`\1ba` controls the first ring. Bare blur tags clear that layer's filter
override, so it inherits the event blur again. The shadow uses the complete
outer silhouette. The normal transform and layer/read order apply.

~~~ass
{\bs5\bord5\2bs3\2bgrd(30,$white,$black)\2bblur1.5\3bs2\3bc&HFF0000&}Three rings
{\bord2\2bs5\t(0,1000,\2bs~+3\2bgrd(90,$black,$white))}Animated outward ring
~~~

`\bs1` selects the ordinary outline, `\bs3` the existing opaque box,
`\bs4` a transformed event box and `\bs5` geometric outlines with a 2×
miter limit and bevel fallback. The first valid inline `\bs` selects the
event override. Styles can also select BorderStyle 4 or 5.

## Event boxes

`\box1` and `\box0` enable and disable the event box. `\boxr` sets its
corner radius. `\boxp`, `\boxpx` and `\boxpy` set padding in script units.
Tuple order is `(left,right,top,bottom)`, `(left,right)` or `(top,bottom)`.
Fill uses the fourth paint channel and the first visible word's geometry state.
Native shapes and their outward rings share the same local transform and clip.

Box borders use `\Nbbs`, `\Nbbc`, `\Nbba`, `\Nbbvc`, `\Nbbva`,
`\Nbbgrd`, `\Nbbga`, `\Nbbgrds`, `\Nbbgas`, `\Nbbvcs` and
`\Nbbvas`. Their unnumbered forms address ring 1.

~~~ass
{\an5\pos(640,360)\bs4\boxp(8,18,10,12)\boxr10\4grd(45,$black,$white)\bbs4\2bbs6\2bbc&H00FF00&\frz25}Event box
~~~

## Paint, positioned masks and karaoke

- `\5c`, `\5a`, `\5grd`, `\5gra`, `\5grds` and `\5gras` paint
  underline and strikeout independently of the glyphs. Unspecified components
  inherit the matching glyph paint. Geometry comes from the selected GDI font.
- `\pgrd`, `\1pgrd` through `\5pgrd` and `\Nbpgrd` sample a fixed
  script-space rectangle. Outside it, the target uses its fallback RGB.
  `\pgrds`, `\1pgrds` through `\5pgrds` and `\Nbpgrds` set the
  matching slide width.
- `\pgrdalpha` and `\pgrdalphas` control a positioned opacity field.
  Non-box draws sharing a field use ordered scanline compositing. Box
  fills and rings keep their ordinary opacity. Existing RGB fades also
  apply to the new surfaces.
- `\3sc`, `\3svc` and `\3sgrd` select waiting paint on the first
  outline. `\3svcs` and `\3sgrds` select its slide width. Higher rings
  use flat `\Nbsc` waiting colors. An uppercase `\kO` reveals a
  segment at its karaoke start. Lowercase `\ko` retains outline suppression.

~~~ass
{\move(100,300,500,300)\pgrd(200,260,480,400,0,$white,$black)}Moving through stationary color
{\bord5\3sc&HFF0000&\2bs3\2bsc&H00FF00&\kf80}Waiting outline paint
{\u1\5grd(0,&H0000FF&,&HFF0000&)\5a$160}Independent decoration paint
~~~

## Numeric arguments, positioning and text alignment

`~+` and `~-` resolve against the current argument state. Relative screen
position uses rightward-positive X and upward-positive Y. Explicit relative
rectangular clip corners add in screen coordinates. `\clippos` translates
the active rectangle or vector mask, including in a transform.

Legacy signed literals on established scalar tags retain their VSFilterMod
interpretation. Use `\fs~+10` for additive font size. `\fsc` now stores
an independent uniform glyph multiplier, so changing `\fscx` or
`\fscy` leaves that multiplier active. `\scale` also scales local
geometry such as native thickness, shadow offsets, Gaussian blur and box
padding. Script coordinates and times keep their original units.

Supported numeric arguments accept bounded decimal arithmetic, unary signs,
parentheses and the variables `$start`, `$mid`, `$end`, `$dur`, `$width`,
`$height`, `$left`, `$right`, `$center`, `$top`, `$bottom`, `$middle`.
Timing percentages work in `\t`. Geometry variables use static event-start
layout before collision displacement. Named colors and `$` decimal alpha
work in the ordinary paint tags. Unicode decimal digits are accepted in
the extension operands and ASS decimal file fields handled by Oborodzuki.
Hexadecimal color and alpha remain ASCII. Existing `\fshp` is retained.

`\pos` inside `\t` generates a motion intent. Overlapping intents rebase
at each start or end boundary, so seeking gives the same position as playback.
`\tan` selects the text's horizontal anchor. `\ta` aligns a logical line
within its event block. The first valid `\ta` on a line applies to that
line and the latest valid value carries across subsequent `\N` breaks.

~~~ass
{\pos(500,500)\t(0,500,\pos(200,500))\t(250,800,\pos(~+350,~+100))}Competing position transforms
{\bord($height/20)\grds($width/2)\t(0,25%,\fscx120)}Layout and timing expressions
{\ta7}First line{\ta3}\NSecond line\NThird line
~~~

## Compatibility limits

- The parser caps expressions at 256 characters, 64 instructions and 16
  nesting levels. Native cumulative radii for separated masks are bounded at
  256 output pixels per axis. An event can create up to 32 positioned-opacity
  scopes, and a position track can contain up to 64 intents.
- Existing Lua pixel callbacks own their custom paint. They run on the
  calling thread, and custom pixel handlers and path transforms retain their
  established behavior. Layout-dependent arithmetic has no static layout
  measurement when external Lua is active.
- Relative and arithmetic support covers standard movement, rectangular
  clipping, typography, effects and the new numeric paint properties. The
  established radial and spline motion tags retain their VSFilterMod operands.
- Furigana, vertical layout, curved text, columns, chat, new scrolling,
  new projective geometry and metadata transport remain outside this port.

The Windows Actions suite checks selected analytic and Mangetsu pixels on
equivalent drawings, actual decorated text, backward seeks, and exact
Oborodzuki hashes across 1, 2, 4, 6 and 8 workers.
