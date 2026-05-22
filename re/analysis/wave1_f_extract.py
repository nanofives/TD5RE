#!/usr/bin/env python3
"""Wave 1 Agent F — extract three Wave-2 worklists from port source.

Outputs:
  re/analysis/followup_sessions/wave1_f_signature_refinement_worklist.csv
  re/analysis/followup_sessions/wave1_f_comment_sync_plan.csv
  re/analysis/followup_sessions/wave1_f_local_naming_targets.csv
  re/analysis/followup_sessions/wave1_f_overview.md
"""
from __future__ import annotations
import csv
import re
import sys
from pathlib import Path
from collections import defaultdict

ROOT = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT / "td5mod" / "src" / "td5re"
OUT_DIR = ROOT / "re" / "analysis" / "followup_sessions"
OUT_DIR.mkdir(exist_ok=True, parents=True)
CONFIDENCE_CSV = ROOT / "re" / "analysis" / "ghidra_confidence_map_2026-05-18.csv"

# -----------------------------------------------------------------------------
# Pattern matchers
# -----------------------------------------------------------------------------
# [CONFIRMED @ 0x004340C0]    or   [CONFIRMED @ 0x434FFB]
CONFIRMED_RE = re.compile(r"\[CONFIRMED\s*@\s*0x([0-9a-fA-F]+)\]", re.IGNORECASE)
# [ARCH-DIVERGENCE: ...]      or   [ARCH-DIVERGENCE @ 0x...]
ARCH_RE = re.compile(r"\[ARCH-DIVERGENCE[^\]]*\]", re.IGNORECASE)
# Capture addr inside ARCH-DIVERGENCE block if present
ARCH_ADDR_RE = re.compile(r"\[ARCH-DIVERGENCE[^\]]*?0x([0-9a-fA-F]+)[^\]]*\]", re.IGNORECASE)
# FUN_XXXXXXXX naming pattern from Ghidra
FUN_RE = re.compile(r"\bFUN_([0-9a-fA-F]{6,8})\b")
# In-comment `(0x00432B30)` or `@ 0x00432B30` or `0x00432B30:` address mentions
ANY_ADDR_RE = re.compile(r"0x([0-9a-fA-F]{6,8})\b")

# Function definition: collect return type + name + parameter list. Allow
# multi-line signatures.
FUNC_DEF_RE = re.compile(
    r"^([A-Za-z_][\w *\t]*?)\s+([a-zA-Z_]\w*)\s*\(([^)]*)\)\s*\{?\s*$",
    re.MULTILINE,
)

# Block-comment opener `/* ` and closer ` */`.
COMMENT_BLOCK_RE = re.compile(r"/\*([\s\S]*?)\*/", re.MULTILINE)

# -----------------------------------------------------------------------------
# Source loading
# -----------------------------------------------------------------------------
def load_src_files() -> list[Path]:
    files = []
    for p in sorted(SRC_DIR.glob("*.c")):
        files.append(p)
    for p in sorted(SRC_DIR.glob("*.h")):
        files.append(p)
    return files

# -----------------------------------------------------------------------------
# Function discovery
# -----------------------------------------------------------------------------
def parse_function_defs(path: Path) -> list[dict]:
    """Return list of {name, signature, ret, params, start_line, end_line}.
    Only top-level function definitions (column 0 start).
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    funcs = []

    # Pattern: starts in column 0, contains '(' (function signature), eventually
    # ends with ')' followed by either '{' on same line or '{' on next non-blank
    # line. We track parenthesis nesting to handle multi-line parameter lists.
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if not line:
            i += 1
            continue
        # Must start in column 0
        if line[0] in " \t":
            i += 1
            continue
        stripped = line.strip()
        if not stripped or stripped.startswith("//") or stripped.startswith("/*"):
            i += 1
            continue
        if stripped.startswith("#") or stripped.startswith("typedef"):
            i += 1
            continue
        if any(stripped.startswith(k) for k in
               ("struct ", "enum ", "union ", "static struct",
                "extern struct", "static enum", "const struct",
                "static const struct")):
            i += 1
            continue
        # Reject simple variable declarations: no '(' at all OR '(' inside an
        # initializer string like `= (foo){...}`. Require '(' before any '='
        # or ';' on this line.
        eq_pos = line.find("=")
        sc_pos = line.find(";")
        paren_pos = line.find("(")
        if paren_pos < 0:
            i += 1
            continue
        if 0 <= eq_pos < paren_pos:
            i += 1
            continue
        if 0 <= sc_pos < paren_pos:
            i += 1
            continue

        # Collect lines until parenthesis balance hits 0 AND we see '{' or ';'
        buf = ""
        j = i
        paren_balance = 0
        started_paren = False
        saw_brace = False
        saw_semi = False
        while j < n:
            l = lines[j]
            for ch in l:
                if ch == "(":
                    paren_balance += 1
                    started_paren = True
                elif ch == ")":
                    paren_balance -= 1
                elif ch == "{" and started_paren and paren_balance == 0:
                    saw_brace = True
                elif ch == ";" and started_paren and paren_balance == 0:
                    saw_semi = True
            buf += l + " "
            if saw_brace or saw_semi:
                break
            j += 1
            if j - i > 30:
                # Pathological — bail
                break
        if saw_semi and not saw_brace:
            i = j + 1
            continue
        if not saw_brace:
            i = j + 1
            continue
        # Extract signature: from start to closing ')' of top-level paren.
        # Find indices of first '(' and matching ')' in buf.
        first_paren = buf.find("(")
        if first_paren < 0:
            i = j + 1
            continue
        depth = 0
        close_paren = -1
        for k in range(first_paren, len(buf)):
            if buf[k] == "(":
                depth += 1
            elif buf[k] == ")":
                depth -= 1
                if depth == 0:
                    close_paren = k
                    break
        if close_paren < 0:
            i = j + 1
            continue
        signature_text = buf[:close_paren + 1].strip()
        params_text = buf[first_paren + 1:close_paren].strip()
        # Now name = last identifier before first_paren.
        before = buf[:first_paren].strip()
        name_match = re.search(r"([A-Za-z_]\w*)\s*$", before)
        if not name_match:
            i = j + 1
            continue
        name = name_match.group(1)
        ret_type = before[:name_match.start()].strip()
        # Skip language keywords as names
        if name in ("if", "while", "for", "switch", "return", "sizeof",
                    "do", "else", "case", "default"):
            i = j + 1
            continue
        if not ret_type or ret_type in (",", ";"):
            i = j + 1
            continue
        funcs.append({
            "path": path,
            "name": name,
            "ret": ret_type,
            "params": params_text,
            "signature": f"{ret_type} {name}({params_text})",
            "start_line": i + 1,
            "end_line": j + 1,
        })
        i = j + 1

    return funcs

# -----------------------------------------------------------------------------
# Function header comment extraction — find the comment block immediately
# preceding each function definition.
# -----------------------------------------------------------------------------
def header_comment_for_func(path_text: str, func_start_idx: int) -> str:
    """Walk backwards from `func_start_idx` (0-based line) to gather up to
    ~50 lines of comment context preceding the function. Returns the joined
    text. Handles cases where multiple `/* ... */` blocks sit above the def
    or there are forward-decl `extern` lines in between.
    """
    lines = path_text.splitlines()
    i = func_start_idx - 1
    end = i
    # Collect up to 50 prior lines that are part of /* ... */ blocks, blank,
    # or `*` continuation lines.
    collected_start = i
    walked = 0
    in_block = False
    while i >= 0 and walked < 60:
        l = lines[i]
        stripped = l.strip()
        if not stripped:
            collected_start = i
            i -= 1
            walked += 1
            continue
        # Inside a comment block or comment line?
        if "*/" in l:
            in_block = True
            collected_start = i
            i -= 1
            walked += 1
            continue
        if in_block:
            collected_start = i
            if "/*" in l:
                in_block = False
            i -= 1
            walked += 1
            continue
        if stripped.startswith("*") or stripped.startswith("//"):
            collected_start = i
            i -= 1
            walked += 1
            continue
        if stripped.startswith("/*"):
            collected_start = i
            i -= 1
            walked += 1
            continue
        # Allow forward-decl `extern void foo(...)` lines as context bridges.
        if stripped.startswith("extern ") and ";" in stripped:
            i -= 1
            walked += 1
            continue
        # Otherwise stop.
        break
    if collected_start > end:
        return ""
    block = "\n".join(lines[collected_start:end + 1])
    return block

# -----------------------------------------------------------------------------
# Signature quality heuristic
# -----------------------------------------------------------------------------
GENERIC_TYPES = {
    "int", "unsigned", "long", "short", "char", "void", "uint8_t",
    "uint16_t", "uint32_t", "int8_t", "int16_t", "int32_t", "size_t",
    "ptrdiff_t", "intptr_t", "uintptr_t", "void*", "void *", "int*",
    "int *", "char*", "char *", "u8", "u16", "u32", "i8", "i16", "i32",
    "BOOL", "bool", "float", "double", "byte", "word", "dword",
}
TYPED_HINTS = {
    "TD5_", "td5_", "Actor", "RaceActor", "Cardef", "Frame", "MenuScreen",
    "Track", "Span", "Camera", "AI", "RouteState", "DDraw", "Wheel",
    "VFX", "HUD", "Track", "Mesh", "Texture", "ZipEntry", "ZipArchive",
}

def signature_quality(params: str) -> str:
    if not params or params.strip() in ("", "void"):
        return "good"  # void is fine
    parts = [p.strip() for p in re.split(r",(?![^(]*\))", params)]
    if not parts:
        return "weak"
    typed = 0
    generic = 0
    for p in parts:
        # Strip variable name (last identifier)
        tokens = p.replace("*", " * ").split()
        if not tokens:
            continue
        # Heuristic: token containing 'TD5_' or 'td5_' types indicates typed.
        has_typed = any(any(h in tok for h in TYPED_HINTS) for tok in tokens)
        is_generic_only = all(
            (tok in GENERIC_TYPES or tok in ("*", "const", "volatile",
                                              "restrict", "register",
                                              "unsigned", "signed"))
            or tok.lower() in GENERIC_TYPES
            for tok in tokens[:-1]  # exclude variable name
        )
        if has_typed:
            typed += 1
        elif is_generic_only:
            generic += 1
        else:
            # Could be a typedef like FILE *, size_t, etc.
            generic += 1
    total = typed + generic
    if total == 0:
        return "weak"
    if typed == total:
        return "good"
    if typed > 0:
        return "partial"
    return "weak"

# -----------------------------------------------------------------------------
# Address extraction from header comment + function body
# -----------------------------------------------------------------------------
def addrs_in_text(text: str) -> tuple[set[str], set[str]]:
    confirmed = set(m.group(1).lower() for m in CONFIRMED_RE.finditer(text))
    arch = set(m.group(1).lower() for m in ARCH_ADDR_RE.finditer(text))
    return confirmed, arch

# Also: identify any `@ 0xADDR` mention in the header comment as the
# canonical mapping address.
HEADER_ADDR_RE = re.compile(r"@\s*0x([0-9a-fA-F]{6,8})\b", re.IGNORECASE)

def _valid_orig_addr(addr_hex: str) -> bool:
    """Filter out short / spurious numbers. TD5_d3d.exe text section is
    in the 0x00401000 - 0x004BXXXX range."""
    try:
        v = int(addr_hex, 16)
    except ValueError:
        return False
    return 0x00400000 <= v <= 0x004FFFFF

def canonical_addr_from_header(comment: str) -> str | None:
    # Prefer CONFIRMED @ 0x...
    for m in CONFIRMED_RE.finditer(comment):
        h = m.group(1).lower()
        if _valid_orig_addr(h):
            return h.zfill(8)
    # Else ARCH-DIVERGENCE addr
    for m in ARCH_ADDR_RE.finditer(comment):
        h = m.group(1).lower()
        if _valid_orig_addr(h):
            return h.zfill(8)
    # Else FUN_XXXXXXXX
    for m in FUN_RE.finditer(comment):
        h = m.group(1).lower()
        if _valid_orig_addr(h):
            return h.zfill(8)
    # Else '@ 0x...' or '(0x...)' inside header
    for m in HEADER_ADDR_RE.finditer(comment):
        h = m.group(1).lower()
        if _valid_orig_addr(h):
            return h.zfill(8)
    # Else any orig-range 0x...
    for m in ANY_ADDR_RE.finditer(comment):
        h = m.group(1).lower()
        if _valid_orig_addr(h):
            return h.zfill(8)
    return None

# -----------------------------------------------------------------------------
# Task 1: function signature refinement
# -----------------------------------------------------------------------------
def compute_body_extent(path_text_lines: list[str],
                         func_start_idx: int, max_lines: int = 4000) -> int:
    """Return the number of lines this function spans (>=0). Uses brace
    counting from the first `{` after the start line."""
    depth = 0
    started = False
    for i in range(func_start_idx, min(func_start_idx + max_lines,
                                       len(path_text_lines))):
        line = path_text_lines[i]
        # Strip strings + comments first (approximate)
        s = re.sub(r'"(?:\\.|[^\\"])*"', '""', line)
        s = re.sub(r"'(?:\\.|[^\\'])*'", "''", s)
        for ch in s:
            if ch == "{":
                depth += 1
                started = True
            elif ch == "}":
                depth -= 1
                if started and depth == 0:
                    return i - func_start_idx
    return 0

def task1_signature_refinement(funcs_by_path: dict[Path, list[dict]],
                                confidence_by_addr: dict[str, dict]
                                ) -> list[dict]:
    # Collect candidates per addr, then pick the best.
    candidates_by_addr: dict[str, list[dict]] = defaultdict(list)
    for path, funcs in funcs_by_path.items():
        text = path.read_text(encoding="utf-8", errors="replace")
        text_lines = text.splitlines()
        for f in funcs:
            comment = header_comment_for_func(text, f["start_line"] - 1)
            addr = canonical_addr_from_header(comment)
            if not addr:
                continue
            quality = signature_quality(f["params"])
            sig = f["signature"].replace("\n", " ").replace("  ", " ").strip()
            sig = sig.rstrip("{").strip()
            is_pilot = f["name"].startswith(("td5_pilot_", "td5_trace_",
                                             "pilot_", "trace_"))
            body_size = compute_body_extent(text_lines, f["start_line"] - 1)
            score = 0
            if not is_pilot:
                score += 1000
            if quality == "good":
                score += 100
            elif quality == "partial":
                score += 50
            score += min(body_size, 500)
            candidates_by_addr[addr].append({
                "score": score,
                "row": {
                    "orig_address": "0x" + addr,
                    "port_function_name": f["name"],
                    "port_signature": sig,
                    "signature_quality_hint": quality,
                },
            })
    rows = []
    for addr, cands in candidates_by_addr.items():
        cands.sort(key=lambda c: -c["score"])
        rows.append(cands[0]["row"])
    rows.sort(key=lambda r: r["orig_address"])
    return rows

# -----------------------------------------------------------------------------
# Task 2: comment sync plan — every [CONFIRMED] / [ARCH-DIVERGENCE] header
# -----------------------------------------------------------------------------
def task2_comment_sync(files: list[Path]) -> list[dict]:
    rows = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        lines = text.splitlines()
        # Locate every [CONFIRMED ...] / [ARCH-DIVERGENCE ...] mention with
        # line numbers.
        for ln_idx, line in enumerate(lines):
            for m in CONFIRMED_RE.finditer(line):
                addr = m.group(1).lower().zfill(8)
                summary = _summarize_around(lines, ln_idx)
                rows.append({
                    "orig_address": "0x" + addr,
                    "comment_kind": "CONFIRMED",
                    "port_file": path.name,
                    "port_line": ln_idx + 1,
                    "comment_text_short": summary,
                })
            for m in ARCH_RE.finditer(line):
                # Try to extract address(es); if none on this line, attempt
                # to find one nearby (within 5 lines above/below).
                line_addrs = []
                for ma in ANY_ADDR_RE.finditer(line):
                    h = ma.group(1).lower()
                    if _valid_orig_addr(h):
                        line_addrs.append(h.zfill(8))
                if not line_addrs:
                    nearby = _nearby_addr(lines, ln_idx)
                    if nearby:
                        line_addrs = [nearby]
                summary = _summarize_around(lines, ln_idx)
                if not line_addrs:
                    rows.append({
                        "orig_address": "",
                        "comment_kind": "ARCH-DIVERGENCE",
                        "port_file": path.name,
                        "port_line": ln_idx + 1,
                        "comment_text_short": summary,
                    })
                else:
                    for addr in line_addrs:
                        rows.append({
                            "orig_address": "0x" + addr,
                            "comment_kind": "ARCH-DIVERGENCE",
                            "port_file": path.name,
                            "port_line": ln_idx + 1,
                            "comment_text_short": summary,
                        })
    return rows

def _summarize_around(lines: list[str], idx: int) -> str:
    """Compose a one-line ~150-char summary near `idx`."""
    text = lines[idx].strip().lstrip("*/ \t").lstrip("* ")
    if len(text) >= 80:
        return text[:150]
    # Append the next line if present
    if idx + 1 < len(lines):
        text2 = lines[idx + 1].strip().lstrip("*/ \t").lstrip("* ")
        text = (text + " " + text2).strip()
    return text[:150]

def _nearby_addr(lines: list[str], idx: int) -> str | None:
    """Search ±5 lines around idx for a 4-8 hex digit @ 0x... addr."""
    for off in range(1, 6):
        for j in (idx - off, idx + off):
            if 0 <= j < len(lines):
                m = HEADER_ADDR_RE.search(lines[j])
                if m:
                    return m.group(1).lower().zfill(8)
    return None

# -----------------------------------------------------------------------------
# Task 3: local variable naming targets — pick top 50 by port_citations.
# -----------------------------------------------------------------------------
def load_confidence_csv() -> list[dict]:
    rows = []
    with CONFIDENCE_CSV.open(encoding="utf-8") as fp:
        rdr = csv.DictReader(fp)
        for r in rdr:
            try:
                r["port_citations_i"] = int(r.get("port_citations") or 0)
            except ValueError:
                r["port_citations_i"] = 0
            rows.append(r)
    return rows

def task3_local_names(funcs_by_path: dict[Path, list[dict]],
                       confidence_rows: list[dict]) -> list[dict]:
    # Filter: only rows with non-empty `files_cited` and port_citations >= 1
    cands = [r for r in confidence_rows
             if r.get("files_cited") and r["port_citations_i"] >= 1]
    cands.sort(key=lambda r: -r["port_citations_i"])
    # Take top 50 mapped functions where we can find a port body to inspect.
    port_def_by_name: dict[str, dict] = {}
    port_defs_by_header_addr: dict[str, dict] = {}
    for funcs in funcs_by_path.values():
        for f in funcs:
            port_def_by_name.setdefault(f["name"], f)

    # Build addr -> port_def index using header comments. Keep ALL candidates
    # so the local-extraction step can fall back if the top pick has no
    # useful locals.
    addr_candidates_full: dict[str, list[dict]] = defaultdict(list)
    for path, funcs in funcs_by_path.items():
        text = path.read_text(encoding="utf-8", errors="replace")
        text_lines = text.splitlines()
        for f in funcs:
            comment = header_comment_for_func(text, f["start_line"] - 1)
            addr = canonical_addr_from_header(comment)
            if not addr:
                continue
            is_pilot = f["name"].startswith(("td5_pilot_", "td5_trace_",
                                             "pilot_", "trace_"))
            body_size = compute_body_extent(text_lines, f["start_line"] - 1)
            f["_body_size"] = body_size
            score = (0 if is_pilot else 1000) + min(body_size, 500)
            addr_candidates_full[addr].append((score, f))
    for addr, ac in addr_candidates_full.items():
        ac.sort(key=lambda x: -x[0])
        port_defs_by_header_addr[addr] = ac[0][1]

    # Build addr -> body-mention-based port def index. For each port function,
    # if its body contains the orig addr as a literal (e.g. `0x00404030`), and
    # the addr is part of the top-N list, accept that function as the
    # candidate. This catches port functions whose names differ but contain
    # explicit `[CONFIRMED @ 0x...]` body annotations.
    port_defs_by_body_addr: dict[str, dict] = {}
    for path, funcs in funcs_by_path.items():
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        # Build sorted func boundaries for fast lookup
        sorted_funcs = sorted(funcs, key=lambda f: f["start_line"])
        for idx, f in enumerate(sorted_funcs):
            start = f["start_line"] - 1
            end = sorted_funcs[idx + 1]["start_line"] - 1 \
                  if idx + 1 < len(sorted_funcs) else len(lines)
            body = "\n".join(lines[start:end])
            # Capture first CONFIRMED addr inside body
            for m in CONFIRMED_RE.finditer(body):
                h = m.group(1).lower()
                if _valid_orig_addr(h):
                    port_defs_by_body_addr.setdefault(h.zfill(8), f)
                    break

    top_funcs = []
    seen_port_defs = set()
    for r in cands:
        if len(top_funcs) >= 50:
            break
        gname = r["name"]
        addr_norm = r["address"].lower().zfill(8)
        # Build ordered list of port-def candidates for this addr.
        port_def_cands: list[dict] = []
        candidates = {gname,
                      _camel_to_snake(gname),
                      "td5_" + _camel_to_snake(gname),
                      gname.lower(),
                      "_" + gname,
                     }
        for c in candidates:
            if c in port_def_by_name:
                port_def_cands.append(port_def_by_name[c])
                break
        # All header-comment-by-addr matches
        if addr_norm in addr_candidates_full:
            for score, f in addr_candidates_full[addr_norm]:
                if f not in port_def_cands:
                    port_def_cands.append(f)
        # Body-CONFIRMED-by-addr fallback
        if addr_norm in port_defs_by_body_addr:
            f = port_defs_by_body_addr[addr_norm]
            if f not in port_def_cands:
                port_def_cands.append(f)
        if not port_def_cands:
            continue
        # Pick the FIRST candidate that yields >= 1 useful local. Skip ones
        # already claimed by other addrs.
        chosen = None
        for cand in port_def_cands:
            def_key = (cand["path"].name, cand["name"], cand["start_line"])
            if def_key in seen_port_defs:
                continue
            # Quick check: extract locals
            path = cand["path"]
            text = path.read_text(encoding="utf-8", errors="replace")
            lines = text.splitlines()
            body_end = _find_body_end(lines, cand["start_line"] - 1)
            body_lines = lines[cand["start_line"] - 1: body_end + 1]
            locs = _extract_locals(body_lines, cand["start_line"])
            if len(locs) >= 1:
                chosen = (cand, locs)
                break
        if chosen is None:
            # Accept the first one with the lowest seen overlap, even if 0
            # locals, so addr coverage is recorded.
            for cand in port_def_cands:
                def_key = (cand["path"].name, cand["name"], cand["start_line"])
                if def_key in seen_port_defs:
                    continue
                chosen = (cand, [])
                break
        if chosen is None:
            continue
        port_def, _locs = chosen
        def_key = (port_def["path"].name, port_def["name"], port_def["start_line"])
        seen_port_defs.add(def_key)
        top_funcs.append({
            "addr": addr_norm,
            "ghidra_name": gname,
            "port_def": port_def,
            "_precomputed_locals": _locs,
        })

    rows = []
    for entry in top_funcs:
        port_def = entry["port_def"]
        path = port_def["path"]
        locals_found = entry.get("_precomputed_locals") or []
        if not locals_found:
            text = path.read_text(encoding="utf-8", errors="replace")
            lines = text.splitlines()
            start = port_def["start_line"]
            body_end = _find_body_end(lines, start - 1)
            body_lines = lines[start - 1: body_end + 1]
            locals_found = _extract_locals(body_lines, start)
        # Take up to 6 best per func
        locals_found = locals_found[:6]
        for loc in locals_found:
            rows.append({
                "orig_address": "0x" + entry["addr"],
                "port_file": path.name,
                "port_line": loc["line"],
                "suggested_name": loc["name"],
                "suggested_type": loc["type"],
                "context_snippet": loc["context"],
            })
    return rows

def _camel_to_snake(name: str) -> str:
    s = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s).lower()

def _find_body_end(lines: list[str], start_idx: int) -> int:
    """Given function-def line index (0-based), find matching closing brace.
    Returns 0-based index of `}` line.
    """
    depth = 0
    started = False
    for i in range(start_idx, min(start_idx + 4000, len(lines))):
        for ch in lines[i]:
            if ch == "{":
                depth += 1
                started = True
            elif ch == "}":
                depth -= 1
                if started and depth == 0:
                    return i
    return min(start_idx + 200, len(lines) - 1)

# Local-decl pattern: type + name + optional [= ...] or [N]. Keep it
# conservative — we want names that look meaningful, not 1-2 char placeholders.
LOCAL_DECL_RE = re.compile(
    r"^\s*(?:const\s+|volatile\s+|register\s+|static\s+)*"
    r"((?:struct\s+|enum\s+|union\s+)?[A-Za-z_][\w]*(?:\s*\*+|\s+\*+)?)"
    r"\s+([a-z_][a-z0-9_]{3,40})\s*(?:\[[^\]]*\])?\s*(?:=|;)",
    re.MULTILINE,
)

SKIP_NAMES = {
    "if", "while", "for", "switch", "return", "do", "else", "case",
    "default", "typedef", "static", "extern", "const",
    "volatile", "register", "auto", "break", "continue", "goto",
    "sizeof", "i", "j", "k", "x", "y", "z", "w", "tmp", "ret",
}
SKIP_TYPES = {
    "if", "while", "for", "switch", "return", "do", "else", "case",
    "default", "typedef",
}

def _extract_locals(body_lines: list[str], func_start: int) -> list[dict]:
    """Heuristic: scan body for variable decls with a meaningful name
    (>=4 chars, not a keyword, not a placeholder). Return up to 8."""
    results = []
    seen = set()
    in_body = False
    for rel_idx, raw in enumerate(body_lines):
        line = raw.rstrip("\n")
        if not in_body:
            if "{" in line:
                in_body = True
            continue
        # Stop scanning beyond ~250 lines to keep work bounded
        if rel_idx > 250:
            break
        # Skip comment-only lines
        s = line.strip()
        if s.startswith("/*") or s.startswith("*") or s.startswith("//"):
            continue
        m = LOCAL_DECL_RE.match(line)
        if not m:
            continue
        type_str = m.group(1).strip()
        name = m.group(2).strip()
        if name in SKIP_NAMES or type_str.lower() in SKIP_TYPES:
            continue
        if name in seen:
            continue
        if name.startswith("__"):
            continue
        if len(name) < 4:
            continue
        # Reject placeholder-style names
        if name in ("temp", "buf", "ptr", "len", "val", "out", "src", "dst",
                    "arg", "obj", "idx", "tmp1", "tmp2", "var0", "var1",
                    "var2", "var3"):
            continue
        if not type_str or type_str in ("static", "const"):
            continue
        seen.add(name)
        results.append({
            "name": name,
            "type": type_str,
            "line": func_start + rel_idx,
            "context": line.strip()[:120],
        })
        if len(results) >= 8:
            break
    return results

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
def main() -> int:
    print("[wave1_f] loading source files...", flush=True)
    files = load_src_files()
    print(f"[wave1_f] {len(files)} files", flush=True)

    funcs_by_path: dict[Path, list[dict]] = {}
    total_funcs = 0
    for p in files:
        funcs = parse_function_defs(p)
        funcs_by_path[p] = funcs
        total_funcs += len(funcs)
    print(f"[wave1_f] {total_funcs} function defs parsed", flush=True)

    confidence_rows = load_confidence_csv()
    confidence_by_addr = {r["address"].lower().zfill(8): r
                          for r in confidence_rows}

    # Task 1
    rows1 = task1_signature_refinement(funcs_by_path, confidence_by_addr)
    out1 = OUT_DIR / "wave1_f_signature_refinement_worklist.csv"
    with out1.open("w", encoding="utf-8", newline="") as fp:
        w = csv.DictWriter(fp, fieldnames=[
            "orig_address", "port_function_name", "port_signature",
            "signature_quality_hint"])
        w.writeheader()
        w.writerows(rows1)
    print(f"[wave1_f] task1: {len(rows1)} rows -> {out1.name}", flush=True)

    # Task 2
    rows2 = task2_comment_sync(files)
    # Dedup by (addr, file, line)
    seen2 = set()
    uniq2 = []
    for r in rows2:
        key = (r["orig_address"], r["port_file"], r["port_line"])
        if key in seen2:
            continue
        seen2.add(key)
        uniq2.append(r)
    out2 = OUT_DIR / "wave1_f_comment_sync_plan.csv"
    with out2.open("w", encoding="utf-8", newline="") as fp:
        w = csv.DictWriter(fp, fieldnames=[
            "orig_address", "comment_kind", "port_file", "port_line",
            "comment_text_short"])
        w.writeheader()
        w.writerows(uniq2)
    print(f"[wave1_f] task2: {len(uniq2)} rows -> {out2.name}", flush=True)

    # Task 3
    rows3 = task3_local_names(funcs_by_path, confidence_rows)
    out3 = OUT_DIR / "wave1_f_local_naming_targets.csv"
    with out3.open("w", encoding="utf-8", newline="") as fp:
        w = csv.DictWriter(fp, fieldnames=[
            "orig_address", "port_file", "port_line", "suggested_name",
            "suggested_type", "context_snippet"])
        w.writeheader()
        w.writerows(rows3)
    print(f"[wave1_f] task3: {len(rows3)} rows -> {out3.name}", flush=True)

    return 0

if __name__ == "__main__":
    sys.exit(main())
