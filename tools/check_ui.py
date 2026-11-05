#!/usr/bin/env python3
"""Static checks for data/*.html against Owie's %PLACEHOLDER% template engine.

Run from the repo root: python3 tools/check_ui.py
"""
import re, sys, glob, os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
known = set()
for f in ("src/network.cpp", "src/async_ota.cpp"):
    known |= set(re.findall(r'(?:var|varName) == "([A-Za-z_]+)"', open(os.path.join(root, f)).read()))
errors = 0
for path in sorted(glob.glob(os.path.join(root, "data", "*.html"))):
    s = open(path).read()
    name = os.path.basename(path)
    # Any remaining % must belong to a %NAME% pair, otherwise the engine pairs
    # unrelated percent signs and yields an UNKNOWN PLACEHOLDER alert.
    rest = re.sub(r"%[A-Za-z_]+%", "", s).replace("%%", "")
    if "%" in rest:
        i = rest.index("%")
        print(f"{name}: stray % near: {rest[max(0,i-30):i+30]!r}")
        errors += 1
    for n in set(re.findall(r"%([A-Za-z_]+)%", s)):
        if n not in known:
            print(f"{name}: unknown placeholder {n}")
            errors += 1
    if re.search(r'rel="icon"', s):
        print(f"{name}: data-URI favicon not allowed (contains %)")
        errors += 1
print("OK" if not errors else f"{errors} problem(s)")
sys.exit(1 if errors else 0)
