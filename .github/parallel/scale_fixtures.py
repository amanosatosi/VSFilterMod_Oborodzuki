"""Shared worker-scaling fixtures."""


def scale_fixtures(source, root):
    fixture = source.read_text(encoding="utf-8")
    header = fixture[:fixture.index("Dialogue:")]
    scenarios = {"idle": (1, ""), "static": (1, ""), "animated": (1, ""), "banner": (8, ""), "scroll": (8, ""),
                 "blend": (16, r"\blendadd\alpha&H60&\clip(100,100,1800,1000)"),
                 "distortion": (16, r"\distort(1,0.1,0.9,1,0.1,0.9)\frx20"),
                 **{f"overlap-{n}": (n, "") for n in [4, 8, 16, 32, 64]},
                 "border": (16, r"\bord8"), "blur": (16, r"\bord6\blur6"),
                 "clip": (16, r"\clip(100,100,1800,1000)\frx15\fry20"),
                 "gradient": (16, r"\1vc(&H00FF00&,&H0000FF&,&HFF0000&,&HFFFFFF&)\alpha&H40&"),
                 "mixed": (16, r"\1vc(&H00FF00&,&H0000FF&,&HFF0000&,&HFFFFFF&)\alpha&H40&"
                           r"\iclip(m 800 300 l 1200 300 1200 750 800 750)\fry20"),
                 "random": (16, r"\rnd0.2\rnds42\jitter(1,1,1,1,50,7)"),
                 "lua": (16, r"\eventstyle")}
    lua = root / "callbacks.lua"
    lua.write_text("""counter = 0
    function eventstyle(line)
        counter = counter + 1
        local result = {style = {frz = counter % 7 * 2, fscx = 100 + line.id % 3 * 3}}
        if line.id % 8 == 0 then
            result.user = {color = counter % 13 * 0x1000 + 0x4080ff}
            result.renderer = "eventpixel"
        end
        return result
    end

    function eventpixel(line, pixels)
        local y = math.floor(line.height / 2)
        for x = 0, line.width - 1 do
            local coverage = pixels:get(x, y)
            if coverage > 0 then pixels:mix(x, y, line.user.color, 255) end
        end
    end
    """, encoding="ascii")
    for name, (count, effects) in scenarios.items():
        text = header
        if name == "lua":
            text = text.replace("[Script Info]", "[Script Info]\nLua: " + lua.resolve().as_posix())
        for index in range(count):
            motion = "" if name in ("idle", "static") else r"\t(0,4000,\frz30\fscx120\fscy85)"
            event_effects = effects
            if name == "blend":
                mode = ["over", "add", "sub", "mult", "scr", "diff", "rsub", "isub"][index % 8]
                event_effects = effects.replace(r"\blendadd", r"\blend" + mode)
            effect = "Banner;10;0;40" if name == "banner" else "Scroll up;80;900;20;40" if name == "scroll" else ""
            text += (f"Dialogue: {index},0:00:01.00,0:00:05.00,Default,,0,0,0,{effect},"
                     rf"{{\an5\pos({350 + index % 8 * 160},{180 + index % 6 * 120}){motion}{event_effects}}}"
                     f"Worker overlap event {index}"
                     + (" 日本語 မြန်မာ" if name == "mixed" else "") + "\n")
        (root / f"{name}.ass").write_text(text, encoding="utf-8")
        if name == "lua":
            (root / "lua-disabled.ass").write_text(
                text.replace("Lua: " + lua.resolve().as_posix() + "\n", ""), encoding="utf-8")
    return tuple(scenarios)
