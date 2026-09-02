"""[R14 BRANCH] Prepend one row to pending_to_test.csv, QUOTE_ALL, then RE-PARSE
the whole file with csv.reader and assert every row has exactly 3 columns.

A previous round shipped a row that parsed as 69 columns, so the write is not
trusted -- the file is read back and validated before it is left on disk.

Usage: python r14branch_pending.py <csv> <summary.txt> <detail.txt>
"""
import csv, io, sys, os


def main():
    path, sfile, dfile = sys.argv[1], sys.argv[2], sys.argv[3]
    summary = open(sfile, encoding='utf-8').read().strip()
    detail = open(dfile, encoding='utf-8').read().strip()
    detail = ' '.join(detail.split())

    with open(path, newline='', encoding='utf-8') as f:
        rows = list(csv.reader(f))
    if not rows or rows[0] != ['summary', 'detail', 'status']:
        raise SystemExit('unexpected header: %r' % (rows[0] if rows else None))
    bad = [(i, len(r)) for i, r in enumerate(rows) if len(r) != 3]
    if bad:
        raise SystemExit('file already malformed: %r' % bad[:5])

    rows.insert(1, [summary, detail, 'pending'])

    buf = io.StringIO()
    w = csv.writer(buf, quoting=csv.QUOTE_ALL, lineterminator='\n')
    w.writerows(rows)
    text = buf.getvalue()

    # RE-PARSE what we are about to write, before it touches disk.
    back = list(csv.reader(io.StringIO(text)))
    widths = sorted(set(len(r) for r in back))
    if widths != [3]:
        raise SystemExit('REJECTED: row widths %r' % widths)
    if len(back) != len(rows):
        raise SystemExit('REJECTED: %d rows in, %d out' % (len(rows), len(back)))
    if back[1][0] != summary or back[1][2] != 'pending':
        raise SystemExit('REJECTED: new row did not round-trip')

    with open(path, 'w', newline='', encoding='utf-8') as f:
        f.write(text)
    print('OK: %d rows, all 3 columns; new row is row 1 (%d chars of detail)'
          % (len(back), len(back[1][1])))


main()
