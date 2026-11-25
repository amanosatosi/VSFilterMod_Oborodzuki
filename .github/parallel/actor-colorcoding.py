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
INNER = (r"\fs52\1a&H40&\1grd(0,&H224422&,&HBBBBFF&)\2bs3"
         r"\2bvc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)\2ba&H50&")
INNER_WINDOW = (4000, 5000)
EDITED_WINDOW = (4100, 5500)
TIMES = [4000, 1000, 2999, 3000, 3999, 4000, 4250, 4999, 5000, 7999, 8000,
         8999, 9000, 9200, 4250, 2999, 9900]


def timestamp(ms):
    return f"0:00:{ms // 1000:02}.{ms % 1000 // 10:02}"


def line(start, end, actor="", effect="", text="", style="Default"):
    return (f"Dialogue: 0,{timestamp(start)},{timestamp(end)},"
            f"{style},{actor},0,0,0,{effect},{text}\n")


def fixture(transport=None, *, window=INNER_WINDOW, frame=None, actor_comment="",
            voices=8, events_format=False):
    rows = []

    def emit(start, end, *args, **kwargs):
        if frame is None or start <= frame < end:
            rows.append(line(start, end, *args, **kwargs))

    inner_start, inner_end = window
    if transport is not None:
        style_effect = "oborodzuki-style" if transport == "legacy" else "OBRS"
        outer_effect = "OBRCC" if transport == "compact" else "oborodzuki-colorcoding"
        whitelist = "OBRCCS" if transport == "compact" else "oborodzuki-colorcode-applied-styles"
        effect = "oborodzuki-colorcoding" if transport == "legacy" else "OBRCC"
        emit(0, 10000, effect=style_effect, text=r"{\furis70}HIDDEN")
        emit(0, 10000, whitelist, effect, "{Default}{Alt}")
        emit(0, 9000, "Nene", effect, "{" + NORMAL + "}")
        emit(3000, 8000, "Nene", outer_effect, "{" + OUTER + "}")
        # This lower-priority declaration starts later than the winner.
        emit(4200, 4300, "Nene", effect, "{" + NORMAL + "}")
        emit(inner_start, inner_end, "Nene", effect, actor_comment + "{" + INNER + "}")
        phases = [(1000, 9800, None)]
    else:
        phases = [(1000, 3000, NORMAL), (3000, inner_start, OUTER),
                  (inner_start, inner_end, INNER), (inner_end, 8000, OUTER),
                  (8000, 9000, NORMAL), (9000, 9800, "")]

    for start, end, patch in phases:
        for i in range(voices):
            x, y = 370 + 500 * (i % 3), 160 + 330 * (i // 3)
            reset = r"\rAlt" if i == 2 else r"\rSign" if i == 3 else ""
            tags = reset + rf"\an5\pos({x},{y})\bord2"
            if patch is not None:
                if i != 3:
                    tags = reset + patch + tags[len(reset):]
                if not reset:
                    tags += r"\furis70"
            if i == 5:
                tags += r"\vert1\vdir2"
            if i == 6 and patch is None:
                # The inline reference already contains the reset default.
                tags += r"\1a&H70&\1a"
            # OBRCCS is ordinary when used as Effect.
            ordinary_effect = "OBRCCS" if i == 7 else ""
            body = SAMPLE if not reset else "明日 မင်္ဂလာပါ"
            emit(start, end, "Nene", ordinary_effect, "{" + tags + "}" + body)

    header = HEADER
    if frame is not None:
        # Toshi-ban retains the Events Format line in its filtered serialization.
        styles, events = header.split("[Events]\n", 1)
        styles = "\n".join(row for row in styles.split("\n") if not row.startswith("Format:"))
        header = styles + "[Events]\n" + (events if events_format else "")
    return (header + "".join(rows)).encode("utf-8-sig")


def bench(mode, path):
    env = dict(os.environ, OBORODZUKI_BENCH_ITERATION="1")
    result = subprocess.run(
        [str(BENCH), mode, str(MANGETSU.resolve()), str(path), "real", "950", "9900"],
        check=True, env=env, capture_output=True, text=True, timeout=180)
    return json.loads(result.stdout)["hashes"]


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    tests = {name: fixture(kind) for name, kind in [
        ("inline", None), ("compact", "compact"), ("mixed", "mixed"), ("legacy", "legacy")]}
    tests["ignored-comment"] = fixture("compact", actor_comment="{OBRCC:100,200}")
    for name, data in tests.items():
        (ROOT / f"{name}.ass").write_bytes(data)
    boundary_times = [2999, 3000, 3999, 4000, 4999, 5000, 8000, 9000, 4000]
    single = RENDER(CSRI, fixture("compact", voices=1), 1, boundary_times)
    assert single == RENDER(CSRI, fixture(voices=1), 1, boundary_times)
    assert single[0] != single[1] and single[2] != single[3] and single[4] != single[5]
    assert single[5] == single[1] and single[8] == single[3]

    expected = RENDER(CSRI, tests["inline"], 1, TIMES)
    for before, after in ((2999, 3000), (3999, 4000), (4999, 5000),
                          (7999, 8000), (8999, 9000)):
        assert expected[TIMES.index(before)] != expected[TIMES.index(after)], (before, after)
    for workers in (1, 2, 4, 6, 8):
        for name in ("compact", "mixed", "legacy", "ignored-comment"):
            actual = RENDER(CSRI, tests[name], workers, TIMES)
            assert actual == expected, (name, workers, next(
                i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1]))

    # An initial frame can arrive in any order, with only currently active rows.
    for ms in (5000, 4000, 2999, 3000, 4250, 8000, 9000, 9900):
        ref = RENDER(CSRI, fixture(frame=ms), 4, [ms])
        for events_format in (False, True):
            got = RENDER(CSRI, fixture("mixed", frame=ms, events_format=events_format), 4, [ms])
            assert got == ref, ("filtered seek", ms, events_format)

    # Move Start and End while paused on the old boundaries and reopen.
    for ms in INNER_WINDOW:
        paused = RENDER(CSRI, fixture("compact", frame=ms, events_format=True), 2, [ms])
        edited = RENDER(CSRI, fixture("compact", frame=ms, window=EDITED_WINDOW, events_format=True), 2, [ms])
        assert paused != edited, ("timing edit did not change pixels", ms)
        assert edited == RENDER(CSRI, fixture(frame=ms, window=EDITED_WINDOW), 2, [ms]), ms

    changed = fixture("compact", window=EDITED_WINDOW)
    edited = RENDER(CSRI, fixture(window=EDITED_WINDOW), 2, TIMES)
    with ThreadPoolExecutor(max_workers=2) as executor:
        a = executor.submit(RENDER, CSRI, changed, 2, TIMES)
        b = executor.submit(RENDER, CSRI, tests["compact"], 2, TIMES)
        assert a.result() == edited, "edited timing was not loaded"
        assert b.result() == expected, "concurrent instance shared actor metadata"

    # Compare against each renderer's inline reference.
    for mode in ("legacy", "rgba"):
        reference = bench(mode, ROOT / "inline.ass")
        for name in ("compact", "mixed", "legacy", "ignored-comment"):
            assert bench(mode, ROOT / f"{name}.ass") == reference, (mode, name)
    print("One visible dialogue changed appearance at its actor's ASS boundaries through CSRI, legacy and RGBA")


if __name__ == "__main__":
    main()
