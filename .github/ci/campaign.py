"""Run paired 1080p and 4K trials with the Windows benchmark host built by CI."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def fixtures(output):
    shared = Path(".github/ci/shared.ass").read_text(encoding="utf-8")
    header = shared.split("Dialogue:")[0]
    style = next(line for line in header.splitlines() if line.startswith("Style:"))
    header = header.replace(style, style + "\n" + style.replace("Default,Arial,48", "Aside,Arial,38"))

    def dialogue(i, start, end, text):
        return f"Dialogue: {i % 3},0:00:0{start},0:00:0{end},{'Aside' if i % 3 else 'Default'},actor{i},0,0,0,,{text}\n"

    conversations = "".join(dialogue(i, f"{1 + i//3}.{i%3*30:02}", f"{2 + i//3}.{i%3*30:02}",
                                     s) for i, s in enumerate([
        "Wait here.", "Five minutes?", "That's what she said.", "There she is.", "Over here!",
        "Can you see us?", "Yes, now I can.", "Let's go.", "After you.", "Thanks.", "See you!", "Bye."]))
    unicode = dialogue(0, "1.00", "5.00", r"{\fnMyanmar Text}မြန်မာနိုင်ငံ {\fnMeiryo}日本語")
    unicode += dialogue(1, "1.00", "5.00", r"{\an8\fnMissing Font For Fallback\t(100,900,\frz8)}漢字 かんじ မြန်မာ")
    dense = "".join(dialogue(i, "1.00", "5.00",
        rf"{{\an5\move({180+i%8*210},{120+i//8*110},{210+i%8*210},{150+i//8*110})"
        rf"\bord3\shad2\alpha&H50&\t(150,2800,\frz{8+i%5})}}Layer {i%4}") for i in range(64))
    typeset = "".join(dialogue(i, "1.00", "5.00",
        rf"{{\an5\pos({340+i%4*360},{270+i//4*360})\bord6\blur2\shad5"
        r"\clip(100,80,1790,980)\1vc(&H2040FA&,&HC0A080&,&H60DAFE&,&H304080&)"
        r"\t(100,1500,\frz12\fscx115)}Typesetting") for i in range(8))
    for name, script in [("dialogue", conversations), ("typeset", typeset), ("density64", dense),
                         ("unicode", unicode), ("seek", typeset)]:
        path = output / (name + ".ass")
        path.write_text(header + script, encoding="utf-8-sig")
        yield name, path, ["seek" if name == "seek" else "animation"]
    yield "static", Path(".github/ci/shared.ass"), ["static"]
    yield "kfx-window", Path(".github/ci/op.fx.4testing.ass"), ["real", "55000", "65000"]


def sample(args, env):
    with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env) as process:
        before = time.perf_counter()
        stdout, stderr = process.communicate(timeout=240)
        wall = time.perf_counter() - before
        if process.returncode:
            raise RuntimeError(stderr.decode(errors="replace"))
        # The Windows process handle remains valid after exit until it is closed.
        times = [ctypes.c_uint64() for _ in range(4)]
        if not ctypes.windll.kernel32.GetProcessTimes(ctypes.c_void_p(int(process._handle)),
                                                     *(ctypes.byref(v) for v in times)):
            raise ctypes.WinError()
        return dict(json.loads(stdout), process_wall_ms=wall * 1000,
                    process_cpu_ms=(times[2].value + times[3].value) / 10000)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("host", "baseline", "candidate", "original", "mangetsu", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--trials", type=int, default=3)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binaries = {k: hashlib.sha256(getattr(args, k).read_bytes()).hexdigest()
                for k in ("host", "baseline", "candidate", "original", "mangetsu")}
    (args.output / "binaries.json").write_text(json.dumps(binaries, indent=2))
    configs = [("original", 1, "csri"), ("mangetsu", 1, "legacy"), ("mangetsu", 1, "rgba")]
    configs += [(engine, n, "csri") for n in (1, 2, 4, 6, 8) for engine in ("baseline", "candidate")]
    with (args.output / "trials.jsonl").open("w", encoding="utf-8") as results:
        for name, path, schedule in fixtures(args.output):
            for width in (1920, 3840):
                if width == 3840 and name not in ("dialogue", "typeset", "unicode", "kfx-window"): continue
                reference = None
                for trial in range(1, args.trials + 1):
                    for engine, workers, mode in (configs if trial % 2 else reversed(configs)):
                        env = dict(os.environ, OBORODZUKI_BENCH_ITERATION="1", OBORODZUKI_WORKERS=str(workers))
                        env.pop("OBORODZUKI_BENCH_4K", None)
                        if width == 3840: env["OBORODZUKI_BENCH_4K"] = "1"
                        data = sample([args.host, mode, getattr(args, engine), path, *schedule], env)
                        if engine in ("baseline", "candidate"):
                            if reference is None: reference = data["hashes"]
                            if data["hashes"] != reference:
                                raise RuntimeError(f"Pixel mismatch: {name}, {engine}, {workers}, {width}")
                        results.write(json.dumps(dict(data, workload=name, width=width, trial=trial,
                            engine=engine, mode=mode, workers=workers)) + "\n")
                        results.flush()
                print(f"Completed {name} at {width} pixels", flush=True)


if __name__ == "__main__":
    main()
