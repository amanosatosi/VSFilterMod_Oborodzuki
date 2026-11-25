"""Shared fixtures for CI and the portable renderer benchmark."""


def sampled_fixtures(source, output):
    shared = source.read_text(encoding="utf-8")
    animated = output / "animated.ass"
    animated.write_text(shared + (
        r"Dialogue: 1,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
        r"{\move(450,360,1440,680)\clip(200,200,1700,920)\t(\frz30\blur2)\fad(100,180)}"
        r"{\kf90}Moving {\kf140}lyrics {\kf170}and effects"
        "\n"
    ), encoding="utf-8")
    heavy = output / "heavy.ass"
    heavy.write_text(shared + "".join(
        f"Dialogue: {i + 1},0:00:01.00,0:00:05.00,Default,,0,0,0,,"
        rf"{{\move({180 + i * 65},{120 + i * 31},{340 + i * 65},{260 + i * 31})"
        rf"\bord4\blur3\alpha&H20&\t(\frz25)}}{{\kf100}}Heavy {{\kf300}}karaoke {i + 1}"
        "\n" for i in range(24)
    ), encoding="utf-8")

    # Keep renderer-specific tags out of the shared performance comparison.
    compatibility = output / "compatibility.ass"
    compatibility.write_text(shared + (
        r"Dialogue: 1,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
        r"{\an8\pos(960,120)\fnMissing Font For Fallback\3c&H336699&}"
        "日本語 မြန်မာ العربية\n"
        r"Dialogue: 2,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
        r"{\an5\pos(960,540)\frx25\fry-20\fax0.2\iclip(950,400,980,650)\blur1.5}"
        r"{\1vc(&H0033FF&,&H995500&,&H3300FF&,&HFFFF00&)}Perspective and gradient" "\n"
        r"Dialogue: 3,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
        r"{\an7\pos(600,500)\c&H304050&\alpha&H44&\clip(m 400 300 l 1500 300 1500 800 400 800)}"
        r"{\p1}m 0 0 l 600 0 600 120 0 120{\p0}" "\n"
        r"Dialogue: 4,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
        r"{\an7\pos(0,280)\xbord0\ybord3\xblur2\yblur0}Horizontal blur / vertical outline" "\n"
        r"Dialogue: 5,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
        r"{\an3\pos(1920,900)\xbord4\ybord0\xblur0\yblur2}Vertical blur / horizontal outline" "\n"
    ), encoding="utf-8-sig")
    ssa = output / "legacy.ssa"
    ssa.write_text(
        "[Script Info]\nScriptType: v4.00\nPlayResX: 1920\nPlayResY: 1080\n\n"
        "[V4 Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, TertiaryColour, BackColour,"
        " Bold, Italic, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, AlphaLevel, Encoding\n"
        "Style: Default,Arial,48,16777215,65535,0,0,-1,0,1,3,2,2,30,30,40,0,1\n\n"
        "[Events]\nFormat: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: Marked=0,0:00:01.00,0:00:05.00,Default,,0,0,0,,SSA with an outline and shadow\n",
        encoding="ascii"
    )
    return animated, heavy, compatibility, ssa
