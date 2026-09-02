"""[R14 JUNCTION] Prepend ONE row to pending_to_test.csv, safely.

The detail field is long prose containing commas, quotes and numbers, and a
previous round had to ship a repair commit for an unquoted detail field. So the
row is never hand-edited: it is written with csv.writer(QUOTE_ALL) and the whole
file is then RE-PARSED with csv.reader and asserted to be exactly 3 columns on
every row before it is allowed to stay.

Usage: python verify/r14junc_pending.py <summary_file> <detail_file>
"""
import csv, io, os, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV = os.path.join(HERE, "td5mod", "src", "td5re", "pending_to_test.csv")


def main():
    summary = open(sys.argv[1], encoding="utf-8").read().strip()
    detail = open(sys.argv[2], encoding="utf-8").read().strip()
    # Collapse to a single logical line: embedded newlines are legal inside a
    # quoted CSV field, but every other row in this file is one physical line
    # and the in-game overlay reads it that way.
    detail = " ".join(detail.split())
    summary = " ".join(summary.split())

    with open(CSV, newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    if not rows or rows[0] != ["summary", "detail", "status"]:
        sys.exit("unexpected header: %r" % (rows[0] if rows else None))

    new = [summary, detail, "pending"]
    out = [rows[0], new] + rows[1:]

    buf = io.StringIO()
    csv.writer(buf, quoting=csv.QUOTE_ALL, lineterminator="\n").writerows(out)
    text = buf.getvalue()

    # RE-PARSE before committing anything to disk.
    check = list(csv.reader(io.StringIO(text)))
    bad = [(i, len(r)) for i, r in enumerate(check) if len(r) != 3]
    if bad:
        sys.exit("re-parse failed, rows with != 3 cols: %r" % bad[:5])
    if check[0] != ["summary", "detail", "status"]:
        sys.exit("re-parse lost the header")
    if check[1] != new:
        sys.exit("re-parse did not round-trip the new row")
    if len(check) != len(rows) + 1:
        sys.exit("row count %d != expected %d" % (len(check), len(rows) + 1))

    with open(CSV, "w", newline="", encoding="utf-8") as f:
        f.write(text)
    print("OK rows=%d (was %d), new row cols=%d, summary=%r"
          % (len(check), len(rows), len(check[1]), summary[:60]))


if __name__ == "__main__":
    main()
