#!/usr/bin/env python3
"""
Byte-faithful extraction of SNK_* frontend label strings from TD5 Language.dll.

Parses the PE export directory, then for each SNK_* export resolves the value:
  (a) inline C string at the export RVA
  (b) a pointer (VA) -> follow once -> string
  (c) an array of consecutive pointers -> follow each -> string table
Resolves VA<->RVA<->file-offset via section headers and ImageBase.
RESEARCH ONLY.
"""
import sys, struct
import pefile

DLL = r"C:/Users/maria/Desktop/Proyectos/TD5RE/original/Language/English/Language.dll"
OUT = r"C:/Users/maria/Desktop/Proyectos/TD5RE/re/analysis/frontend_snk_strings.md"

pe = pefile.PE(DLL, fast_load=True)
pe.parse_data_directories(directories=[
    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])

image_base = pe.OPTIONAL_HEADER.ImageBase
size_of_image = pe.OPTIONAL_HEADER.SizeOfImage

# Build a quick map: which RVAs are valid (within mapped image)
def rva_in_image(rva):
    return 0 < rva < size_of_image

def rva_to_off(rva):
    """RVA -> file offset, or None if not in a section."""
    for s in pe.sections:
        start = s.VirtualAddress
        # use max(raw size, virt size) for the mapped extent
        vend = start + max(s.Misc_VirtualSize, s.SizeOfRawData)
        if start <= rva < vend:
            delta = rva - start
            if delta < s.SizeOfRawData:
                return s.PointerToRawData + delta
            return None  # in BSS/uninitialized part, no file bytes
    return None

raw = pe.__data__  # the raw file bytes

def read_u32_at_rva(rva):
    off = rva_to_off(rva)
    if off is None or off + 4 > len(raw):
        return None
    return struct.unpack_from("<I", raw, off)[0]

def read_cstring_at_rva(rva, maxlen=512):
    off = rva_to_off(rva)
    if off is None:
        return None
    end = off
    while end < len(raw) and end - off < maxlen and raw[end] != 0:
        end += 1
    b = raw[off:end]
    return b

def is_printable_ascii(b):
    if b is None or len(b) == 0:
        return False
    for c in b:
        # allow tab, common punctuation, printable; reject control chars
        if c == 9:
            continue
        if c < 0x20 or c > 0x7E:
            return False
    return True

def looks_like_va_ptr(val):
    if val is None:
        return False
    if val < image_base:
        return False
    rva = val - image_base
    return rva_in_image(rva)

def va_to_rva(val):
    return val - image_base

# --- MSVC name demangling (just enough for SNK_ data exports) ---
# Decorated form: ?SNK_Name@@3<type>A
#   PAD            -> char *            (pointer to string)
#   PAPAD          -> char **           (array of char* / string table)
#   PAPAPAD        -> char ***          (array of char**)
#   PAY0NN@D       -> char[NN]          (inline char array; NN base-16 in @@ digits)
#   PAY0NN@PAD     -> char*[NN]         (array of NN char*)
#   PAY0NN@PAY0MM@D-> char[NN][MM]      (2D char block)
# The trailing 'A' is the cv-qualifier (none). The '3' marks a data (non-fn) symbol.
def parse_msvc_number(s):
    """Parse an MSVC-mangled array dimension at the start of s.
    Encoding (validated against this DLL's data):
      single decimal digit 'd' (0-9)  -> value = d+1
      else hex nibbles 'A'..'P' (A=0..P=15) terminated by '@' -> value = raw hex
    Returns (value, rest)."""
    if not s:
        return (0, s)
    c = s[0]
    if c.isdigit():
        return (int(c) + 1, s[1:])
    at = s.index('@')
    digits = s[:at]
    val = 0
    for ch in digits:
        val = val * 16 + (ord(ch) - ord('A'))
    return (val, s[at + 1:])

def demangle(name):
    """Return (clean_name, kind, info).
    kind in {'string','array_ptr','char_block','car_names_2d','unknown'}:
      'string'      -> char*   : inline C string at RVA
      'array_ptr'   -> char**  : NUL/term-walked table of char* (info=None)
      'char_block'  -> char[N] : 2D block of fixed-stride rows; info=stride
      'car_names_2d'-> char*[K][P] paint-name pointer matrix; info=paint_slots
      'unknown'
    """
    if not name.startswith('?'):
        return (name, 'unknown', None)
    body = name[1:]
    if '@@3' not in body:
        return (name, 'unknown', None)
    clean, typ = body.split('@@3', 1)
    t = typ
    if t == 'PADA':                # char *
        return (clean, 'string', None)
    if t == 'PAPADA':              # char **  (table of char*)
        return (clean, 'array_ptr', None)
    if t == 'PAPAPADA':            # char *** (table of char**)
        return (clean, 'array_ptr', None)
    if t.startswith('PAY'):
        # MSVC array type: 'Y' <ndims-1 indicator '0'/'1'/...> <dim numbers...> <elem>
        # 'Y0' = 1 dim, 'Y1' = 2 dims, etc. Each dim is an MSVC <number>.
        rest = t[2:]               # drop leading 'PA'
        assert rest[0] == 'Y'
        ndims = int(rest[1]) + 1   # 'Y0'->1 dim, 'Y1'->2 dims
        after = rest[2:]
        dims = []
        for _ in range(ndims):
            d, after = parse_msvc_number(after)
            dims.append(d)
        if after.startswith('PAD'):
            # element is char* -> pointer array/matrix.
            #   CarLongNames Y04PAD = char*[5] (paint slots/car) -> 2D name matrix
            #   Info_Values  Y109PAD = char*[1][10] -> flat pointer table
            if ndims == 1 and 1 <= dims[0] <= 8:
                return (clean, 'car_names_2d', dims[0])
            return (clean, 'array_ptr', None)
        if after.startswith('D'):
            # element is char. The fixed-row stride is the LAST (innermost) dim.
            return (clean, 'char_block', dims[-1])
        return (clean, 'unknown', t)
    return (clean, 'unknown', t)

# Gather exports (sorted by RVA so we can measure byte-size = gap to next export)
ed = pe.DIRECTORY_ENTRY_EXPORT
raw_syms = []
for exp in ed.symbols:
    raw_name = exp.name.decode('latin-1') if exp.name else f"<ord {exp.ordinal}>"
    raw_syms.append((exp.address, raw_name, exp.ordinal))
rva_sorted = sorted(set(a for a, _, _ in raw_syms))
def export_byte_size(rva):
    nxts = [r for r in rva_sorted if r > rva]
    return (min(nxts) - rva) if nxts else 256

exports = []
for rva, raw_name, ordn in raw_syms:
    clean, kind, info = demangle(raw_name)
    exports.append((clean, rva, ordn, raw_name, kind, info))
exports.sort(key=lambda e: e[0])

def follow_ptr_array(rva, declared_n=None):
    """Walk consecutive char* pointers at rva. Returns list of (idx, text).
    Stops at first non-VA-pointer or non-ASCII target, or at declared_n if given."""
    entries = []
    cur = rva
    idx = 0
    while True:
        if declared_n is not None and idx >= declared_n:
            break
        w = read_u32_at_rva(cur)
        if not looks_like_va_ptr(w):
            break
        s = read_cstring_at_rva(va_to_rva(w))
        if not is_printable_ascii(s):
            # allow empty string targets (ptr to a NUL)
            if s is not None and len(s) == 0:
                entries.append((idx, ''))
                idx += 1
                cur += 4
                continue
            break
        entries.append((idx, s.decode('latin-1')))
        idx += 1
        cur += 4
        if idx > 512:
            break
    return entries

def resolve_export(rva, kind, info, raw_name):
    """
    Returns (type_str, values_list, note) using the demangled type as the guide.
    """
    total = export_byte_size(rva)

    # char * -> single inline string at the export RVA.
    if kind == 'string':
        s = read_cstring_at_rva(rva)
        if s is not None and is_printable_ascii(s):
            return ('string', [(None, s.decode('latin-1'))], '')
        if s is not None and len(s) == 0:
            return ('string', [(None, '')], 'empty')
        return ('UNRESOLVED', [], f'char* target not ASCII (off={rva_to_off(rva)})')

    # char[N] -> a 2D block of fixed-stride rows. info = stride.
    # Row count = total bytes available / stride.
    if kind == 'char_block':
        stride = info or 0
        off = rva_to_off(rva)
        if off is None or stride <= 0:
            # fall back: treat as a single inline string
            s = read_cstring_at_rva(rva)
            if s is not None and is_printable_ascii(s):
                return ('string', [(None, s.decode('latin-1'))], 'char_block fallback')
            return ('UNRESOLVED', [], f'char_block stride={stride} off={off}')
        count = max(1, total // stride)
        entries = []
        for i in range(count):
            row = raw[off + i*stride : off + (i+1)*stride]
            z = row.find(b'\x00')
            if z >= 0:
                row = row[:z]
            if is_printable_ascii(row):
                entries.append((i, row.decode('latin-1')))
            elif len(row) == 0:
                entries.append((i, ''))
            else:
                entries.append((i, '[non-ascii: %s]' % row[:8].hex()))
        # If only 1 row, present as a plain string for readability.
        if count == 1:
            return ('string', [(None, entries[0][1])], 'char[%d]' % stride)
        return ('array[%d] (char[%d][%d])' % (count, count, stride), entries, '')

    # char**  -> table of char* pointers; the true element count is the byte gap
    # to the next export / 4 (these tables are densely packed in .rdata).
    if kind == 'array_ptr':
        cap = (total // 4) if total else None
        entries = follow_ptr_array(rva, declared_n=cap)
        if not entries:
            return ('UNRESOLVED', [], f'array_ptr: no ASCII targets at off={rva_to_off(rva)}')
        return ('array[%d]' % len(entries), entries, '')

    # char*[K] paint-name matrix (CarLongNames). info = paint slots per car.
    if kind == 'car_names_2d':
        slots = info or 1
        nptr = total // 4
        entries = []
        for i in range(nptr):
            w = read_u32_at_rva(rva + i*4)
            if not looks_like_va_ptr(w):
                break
            s = read_cstring_at_rva(va_to_rva(w))
            if s is None:
                break
            txt = s.decode('latin-1', 'replace') if is_printable_ascii(s) else ''
            if txt:  # only record non-empty paint-name slots
                car = i // slots
                paint = i % slots
                entries.append(('%d.%d' % (car, paint), txt))
        if not entries:
            return ('UNRESOLVED', [], 'car_names_2d: no strings')
        return ('matrix[%d ptrs / %d slots]' % (nptr, slots), entries, '')

    # unknown -> best effort.
    s = read_cstring_at_rva(rva)
    if s is not None and is_printable_ascii(s) and len(s) > 0:
        return ('string', [(None, s.decode('latin-1'))], 'kind=unknown(inline)')
    w = read_u32_at_rva(rva)
    if looks_like_va_ptr(w):
        entries = follow_ptr_array(rva)
        if entries:
            tn = 'string' if len(entries) == 1 else 'array[%d]' % len(entries)
            return (tn, entries, 'kind=unknown(ptr)')
    return ('UNRESOLVED', [], f'kind=unknown ({raw_name})')

# Resolve all
results = []
snk_total = 0
snk_resolved = 0
for clean, rva, ordn, raw_name, kind, info in exports:
    is_snk = clean.startswith('SNK_')
    if is_snk:
        snk_total += 1
    t, vals, note = resolve_export(rva, kind, info, raw_name)
    if is_snk and t != 'UNRESOLVED':
        snk_resolved += 1
    results.append((clean, rva, ordn, t, vals, note, is_snk))

# Write markdown
def esc(s):
    return s.replace('|', '\\|').replace('\n', '\\n').replace('\r', '\\r')

lines = []
lines.append("# TD5 Language.dll — SNK_* frontend label string extraction\n")
lines.append(f"- DLL: `{DLL}`")
lines.append(f"- ImageBase: 0x{image_base:08X}  SizeOfImage: 0x{size_of_image:X}")
lines.append(f"- Total exports: {len(exports)}")
lines.append(f"- SNK_* exports: {snk_total}  resolved: {snk_resolved}  unresolved: {snk_total - snk_resolved}")
lines.append("- Method: PE export-directory parse; per export read word at RVA; "
             "VA-pointer heuristic (>= ImageBase and in image) -> follow as ptr/array, "
             "else inline C string. RESEARCH ONLY.\n")
lines.append("## All SNK_* exports\n")
lines.append("| SNK_symbol | RVA | type | value(s) |")
lines.append("|---|---|---|---|")
for name, rva, ordn, t, vals, note, is_snk in results:
    if not is_snk:
        continue
    if t == 'UNRESOLVED':
        valcell = f"[UNRESOLVED] {esc(note)}"
    elif t == 'string':
        valcell = f'"{esc(vals[0][1])}"'
        if note:
            valcell += f"  ({note})"
    else:
        parts = [f'[{i}]="{esc(v)}"' for (i, v) in vals]
        valcell = "; ".join(parts)
    lines.append(f"| {esc(name)} | 0x{rva:06X} | {t} | {valcell} |")

# Also dump non-SNK exports (resolvable) for completeness
lines.append("\n## Non-SNK_* exports (resolved, for reference)\n")
lines.append("| symbol | RVA | type | value(s) |")
lines.append("|---|---|---|---|")
for name, rva, ordn, t, vals, note, is_snk in results:
    if is_snk:
        continue
    if t == 'UNRESOLVED':
        continue
    if t == 'string':
        valcell = f'"{esc(vals[0][1])}"'
    else:
        parts = [f'[{i}]="{esc(v)}"' for (i, v) in vals]
        valcell = "; ".join(parts)
    lines.append(f"| {esc(name)} | 0x{rva:06X} | {t} | {valcell} |")

with open(OUT, 'w', encoding='utf-8') as f:
    f.write("\n".join(lines) + "\n")

# Console summary for the agent
print(f"TOTAL_EXPORTS={len(exports)} SNK_TOTAL={snk_total} SNK_RESOLVED={snk_resolved} SNK_UNRESOLVED={snk_total-snk_resolved}")
print("=== HIGH-VALUE LABELS ===")
HIGH = [
    'SNK_SelectTrackButTxt','SNK_OkButTxt','SNK_RaceMenuButTxt','SNK_QuickRaceButTxt',
    'SNK_TwoPlayerButTxt','SNK_NetPlayButTxt','SNK_OptionsButTxt','SNK_HiScoreButTxt',
    'SNK_EnterPlayerNameButTxt','SNK_FoggingButTxt',
    'SNK_SFX_Modes','SNK_Split_Modes','SNK_OnOffTxt','SNK_DynamicsTxt','SNK_RaceTypeText',
    'SNK_GameTypeTxt','SNK_ControlText','SNK_Layout_Types','SNK_SpeedReadTxt',
    'SNK_NameTxt','SNK_BestTxt','SNK_CarTxt','SNK_AvgTxt','SNK_TopTxt','SNK_TimeTxt',
    'SNK_LapTxt','SNK_PtsTxt','SNK_SpdTxt','SNK_NowPlayingTxt',
]
rmap = {name:(t,vals,note) for (name,rva,ordn,t,vals,note,is_snk) in results}
for h in HIGH:
    if h in rmap:
        t, vals, note = rmap[h]
        if t == 'string':
            print(f"  {h}\t{t}\t{vals[0][1]!r}")
        elif t == 'UNRESOLVED':
            print(f"  {h}\t{t}\t{note}")
        else:
            print(f"  {h}\t{t}\t" + " | ".join(f'[{i}]={v!r}' for i,v in vals))
    else:
        print(f"  {h}\t<NOT EXPORTED>")

print("=== ALL *ButTxt ===")
for (name,rva,ordn,t,vals,note,is_snk) in results:
    if is_snk and name.endswith('ButTxt'):
        if t == 'string':
            print(f"  {name}\t{vals[0][1]!r}")
        elif t == 'UNRESOLVED':
            print(f"  {name}\t[UNRESOLVED] {note}")
        else:
            print(f"  {name}\t{t}\t" + " | ".join(f'[{i}]={v!r}' for i,v in vals))

print("=== UNRESOLVED SNK_* ===")
for (name,rva,ordn,t,vals,note,is_snk) in results:
    if is_snk and t == 'UNRESOLVED':
        print(f"  {name}\t{note}")
print(f"OUT={OUT}")
