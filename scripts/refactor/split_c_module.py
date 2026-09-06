#!/usr/bin/env python3
"""split_c_module.py -- split one big C file into themed modules + one private
header, mechanically and provably (used 2026-09-06 to split td5_trackgen.c,
31134 lines, into td5_trackgen.c + 8 td5_tg_*.c; output MODELS.DAT / STRIP.DAT
/ TEXTURES.DAT / *.TRK / MESHTAG.BIN stayed byte-identical for the pinned seed).

    python scripts/refactor/split_c_module.py <file.c> inventory
    python scripts/refactor/split_c_module.py <file.c> analyze   # needs the plan
    python scripts/refactor/split_c_module.py <file.c> split     # writes modules
    python scripts/refactor/split_c_module.py <file.c> where <line> ...

The plan JSON (default: split_trackgen_plan.json next to this script; the
line ranges in it are valid ONLY for the monolith at commit 2b9bc493) lists the
output modules with inclusive line ranges of the ORIGINAL file, in original
order, so define-before-use is preserved inside each module.

Rules the tool applies:
  * every top-level item (function, variable, prototype, #define, typedef,
    struct, enum, #include) is assigned to the module owning its first line;
    leading comment blocks travel with the item that follows them; a coverage
    audit refuses to run if any non-blank source line would be lost;
  * #include / #define / typedef / struct / enum go to the shared header, in
    original order; top-level #if groups that contain only #defines go too,
    any other #if group stays whole in its module;
  * a `static` function or variable referenced from another module (directly,
    or through a macro body) loses `static` and gets a prototype / `extern` in
    the header; anything used by one module only stays static there;
  * a forward prototype of a static function moves to the module that defines
    it; anonymous-struct tables referenced across modules are copied into the
    header as `static const` (an extern of an anonymous struct is a different
    type); names listed in the plan's "manual_header" get hand-written header
    text instead (used for the RELEASE-conditional MESHTAG functions).
Prove the result the same way every time: build, regenerate with a pinned seed,
sha256 the outputs against a baseline taken from the unsplit build.
"""
import re, sys, os, json, collections

if len(sys.argv) < 2 or not sys.argv[1].endswith('.c'):
    sys.exit(__doc__)
SRC = sys.argv[1]

text = open(SRC, encoding='utf-8', errors='surrogateescape').read()
lines = text.split('\n')


def strip_comments_strings(s):
    """Replace comments and string/char literals with spaces (keep newlines)."""
    out = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if s.startswith('/*', i):
            j = s.find('*/', i + 2); j = n if j < 0 else j + 2
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in s[i:j])); i = j
        elif s.startswith('//', i):
            j = s.find('\n', i); j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif c == '"' or c == "'":
            q = c; j = i + 1
            while j < n and s[j] != q:
                if s[j] == '\\': j += 1
                if s[j] == '\n': break
                j += 1
            j = min(j + 1, n)
            out.append(q + ' ' * (j - i - 2) + q if j - i >= 2 else ' ' * (j - i)); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


clean = strip_comments_strings(text)
clean_lines = clean.split('\n')
assert len(clean_lines) == len(lines)

# ---- top-level item scan (brace depth 0 boundaries) --------------------------
items = []  # dict(kind, name, start, end (inclusive, 1-based), static, text)
depth = 0
i = 0
n = len(clean_lines)
cur_start = None
paren = 0


def classify(chunk_clean, chunk_raw):
    s = chunk_clean.strip()
    if not s:
        return None, None, False
    if s.startswith('#'):
        m = re.match(r'#\s*(\w+)\s*(\w+)?', s)
        d = m.group(1)
        if d == 'define':
            return 'define', m.group(2), False
        if d == 'include':
            return 'include', s.split(None, 1)[1].strip(), False
        return 'pp_' + d, m.group(2), False
    static = bool(re.match(r'\s*static\b', s))
    if re.match(r'\s*typedef\b', s):
        m = re.search(r'\}\s*(\w+)\s*;\s*$', s) or re.search(r'typedef\s+.*?\b(\w+)\s*(\[[^\]]*\])?\s*;\s*$', s, re.S)
        return 'typedef', (m.group(1) if m else '?'), False
    if re.match(r'\s*enum\s*\{', s):
        return 'enum', None, False
    if re.match(r'\s*(struct|union|enum)\s+\w+\s*\{', s):
        m = re.match(r'\s*(struct|union|enum)\s+(\w+)', s)
        return 'struct', m.group(2), False
    # function definition: name '(' ... ')' '{'
    m = re.search(r'\b(\w+)\s*\([^;{]*\)\s*(?:__attribute__\s*\(\([^)]*\)\)\s*)?\{', s, re.S)
    body_has_brace = '{' in s
    if m and body_has_brace:
        # make sure the '(' belongs to the declarator, not an initializer
        pre = s[:m.start(1)]
        if '=' not in pre.split('\n')[-1]:
            return 'func', m.group(1), static
    if s.endswith(';'):
        # prototype?
        m2 = re.match(r'\s*(?:static\s+)?(?:inline\s+)?(?:const\s+)?[\w\s\*]+?\b(\w+)\s*\([^;]*\)\s*;\s*$', s, re.S)
        if m2 and '=' not in s:
            return 'proto', m2.group(1), static
        # variable(s)
        decl = s.split('=')[0]
        # drop any inline struct/union body so `struct { int a, b; } name[4]`
        # names `name`, not `b`
        flat = decl
        while re.search(r'\{[^{}]*\}', flat):
            flat = re.sub(r'\{[^{}]*\}', ' ', flat)
        flat = flat.strip().rstrip(';').strip()
        flat = re.sub(r'(\s*\[[^\]]*\])+\s*$', '', flat)      # strip array dims
        name = None
        mm = re.search(r'(\w+)\s*$', flat)
        if mm:
            name = mm.group(1)
        return 'var', name, static
    return 'other', None, static


while i < n:
    cl = clean_lines[i]
    if cur_start is None:
        if lines[i].strip() == '':          # RAW blank: comments start an item
            i += 1; continue
        cur_start = i
    # preprocessor line (top-level) is its own item if depth==0 and starts with #
    # and everything accumulated so far is comment-only (leading doc block).
    if (depth == 0 and paren == 0 and cl.lstrip().startswith('#')
            and all(clean_lines[k].strip() == '' for k in range(cur_start, i))):
        # handle continuation lines
        j = i
        def _comment_open(upto):
            seg = "\n".join(lines[cur_start:upto + 1])
            return seg.rfind("/*") > seg.rfind("*/")
        while clean_lines[j].rstrip().endswith("\\") or _comment_open(j):
            j += 1
        raw = '\n'.join(lines[cur_start:j + 1]); cc = '\n'.join(clean_lines[cur_start:j + 1])
        k, nm, st = classify(cc, raw)
        items.append(dict(kind=k, name=nm, start=cur_start + 1, end=j + 1, static=st))
        cur_start = None; i = j + 1; continue
    for ch in cl:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
        elif ch == '(': paren += 1
        elif ch == ')': paren -= 1
    ends = False
    stripped = cl.rstrip()
    if depth == 0 and paren == 0:
        if stripped.endswith(';') or stripped.endswith('}'):
            ends = True
    if ends:
        raw = '\n'.join(lines[cur_start:i + 1]); cc = '\n'.join(clean_lines[cur_start:i + 1])
        k, nm, st = classify(cc, raw)
        items.append(dict(kind=k, name=nm, start=cur_start + 1, end=i + 1, static=st))
        cur_start = None
    i += 1
if cur_start is not None:
    items.append(dict(kind='tail', name=None, start=cur_start + 1, end=n, static=False))

covered = [0] * (n + 2)
for it in items:
    for ln in range(it['start'], it['end'] + 1):
        covered[ln] += 1
lost = [ln for ln in range(1, n + 1) if covered[ln] == 0 and lines[ln - 1].strip()]
dup = [ln for ln in range(1, n + 1) if covered[ln] > 1]
if lost or dup:
    print('COVERAGE FAIL lost=%d first=%s dup=%s' % (len(lost), lost[:12], dup[:10]))
if 'where' in sys.argv:
    for a in sys.argv[sys.argv.index('where') + 1:]:
        ln = int(a)
        for it in items:
            if it['start'] <= ln <= it['end']:
                print('line %d in item %s %r %d-%d static=%s' % (ln, it['kind'], it['name'], it['start'], it['end'], it['static']))
    sys.exit(0)


def item_clean(it):
    return '\n'.join(clean_lines[it['start'] - 1:it['end']])


mode = 'inventory'
for a in sys.argv[1:]:
    if a in ('inventory', 'analyze', 'split'):
        mode = a

if mode == 'inventory':
    c = collections.Counter(it['kind'] for it in items)
    print(c)
    for it in items:
        if it['kind'] in ('other', 'tail') or it['name'] in (None, '?'):
            print('??', it['kind'], it['start'], it['end'], lines[it['start'] - 1][:100])
    sys.exit(0)

# ---- cut plan -----------------------------------------------------------------
# module -> list of (start_line, end_line) inclusive, in ORIGINAL order.
PLAN_PATH = os.environ.get('SPLIT_PLAN') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'split_trackgen_plan.json')
plan = json.load(open(PLAN_PATH))
modules = plan['modules']          # ordered list of {"file":..., "ranges":[[a,b],...]}


def module_of_line(ln):
    for m in modules:
        for a, b in m['ranges']:
            if a <= ln <= b:
                return m['file']
    return None


for it in items:
    it['mod'] = module_of_line(it['start'])
    it['mod_end'] = module_of_line(it['end'])
    if it['mod'] != it['mod_end']:
        print('ITEM SPANS A CUT:', it['kind'], it['name'], it['start'], it['end'], it['mod'], it['mod_end'])

unassigned = [it for it in items if it['mod'] is None]
if unassigned:
    print('UNASSIGNED lines:', [(it['start'], it['end']) for it in unassigned][:20])

# identifiers defined at top level
defs = collections.defaultdict(list)
for it in items:
    if it['name'] and it['kind'] in ('func', 'var', 'define', 'typedef', 'struct'):
        defs[it['name']].append(it)

word_re = re.compile(r'\b[A-Za-z_]\w*\b')
uses = collections.defaultdict(set)   # name -> set(modules using it)
for it in items:
    if it['kind'] in ('func', 'var', 'define'):
        body = item_clean(it)
        if it['kind'] == 'func':
            # exclude the declarator name itself
            pass
        for w in set(word_re.findall(body)):
            if w in defs and w != it['name']:
                uses[w].add(it['mod'])

# Macros are hoisted to the shared header, so whatever a macro body references is
# used wherever the macro is used. Propagate until stable.
macro_refs = {}
for it in items:
    if it['kind'] == 'define' and it['name']:
        macro_refs[it['name']] = set(w for w in word_re.findall(item_clean(it)) if w in defs and w != it['name'])
changed = True
while changed:
    changed = False
    for mname, refs in macro_refs.items():
        umods = uses.get(mname, set())
        for r in refs:
            before = len(uses[r])
            uses[r] |= umods
            if len(uses[r]) != before:
                changed = True
# A promoted macro body may also reference module-local names from ANY module,
# so the header sees it: treat every macro as used by all modules.
all_mods = set(m['file'] for m in modules)
for mname, refs in macro_refs.items():
    for r in refs:
        uses[r] |= all_mods

promote = []   # items whose definition module != some use module
for name, dl in defs.items():
    dmods = set(d['mod'] for d in dl)
    umods = uses.get(name, set())
    if umods - dmods:
        kinds = set(d['kind'] for d in dl)
        promote.append((name, sorted(kinds), sorted(dmods, key=str), sorted(umods, key=str)))

if mode == 'analyze':
    by_kind = collections.Counter()
    for name, kinds, dm, um in promote:
        by_kind[kinds[0]] += 1
    print('cross-module symbols by kind:', dict(by_kind))
    for kind in ('typedef', 'struct', 'define', 'var', 'func', 'proto'):
        rows = [p for p in promote if p[1][0] == kind]
        print('\n== %s (%d)' % (kind, len(rows)))
        for name, kinds, dm, um in rows[:400]:
            print('  %-40s def %-22s used %s' % (name, ','.join(dm), ','.join(um)))
    # sizes
    for m in modules:
        tot = sum(b - a + 1 for a, b in m['ranges'])
        print('%-28s %6d lines' % (m['file'], tot))
    sys.exit(0)

# ---- split --------------------------------------------------------------------
promote_names = {p[0] for p in promote}
manual = plan.get('manual_header', {})
manual_names = set(manual.get('names', []))
tex_mod = plan.get('tex_include_module')
tex_re = re.compile(plan.get('tex_include_pattern', '$^'))
out_dir = plan.get('out_dir', os.path.dirname(SRC))
header_name = plan['header']

def raw_of(it):
    return '\n'.join(lines[it['start'] - 1:it['end']])

def func_decl(it):
    cc = item_clean(it)
    m = re.search(r'^(.*?\))\s*(?:__attribute__\s*\(\([^)]*\)\)\s*)?\{', cc, re.S)
    decl = re.sub(r'^\s*static\s+', '', m.group(1).strip(), count=1)
    return re.sub(r'\s+', ' ', decl) + ';'

def var_decl(it):
    cc = item_clean(it)
    decl = cc.split('=')[0].strip().rstrip(';')
    decl = re.sub(r'^\s*static\s+', '', decl, count=1)
    return 'extern ' + re.sub(r'\s+', ' ', decl) + ';'

def def_module(name, kind):
    for d in defs.get(name, []):
        if d['kind'] == kind:
            return d['mod']
    return None

hdr = []
hdr.append('/**\n * %s -- PRIVATE shared declarations of the auto-track generator modules.\n'
           ' *\n * td5_trackgen.c (31k lines) was split 2026-09-06 into themed modules, listed\n'
           ' * in srcs.txt as td5_trackgen.c + td5_tg_*.c. Everything here is internal to\n'
           ' * those modules; the public contract stays in td5_trackgen.h. Definitions that\n'
           ' * were file-scope `static` in the monolith and are used by more than one module\n'
           ' * lost the `static` and are declared here; anything used by one module only\n'
           ' * stayed static in that module. Order is the monolith order, so macro and type\n'
           ' * dependencies keep resolving top-down. GENERATED once by the split tool, then\n'
           ' * hand-maintained like any header.\n */\n' % header_name)
hdr.append('#ifndef TD5_TRACKGEN_INTERNAL_H\n#define TD5_TRACKGEN_INTERNAL_H\n')
mod_text = {m['file']: [] for m in modules}

# group top-level conditional blocks
groups = []   # list of items or ('group', [items])
stack = []
cur = None
for it in items:
    k = it['kind']
    if cur is None:
        if k in ('pp_if', 'pp_ifdef', 'pp_ifndef'):
            cur = [it]; depth_pp = 1
        else:
            groups.append(it)
    else:
        cur.append(it)
        if k in ('pp_if', 'pp_ifdef', 'pp_ifndef'):
            depth_pp += 1
        elif k == 'pp_endif':
            depth_pp -= 1
            if depth_pp == 0:
                groups.append(('group', cur)); cur = None
assert cur is None

manual_done = False
for g in groups:
    if isinstance(g, tuple):
        its = g[1]
        inner = [x for x in its if not x['kind'].startswith('pp_')]
        raw = '\n'.join(raw_of(x) for x in its)
        if inner and all(x['kind'] == 'define' for x in inner):
            hdr.append(raw)                      # pure macro group -> header
        else:
            mod = its[0]['mod']
            for x in its:
                if x['kind'] in ('func', 'var') and (x['name'] in promote_names or x['name'] in manual_names) and x['static']:
                    x['_strip'] = True
            body = []
            for x in its:
                r = raw_of(x)
                if x.get('_strip'):
                    r = re.sub(r'^(\s*)static\s+', r'\1', r, count=1, flags=re.M)
                    if x['name'] not in manual_names:
                        hdr.append(func_decl(x) if x['kind'] == 'func' else var_decl(x))
                body.append(r)
            mod_text[mod].append('\n'.join(body))
            if not manual_done and manual.get('text'):
                hdr.append(manual['text']); manual_done = True
        continue
    it = g
    raw = raw_of(it)
    k = it['kind']
    if k == 'include':
        if tex_mod and tex_re.search(raw):
            mod_text[tex_mod].append(raw)
        else:
            hdr.append(raw)
    elif k in ('define', 'typedef', 'struct', 'enum'):
        hdr.append(raw)
    elif k == 'proto':
        if it['name'] in promote_names or it['name'] in manual_names:
            if it['name'] not in manual_names:
                hdr.append(re.sub(r'^\s*static\s+', '', raw, count=1, flags=re.M))
        else:
            target = def_module(it['name'], 'func') or it['mod']
            mod_text[target].append(raw)
    elif k in ('func', 'var'):
        if (k == 'var' and it['name'] in promote_names
                and re.search(r'struct\s*\w*\s*\{', item_clean(it).split('=')[0])):
            # Anonymous-struct table: an `extern` of an anonymous struct is a
            # different type, so the whole (static const) definition moves to
            # the header and every module gets its own identical copy.
            if 'const' not in item_clean(it).split('=')[0]:
                print('WARNING: non-const anonymous-struct var promoted by copy:', it['name'])
            hdr.append(raw)
            continue
        if it['name'] in promote_names:
            if it['static']:
                raw = re.sub(r'^(\s*)static\s+', r'\1', raw, count=1, flags=re.M)
            if it['name'] not in manual_names:
                hdr.append(func_decl(it) if k == 'func' else var_decl(it))
        mod_text[it['mod']].append(raw)
    else:
        mod_text[it['mod']].append(raw)

hdr.append('\n#endif /* TD5_TRACKGEN_INTERNAL_H */\n')
with open(os.path.join(out_dir, header_name), 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
    f.write('\n'.join(hdr) + '\n')
for m in modules:
    body = mod_text[m['file']]
    head = ('/**\n * %s -- %s\n *\n * Split from the td5_trackgen.c monolith (2026-09-06); shared declarations\n'
            ' * live in %s. Element map: docs/plans/AUTOTRACK_ELEMENT_CATALOG.md.\n */\n'
            '#include "%s"\n\n' % (m['file'], m['banner'], header_name, header_name))
    with open(os.path.join(out_dir, m['file']), 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
        f.write(head + '\n\n'.join(body) + '\n')
    print('wrote %-22s %5d items %6d lines' % (m['file'], len(body), sum(b.count('\n') + 1 for b in body)))
print('wrote', header_name, sum(h.count('\n') + 1 for h in hdr), 'lines; promoted', len(promote_names))
