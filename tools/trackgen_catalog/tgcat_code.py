"""tgcat_code.py -- everything the catalogue learns from the SOURCE (no level).

Read-only over td5mod/src/td5re. Three products:

  * page_registry()   the TEXTURES.DAT slot map: every `#define TD5_TG_PAGE_*`
                      in td5_trackgen_internal.h evaluated to a number, with the
                      variant count that goes with it (`TD5_TG_<X>_N`,
                      `*_VARIANTS`, `*_COUNT`, ...), so every slot gets the most
                      specific name that covers it.
  * functions()       function ranges per trackgen file (same heuristic as
                      scripts/gen_codemap.py: column-0 signature + brace walk),
                      so any line can be attributed to file:function:line.
  * emitter_sites()   every `tg_guard_mark(..., TG_GK_<KIND>, ...)` /
                      `tg_meshtag_set(..., TG_GK_<KIND>)` call with the emitter
                      call(s) right before it: the code path that produced a
                      MESHTAG kind.
  * page_refs()       every use of a TD5_TG_PAGE_* macro in the trackgen code,
                      attributed to a function, split into "writes the page"
                      (td5_tg_pages.c, the TEXTURES.DAT emitter) and "puts it on
                      geometry" (everything else).
  * knobs_in()        the TD5RE_* environment knobs a function reads.
  * biomes()          the k_biomes table rows (name + the page macros they use).

All parsing is regex + brace tracking. It is good enough for a catalogue and
it re-runs on every build of the page, so it follows the code as it changes.
"""
from __future__ import annotations

import os
import re
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "td5mod", "src", "td5re")
INTERNAL_H = os.path.join(SRC, "td5_trackgen_internal.h")


def tg_sources():
    """The auto-track generator's .c files (trackgen + every td5_tg_*.c)."""
    out = ["td5_trackgen.c"]
    for f in sorted(os.listdir(SRC)):
        if f.startswith("td5_tg_") and f.endswith(".c"):
            out.append(f)
    return out


def read_lines(name):
    with open(os.path.join(SRC, name), encoding="utf-8", errors="replace") as fh:
        return fh.read().split("\n")


# ---------------------------------------------------------------------------
# #define evaluation
# ---------------------------------------------------------------------------

DEFINE_RE = re.compile(r"^\s*#\s*define\s+(TD5_TG_\w+)\s+(.*?)\s*$")


def _strip_comment(s):
    s = re.sub(r"/\*.*?\*/", "", s)
    s = s.split("//")[0]
    return s.strip()


def _trailing_comment(s):
    m = re.search(r"/\*\s*(.*?)\s*\*/", s)
    return m.group(1) if m else ""


def parse_defines(path=INTERNAL_H):
    """{NAME: (expr, line, trailing_comment)} for object-like TD5_TG_* macros.
    Handles a backslash-continued value."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")
    out = {}
    i = 0
    while i < len(lines):
        m = DEFINE_RE.match(lines[i])
        if m and "(" not in m.group(1):
            name, rest = m.group(1), m.group(2)
            if rest.startswith("("):
                pass
            line_no = i + 1
            comment = _trailing_comment(rest)
            while rest.rstrip().endswith("\\") and i + 1 < len(lines):
                i += 1
                rest = rest.rstrip()[:-1] + " " + lines[i]
            # function-like macro: NAME( directly, no space
            raw = lines[line_no - 1]
            if re.search(r"#\s*define\s+" + re.escape(name) + r"\(", raw):
                i += 1
                continue
            out[name] = (_strip_comment(rest), line_no, comment)
        i += 1
    return out


def eval_defines(defs):
    vals = {}

    def ev(name, depth=0):
        if name in vals:
            return vals[name]
        if depth > 50 or name not in defs:
            return None
        expr = defs[name][0]
        expr = re.sub(r"(\d+)[uUlL]+\b", r"\1", expr)
        names = set(re.findall(r"\b[A-Z_][A-Z0-9_]*\b", expr))
        env = {}
        for n in names:
            v = ev(n, depth + 1)
            if v is None:
                return None
            env[n] = v
        try:
            v = eval(expr, {"__builtins__": {}}, env)  # noqa: S307 (own header)
        except Exception:
            return None
        if not isinstance(v, int):
            return None
        vals[name] = v
        return v

    for n in defs:
        ev(n)
    return vals


# Count macro that goes with a page macro when it is not TD5_TG_<X>_N.
_COUNT_ALIASES = {
    "WALL_EXTRA": ("TD5_TG_WALL_VARIANTS", -1),
    "STORE": ("TD5_TG_STORE_VARIANTS", 0),
    "TREE_EXTRA": ("TD5_TG_TREE_VARIANTS", -1),
    "PROP": ("TD5_TG_PROP_COUNT", 0),
    "ROAD_EXTRA": ("TD5_TG_ROAD_VARIANTS", -1),
    "TUNNEL_VAR": ("TD5_TG_TUNNEL_VARIANTS", -1),
    "R8V_BANNER": ("TD5_TG_R8V_BANNER_N", 0),
    "R8V_BANNER_NIGHT": ("TD5_TG_R8V_BANNER_N", 0),
    "LM_BASE": ("TD5_TG_LM_PAGES", 0),
    "RS_TARMAC": ("TD5_TG_RS_PER_CLASS", 0),
    "RS_CONCRETE": ("TD5_TG_RS_PER_CLASS", 0),
    "RS_COBBLE": ("TD5_TG_RS_PER_CLASS", 0),
    "RS_DIRT": ("TD5_TG_RS_PER_CLASS", 0),
    "RS_ICE": ("TD5_TG_RS_PER_CLASS", 0),
}


def page_registry():
    """Returns dict:
        count        TD5_TG_PAGE_COUNT
        names        [{name, short, start, n, line, comment}]   every page macro
        slot_name    {slot: most specific short name (+offset)}
        slot_chain   {slot: [short names covering it, widest first]}
    """
    defs = parse_defines()
    vals = eval_defines(defs)
    count = vals.get("TD5_TG_PAGE_COUNT", 0)
    names = []
    for name, (expr, line, comment) in defs.items():
        if not name.startswith("TD5_TG_PAGE_") or name == "TD5_TG_PAGE_COUNT":
            continue
        if name not in vals:
            continue
        short = name[len("TD5_TG_PAGE_"):]
        n = 1
        if short in _COUNT_ALIASES:
            cname, adj = _COUNT_ALIASES[short]
            if cname in vals:
                n = vals[cname] + adj
        elif ("TD5_TG_" + short + "_N") in vals:
            n = vals["TD5_TG_" + short + "_N"]
        names.append({"name": name, "short": short, "start": vals[name],
                      "n": max(1, n), "line": line, "comment": comment})
    # *_BASE blocks with no count run to the next BASE (or to PAGE_COUNT).
    all_bases = sorted([e for e in names if e["short"].endswith("_BASE")],
                       key=lambda e: e["start"])
    bases = [e for e in all_bases if e["short"] not in _COUNT_ALIASES]
    for e in bases:
        later = [b["start"] for b in all_bases if b["start"] > e["start"]]
        nxt = min(later) if later else count
        e["n"] = max(1, nxt - e["start"])
        e["block"] = True

    slot_name, slot_chain = {}, defaultdict(list)
    order = sorted(names, key=lambda e: (-e["n"], e["line"]))
    for e in order:
        for k in range(e["n"]):
            s = e["start"] + k
            if s >= count:
                break
            slot_chain[s].append(e["short"])
            slot_name[s] = e["short"] if e["n"] == 1 else f"{e['short']}+{k}"
    return {"count": count, "names": names, "slot_name": slot_name,
            "slot_chain": dict(slot_chain), "values": vals, "defs": defs}


# ---------------------------------------------------------------------------
# Functions, knobs, references
# ---------------------------------------------------------------------------

FUNC_RE = re.compile(r"^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(")
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "else", "do", "typedef"}


def functions(fname, lines=None):
    """[(name, first_line, last_line)] 1-based, for one .c file."""
    lines = lines if lines is not None else read_lines(fname)
    out = []
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        m = FUNC_RE.match(line)
        if (m and m.group(1) not in KEYWORDS and not line.rstrip().endswith(";")
                and not line.startswith(("#", "typedef", "extern"))
                and "=" not in line.split("(")[0]):
            # find the opening brace within a few lines (K&R or next-line)
            j, opened = i, False
            while j < min(n, i + 12):
                if ";" in lines[j] and "{" not in lines[j]:
                    break
                if "{" in lines[j]:
                    opened = True
                    break
                j += 1
            if opened:
                depth, k = 0, j
                while k < n:
                    depth += lines[k].count("{") - lines[k].count("}")
                    if depth <= 0 and k >= j:
                        break
                    k += 1
                out.append((m.group(1), i + 1, k + 1))
                i = k + 1
                continue
        i += 1
    return out


class CodeIndex:
    """Lines + function ranges of every trackgen file, loaded once."""

    def __init__(self):
        self.files = tg_sources()
        self.lines = {f: read_lines(f) for f in self.files}
        self.funcs = {f: functions(f, self.lines[f]) for f in self.files}
        self.by_name = {}
        for f, fl in self.funcs.items():
            for (name, a, b) in fl:
                self.by_name.setdefault(name, (f, a, b))

    def func_at(self, f, line):
        for (name, a, b) in self.funcs[f]:
            if a <= line <= b:
                return name
        return "(file scope)"

    def body(self, name):
        if name not in self.by_name:
            return []
        f, a, b = self.by_name[name]
        return self.lines[f][a - 1:b]

    def knobs_in(self, name):
        return sorted(set(re.findall(r'"(TD5RE_[A-Z0-9_]+)"', "\n".join(self.body(name)))))

    def pages_in(self, name):
        return sorted(set(re.findall(r"\bTD5_TG_PAGE_(\w+)", "\n".join(self.body(name)))))

    def calls_in(self, name):
        body = "\n".join(self.body(name)[1:])
        found = re.findall(r"\b(tg_\w+|td5_tg_\w+)\s*\(", body)
        seen, out = set(), []
        for c in found:
            if c in self.by_name and c not in seen and c != name:
                seen.add(c)
                out.append(c)
        return out


GK_SITE_RE = re.compile(r"\b(?:tg_guard_mark|tg_meshtag_set)\s*\([^;]*?\bTG_GK_([A-Z]+)\b")
EMIT_CALL_RE = re.compile(r"\b(tg_(?:emit|net_emit|prefab_emit|building|side_terrain)\w*)\s*\(")


def emitter_sites(ci: CodeIndex):
    """{KIND: [{file, line, func, emitters:[names]}]} -- where each MESHTAG kind
    is stamped and which emitter call(s) produced the geometry it covers."""
    out = defaultdict(list)
    for f in ci.files:
        L = ci.lines[f]
        for i, line in enumerate(L):
            m = GK_SITE_RE.search(line)
            if not m:
                continue
            kind = m.group(1)
            emitters = []
            # the emitter calls between the previous mark and this one
            for j in range(i - 1, max(-1, i - 14), -1):
                if GK_SITE_RE.search(L[j]):
                    break
                for c in EMIT_CALL_RE.findall(L[j]):
                    if c not in emitters:
                        emitters.append(c)
            emitters.reverse()
            if not emitters:
                emitters = [ci.func_at(f, i + 1)]
            out[kind].append({"file": f, "line": i + 1,
                              "func": ci.func_at(f, i + 1),
                              "emitters": emitters})
    return dict(out)


def page_refs(ci: CodeIndex):
    """{SHORT: [{file, line, func, role}]}; role 'page' = TEXTURES.DAT writer
    (td5_tg_pages.c), 'geom' = geometry that samples it."""
    out = defaultdict(list)
    for f in ci.files:
        role = "page" if f == "td5_tg_pages.c" else "geom"
        for i, line in enumerate(ci.lines[f]):
            code = line.split("//")[0]
            for short in set(re.findall(r"\bTD5_TG_PAGE_(\w+)", code)):
                if short == "COUNT":
                    continue
                out[short].append({"file": f, "line": i + 1,
                                   "func": ci.func_at(f, i + 1), "role": role})
    return dict(out)


def biomes(ci: CodeIndex):
    """[{index, name, line, row, pages}] from k_biomes[] in td5_tg_terrain.c."""
    f = "td5_tg_terrain.c"
    L = ci.lines[f]
    start = next((i for i, l in enumerate(L) if "const TG_Biome k_biomes[]" in l), None)
    if start is None:
        return []
    out, i = [], start + 1
    while i < len(L) and not L[i].startswith("};"):
        m = re.match(r'\s*\{\s*"([A-Z_]+)"', L[i])
        if m:
            row, j = L[i], i
            while "}," not in L[j] and "}" not in L[j].split('"')[-1] and j < i + 8:
                j += 1
                row += " " + L[j].strip()
            if j == i:
                # row continues until the closing brace of the initializer
                j = i + 1
                while j < len(L) and not re.match(r"\s*\{|\s*/\*|\};", L[j]):
                    row += " " + L[j].strip()
                    j += 1
            out.append({"index": len(out), "name": m.group(1), "line": i + 1,
                        "row": re.sub(r"\s+", " ", row.strip()),
                        "pages": sorted(set(re.findall(r"TD5_TG_PAGE_(\w+)", row)))})
        i += 1
    return out


def kind_names():
    """TG_GK_* enum order -> names, from td5_trackgen_internal.h."""
    with open(INTERNAL_H, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    m = re.search(r"TG_GK_OTHER\s*=\s*0\s*,(.*?)TG_GK_COUNT", src, re.S)
    rest = re.findall(r"TG_GK_([A-Z]+)", m.group(1)) if m else []
    return ["OTHER"] + rest


if __name__ == "__main__":
    reg = page_registry()
    print("TD5_TG_PAGE_COUNT =", reg["count"], " named macros:", len(reg["names"]))
    unnamed = [s for s in range(reg["count"]) if s not in reg["slot_name"]]
    print("slots without a name:", len(unnamed), unnamed[:20])
    ci = CodeIndex()
    print("files:", len(ci.files), " functions:", sum(len(v) for v in ci.funcs.values()))
    es = emitter_sites(ci)
    for k, v in sorted(es.items()):
        print(f"  {k:12s} {len(v):2d} sites  e.g. {v[0]['file']}:{v[0]['line']} {v[0]['emitters']}")
    print("kinds:", kind_names())
    print("biomes:", [b["name"] for b in biomes(ci)])
