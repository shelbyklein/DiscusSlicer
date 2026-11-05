#!/usr/bin/env python3
"""DiscusSlicer: recolour OrcaSlicer's teal accent in resources/web (HTML pages can't see the
DISCUS_SLICER define). Uses the same palette as StateColor::discusAccentFor() in
src/slic3r/GUI/Widgets/StateColor.cpp. Idempotent; re-run after rebasing onto a new Orca release."""
import pathlib, re

LIGHT = {"009688": "1A1A1A", "00675B": "000000", "26A69A": "333333", "008172": "333333",
         "BFE1DE": "DEDEDE", "223C3C": "DEDEDE", "E5F0EE": "EDEDED", "283232": "EDEDED"}
DARK  = {"009688": "8A8A90", "00675B": "8A8A90", "26A69A": "9E9EA4", "008172": "9E9EA4",
         "BFE1DE": "4A4A50", "223C3C": "4A4A50", "E5F0EE": "3C3C42", "283232": "3C3C42"}
HEX = re.compile(r"#(" + "|".join(LIGHT) + r")\b", re.I)

def recolor(text, table):
    return HEX.sub(lambda m: "#" + table[m.group(1).upper()], text)

web = pathlib.Path(__file__).resolve().parent.parent / "resources" / "web"
changed = 0
for path in sorted(list(web.rglob("*.css")) + list(web.rglob("*.svg")) + list(web.rglob("*.html"))):
    text = path.read_text(encoding="utf-8")
    if path.name == "dark.css":
        new = recolor(text, DARK)
    else:
        # dark-mode blocks inside a light stylesheet (include/global.css)
        parts = re.split(r"(@media \(prefers-color-scheme: dark\)\s*\{.*?\n\}\n?)", text, flags=re.S)
        new = "".join(recolor(p, DARK if p.startswith("@media (prefers-color-scheme: dark)") else LIGHT) for p in parts)
    if new != text:
        path.write_text(new, encoding="utf-8")
        changed += 1
        print("recoloured", path.relative_to(web))
print(changed, "files changed")
