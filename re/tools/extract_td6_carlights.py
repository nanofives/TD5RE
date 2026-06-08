#!/usr/bin/env python3
"""extract_td6_carlights.py — pull authored CAR_LIGHTS (brake/tail-light) and
HEAD_LIGHTS positions from the TD6 per-car .scr scripts and (optionally) compare
against the binary carparam.dat the TD5RE renderer currently reads at +0x60/+0x68.

TD6 ships human-readable car scripts (Test Drive 6/param/<code>param.scr) parsed
by TD6.exe at runtime. The :CAR_LIGHTS0/1: fields are the authored rear/brake
light positions (x,y,z, model space, rear = -Z). These are the faithful per-car
positions to port — the binary carparam.dat copied by convert_td6_cars.py has
different (wrong) values at +0x60, which buries the lights inside long cars.

Usage: python re/tools/extract_td6_carlights.py [--json]
"""
import re, struct, os, sys, json

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PARAM_DIR = os.path.join(ROOT, "Test Drive 6", "param")
EXE = os.path.join(ROOT, "Test Drive 6", "TD6.exe")
CARS = os.path.join(ROOT, "re", "assets", "cars")

# TD6 car codes (convert_td6_cars order, indices 37..75)
CODES = ["390","400","atl","att","aud","bmw","cer","chd","chr","cp1","cp2","cp3",
         "cp4","db7","eli","esp","flx","g40","grf","gts","lgt","lit","lot","mam",
         "mcj","mcl","mgt","pan","pro","pwr","s12","shl","sub","sup","toy","tur",
         "tus","xjr","xk1"]

def scr_field(text, name):
    m = re.search(r':' + re.escape(name) + r':\{(-?\d+),(-?\d+),(-?\d+)\}', text)
    return tuple(int(x) for x in m.groups()) if m else None

def exe_has_tokens():
    if not os.path.exists(EXE):
        return {}
    data = open(EXE, "rb").read()
    sset = re.findall(rb'[\x20-\x7e]{4,}', data)
    sset = [s.decode('latin1') for s in sset]
    out = {}
    for tok in ["CAR_LIGHTS", "HEAD_LIGHTS", "CARPARAM", "WHEEL_CENTRES",
                "CORNER_POS", "param.scr", "CAR_LIGHTS0"]:
        out[tok] = any(tok in s for s in sset)
    return out

def emit_c_table():
    print("/* Authored TD6 per-car rear (brake/tail) light positions, extracted")
    print(" * from Test Drive 6 param/<code>param.scr :CAR_LIGHTS0/1: by")
    print(" * re/tools/extract_td6_carlights.py. Model space, rear = -Z; light0 = +X")
    print(" * (right), light1 = -X (left). These REPLACE the wrong values the binary")
    print(" * carparam.dat carries at +0x60/+0x68 for TD6 cars. Cars without a .scr")
    print(" * (donor-param aud/pro/xjr) are omitted -> renderer falls back to carparam. */")
    print("static const struct td6_car_lights {")
    print("    const char *code; int16_t l0[3]; int16_t l1[3];")
    print("} k_td6_car_lights[] = {")
    for code in CODES:
        scr = os.path.join(PARAM_DIR, code + "param.scr")
        if not os.path.exists(scr):
            continue
        t = open(scr, "r", errors="replace").read()
        cl0 = scr_field(t, "CAR_LIGHTS0"); cl1 = scr_field(t, "CAR_LIGHTS1")
        if not (cl0 and cl1):
            continue
        print('    {{ "{}", {{{:>5},{:>4},{:>5}}}, {{{:>5},{:>4},{:>5}}} }},'.format(
            code, cl0[0], cl0[1], cl0[2], cl1[0], cl1[1], cl1[2]))
    print("};")

def main():
    if "--c-table" in sys.argv:
        emit_c_table(); return
    as_json = "--json" in sys.argv
    toks = exe_has_tokens()
    result = {}
    rows = []
    for code in CODES:
        scr = os.path.join(PARAM_DIR, code + "param.scr")
        if not os.path.exists(scr):
            rows.append((code, None, None, None, "no .scr"))
            continue
        t = open(scr, "r", errors="replace").read()
        cl0 = scr_field(t, "CAR_LIGHTS0")
        cl1 = scr_field(t, "CAR_LIGHTS1")
        hl0 = scr_field(t, "HEAD_LIGHTS0")
        # binary carparam +0x60/+0x68 (current renderer source)
        binv = None
        cp = os.path.join(CARS, code, "carparam.dat")
        if os.path.exists(cp):
            d = open(cp, "rb").read()
            binv = (struct.unpack_from('<3h', d, 0x60), struct.unpack_from('<3h', d, 0x68))
        result[code] = {"car_lights0": cl0, "car_lights1": cl1, "head_lights0": hl0,
                        "bin_60": list(binv[0]) if binv else None,
                        "bin_68": list(binv[1]) if binv else None}
        match = (cl0 == binv[0] and cl1 == binv[1]) if (binv and cl0 and cl1) else False
        rows.append((code, cl0, cl1, binv, "OK" if match else "DIFF"))

    if as_json:
        print(json.dumps(result, indent=1))
        return

    print("=== TD6.exe parser-token presence (proves .scr is runtime-parsed) ===")
    for k, v in toks.items():
        print(f"  {k:14s}: {v}")
    print()
    print(f"{'code':5s} {'scr CAR_LIGHTS0':>17s} {'scr CAR_LIGHTS1':>17s} | {'bin +0x60':>15s} {'bin +0x68':>15s} | {'?':4s}")
    ndiff = 0
    for code, cl0, cl1, binv, st in rows:
        b0 = binv[0] if binv else None
        b1 = binv[1] if binv else None
        if st == "DIFF":
            ndiff += 1
        print(f"{code:5s} {str(cl0):>17s} {str(cl1):>17s} | {str(b0):>15s} {str(b1):>15s} | {st}")
    print(f"\ncars where authored CAR_LIGHTS != binary +0x60/+0x68: {ndiff}/{len(CODES)}")

if __name__ == "__main__":
    main()
