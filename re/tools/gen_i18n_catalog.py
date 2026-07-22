#!/usr/bin/env python3
"""Merge every TR("...") literal in the td5re sources into the es_AR catalog.

Scans td5mod/src/td5re/*.c/h for TR("...") call sites (plus the SNK label
table, which is TR-wrapped by gen_snk_labels.py), then updates
re/assets/frontend/lang/es_AR.txt IN PLACE:
  - existing translations are preserved verbatim;
  - keys found in the source but missing from the catalog are appended as
    `KEY=` (empty value = untranslated -> English fallback at runtime) under
    an "# UNTRANSLATED" section;
  - keys in the catalog that no longer appear in any TR() call are reported
    (not deleted -- some are composed at runtime).
Idempotent and diff-friendly. RESEARCH/BUILD tool.
"""
import re
import sys
import glob
import os

ROOT = r"C:/Users/maria/Desktop/Proyectos/TD5RE"
SRC = ROOT + "/td5mod/src/td5re"
CATALOG = ROOT + "/re/assets/frontend/lang/es_AR.txt"

# TR("...") with C string-literal escapes; no concatenation support on purpose
# (wrap the WHOLE literal at call sites, never composed pieces).
TR_RE = re.compile(r'\bTR\(\s*"((?:[^"\\]|\\.)*)"\s*\)')


def c_unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def catalog_escape(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n")


def main():
    keys = []
    for path in sorted(glob.glob(SRC + "/*.c") + glob.glob(SRC + "/*.h")):
        base = os.path.basename(path)
        text = open(path, encoding="latin-1").read()
        for m in TR_RE.finditer(text):
            k = c_unescape(m.group(1))
            if k and k not in keys:
                keys.append(k)

    lines = open(CATALOG, encoding="utf-8").read().splitlines()
    have = set()
    for l in lines:
        if l and not l.startswith("#") and "=" in l:
            have.add(c_unescape(l.split("=", 1)[0].replace("\\n", "\n")))

    missing = [k for k in keys if k not in have]
    if missing:
        if "# UNTRANSLATED" not in lines:
            lines += ["", "# UNTRANSLATED (gen_i18n_catalog.py) — fill values;"
                          " empty = English fallback"]
        for k in missing:
            lines.append(catalog_escape(k) + "=")
        open(CATALOG, "w", encoding="utf-8", newline="\n").write(
            "\n".join(lines) + "\n")

    stale = sorted(have - set(keys))
    print("TR() keys in source: %d | in catalog: %d | appended: %d"
          % (len(keys), len(have), len(missing)))
    if stale:
        print("catalog-only keys (composed at runtime or stale): %d" % len(stale))
        for k in stale[:20]:
            print("  ?", k.replace("\n", "\\n"))


if __name__ == "__main__":
    sys.exit(main())
