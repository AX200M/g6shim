#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import json
import sys
from math import ceil
from pathlib import Path
from xml.sax.saxutils import escape

sys.path.insert(0, str(Path(__file__).resolve().parent))

from def_coverage import classify, parse_def

ROOT = Path(__file__).resolve().parent.parent
HLE = Path("hle")
PROGRESS = Path("docs") / "progress"
README = Path("README.md")
README_BEGIN = "<!-- progress:begin -->"
README_END = "<!-- progress:end -->"
README_HEADING = "## Progress"
README_ANCHOR = "## Build"
SVG_NAMESPACE = "http://www.w3.org/2000/svg"
FONT_FAMILY = "ui-monospace, SFMono-Regular, Menlo, Consolas, 'Liberation Mono', monospace"
TEXT_COLOUR = "#6b7280"
PAD = 14
FONT_SIZE = 11
CHAR_WIDTH = 6.6
ROW_HEIGHT = 12
TEXT_BASELINE = 9
SQUARE = 8
SQUARE_TOP = 2
PITCH = 9
COLUMN_GAP = 10
LEGEND_GAP = 12
LEGEND_SWATCH = 8
LEGEND_BASELINE = 7
LEGEND_HEIGHT = 10
LEGEND_ITEM_GAP = 14
LEGEND_TEXT_GAP = 4
MAX_ROW_SQUARES = 64
MAX_SHADER_SQUARES = 120
SHADER_STAGES = ("vs", "ps", "cs", "ms", "gs")
FUNCTION_LEGEND = ("implemented", "forwarded", "stubbed", "missing")
HLE_LEGEND = ("implemented", "forwarded", "stubbed")
SHADER_LEGEND = ("translated", "failed", "not attempted")
COLOURS = {
    "implemented": "#2ea043",
    "forwarded": "#388bfd",
    "stubbed": "#d29922",
    "missing": "#57606a",
    "translated": "#2ea043",
    "failed": "#cf222e",
    "not attempted": "#57606a",
}
COVERAGE_ORDER = {"implemented": 0, "forwarded": 1, "stubbed": 2}
NO_SHADER_DATA = "no data yet"
LEGEND_SENTENCE = (
    "Each square is one function: green implemented, blue forwarded, amber stubbed, grey missing; "
    "in the shader maps green is translated, red failed and grey not attempted."
)


def text_width(text):
    return ceil(len(text) * CHAR_WIDTH)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def dll_records(hle):
    found = {}
    for path in sorted(hle.glob("*/*.def")):
        dll, exports = parse_def(path)
        items = sorted((export.partition("=")[0], classify(export)) for export in exports)
        found.setdefault(dll.lower(), {"dll": dll, "items": items})
    return found


def title_rows(imports, known):
    rows = []
    modules = sorted(imports.get("modules") or [], key=lambda module: (module.get("dll", "").lower(), module.get("dll", "")))
    for module in modules:
        record = known.get(module.get("dll", "").lower())
        statuses = dict(record["items"]) if record else {}
        items = [(name, statuses.get(name, "missing")) for name in sorted(module.get("functions") or [])]
        rows.append((module.get("dll", ""), items))
    return rows


def hle_rows(known):
    rows = []
    for record in sorted(known.values(), key=lambda record: record["dll"].lower()):
        items = sorted(record["items"], key=lambda item: (COVERAGE_ORDER[item[1]], item[0]))
        rows.append((record["dll"], items))
    return rows


def count_text(items):
    return f"{sum(1 for _, status in items if status == 'implemented')}/{len(items)}"


def map_rows(rows):
    return [{"label": label, "count": count_text(items), "items": [status for _, status in items]} for label, items in rows]


def scale_note(scale):
    return f"1 square = {scale} shader" if scale == 1 else f"1 square = {scale} shaders"


def stage_counts(stage):
    total = max(0, int(stage.get("total", 0)))
    translated = min(total, max(0, int(stage.get("translated", 0))))
    failed = min(total - translated, max(0, int(stage.get("failed", 0))))
    return total, translated, failed


def shader_rows(shaders):
    stages = shaders.get("stages") or {}
    names = [name for name in SHADER_STAGES if name in stages]
    names += sorted(name for name in stages if name not in SHADER_STAGES)
    widest = max((stage_counts(stages[name])[0] for name in names), default=0)
    scale = max(1, -(-widest // MAX_SHADER_SQUARES))
    rows = []
    for name in names:
        total, translated, failed = stage_counts(stages[name])
        squares = -(-total // scale)
        green = min(squares, (translated + scale // 2) // scale)
        red = min(squares - green, (failed + scale // 2) // scale)
        rows.append({
            "label": name,
            "count": f"{translated}/{total}",
            "items": ["translated"] * green + ["failed"] * red + ["not attempted"] * (squares - green - red),
        })
    return rows, scale


def legend_entries(legend, note):
    entries = [(COLOURS[name], name) for name in legend]
    if note:
        entries.append((None, note))
    return entries


def legend_width(entries):
    widths = [text_width(label) + (LEGEND_SWATCH + LEGEND_TEXT_GAP if colour else 0) for colour, label in entries]
    return sum(widths) + LEGEND_ITEM_GAP * max(len(entries) - 1, 0) + 2 * PAD


def render_map(rows, legend, title, note=None):
    label_width = max((text_width(row["label"]) for row in rows), default=0)
    count_width = max((text_width(row["count"]) for row in rows), default=0)
    x_count = PAD + label_width + COLUMN_GAP + count_width
    x_squares = x_count + COLUMN_GAP
    row_lines = [max(1, -(-len(row["items"]) // MAX_ROW_SQUARES)) for row in rows]
    widest = min(max((len(row["items"]) for row in rows), default=0), MAX_ROW_SQUARES)
    data_width = (x_squares + widest * PITCH - (PITCH - SQUARE) if widest else x_count) + PAD
    entries = legend_entries(legend, note)
    width = max(data_width, legend_width(entries))
    legend_y = PAD + sum(row_lines) * ROW_HEIGHT + LEGEND_GAP
    height = legend_y + LEGEND_HEIGHT + PAD
    svg = [
        f'<svg xmlns="{SVG_NAMESPACE}" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img">',
        f"<title>{escape(title)}</title>",
        f'<g font-family="{FONT_FAMILY}" font-size="{FONT_SIZE}" fill="{TEXT_COLOUR}">',
    ]
    top = PAD
    for row, lines in zip(rows, row_lines):
        svg.append(f'<text x="{PAD}" y="{top + TEXT_BASELINE}">{escape(row["label"])}</text>')
        svg.append(f'<text x="{x_count}" y="{top + TEXT_BASELINE}" text-anchor="end">{escape(row["count"])}</text>')
        squares = [
            f'<rect x="{x_squares + (index % MAX_ROW_SQUARES) * PITCH}" y="{top + (index // MAX_ROW_SQUARES) * ROW_HEIGHT + SQUARE_TOP}" width="{SQUARE}" height="{SQUARE}" fill="{COLOURS[status]}"/>'
            for index, status in enumerate(row["items"])
        ]
        if squares:
            svg.append('<g shape-rendering="crispEdges">')
            svg.extend(squares)
            svg.append("</g>")
        top += lines * ROW_HEIGHT
    svg.append('<g shape-rendering="crispEdges">')
    x = PAD
    for colour, label in entries:
        if colour:
            svg.append(f'<rect x="{x}" y="{legend_y}" width="{LEGEND_SWATCH}" height="{LEGEND_SWATCH}" fill="{colour}"/>')
            x += LEGEND_SWATCH + LEGEND_TEXT_GAP
        svg.append(f'<text x="{x}" y="{legend_y + LEGEND_BASELINE}">{escape(label)}</text>')
        x += text_width(label) + LEGEND_ITEM_GAP
    svg.append("</g>")
    svg.append("</g>")
    svg.append("</svg>")
    return "\n".join(svg) + "\n"


def render_note(note, title):
    width = 2 * PAD + text_width(note)
    height = 2 * PAD + ROW_HEIGHT
    svg = [
        f'<svg xmlns="{SVG_NAMESPACE}" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img">',
        f"<title>{escape(title)}</title>",
        f'<text x="{PAD}" y="{PAD + TEXT_BASELINE}" font-family="{FONT_FAMILY}" font-size="{FONT_SIZE}" fill="{TEXT_COLOUR}">{escape(note)}</text>',
        "</svg>",
    ]
    return "\n".join(svg) + "\n"


def progress_section(titles):
    lines = [README_BEGIN, LEGEND_SENTENCE, ""]
    for title_id, summary in titles:
        lines += [
            f"### {title_id}",
            "",
            f"![{title_id} API coverage]({PROGRESS.as_posix()}/{title_id}.apis.svg)",
            "",
            f"![{title_id} shader coverage]({PROGRESS.as_posix()}/{title_id}.shaders.svg)",
            "",
            summary,
            "",
        ]
    lines += [f"![Replacement DLL coverage]({PROGRESS.as_posix()}/hle.svg)", README_END]
    return lines


def render_readme(text, titles):
    text = text.replace("\r\n", "\n")
    if README_BEGIN not in text or README_END not in text:
        anchor = text.find(README_ANCHOR)
        if anchor < 0:
            raise SystemExit("README.md has no progress markers and no Build section to put them before")
        text = text[:anchor] + f"{README_HEADING}\n\n{README_BEGIN}\n{README_END}\n\n" + text[anchor:]
    start = text.index(README_BEGIN)
    end = text.index(README_END) + len(README_END)
    return text[:start] + "\n".join(progress_section(titles)) + text[end:]


def generate(root):
    known = dll_records(root / HLE)
    progress = root / PROGRESS
    texts, titles = {}, []
    for path in sorted(progress.glob("*.imports.json")):
        title_id = path.name.split(".")[0]
        imports = read_json(path)
        rows = map_rows(title_rows(imports, known))
        counts = {status: 0 for status in FUNCTION_LEGEND}
        for row in rows:
            for status in row["items"]:
                counts[status] += 1
        texts[progress / f"{title_id}.apis.svg"] = render_map(rows, FUNCTION_LEGEND, f"API coverage for {title_id}")
        shaders_path = progress / f"{title_id}.shaders.json"
        if shaders_path.is_file():
            shaders = read_json(shaders_path)
            shader_map, scale = shader_rows(shaders)
            texts[progress / f"{title_id}.shaders.svg"] = render_map(
                shader_map, SHADER_LEGEND, f"Shader coverage for {title_id}", note=scale_note(scale)
            )
            translated = sum(stage_counts(stage)[1] for stage in (shaders.get("stages") or {}).values())
            total = sum(stage_counts(stage)[0] for stage in (shaders.get("stages") or {}).values())
            shader_summary = f"shaders: {translated}/{total} translated"
        else:
            texts[progress / f"{title_id}.shaders.svg"] = render_note(NO_SHADER_DATA, f"Shader coverage for {title_id}")
            shader_summary = f"shaders: {NO_SHADER_DATA}"
        summary = (
            f"{counts['implemented']}/{sum(counts.values())} functions implemented, "
            f"{counts['forwarded']} forwarded, {counts['stubbed']} stubbed, {counts['missing']} missing; "
            f"{shader_summary}."
        )
        titles.append((title_id, summary))
    texts[progress / "hle.svg"] = render_map(map_rows(hle_rows(known)), HLE_LEGEND, "Coverage of the replacement DLLs")
    texts[root / README] = render_readme((root / README).read_bytes().decode("utf-8"), titles)
    return texts


def normalise(text):
    return text.replace("\r\n", "\n")


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(normalise(text).encode("utf-8"))


def stale_files(root, texts):
    stale = []
    for path, text in texts.items():
        if not path.is_file() or normalise(path.read_bytes().decode("utf-8", "replace")) != normalise(text):
            stale.append(path)
    expected = {path.resolve() for path in texts}
    for path in sorted((root / PROGRESS).glob("*.svg")):
        if path.resolve() not in expected:
            stale.append(path)
    return stale


def main(argv=None):
    parser = argparse.ArgumentParser(description="generate the progress maps and the README progress section")
    parser.add_argument("--check", action="store_true", help="exit 1 when a generated file on disk is stale")
    args = parser.parse_args(argv)
    texts = generate(ROOT)
    if args.check:
        stale = stale_files(ROOT, texts)
        for path in stale:
            print(f"stale: {path.relative_to(ROOT).as_posix()}")
        if stale:
            print("run: python tools/progress_map.py")
            return 1
        return 0
    for path, text in texts.items():
        write_text(path, text)
    print(f"wrote {len(texts)} file(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
