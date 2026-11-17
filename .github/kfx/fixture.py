"""Inspect the untouched KFX fixture and construct its frame annotation plan."""
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

FIXTURE = Path(".github/ci/op.fx.4testing.ass")
EXPECTED_SHA256 = "b814f38e854f9dddf08226e6d1f943e74d2d24b63e2e000f5a0a85f8ac0d7ec3"
FPS = 60
FRAMES = 5400


def timestamp_ms(value):
    parts = re.fullmatch(r"(\d+):([0-5]\d):([0-5]\d)\.(\d\d)", value.strip())
    if not parts:
        raise ValueError(f"Invalid ASS timestamp {value!r}")
    h, m, s, cs = map(int, parts.groups())
    return ((h * 60 + m) * 60 + s) * 1000 + cs * 10


def inspect_fixture():
    data = FIXTURE.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    if sha != EXPECTED_SHA256:
        raise ValueError(f"KFX input SHA-256 mismatch: {sha}")
    section = ""
    event_fields = []
    style_fields = []
    styles = {}
    tags = Counter()
    comments = 0
    events = []
    for line_number, line in enumerate(data.decode("utf-8-sig").splitlines(), 1):
        line = line.strip()
        if line.startswith("["):
            section = line.casefold()
            continue
        key, separator, value = line.partition(":")
        if not separator:
            continue
        key = key.casefold()
        try:
            if section == "[v4+ styles]":
                if key == "format":
                    style_fields = [field.strip().casefold() for field in value.split(",")]
                elif key == "style":
                    style = dict(zip(style_fields, (part.strip() for part in value.split(",")), strict=True))
                    styles[style["name"].casefold()] = style["fontname"]
            if section != "[events]":
                continue
            if key == "format":
                event_fields = [field.strip().casefold() for field in value.split(",")]
                if event_fields[-1] != "text" or not {"start", "end", "style"} <= set(event_fields):
                    raise ValueError("Unexpected ASS event format")
            elif key == "comment":
                comments += 1
            elif key == "dialogue":
                if not event_fields:
                    raise ValueError("Dialogue before its format line")
                event = dict(zip(event_fields, value.split(",", len(event_fields) - 1), strict=True))
                start, end = timestamp_ms(event["start"]), timestamp_ms(event["end"])
                if end < start:
                    raise ValueError("Dialogue end precedes its start")
                if event["style"].strip().casefold() not in styles:
                    raise ValueError(f"Unknown dialogue style {event['style']!r}")
                text = event["text"]
                if text.count("{") != text.count("}"):
                    raise ValueError("Unbalanced ASS override braces")
                for override in re.findall(r"\{([^{}]*)\}", text):
                    tags.update(tag.casefold() for tag in re.findall(r"\\([1-4]?[a-zA-Z]+)", override))
                events.append((start, end))
        except (ValueError, KeyError) as error:
            raise ValueError(f"Invalid ASS at line {line_number}: {error}") from error

    if not events:
        raise ValueError("No KFX dialogue events found")
    diffs = [0] * (FRAMES + 1)
    for start, end in events:
        start_frame = min(FRAMES, (start * FPS + 999) // 1000)
        end_frame = min(FRAMES, (end * FPS + 999) // 1000)
        diffs[start_frame] += 1
        diffs[end_frame] -= 1
    active = []
    count = 0
    for delta in diffs[:-1]:
        count += delta
        active.append(count)
    peak = max(range(FRAMES), key=active.__getitem__)
    captures = sorted({0, 5399, peak, *range(600, 5400, 600)})
    return {
        "fixture": FIXTURE.as_posix(), "sha256": sha, "size_bytes": len(data),
        "dialogue_events": len(events), "comment_events": comments,
        "zero_duration_events": sum(start == end for start, end in events),
        "first_event_ms": min(start for start, _ in events),
        "last_event_ms": max(end for _, end in events),
        "style_fonts": sorted(set(styles.values())), "override_tags": dict(sorted(tags.items())),
        "frames": FRAMES, "fps": FPS, "width": 1920, "height": 1080,
        "capture_frames": captures, "active_events": active,
    }


if __name__ == "__main__":
    plan = inspect_fixture()
    Path("kfx-plan.json").write_text(json.dumps(plan, indent=2), encoding="utf-8")
    print(f"{FIXTURE.name}: {plan['dialogue_events']} dialogue events, SHA-256 {plan['sha256']}")
