#!/usr/bin/env python3
"""gen_codemap.py -- regenerate codemap/ : small greppable TSV indexes of the
source port, so a session (human, Claude, or a repo-fleet worker) can answer
"where is X / how big is it / who touches Y" with ONE grep on a small file
instead of fanning out across ~145k LOC.

    python scripts/gen_codemap.py          # writes codemap/*.tsv at repo root

Outputs (all TAB-separated, one fact per line, # header comments):
    functions.tsv   file  line  kind(static|public)  name  loc  desc
    sections.tsv    file  line  banner-text          (existing /* ==== banners)
    xref_g_td5.tsv  field  file  reads  writes       (g_td5.* / g_td5.ini.*)
    includes.tsv    file  included-header            (project-local "" includes)
    statics.tsv     file  line  name                 (file-scope mutable state)

codemap/ is GITIGNORED (line numbers churn every commit); build_all.bat
regenerates it after every successful build, and any session may re-run this
script directly. Parsing is heuristic (regex + brace tracking, no real C
parser) -- good enough for navigation, never authoritative.
"""
import io, os, re, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIRS = [
    os.path.join(REPO, 'td5mod', 'src', 'td5re'),
    os.path.join(REPO, 'td5mod', 'ddraw_wrapper', 'src'),
]
OUT = os.path.join(REPO, 'codemap')
VENDORED = {'cJSON.c', 'cJSON.h', 'stb_truetype.h', 'stb_image_write.h'}

FUNC_RE = re.compile(r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(')
KEYWORDS = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'else', 'do', 'typedef'}
BANNER_RE = re.compile(r'^/\* ={4,}')
SECTION_RE = re.compile(r'^/\* =+ SECTION: (.+?) =+ \*/')
STATIC_VAR_RE = re.compile(r'^static\s+(?:const\s+)?[\w \t\*]+?\b([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*(=|;)')
GTD5_RE = re.compile(r'g_td5\.(ini\.)?(\w+)')
INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"')

def rel(path):
    return os.path.relpath(path, REPO).replace('\\', '/')

def strip_comments_keep_len(line, state):
    """Blank out comment/string contents (keeps braces countable). state = in_block_comment."""
    out = []
    i, n = 0, len(line)
    in_str = None
    while i < n:
        c = line[i]
        if state['blk']:
            if line.startswith('*/', i):
                state['blk'] = False; i += 2
            else:
                i += 1
            continue
        if in_str:
            if c == '\\': i += 2; continue
            if c == in_str: in_str = None
            i += 1
            continue
        if line.startswith('/*', i):
            state['blk'] = True; i += 2; continue
        if line.startswith('//', i):
            break
        if c in '"\'':
            in_str = c; i += 1; continue
        out.append(c); i += 1
    return ''.join(out)

def comment_desc(lines, def_idx):
    """First sentence of the comment block immediately above a definition."""
    i = min(def_idx, len(lines)) - 1
    while i >= 0 and lines[i].strip() == '':
        i -= 1
    if i < 0: return ''
    tail = lines[i].rstrip()
    if not tail.endswith('*/'): return ''
    # walk back to the comment opener, keep the first meaningful line
    j = i
    while j >= 0 and '/*' not in lines[j]:
        j -= 1
    if j < 0: return ''
    for k in range(j, i + 1):
        t = re.sub(r'^\s*/?\*+/?', '', lines[k]).strip().rstrip('*/').strip()
        t = re.sub(r'^-+\s*|\s*-+$', '', t).strip()
        if t and not re.match(r'^=+$', t):
            return t[:100]
    return ''

def scan_file(path, facts):
    raw = io.open(path, encoding='utf-8', errors='replace').read()
    lines = raw.splitlines()
    f = rel(path)
    is_c = path.endswith('.c')

    # includes + banners + statics + g_td5 xref (line-oriented)
    xref = {}
    for idx, line in enumerate(lines, 1):
        m = INCLUDE_RE.match(line)
        if m:
            facts['includes'].append((f, m.group(1)))
        if BANNER_RE.match(line):
            # banner text = next line's comment payload, or same-line text
            txt = re.sub(r'[/\*=]', '', line).strip()
            if not txt and idx < len(lines):
                txt = re.sub(r'^\s*\*+\s*', '', lines[idx]).strip().rstrip('*/').strip()
            m2 = SECTION_RE.match(line)
            if m2: txt = m2.group(1)
            if txt:
                facts['sections'].append((f, idx, txt[:100]))
        for m in GTD5_RE.finditer(line):
            field = ('ini.' if m.group(1) else '') + m.group(2)
            after = line[m.end():]
            is_write = bool(re.match(r'\s*(\[[^\]]*\])?\s*([+\-|&^]?=[^=]|\+\+|--)', after))
            r, w = xref.get((field), (0, 0))
            xref[field] = (r + (0 if is_write else 1), w + (1 if is_write else 0))
    for field, (r, w) in sorted(xref.items()):
        facts['xref'].append((field, f, r, w))

    if not is_c:
        return

    # functions + file-scope statics via brace-depth walk
    depth = 0
    state = {'blk': False}
    pend_sig = None   # (line_idx, name) candidate awaiting '{'
    pend_join = ''    # accumulated signature text while params span lines
    for idx, line in enumerate(lines, 1):
        code = strip_comments_keep_len(line, state)
        if depth == 0 and code.strip():
            s = code.rstrip()
            if pend_sig is None and not s.startswith(('#', '{', '}', '/', '*')):
                m = FUNC_RE.match(s)
                if m and m.group(1) not in KEYWORDS and not s.endswith(';'):
                    pend_sig = (idx, m.group(1), s.startswith('static'))
                    pend_join = s
                else:
                    mv = STATIC_VAR_RE.match(s)
                    if mv and '(' not in s.split('=')[0].replace(mv.group(1), '', 1):
                        facts['statics'].append((f, idx, mv.group(1)))
            elif pend_sig is not None:
                pend_join += ' ' + s.strip()
                if s.endswith(';'):          # was a prototype after all
                    pend_sig = None
        opens = code.count('{'); closes = code.count('}')
        if pend_sig is not None and opens > 0 and depth == 0:
            di, name, is_static = pend_sig
            facts['_open_fn'] = (f, di, name, is_static)
            pend_sig = None
        prev_depth = depth
        depth += opens - closes
        if depth < 0: depth = 0
        if prev_depth > 0 and depth == 0 and facts.get('_open_fn'):
            ff, di, name, is_static = facts.pop('_open_fn')
            facts['functions'].append(
                (ff, di, 'static' if is_static else 'public', name, idx - di + 1,
                 comment_desc(lines, di - 1)))
    # never leak an unclosed candidate into the next file (heuristic miscount)
    facts.pop('_open_fn', None)

def main():
    facts = {'functions': [], 'sections': [], 'xref': [], 'includes': [], 'statics': []}
    n_files = 0
    for d in SRC_DIRS:
        for root, dirs, files in os.walk(d):
            dirs[:] = [x for x in dirs if x not in ('build', 'build_release', 'shaders')]
            for fn in sorted(files):
                if fn in VENDORED or not fn.endswith(('.c', '.h')):
                    continue
                scan_file(os.path.join(root, fn), facts)
                n_files += 1
    facts.pop('_open_fn', None)

    os.makedirs(OUT, exist_ok=True)
    stamp = time.strftime('%Y-%m-%d %H:%M')
    def write(name, header, rows):
        p = os.path.join(OUT, name)
        with io.open(p, 'w', encoding='utf-8', newline='\n') as fp:
            fp.write('# GENERATED by scripts/gen_codemap.py (%s) -- do not edit; regen if stale.\n' % stamp)
            fp.write('# %s\n' % header)
            for r in rows:
                fp.write('\t'.join(str(x) for x in r) + '\n')
        return len(rows)

    counts = {
        'functions.tsv': write('functions.tsv', 'file\tline\tkind\tname\tloc\tdesc',
                               facts['functions']),
        'sections.tsv': write('sections.tsv', 'file\tline\tbanner', facts['sections']),
        'xref_g_td5.tsv': write('xref_g_td5.tsv', 'field\tfile\treads\twrites',
                                facts['xref']),
        'includes.tsv': write('includes.tsv', 'file\theader', facts['includes']),
        'statics.tsv': write('statics.tsv', 'file\tline\tname', facts['statics']),
    }
    print('codemap: %d files scanned -> %s' %
          (n_files, ', '.join('%s:%d' % kv for kv in sorted(counts.items()))))

if __name__ == '__main__':
    sys.exit(main())
