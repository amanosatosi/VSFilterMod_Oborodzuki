VSFilterMod Oborodzuki, based on VSFilterMod by Vmoe Fansub

New override tags: [wiki page](https://github.com/computerfan/VSFilterMod/wiki)

The [KFX paint extensions guide](docs/kfx-paint-extensions.md) covers attached color and alpha
gradients, sliding transforms, and extended color fades.
The [Round 2 guide](docs/mangetsu-extensions-round-2.md) covers native outlines, event boxes,
positioned paint, karaoke, numeric expressions, motion, and text alignment.
The [Round 3 guide](docs/mangetsu-extensions-round-3.md) covers native furigana, vertical and
column layout, curved text, geometric warping, and pattern paint.

Important note: **DO NOT use mod tags in a softsub (ASS file) distribution**, because most players just ignore them, and they may not efficient enough for real-time rendering.

Base branches:
* Masaiki/VSFilterMod: r5.2.7
* sorayuki/VSFilterMod: r5.2.6
* teplofizik/VSFilterMod: @3651264 (Aug 21, 2014)

## GitHub Actions

The **Windows renderer** workflow builds `Release (MOD)` with MSVC v143 for x86 and x64.
Download the artifact for your host's architecture after the DLL loading and CSRI render checks pass.

The **Renderer benchmarks** workflow builds pinned VSFilterMod r5.2.7, pre-optimization
Oborodzuki, the candidate, and Mangetsu in parallel. A single Windows runner downloads the
four renderer artifacts and the compiled benchmark host, then runs the trials sequentially.
The primary fixture is `op.fx.4testing.ass`. The results include the file's SHA-256, per-trial
JSON, build and runner details, and comparison tables. Profiling uses a separate job and artifact.
The supplementary synthetic workloads cover shared ASS features. The legacy Mangetsu path includes the harness's scalar
compositor, while the RGBA path uses Mangetsu's BGRA compositor. Cross-engine pixel output
can differ because the renderers use different typography and rasterization implementations.
The visual regression checks compare the VSFilterMod variants separately.
Development pushes use the iteration suite. Select `full` for a workflow dispatch, or
include `Benchmark-Suite: full` in the message of a development commit that triggers the
workflow. Use the same suite when evaluating changes in render latency.

The [pre-optimization synthetic timings](benchmarks/baseline-44091239735/summary.md)
preserve the results and renderer revisions from
[run 44091239735](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44091239735).

Usage
=====
## Aegisub
Extract VSFilterModOborodzuki.dll into the csri directory of an Aegisub build with CSRI support. Choose the DLL architecture that matches Aegisub. In the Video section of Advanced Options, select `csri/vsfiltermod_oborodzuki`. The CSRI display name is `vsfiltermod oborodzuki`.

## Vapousynth
    vsfm.TextSubMod(clip clip, string file[, int charset=1, float fps=-1.0, string vfr='', int accurate=0])
    vsfm.VobSub(clip clip, string file)

* clip: Clip to process. Only YUV420P8, YUV420P10, YUV420P16 and RGB24 are supported.
* accurate: 1: enable accurate render for 10/16bit (~2x slower). / 0: disable (default)

## MPC-BE
Oborodzuki uses the same DirectShow class IDs as VSFilterMod. Registering Oborodzuki replaces the existing registration for those classes.
1. run `regsvr32.exe VSFilterModOborodzuki.dll` with administrator privileges
2. Select "VSFilter/xy-VSFilter" on the select of Options/Subtitles/Subtitle renderer

Knowing Issues
=====
* Opentype font (such as Source Han Sans) has a much smaller size when displayed vertically (used like @Source Han Sans). (subtitle renders which origin from VSFilter use GDI to render fonts, but GDI performs badly on opentype fonts.)

[Actor Colorcoding and shared ASS metadata](docs/actor-colorcoding.md)
