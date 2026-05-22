"""Build the application plan for Wave 2D signature push.

Reads the CSV worklist and produces a JSON plan with per-row decisions:
- action: 'apply' | 'skip'
- reason: short string
- ghidra_signature: rebuilt C signature using Ghidra-known types (if apply)

Type translation map (port -> Ghidra):
  TD5_Actor*           -> RuntimeSlotActor*
  TD5_Actor const*     -> RuntimeSlotActor*  (drop const if Ghidra doesn't model it)
  const TD5_Actor*     -> RuntimeSlotActor*

Types without a Ghidra equivalent that should fall back to void* or int based on context:
  TD5_MeshHeader*      -> void*
  TD5_CarDef*          -> void*
  TD5_TrackSegment*    -> void*

For now we only translate TD5_Actor*. Other unknown types -> SKIP the row
(per Hard Skip Rule 6).
"""

import csv
import json
import re
import sys

CSV_PATH = "re/analysis/followup_sessions/wave1_f_signature_refinement_worklist.csv"
OUT_PATH = "re/analysis/followup_sessions/wave2d_apply_plan.json"

# Whitelist of types Ghidra knows about (verified earlier)
GHIDRA_KNOWN_TYPES = {
    "void", "int", "uint", "short", "ushort", "char", "uchar",
    "long", "ulong", "float", "double", "bool",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "size_t", "ssize_t", "ptrdiff_t",
    "RuntimeSlotActor", "RuntimeSlotActorArray",
    "RuntimeSlotStateEntry", "RuntimeSlotStateTable",
    "TracksideCameraProfile",
    "TD5_GameState", "TD5_GameType", "TD5_InputBits",
    "TD5_VehicleMode", "TD5_WeatherType",
    "TD5_CpuInfo", "TD5_PostRaceCarSelectionBackup",
    "TD5_RaceFinalResultsSlot",
    # MSVC stdlib types
    "FILE", "HWND", "HINSTANCE", "HANDLE", "LPVOID", "LPCSTR", "LPSTR",
    "DWORD", "WORD", "BYTE", "BOOL", "UINT", "INT",
}

# Port type -> Ghidra type rewrites
PORT_TO_GHIDRA = {
    "TD5_Actor": "RuntimeSlotActor",
}

# Types that explicitly don't exist in Ghidra -> trigger SKIP
PORT_ONLY_TYPES = {
    "TD5_MeshHeader", "TD5_CarDef", "TD5_TrackSegment", "TD5_TrackSpan",
    "TD5_Mesh", "TD5_Texture", "TD5_AudioVoice", "TD5_Camera",
    "TD5_RaceState", "TD5_HudState", "TD5_RouteTable", "TD5_RouteNode",
    "TD5_Particle", "TD5_Smoke", "TD5_TailLight", "TD5_FrontendScreen",
    "TD5_InputState", "TD5_SaveData", "TD5_ConfigEntry",
    # Discovered missing during Wave 2D plan-build
    "TD5_MeshVertex", "TD5_PrimitiveCmd", "TD5_LightZone",
    "TD5_StripSpan", "TD5_StaticHedEntry",
    "OBB_CornerData",
    # Second sweep
    "TD5_Mat3x3", "TD5_RaceRenderPass", "TD5_AtlasEntry",
    "TD5_File", "TD5_ScreenIndex", "TD5_TrackProbeState",
    "TD5_Vec3f", "TD5_ZipEntry",
}

# Replacement of standard C99 fixed-width int types -> Ghidra-equivalent simple types.
# Ghidra's C parser doesn't accept `int32_t`/`uint32_t` etc. directly, so collapse them.
TYPE_REWRITES = [
    ("uint64_t", "ulonglong"),
    ("int64_t", "longlong"),
    ("uint32_t", "uint"),
    ("int32_t", "int"),
    ("uint16_t", "ushort"),
    ("int16_t", "short"),
    ("uint8_t", "byte"),
    ("int8_t", "char"),
]

# Match a typename token (identifier optionally with const/struct/unsigned prefixes).
# We split by whitespace+'*' boundaries.

TYPE_TOKEN_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\b")


def translate_signature(port_sig, ghidra_name):
    """Translate a port-side C signature string to one usable with Ghidra.

    - Replaces port type names with Ghidra equivalents where known.
    - Replaces the function name in the signature with `ghidra_name`.
    - Strips `static`, `inline`, `__cdecl`, `__stdcall` qualifiers.
    - Returns (new_sig, ok, reason). If ok is False, signature should be skipped.
    """
    s = port_sig.strip()
    # Strip leading qualifiers
    for q in ("static inline ", "static ", "inline ", "__cdecl ", "__stdcall "):
        if s.startswith(q):
            s = s[len(q):]

    # Replace port-only types: SKIP
    for ty in PORT_ONLY_TYPES:
        if re.search(rf"\b{re.escape(ty)}\b", s):
            return None, False, f"port-only type {ty}"

    # Map known port -> Ghidra types
    for src, dst in PORT_TO_GHIDRA.items():
        s = re.sub(rf"\b{re.escape(src)}\b", dst, s)
    # Rewrite C99 fixed-width int types to Ghidra primitives
    for src, dst in TYPE_REWRITES:
        s = re.sub(rf"\b{re.escape(src)}\b", dst, s)

    # Replace function name: pattern is `<return_type> <name>(...)`.
    # Find the first '(' and the identifier directly before it.
    paren = s.find("(")
    if paren < 0:
        return None, False, "no '(' in signature"
    head = s[:paren].rstrip()
    tail = s[paren:]
    # Pull last identifier from head
    m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", head)
    if not m:
        return None, False, "no function name in signature"
    port_func_name = m.group(1)
    head = head[: m.start()].rstrip()
    # Reassemble using ghidra_name
    new_sig = f"{head} {ghidra_name}{tail}"
    return new_sig, True, ""


def main():
    plan = []
    counts = {
        "good": {"apply": 0, "skip": 0},
        "partial": {"apply": 0, "skip": 0},
        "weak": {"apply": 0, "skip": 0},
    }
    with open(CSV_PATH, encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            quality = row["signature_quality_hint"].strip()
            port_sig = row["port_signature"].strip().strip('"')
            addr = row["orig_address"].strip()
            port_name = row["port_function_name"].strip()
            entry = {
                "addr": addr,
                "port_name": port_name,
                "port_sig": port_sig,
                "quality": quality,
            }
            # Quality-based pre-screen
            if quality == "weak":
                # For weak: only apply if the signature carries information beyond `int param_N`.
                # If the param list is all `int` or `void`, skip — no improvement.
                paren_open = port_sig.find("(")
                paren_close = port_sig.rfind(")")
                params = port_sig[paren_open + 1 : paren_close].strip() if paren_open >= 0 else ""
                if params == "" or params == "void":
                    # void-arg sig — might still set return type
                    pass
                elif all(p.strip().lstrip("const ").split()[0] in ("int", "unsigned", "void")
                         for p in params.split(",")):
                    # All-int params, no improvement — skip
                    entry["action"] = "skip"
                    entry["reason"] = "weak: all-int params, no improvement"
                    counts[quality]["skip"] += 1
                    plan.append(entry)
                    continue
            # Pre-translate signature: skip if it references a port-only type
            translated, ok, reason = translate_signature(port_sig, "__PLACEHOLDER__")
            if not ok:
                entry["action"] = "skip"
                entry["reason"] = reason
                counts[quality]["skip"] += 1
                plan.append(entry)
                continue
            entry["action"] = "apply"
            entry["translated_template"] = translated  # name=__PLACEHOLDER__
            entry["reason"] = ""
            counts[quality]["apply"] += 1
            plan.append(entry)
    with open(OUT_PATH, "w", encoding="utf-8") as f:
        json.dump({"counts": counts, "rows": plan}, f, indent=2)
    print("counts:", json.dumps(counts, indent=2))
    print(f"total apply: {sum(c['apply'] for c in counts.values())}")
    print(f"total skip: {sum(c['skip'] for c in counts.values())}")
    print(f"wrote {OUT_PATH}")


if __name__ == "__main__":
    main()
