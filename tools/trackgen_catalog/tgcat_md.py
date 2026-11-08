"""tgcat_md.py -- the small Markdown subset HOWTO.md uses, to HTML.

Headings (#..####), paragraphs, - / 1. lists (one level), ``` fenced code,
pipe tables, inline `code`, **bold**, *italic*, [text](url). No dependency.
"""
from __future__ import annotations

import html
import re


def _inline(s):
    out, parts = [], re.split(r"(`[^`]+`)", s)
    for p in parts:
        if p.startswith("`") and p.endswith("`") and len(p) > 1:
            out.append("<code>" + html.escape(p[1:-1]) + "</code>")
            continue
        p = html.escape(p, quote=False)
        p = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", p)
        p = re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"<i>\1</i>", p)
        p = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", r'<a href="\2">\1</a>', p)
        out.append(p)
    return "".join(out)


def to_html(md):
    lines = md.replace("\r\n", "\n").split("\n")
    out, i, n = [], 0, len(lines)
    while i < n:
        line = lines[i]
        if line.startswith("```"):
            j = i + 1
            buf = []
            while j < n and not lines[j].startswith("```"):
                buf.append(lines[j])
                j += 1
            out.append("<pre><code>" + html.escape("\n".join(buf)) + "</code></pre>")
            i = j + 1
            continue
        m = re.match(r"^(#{1,4})\s+(.*)", line)
        if m:
            lv = len(m.group(1))
            text = m.group(2)
            anchor = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
            out.append(f"<h{lv} id='{anchor}'>{_inline(text)}</h{lv}>")
            i += 1
            continue
        if line.strip().startswith("|") and i + 1 < n and re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[i + 1]):
            hdr = [c.strip() for c in line.strip().strip("|").split("|")]
            rows = []
            j = i + 2
            while j < n and lines[j].strip().startswith("|"):
                rows.append([c.strip() for c in lines[j].strip().strip("|").split("|")])
                j += 1
            t = "<table><tr>" + "".join(f"<th>{_inline(c)}</th>" for c in hdr) + "</tr>"
            for r in rows:
                t += "<tr>" + "".join(f"<td>{_inline(c)}</td>" for c in r) + "</tr>"
            out.append(t + "</table>")
            i = j
            continue
        m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)", line)
        if m:
            ordered = m.group(2)[0].isdigit()
            tag = "ol" if ordered else "ul"
            items = []
            j = i
            while j < n:
                mm = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)", lines[j])
                if mm:
                    items.append(mm.group(3))
                elif lines[j].startswith("   ") and lines[j].strip() and items:
                    items[-1] += " " + lines[j].strip()
                else:
                    break
                j += 1
            out.append(f"<{tag}>" + "".join(f"<li>{_inline(t)}</li>" for t in items) + f"</{tag}>")
            i = j
            continue
        if not line.strip():
            i += 1
            continue
        buf = [line]
        j = i + 1
        while j < n and lines[j].strip() and not re.match(r"^(#|```|\s*[-*]\s|\s*\d+\.\s|\s*\|)", lines[j]):
            buf.append(lines[j])
            j += 1
        out.append("<p>" + _inline(" ".join(b.strip() for b in buf)) + "</p>")
        i = j
    return "\n".join(out)
