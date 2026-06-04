#!/usr/bin/env python3
"""measure_car_perf.py — Dump the carparam.dat performance fields for the TD5
and TD6 car fleets so the S07 TD6->TD5 performance normalization can be derived
and audited.

carparam.dat performance fields (file offset = tuning_base 0x8C + tuning offset):
  0x8C+0x68 = 0xF4  drive_torque_mult  (int16)  -> acceleration
  0x8C+0x72 = 0xFE  redline            (int16)  -> engine rev limit
  0x8C+0x74 = 0x100 speed_limit        (int16)  -> top speed (torque cutoff)
  0x8C+0x76 = 0x102 drivetrain_type    (int16)  -> RWD/FWD/AWD (NOT performance)

Sources:
  TD5: original/cars/<code>.zip   -> carparam.dat
  TD6: "Test Drive 6"/cars/param.zip -> <code>param.dat
"""
import os
import struct
import sys
import zipfile

OFF_GRIP = 0xB8     # 0x8C+0x2C tire_grip_coeff (handling)
OFF_TORQUE = 0xF4   # 0x8C+0x68 drive_torque_mult (acceleration)
OFF_BRAKE = 0xFA    # 0x8C+0x6E brake_front
OFF_ENGBRAKE = 0xFC # 0x8C+0x70 engine_brake
OFF_REDLINE = 0xFE  # 0x8C+0x72
OFF_SPEEDLIM = 0x100  # 0x8C+0x74 speed_limit (top speed)
OFF_DRIVETRAIN = 0x102  # 0x8C+0x76

DRIVETRAIN = {1: "RWD", 2: "FWD", 3: "AWD", 0: "?0"}

# TD6-new codes (mirrors convert_td6_cars.TD6_NEW_CODES) — the cars we rescale.
TD6_NEW_CODES = [
    "390", "400", "atl", "att", "aud", "bmw", "cer", "chd", "chr", "cp1",
    "cp2", "cp3", "cp4", "db7", "eli", "esp", "flx", "g40", "grf", "gts",
    "lgt", "lit", "lot", "mam", "mcj", "mcl", "mgt", "pan", "pro", "pwr",
    "s12", "shl", "sub", "sup", "toy", "tur", "tus", "xjr", "xk1",
]


def read_fields(data):
    if len(data) < 0x10C:
        return None
    return dict(
        grip=struct.unpack_from("<h", data, OFF_GRIP)[0],
        torque=struct.unpack_from("<h", data, OFF_TORQUE)[0],
        brake=struct.unpack_from("<h", data, OFF_BRAKE)[0],
        engbrake=struct.unpack_from("<h", data, OFF_ENGBRAKE)[0],
        redline=struct.unpack_from("<h", data, OFF_REDLINE)[0],
        speedlim=struct.unpack_from("<h", data, OFF_SPEEDLIM)[0],
        drivetrain=struct.unpack_from("<h", data, OFF_DRIVETRAIN)[0],
    )


def measure_td5(root):
    cars_dir = os.path.join(root, "original", "cars")
    out = {}
    for fn in sorted(os.listdir(cars_dir)):
        if not fn.endswith(".zip"):
            continue
        code = fn[:-4]
        with zipfile.ZipFile(os.path.join(cars_dir, fn)) as z:
            if "carparam.dat" not in z.namelist():
                continue
            f = read_fields(z.read("carparam.dat"))
        if f:
            out[code] = f
    return out


def measure_td6(root, codes):
    param_zip = os.path.join(root, "Test Drive 6", "cars", "param.zip")
    out = {}
    with zipfile.ZipFile(param_zip) as z:
        names = set(z.namelist())
        for code in codes:
            entry = f"{code}param.dat"
            if entry not in names:
                # aud/pro/xjr fall back to a donor in the real pipeline; skip here.
                continue
            f = read_fields(z.read(entry))
            if f:
                out[code] = f
    return out


def measure_converted(root, codes):
    """Read the post-conversion carparam.dat from re/assets/cars/<code>/."""
    out = {}
    for code in codes:
        p = os.path.join(root, "re", "assets", "cars", code, "carparam.dat")
        if not os.path.isfile(p):
            continue
        with open(p, "rb") as fh:
            f = read_fields(fh.read())
        if f:
            out[code] = f
    return out


def emit_audit_md(td5, td6_src, td6_out):
    """Print a markdown before/after table for the TD6 rescale audit."""
    lo_t, hi_t = stats(td5, "torque")
    lo_s, hi_s = stats(td5, "speedlim")
    lo_g, hi_g = stats(td5, "grip")
    lo_b, hi_b = stats(td5, "brake")
    print("## TD6 -> TD5 performance normalization -- before/after\n")
    print(f"TD5 reference band: torque {lo_t}..{hi_t}, top-speed {lo_s}..{hi_s}, "
          f"grip {lo_g}..{hi_g}, brake {lo_b}..{hi_b}\n")
    print("| code | torque (accel) | top speed | grip (handling) | brake |")
    print("|------|----------------|-----------|-----------------|-------|")
    # Sort by raw TD6 top speed so the slow->fast progression is visible.
    for code in sorted(td6_src, key=lambda c: td6_src[c]["speedlim"]):
        s, o = td6_src[code], td6_out.get(code, td6_src[code])
        print(f"| {code} | {s['torque']}->{o['torque']} | "
              f"{s['speedlim']}->{o['speedlim']} | "
              f"{s['grip']}->{o['grip']} | {s['brake']}->{o['brake']} |")


def fmt_table(title, fleet):
    print(f"\n=== {title} ({len(fleet)} cars) ===")
    print(f"{'code':<6}{'grip':>6}{'torque':>8}{'brake':>7}{'engbrk':>7}"
          f"{'redline':>9}{'spdlim':>8}{'drive':>7}")
    for code, f in sorted(fleet.items(), key=lambda kv: kv[1]["speedlim"]):
        dt = DRIVETRAIN.get(f["drivetrain"], str(f["drivetrain"]))
        print(f"{code:<6}{f['grip']:>6}{f['torque']:>8}{f['brake']:>7}"
              f"{f['engbrake']:>7}{f['redline']:>9}{f['speedlim']:>8}{dt:>7}")


def stats(fleet, key):
    vals = [f[key] for f in fleet.values()]
    return min(vals), max(vals)


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    td5 = measure_td5(root)
    td6 = measure_td6(root, TD6_NEW_CODES)

    if "--md" in sys.argv:
        td6_out = measure_converted(root, TD6_NEW_CODES)
        emit_audit_md(td5, td6, td6_out)
        return

    fmt_table("TD5 (reference fleet)", td5)
    fmt_table("TD6 (to rescale)", td6)

    print("\n=== Range summary ===")
    for label, fleet in (("TD5", td5), ("TD6", td6)):
        for key in ("grip", "torque", "brake", "engbrake", "redline", "speedlim"):
            lo, hi = stats(fleet, key)
            print(f"{label} {key:<9} min={lo:>6} max={hi:>6}")
    if "--converted" in sys.argv:
        fmt_table("TD6 (converted, rescaled)", measure_converted(root, TD6_NEW_CODES))


if __name__ == "__main__":
    main()
