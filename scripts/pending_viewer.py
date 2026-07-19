#!/usr/bin/env python3
"""
TD5RE Pending-to-Test viewer
Browse at http://localhost:7355  (Ctrl-C to stop)
"""
import csv, json, os, webbrowser
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CSV_PATH   = os.path.normpath(os.path.join(SCRIPT_DIR, '..', 'td5mod', 'src', 'td5re', 'pending_to_test.csv'))
PORT       = 7355

# ── CSV I/O ───────────────────────────────────────────────────────────────────

def read_items():
    rows = []
    with open(CSV_PATH, newline='', encoding='utf-8') as f:
        for i, row in enumerate(csv.DictReader(f)):
            rows.append({
                'id':      i,
                'summary': row.get('summary', '').strip(),
                'detail':  row.get('detail',  '').strip(),
                'status':  row.get('status',  'pending').strip(),
            })
    return rows

def write_items(items):
    with open(CSV_PATH, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f, quoting=csv.QUOTE_ALL)
        w.writerow(['summary', 'detail', 'status'])
        for it in items:
            w.writerow([it['summary'], it['detail'], it['status']])

# ── HTML ──────────────────────────────────────────────────────────────────────

HTML = r'''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>TD5RE · Pending to Test</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

:root {
  --bg:      #0d1117;
  --bg2:     #161b22;
  --bg3:     #21262d;
  --border:  #30363d;
  --text:    #e6edf3;
  --sub:     #8b949e;
  --accent:  #58a6ff;
  --green:   #3fb950;
  --yellow:  #d29922;
  --r:       8px;
  --font:    -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui, sans-serif;
  --mono:    "Consolas", "SF Mono", "Fira Code", monospace;
}

html { scroll-behavior: smooth; }

body {
  background: var(--bg);
  color: var(--text);
  font-family: var(--font);
  font-size: 14px;
  line-height: 1.5;
  min-height: 100vh;
}

/* ── Sticky header ── */
.hdr {
  position: sticky; top: 0; z-index: 200;
  background: rgba(13,17,23,.95);
  backdrop-filter: blur(10px);
  border-bottom: 1px solid var(--border);
  padding: 10px 20px;
  display: flex; flex-wrap: wrap; gap: 8px; align-items: center;
}
.hdr-title {
  font-size: 15px; font-weight: 600;
  white-space: nowrap; margin-right: 2px;
  color: var(--text);
}
.hdr-title span { color: var(--sub); font-weight: 400; }

.badge {
  padding: 2px 9px; border-radius: 20px;
  font-size: 11px; font-weight: 700;
  font-family: var(--mono); white-space: nowrap;
}
.bp { background: rgba(88,166,255,.12); color: var(--accent); }
.bd { background: rgba(63,185,80,.12);  color: var(--green); }

.search {
  flex: 1; min-width: 180px; max-width: 380px;
  padding: 5px 11px;
  background: var(--bg3); border: 1px solid var(--border);
  border-radius: var(--r); color: var(--text); font-size: 13px; outline: none;
  transition: border-color .15s;
}
.search:focus { border-color: var(--accent); }
.search::placeholder { color: var(--sub); }

.hdr-actions { display: flex; gap: 6px; }

.btn {
  padding: 4px 12px; border-radius: var(--r);
  border: 1px solid var(--border); background: var(--bg3);
  color: var(--sub); font-size: 12px; cursor: pointer;
  white-space: nowrap; transition: .15s;
}
.btn:hover { background: var(--bg2); color: var(--text); }
.btn.on { border-color: var(--green); color: var(--green); background: rgba(63,185,80,.08); }

/* ── Progress bar ── */
.progress-wrap {
  height: 2px; background: var(--bg3);
  position: sticky; top: 49px; z-index: 199;
}
.progress-bar {
  height: 100%; background: var(--green);
  transition: width .4s ease;
  width: 0%;
}

/* ── Layout ── */
.main { max-width: 860px; margin: 0 auto; padding: 20px 16px 60px; }

.sec-head {
  font-size: 10px; font-weight: 700; text-transform: uppercase;
  letter-spacing: .1em; color: var(--sub);
  margin: 24px 0 8px;
  display: flex; align-items: center; gap: 8px;
}
.sec-head::after { content:''; flex:1; height:1px; background: var(--border); }

/* ── Item card ── */
.item {
  background: var(--bg2);
  border: 1px solid var(--border);
  border-radius: var(--r);
  margin-bottom: 5px;
  overflow: hidden;
  transition: border-color .15s, max-height .35s ease, opacity .3s, margin .3s;
}
.item:hover { border-color: #444c56; }
.item.is-done { border-color: rgba(63,185,80,.25); background: rgba(63,185,80,.03); }
.item.collapsing {
  max-height: 0 !important;
  opacity: 0;
  margin-bottom: 0;
  border-top-width: 0;
  border-bottom-width: 0;
  overflow: hidden;
}

.item-row {
  display: flex; align-items: flex-start; gap: 10px;
  padding: 10px 12px;
  cursor: pointer; user-select: none;
}
.item-row:hover { background: rgba(255,255,255,.025); }

.item-num {
  flex-shrink: 0; min-width: 32px; text-align: right;
  font-size: 11px; font-family: var(--mono); color: var(--sub);
  padding-top: 2px;
}

.item-summary {
  flex: 1; font-size: 13px; font-weight: 500; color: var(--text);
  line-height: 1.45;
}
.item.is-done .item-summary {
  color: var(--sub);
  text-decoration: line-through;
  text-decoration-color: rgba(63,185,80,.4);
}

.item-chevron {
  flex-shrink: 0; color: var(--sub); font-size: 10px;
  padding-top: 4px; transition: transform .2s;
}
.item.expanded .item-chevron { transform: rotate(180deg); }
.item-chevron.hidden { visibility: hidden; }

.check {
  flex-shrink: 0;
  width: 22px; height: 22px; border-radius: 50%;
  border: 2px solid var(--border); background: transparent;
  cursor: pointer; outline: none;
  display: flex; align-items: center; justify-content: center;
  font-size: 11px; color: transparent;
  transition: .15s; flex-shrink: 0;
}
.check:hover { border-color: var(--green); color: rgba(63,185,80,.7); }
.item.is-done .check {
  border-color: var(--green); background: var(--green); color: #fff;
}

/* ── Detail panel ── */
.item-detail {
  display: none;
  padding: 0 12px 14px 54px;
  animation: fadeIn .15s ease;
}
@keyframes fadeIn { from { opacity:0; transform: translateY(-4px); } to { opacity:1; transform: none; } }
.item.expanded .item-detail { display: block; }

.detail-context {
  font-size: 12.5px; color: var(--sub); line-height: 1.65;
  margin-bottom: 10px;
}

.confirm-box {
  border-left: 3px solid var(--accent);
  border-radius: 0 var(--r) var(--r) 0;
  background: rgba(88,166,255,.06);
  padding: 9px 13px;
}
.confirm-label {
  font-size: 9px; font-weight: 800; text-transform: uppercase;
  letter-spacing: .12em; color: var(--accent); margin-bottom: 5px;
}
.confirm-text { font-size: 12.5px; color: var(--text); line-height: 1.65; }
.no-detail { font-size: 12px; color: var(--sub); font-style: italic; }

/* ── Empty states ── */
.empty {
  text-align: center; padding: 60px 20px; color: var(--sub);
}
.empty .icon { font-size: 44px; margin-bottom: 14px; }
.empty h2 { font-size: 20px; color: var(--green); margin-bottom: 6px; }
.empty p { font-size: 13px; }

.no-results { text-align: center; padding: 50px 20px; color: var(--sub); font-size: 13px; }

/* ── Scrollbar ── */
::-webkit-scrollbar { width: 5px; }
::-webkit-scrollbar-thumb { background: var(--border); border-radius: 3px; }
</style>
</head>
<body>

<div class="hdr">
  <div class="hdr-title">TD5RE <span>/ Pending to Test</span></div>
  <span class="badge bp" id="bp">…</span>
  <span class="badge bd" id="bd">…</span>
  <input class="search" id="search" type="search" placeholder="Filter items…" autocomplete="off" spellcheck="false">
  <div class="hdr-actions">
    <button class="btn" id="btn-done" onclick="toggleShowDone()">Show Completed</button>
    <button class="btn" onclick="expandAll()">Expand All</button>
    <button class="btn" onclick="collapseAll()">Collapse All</button>
  </div>
</div>
<div class="progress-wrap"><div class="progress-bar" id="prog"></div></div>

<div class="main">
  <div id="list-pending"></div>
  <div id="sec-done" style="display:none">
    <div class="sec-head">Completed</div>
    <div id="list-done"></div>
  </div>
  <div id="no-results" class="no-results" style="display:none">
    No items match your filter.
  </div>
</div>

<script>
'use strict';

let items    = [];
let showDone = false;

// ── Load ──────────────────────────────────────────────────────────────────────

async function load() {
  const res = await fetch('/items');
  items = await res.json();
  render();
}

// ── Render ────────────────────────────────────────────────────────────────────

function render() {
  const q       = document.getElementById('search').value.toLowerCase().trim();
  const pending = items.filter(it => it.status !== 'tested');
  const done    = items.filter(it => it.status === 'tested');
  const total   = items.length;

  // badges + progress
  document.getElementById('bp').textContent   = `${pending.length} pending`;
  document.getElementById('bd').textContent   = `${done.length} done`;
  const pct = total ? Math.round((done.length / total) * 100) : 0;
  document.getElementById('prog').style.width = pct + '%';

  // filter
  const fp = q ? pending.filter(it => hit(it, q)) : pending;
  const fd = q ? done.filter(it => hit(it, q))    : done;

  // pending list
  const lp = document.getElementById('list-pending');
  if (fp.length === 0 && !q) {
    lp.innerHTML = `<div class="empty"><div class="icon">🏁</div><h2>All done!</h2><p>Nothing left to test.</p></div>`;
  } else {
    lp.innerHTML = fp.map(makeCard).join('');
  }

  // done list
  document.getElementById('list-done').innerHTML = fd.map(makeCard).join('');
  document.getElementById('sec-done').style.display = (showDone && fd.length > 0) ? 'block' : 'none';
  document.getElementById('btn-done').classList.toggle('on', showDone);

  // no results
  const noRes = q && fp.length === 0 && fd.length === 0;
  document.getElementById('no-results').style.display = noRes ? 'block' : 'none';
}

function hit(it, q) {
  return it.summary.toLowerCase().includes(q) || it.detail.toLowerCase().includes(q);
}

function makeCard(it) {
  const done = it.status === 'tested';
  const cls  = done ? ' is-done' : '';

  // parse detail into context + confirm
  let ctx = '', conf = '';
  const ci = it.detail.indexOf('Confirm:');
  if (ci >= 0) {
    ctx  = it.detail.slice(0, ci).trim();
    conf = it.detail.slice(ci + 8).trim();
  } else {
    ctx = it.detail.trim();
  }

  const hasDetail = ctx || conf;
  const chevronCls = hasDetail ? '' : ' hidden';
  const checkmark  = `<svg width="11" height="9" viewBox="0 0 11 9" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M1 4L4 7.5L10 1" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>`;

  let detailHtml = '';
  if (hasDetail) {
    detailHtml = `<div class="item-detail">`;
    if (ctx)  detailHtml += `<div class="detail-context">${esc(ctx)}</div>`;
    if (conf) detailHtml += `<div class="confirm-box"><div class="confirm-label">Confirm</div><div class="confirm-text">${esc(conf)}</div></div>`;
    if (!ctx && !conf) detailHtml += `<div class="no-detail">No detail provided.</div>`;
    detailHtml += `</div>`;
  } else {
    detailHtml = `<div class="item-detail"><div class="no-detail">No detail provided.</div></div>`;
  }

  return `
<div class="item${cls}" id="i${it.id}" data-id="${it.id}">
  <div class="item-row" onclick="toggleExpand(${it.id})">
    <span class="item-num">${it.id + 1}</span>
    <span class="item-summary">${esc(it.summary)}</span>
    <span class="item-chevron${chevronCls}">▾</span>
    <button class="check" onclick="event.stopPropagation();markToggle(${it.id})" title="${done ? 'Unmark' : 'Mark done'}">${checkmark}</button>
  </div>
  ${detailHtml}
</div>`;
}

function esc(s) {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

// ── Interactions ──────────────────────────────────────────────────────────────

function toggleExpand(id) {
  const el = document.getElementById('i' + id);
  if (!el) return;
  if (el.querySelector('.item-chevron.hidden')) return;  // no detail
  el.classList.toggle('expanded');
}

function expandAll() {
  document.querySelectorAll('.item:not(.is-done)').forEach(el => {
    if (!el.querySelector('.item-chevron.hidden')) el.classList.add('expanded');
  });
}

function collapseAll() {
  document.querySelectorAll('.item.expanded').forEach(el => el.classList.remove('expanded'));
}

async function markToggle(id) {
  const item = items.find(it => it.id === id);
  if (!item) return;

  const newStatus = item.status === 'tested' ? 'pending' : 'tested';

  if (newStatus === 'tested') {
    const el = document.getElementById('i' + id);
    if (el) {
      el.style.maxHeight = el.offsetHeight + 'px';
      el.style.overflow  = 'hidden';
      // force reflow then start collapse
      void el.offsetHeight;
      el.classList.add('collapsing');
      await pause(370);
    }
  }

  item.status = newStatus;
  await fetch('/mark', {
    method:  'POST',
    headers: { 'Content-Type': 'application/json' },
    body:    JSON.stringify({ id, status: newStatus }),
  });
  render();
}

function toggleShowDone() {
  showDone = !showDone;
  render();
}

function pause(ms) { return new Promise(r => setTimeout(r, ms)); }

// ── Keyboard ──────────────────────────────────────────────────────────────────

document.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT') return;
  if (e.key === 'f' || e.key === '/') {
    e.preventDefault();
    document.getElementById('search').focus();
  }
});

document.getElementById('search').addEventListener('input', render);

// ── Boot ──────────────────────────────────────────────────────────────────────

load();
</script>
</body>
</html>'''

# ── HTTP handler ──────────────────────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):

    def do_GET(self):
        path = urlparse(self.path).path
        if path == '/':
            self._send(200, 'text/html; charset=utf-8', HTML.encode('utf-8'))
        elif path == '/items':
            self._json(read_items())
        else:
            self._send(404, 'text/plain', b'404')

    def do_POST(self):
        path = urlparse(self.path).path
        if path == '/mark':
            n    = int(self.headers.get('Content-Length', 0))
            data = json.loads(self.rfile.read(n))
            its  = read_items()
            idx  = data.get('id', -1)
            if 0 <= idx < len(its):
                its[idx]['status'] = data.get('status', 'pending')
                write_items(its)
            self._json({'ok': True})
        else:
            self._send(404, 'text/plain', b'404')

    def _json(self, obj):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self._send(200, 'application/json', body)

    def _send(self, code, ct, body):
        self.send_response(code)
        self.send_header('Content-Type', ct)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-cache')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass

# ── Entry ─────────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    srv = HTTPServer(('127.0.0.1', PORT), Handler)
    url = f'http://localhost:{PORT}'
    print(f'TD5RE Pending Viewer  ->  {url}')
    print('Ctrl-C to stop.')
    webbrowser.open(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print('\nStopped.')
