# Actor Colorcoding

Oborodzuki uses ASS Dialogue metadata for Actor Colorcoding and style extensions.
Use these events to share an ASS file between Oborodzuki preview and
Mangetsu encoding. Both renderers intercept the carriers, including any text
outside override blocks. Loading a script preserves its source bytes.

| Field | Preferred identifier | Legacy alias |
| --- | --- | --- |
| Effect | `OBRCC` | `oborodzuki-colorcoding` |
| Effect | `OBRS` | `oborodzuki-style` |
| Actor / Name on an `OBRCC` event | `OBRCCS` | `oborodzuki-colorcode-applied-styles` |

`OBRS` extends the style named in the Style field. `OBRCCS` supplies the style
whitelist for timed actor patches. The last whitelist declaration replaces
the previous one. Style extensions and whitelists are track-wide. The
Dialogue and Comment carriers can be interleaved at the top of `[Events]`.
Dialogue carriers can appear anywhere in `[Events]`. Mangetsu also supports
its original `mangetsu-colorcoding` and `mangetsu-style` Comment block at
the top of `[Events]`, including the `mangetsu-colorcode-applied-styles`
Actor marker.

At each rendered video frame, the last matching actor declaration in
source order whose ASS Start and End bracket the frame wins. Start is
inclusive and End is exclusive. A visible line changes appearance as
declarations begin and expire. When a declaration ends, the next active
one resumes. Each patch supplies only its own properties.
The effective order is the ASS style, extended style defaults, the selected
actor patch, then inline overrides.

When a timed patch is unavailable for the actor or current style, Mangetsu
checks its original global Comment actor defaults and their own whitelist.
Oborodzuki falls back to the active style when no declaration applies.
Comment timestamps still have no effect. Bare `\r` and empty appearance tags
restore effective defaults. Without an `OBRCCS` whitelist, a named `\rStyle`
skips the timed patch. With a whitelist, a named reset reapplies the same
selected patch when the destination style is listed. An empty whitelist
counts as absent.

```ass
Dialogue: 0,0:00:00.00,9:59:59.99,Default,,0,0,0,OBRS,{\furis70}
Dialogue: 0,0:00:00.00,9:59:59.99,Default,OBRCCS,0,0,0,OBRCC,{Default}{Alt}
Dialogue: 0,0:00:00.00,9:59:59.99,Default,NOELLE,0,0,0,OBRCC,{\2bs5\c&HD59B3E&\3c&HFFFFFF&\2bc&H724725&}
Dialogue: 0,0:05:00.00,0:10:00.00,Default,NOELLE,0,0,0,OBRCC,{\c&HAC5CCC&}
Dialogue: 0,0:07:00.00,0:08:00.00,Default,NOELLE,0,0,0,OBRCC,{\c&HFABB41&\1a&H20&}
Dialogue: 0,0:07:30.00,0:08:30.00,Default,NOELLE,0,0,0,,<今日|きょう> မြန်မာနိုင်ငံ
```

The line starting at 07:30 uses the special patch until 08:00, then
changes to the transformation patch while still visible. The normal
appearance resumes at 10:00.

### Filtered CSRI preview

Aegisub can open a fresh CSRI instance with just the Dialogue events
active on the requested frame. The actor declarations needed for that
frame are active by their ordinary ASS times. Track-wide style and
whitelist carriers should span the preview range so the host includes
them in filtered loads. Editing an actor carrier's Start or End changes
the interval immediately when the editor refreshes the frame.
[See Aegisub's frame loading](https://github.com/TypesettingTools/Aegisub/blob/master/src/async_video_provider.cpp#L51-L69).

Rendering comparisons use inline references within each renderer. Text
shaping, rasterization and some paint implementations can produce different
pixels between renderers even when both choose the same appearance.
