"""Resolve the TD5_TG_PAGE_* constants out of td5_trackgen_internal.h.

The page ids are a chain of #defines (each block measured off the previous
block's base), so hardcoding a number here would silently rot the first time a
block grows. This evaluates the chain instead, to a fixed point.

Used by land_models_probe.py so a MODELS.DAT face can be attributed to the
emitter that wrote it (lawn page vs paving page vs planting page).
"""
import os
import re

HDR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "..", "..", "td5mod", "src", "td5re",
                   "td5_trackgen_internal.h")

_DEF = re.compile(r"^#define\s+(TD5_TG_[A-Z0-9_]+)\s+(\(?[^/]*?)\s*(?:/\*.*)?$")


def page_table(header=None):
    """name -> int for every integer TD5_TG_* macro the header defines."""
    path = header or HDR
    raw = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = _DEF.match(line.rstrip("\n"))
            if not m:
                continue
            name, expr = m.group(1), m.group(2).strip()
            if not expr or "(" in expr and ")" not in expr:
                continue            # a multi-line or function-like macro
            raw[name] = expr
    out = {}
    for _ in range(12):             # the chain is ~6 deep; 12 is slack
        progress = False
        for name, expr in raw.items():
            if name in out:
                continue
            try:
                val = eval(expr, {"__builtins__": {}}, dict(out))
            except Exception:
                continue
            if isinstance(val, int):
                out[name] = val
                progress = True
        if not progress:
            break
    return out


if __name__ == "__main__":
    t = page_table()
    for k in sorted(t):
        if "PAGE" in k:
            print("%-28s %d" % (k, t[k]))
