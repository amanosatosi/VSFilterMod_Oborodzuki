"""Render the same ASS bytes in Oborodzuki CSRI and Mangetsu legacy/RGBA."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import runpy
import subprocess

ROOT = Path("parallel-results/actor-colorcoding")
CSRI = Path("renderer/renderer.dll")
MANGETSU = Path("mangetsu-paint/renderer.dll")
BENCH = Path("hosts/render-bench.exe")
RENDER = runpy.run_path(".github/parallel/coverage-check.py")["render"]

HEADER = Path(".github/ci/shared.ass").read_text(encoding="utf-8").split("Dialogue:", 1)[0]
STYLE = next(line for line in HEADER.splitlines() if line.startswith("Style: Default,"))
HEADER = HEADER.replace(STYLE, "\n".join(
    (STYLE, STYLE.replace("Default,", "Alt,", 1), STYLE.replace("Default,", "Sign,", 1))))
SAMPLE = "<今日|きょう> မြန်မာနိုင်ငံ"
NORMAL = r"\1c&H20A0F0&\2bs4\2bc&H245866&"
OUTER = r"\1c&HDC3131&\2bs5\2bc&H88FF00&"
INNER = (r"\1a&H40&\1grd(0,&H224422&,&HBBBBFF&)\2bs3"
         r"\2bvc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)\2ba&H50&")
REPLACEMENT = r"\1c&H2080FF&\1a&H30&"
NORMAL_END = 9000
OUTER_END = 8000
INNER_END = 5000
TIMES = [5500, 1000, 3000, 4000, 5000, 6000, 8000, 9200, 4200, 1000, 9500, 9900]


def timestamp(ms):
    return f"0:00:{ms // 1000:02}.{ms % 1000 // 10:02}"


def line(start, end, actor="", effect="", text="", style="Default"):
    return (f"Dialogue: 0,{timestamp(start)},{timestamp(end)},"
            f"{style},{actor},0,0,0,{effect},{text}\n")


def fixture(transport=None, inner=INNER, frame=None):
    if transport is not None:
        style_effect = "oborodzuki-style" if transport == "legacy" else "OBRS"
        outer_effect = "oborodzuki-colorcoding" if transport == "mixed" else "OBRCC"
        whitelist = "oborodzuki-colorcode-applied-styles" if transport == "mixed" else "OBRCCS"
        if transport == "legacy":
            outer_effect = "oborodzuki-colorcoding"
            whitelist = "oborodzuki-colorcode-applied-styles"
        effect = "oborodzuki-colorcoding" if transport == "legacy" else "OBRCC"
        text = line(0, 10000, effect=style_effect, text=r"{\furis70}HIDDEN")
        text += line(0, 10000, whitelist, effect, "{Default}{Alt}")
        for begin, end, marker, patch in [(0, NORMAL_END, effect, NORMAL),
                                           (3000, OUTER_END, outer_effect, OUTER),
                                           (4000, INNER_END, effect, inner)]:
            override = "{" + patch + "}"
            if transport == "filtered":
                override = f"{{OBRCC:{begin},{end}}}" + override
                begin, end = 0, 10000
            text += line(begin, end, "Nene", marker, override)
    else:
        text = ""
    for start, end, patch in [(1000, 2000, NORMAL), (3000, 4000, OUTER),
                              (4000, 6000, inner), (6000, 7000, OUTER),
                              (8000, 9000, NORMAL), (9000, 9500, ""),
                              (9500, 9800, "")]:
        if frame is not None and not start <= frame < end:
            continue
        for i in range(8):
            x, y = 370 + 500 * (i % 3), 160 + 330 * (i // 3)
            # Named resets apply the update for Alt and suppress it for Sign.
            # Keep ruby cases on Default so OBRS has a direct inline reference.
            reset = r"\rAlt" if i == 2 else r"\rSign" if i == 3 else ""
            tags = reset + rf"\an5\pos({x},{y})\bord2"
            if transport is None:
                if i != 3:
                    tags = reset + patch + tags[len(reset):]
                if not reset:
                    tags += r"\furis70"
            if i == 4:
                tags += r"\t(0,700,\1a&H70&)\fad(100,100)"
            if i == 5:
                tags += r"\vert1\vdir2"
            # OBRCCS is ordinary when used as Effect.
            ordinary_effect = "OBRCCS" if start == 9500 else ""
            body = SAMPLE if not reset else "明日 မင်္ဂလာပါ"
            text += line(start, end, "Nene", ordinary_effect, "{" + tags + "}" + body)
    return (HEADER + text).encode("utf-8-sig")


def bench(mode, path):
    env = dict(os.environ, OBORODZUKI_BENCH_ITERATION="1")
    result = subprocess.run(
        [str(BENCH), mode, str(MANGETSU.resolve()), str(path), "real", "950", "9900"],
        check=True, env=env, capture_output=True, text=True, timeout=180)
    return json.loads(result.stdout)["hashes"]


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    tests = {name: fixture(kind) for name, kind in [
        ("inline", None), ("compact", "compact"), ("mixed", "mixed"),
        ("legacy", "legacy"), ("filtered", "filtered")]}
    for name, data in tests.items():
        (ROOT / f"{name}.ass").write_bytes(data)
    expected = RENDER(CSRI, tests["inline"], 1, TIMES)
    for workers in (1, 2, 4, 6, 8):
        for name in ("compact", "mixed", "legacy", "filtered"):
            actual = RENDER(CSRI, tests[name], workers, TIMES)
            assert actual == expected, (name, workers, next(
                i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1]))

    # Recreate Aegisub's first-frame-only ingestion. Open after expiry, before
    # opening any earlier frame. Keep each retained event's original times.
    for ms in (5500, 1000, 6000, 8000, 4200):
        got = RENDER(CSRI, fixture("filtered", frame=ms), 4, [ms])
        ref = RENDER(CSRI, fixture(frame=ms), 4, [ms])
        assert got == ref, ("filtered seek", ms)

    # Reopen with an edited declaration, including concurrent CSRI instances.
    changed = fixture("filtered", inner=REPLACEMENT)
    edited = RENDER(CSRI, fixture(inner=REPLACEMENT), 2, TIMES)
    assert edited != expected, "editing metadata did not change the rendered scene"
    with ThreadPoolExecutor(max_workers=2) as executor:
        a = executor.submit(RENDER, CSRI, changed, 2, TIMES)
        b = executor.submit(RENDER, CSRI, tests["filtered"], 2, TIMES)
        assert a.result() == edited, "edited metadata was not loaded"
        assert b.result() == expected, "concurrent instance shared actor metadata"

    # Use each renderer's inline reference. Font selection, shaping and
    # rasterization can produce different pixels between implementations.
    for mode in ("legacy", "rgba"):
        reference = bench(mode, ROOT / "inline.ass")
        for name in ("compact", "mixed", "legacy", "filtered"):
            assert bench(mode, ROOT / f"{name}.ass") == reference, (mode, name)
    print("Shared Actor Colorcoding fixtures matched inline references through CSRI, legacy and RGBA")


if __name__ == "__main__":
    main()
