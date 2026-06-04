#!/usr/bin/env python3
"""
convert_td6_cars.py — Asset-prep for porting Test Drive 6 cars into TD5RE.

TD6 is the same Pitbull engine, one format revision up. This tool populates
re/assets/cars/<code>/ with the loose files the source port's car loader
resolves (td5_asset_open_and_read / resolve_png_path), so a TD6 car can be
driven on a TD5 track via the [Game] PlayerCarArchive override.

Per TD6 car it writes:
  himodel.dat   — raw TD6 mesh (render_type 0x104). The port transcodes it at
                  RUNTIME (td5_asset_transcode_td6_mesh) into the TD5 expanded
                  vertex format; we ship it unmodified.
  carparam.dat  — copied from TD6 param.zip <code>param.dat. Byte-layout is
                  identical to TD5's carparam.dat (verified), so it drops in.
  carskin0..3.png — carbody.tga (256x256 BGRA32) converted via the SAME
                  parse_tga used for TD5 carskins, so orientation/convention
                  match. TD6 has one paint; 1..3 are copies of 0.
  carhub0..3.png — TD6 cars bake wheels into the body mesh (no separate hub
                  sprite). We still write a hub PNG (donor or 1x1 stub) so the
                  loader's hub-load step doesn't warn; the mesh never samples it.
  drive/rev/horn/reverb.wav — donor engine sounds from a TD5 car (TD6 car zips
                  ship no per-car wavs; the global sound.zip is a separate pool).
  envmodel.dat  — copied for future env-map work; unused by the port today.

TD6 mesh format (decoded, uniform across all 47 car archives):
  header @0x00: render_type=0x104, [+4]=cmd_count(=1), [+8]=vert_count,
                [+0x0C]=bounding_radius, [+0x2C]=cmds_off, [+0x30]=verts_off,
                [+0x34]=norms_off (0xCDCDCDCD, UNUSED).
  command @cmds_off (24 bytes): [+8]=vert_count, [+0x0C]=index_count(=tris*3),
                [+0x10]=vert_off, [+0x14]=index_off.
  indices @index_off: u16[index_count] (triangle list).
  vertices @verts_off: 32 bytes each = pos.xyz(f32*3) + normal.xyz(f32*3) + uv(f32*2).

Usage:
  python re/tools/convert_td6_cars.py \
      --td6-dir "Test Drive 6" \
      --out-dir <worktree>/re/assets/cars \
      --donor-dir re/assets/cars/tvr \
      [--codes cer,db7,mcl | --all]
"""
import argparse
import os
import shutil
import struct
import sys
import zipfile

import numpy as np
from PIL import Image

# Reuse the project's TGA decoder so TD6 carbody.tga lands in the exact same
# orientation/format convention as the existing TD5 carskin PNGs.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from td5_texture_converter import parse_tga  # noqa: E402

# The 39 TD6-only car codes (not present in TD5's 37-car roster). The 8 shared
# nameplates (cat cob cud jag mus van vip xkr) are intentionally EXCLUDED so we
# never clobber the existing TD5 car assets in re/assets/cars/.
TD6_NEW_CODES = [
    "390", "400", "atl", "att", "aud", "bmw", "cer", "chd", "chr", "cp1",
    "cp2", "cp3", "cp4", "db7", "eli", "esp", "flx", "g40", "grf", "gts",
    "lgt", "lit", "lot", "mam", "mcj", "mcl", "mgt", "pan", "pro", "pwr",
    "s12", "shl", "sub", "sup", "toy", "tur", "tus", "xjr", "xk1",
]

# Authoritative code -> (display name, short name), extracted from the Test
# Drive 6 binary's car name table (TD6.exe: <code>\0<short>\0[<YEAR full name>\0]).
# Display = the full year-prefixed name where TD6 has one, else the short name.
# Spaces are converted to '_' on write (the frontend turns '_' back into ' ').
# aud/pro/xjr are unfinished TD6 betas with no name-table entry — named by model.
TD6_NAME_LUT = {
    "390": ("NISSAN R390 GT-1",                "R390 GT-1"),
    "400": ("1999 VENTURI 400GT",              "400GT"),
    "atl": ("1999 VENTURI ATLANTIQUE",         "ATLANTIQUE"),
    "att": ("1999 AUDI TT COUPE",              "AUDI TT"),
    "aud": ("AUDI TT",                         "AUDI TT"),
    "bmw": ("PITBULL 1",                       "PITBULL 1"),
    "cer": ("1999 TVR CERBERA",                "CERBERA"),
    "chd": ("DODGE CONCEPT",                   "DODGE CONCEPT"),
    "chr": ("1969 DODGE CHARGER",              "CHARGER"),
    "cp1": ("JAGUAR COP CAR",                  "JAGUAR COP"),
    "cp2": ("CHARGER COP CAR",                 "CHARGER COP"),
    "cp3": ("MUSTANG COP CAR",                 "MUSTANG COP"),
    "cp4": ("CERBERA COP CAR",                 "CERBERA COP"),
    "db7": ("1999 ASTON MARTIN DB7 VANTAGE",   "DB7 VANTAGE"),
    "eli": ("1999 LOTUS ELISE",                "ELISE"),
    "esp": ("2000 PANOZ ESPERANTE",            "ESPERANTE"),
    "flx": ("1990 FORD MUSTANG LX 5.0",        "MUSTANG LX 5.0"),
    "g40": ("1968 FORD GT-40",                 "GT-40"),
    "grf": ("1999 TVR GRIFFITH",               "GRIFFITH"),
    "gts": ("1999 DODGE VIPER GTS-R",          "VIPER GTS-R"),
    "lgt": ("LOTUS GT1",                       "LOTUS GT1"),
    "lit": ("1999 FORD F150 LIGHTNING",        "F150 LIGHTNING"),
    "lot": ("1999 LOTUS ESPRIT V8",            "ESPRIT V8"),
    "mam": ("1999 MARCOS MANTARAY",            "MANTARAY"),
    "mcj": ("1968.5 FORD MUSTANG 428 CJ",      "MUSTANG 428 CJ"),
    "mcl": ("PITBULL 2",                       "PITBULL 2"),
    "mgt": ("1999 FORD MUSTANG SALEEN S351",   "SALEEN S351"),
    "pan": ("1999 PANOZ AIV ROADSTER",         "AIV ROADSTER"),
    "pro": ("FORD MUSTANG COBRA",              "MUSTANG COBRA"),
    "pwr": ("1999 PLYMOUTH PROWLER",           "PROWLER"),
    "s12": ("1999 TVR SPEED 12",               "SPEED 12"),
    "shl": ("1999 SHELBY SERIES 1",            "SERIES 1"),
    "sub": ("1999 SUBARU IMPREZA 22B STI",     "IMPREZA 22B"),
    "sup": ("1997 FORD MUSTANG SUPER STALLION","SUPER STALLION"),
    "toy": ("TOYOTA GT-ONE",                   "GT-ONE"),
    "tur": ("1982 LOTUS ESPRIT TURBO",         "ESPRIT TURBO"),
    "tus": ("1999 TVR TUSCAN",                 "TUSCAN"),
    "xjr": ("JAGUAR XJR-15",                   "XJR-15"),
    "xk1": ("JAGUAR XK-180",                   "XK-180"),
}

# Per-car STATS for the spec sheet, matching the TD5 config.nfo fields. Real-world
# figures (price, top, accel, quarter, compression, displacement, lateral, torque,
# hp, tires) are researched; gears/layout/engine reflect the car. Keys:
#   layout  one letter -> k_stat_layout_types  (drivetrain class)
#   engine  one letter -> k_stat_engine_types  (engine type)
#   gears price tire_f tire_r top accel brake quarter compression displacement
#   lateral torque hp   -> the matching spec-sheet rows
# Cars absent from this table fall back to clean defaults.
# layout: A FRONT/REAR  B FRONT/4WD  C FRONT/FRONT  D MID/4WD  E MID/REAR
# engine: A V10ALU B V8ALU-BLK C V8-SC D V8ALU E DOHC-TT F V8 G FORD-IRON
#         H PONTIAC-IRON I V8-IRON J V8-IRON-HEMI K V12 L ALU-BLK M 4CYL
#         N V6-SC O V10-SC P V8-DOHC Q V8-TT R V12-IRON
# torque/hp = "VALUE @ RPM"; displacement = "CUIN/CC"; compression "X.X:1"; lateral "X.XG"
def _S(layout, engine, gears, price, tf, tr, top, accel, brake, quarter,
       comp, disp, lat, torque, hp):
    return dict(layout=layout, engine=engine, gears=str(gears), price=price,
                tire_f=tf, tire_r=tr, top=str(top), accel=accel, brake=str(brake),
                quarter=quarter, compression=comp, displacement=disp, lateral=lat,
                torque=torque, hp=hp)
TD6_STATS = {
    # code        layout eng gr  price       tyre-f        tyre-r        top  0-60  60-0 1/4   comp     disp        lat    torque        hp
    "390": _S("E","Q",6,"$1,000,000","245/40R18","295/35R18",220,"3.5",115,"11.5","9.0:1", "214/3495","1.3G","470 @ 4400","550 @ 6800"),
    "400": _S("E","N",5,"$180,000",  "205/45R17","265/40R18",180,"4.3",118,"12.6","8.5:1", "183/2975","1.0G","384 @ 5500","408 @ 6000"),
    "atl": _S("E","N",5,"$130,000",  "205/45R17","255/40R17",170,"4.9",120,"13.0","8.5:1", "183/2975","1.0G","295 @ 4500","302 @ 5500"),
    "att": _S("B","E",6,"$42,000",   "225/45R17","225/45R17",151,"6.4",125,"14.8","9.5:1", "109/1781","0.9G","207 @ 2200","225 @ 5900"),
    "aud": _S("B","E",6,"$42,000",   "225/45R17","225/45R17",151,"6.4",125,"14.8","9.5:1", "109/1781","0.9G","207 @ 2200","225 @ 5900"),
    "bmw": _S("A","F",6,"$130,000",  "245/45R18","275/40R18",155,"4.7",118,"12.8","11.0:1","244/4000","0.9G","370 @ 3800","400 @ 6600"),
    "cer": _S("A","F",6,"$80,000",   "225/45R16","255/45R16",185,"4.0",112,"12.2","11.3:1","275/4500","0.9G","380 @ 5500","420 @ 6750"),
    "chd": _S("A","A",6,"$90,000",   "255/40R19","295/35R19",200,"4.0",115,"12.2","10.0:1","488/8000","1.0G","525 @ 4200","600 @ 6000"),
    "chr": _S("A","J",4,"$45,000",   "F70-15",   "F70-15",   130,"5.5",145,"14.0","10.1:1","440/7210","0.7G","480 @ 3200","375 @ 4600"),
    "cp1": _S("A","C",5,"N/A",       "255/40R18","255/40R18",170,"4.5",118,"12.7","9.0:1", "244/4000","0.9G","399 @ 3600","450 @ 6150"),
    "cp2": _S("A","J",4,"N/A",       "F70-15",   "F70-15",   130,"5.5",145,"14.0","10.1:1","440/7210","0.7G","480 @ 3200","375 @ 4600"),
    "cp3": _S("A","G",5,"N/A",       "245/45R17","245/45R17",150,"5.0",120,"13.5","9.0:1", "302/4942","0.8G","300 @ 3500","260 @ 5250"),
    "cp4": _S("A","F",6,"N/A",       "225/45R16","255/45R16",185,"4.0",112,"12.2","11.3:1","275/4500","0.9G","380 @ 5500","420 @ 6750"),
    "db7": _S("A","K",6,"$150,000",  "245/40R18","265/35R18",185,"5.0",115,"13.4","10.3:1","362/5935","0.9G","400 @ 5000","420 @ 6000"),
    "eli": _S("E","M",5,"$35,000",   "185/55R15","205/50R16",124,"5.8",110,"14.5","10.5:1","110/1796","0.9G","122 @ 3000","118 @ 5500"),
    "esp": _S("A","F",5,"$85,000",   "245/40R18","285/35R18",155,"5.0",118,"13.5","9.0:1", "281/4601","0.9G","300 @ 3750","305 @ 5250"),
    "flx": _S("A","G",5,"$15,000",   "225/55R16","225/55R16",140,"6.0",135,"14.5","9.0:1", "302/4942","0.8G","300 @ 3200","225 @ 4200"),
    "g40": _S("E","F",5,"$5,000,000","F70-15",   "F70-15",   205,"4.2",120,"12.6","10.5:1","289/4736","1.0G","413 @ 4200","485 @ 6000"),
    "grf": _S("A","F",5,"$50,000",   "225/50R15","245/45R16",167,"4.1",115,"12.8","10.5:1","303/4988","0.9G","320 @ 4250","340 @ 5500"),
    "gts": _S("A","A",6,"$70,000",   "275/35R18","335/30R18",190,"3.9",120,"12.0","9.6:1", "488/7990","1.0G","490 @ 3700","460 @ 5200"),
    "lgt": _S("E","Q",6,"$400,000",  "245/40R18","315/35R18",200,"3.9",115,"11.8","8.5:1", "214/3506","1.2G","443 @ 5500","550 @ 6500"),
    "lit": _S("A","C",4,"$30,000",   "295/45R18","295/45R18",142,"5.8",140,"14.1","8.4:1", "330/5409","0.8G","440 @ 3000","360 @ 4750"),
    "lot": _S("E","Q",5,"$83,000",   "235/40R17","285/35R18",175,"4.4",118,"13.0","8.0:1", "214/3506","0.9G","295 @ 4250","350 @ 6500"),
    "mam": _S("A","F",5,"$70,000",   "235/40R17","255/40R17",160,"4.5",118,"13.2","9.5:1", "281/4601","0.9G","320 @ 3500","320 @ 5500"),
    "mcj": _S("A","G",4,"$60,000",   "F70-14",   "F70-14",   130,"5.7",145,"14.2","10.6:1","428/7014","0.7G","440 @ 3400","335 @ 5200"),
    "mcl": _S("E","K",6,"$1,000,000","235/45R17","315/45R17",240,"3.2",110,"11.1","11.0:1","372/6064","1.0G","479 @ 4000","627 @ 7400"),
    "mgt": _S("A","C",5,"$45,000",   "265/35R18","295/35R18",180,"4.5",115,"12.8","9.0:1", "351/5752","0.9G","490 @ 3500","495 @ 6000"),
    "pan": _S("A","F",5,"$65,000",   "245/45R17","275/40R17",140,"4.5",118,"13.0","9.0:1", "281/4601","0.9G","300 @ 3750","305 @ 5250"),
    "pro": _S("A","F",5,"$32,000",   "245/45R17","245/45R17",155,"5.5",125,"13.8","9.0:1", "281/4601","0.8G","300 @ 3500","320 @ 5500"),
    "pwr": _S("A","L",4,"$45,000",   "225/45R17","295/40R20",118,"6.5",135,"15.0","9.6:1", "215/3518","0.8G","255 @ 3950","253 @ 6400"),
    "s12": _S("A","K",6,"$246,000",  "280/650R18","300/710R18",240,"3.0",110,"11.0","12.5:1","472/7730","2.1G","650 @ 5750","820 @ 7000"),
    "shl": _S("A","F",6,"$170,000",  "265/40R18","315/40R18",170,"4.4",118,"12.8","10.5:1","244/4000","1.0G","290 @ 4500","320 @ 5750"),
    "sub": _S("B","E",5,"$50,000",   "235/40R17","235/40R17",150,"4.7",120,"13.3","8.0:1", "134/2212","0.9G","268 @ 3200","280 @ 6000"),
    "sup": _S("A","C",5,"$200,000",  "265/40R18","315/35R18",180,"4.5",118,"12.8","9.0:1", "330/5409","0.9G","500 @ 3500","400 @ 5500"),
    "toy": _S("E","Q",6,"$2,000,000","245/40R18","295/35R18",220,"3.5",110,"11.0","9.0:1", "219/3578","1.4G","480 @ 4400","600 @ 6000"),
    "tur": _S("E","M",5,"$60,000",   "195/60R15","235/60R15",150,"5.5",125,"14.0","7.5:1", "133/2174","0.9G","200 @ 4500","210 @ 6500"),
    "tus": _S("A","F",5,"$60,000",   "225/45R16","255/40R17",180,"4.0",112,"12.4","11.5:1","239/3996","0.9G","310 @ 5000","360 @ 7000"),
    "xjr": _S("E","K",6,"$960,000",  "235/45R17","315/40R17",191,"3.9",112,"11.8","11.0:1","366/5993","1.0G","420 @ 4500","450 @ 6250"),
    "xk1": _S("A","C",5,"$2,000,000","245/40R18","285/35R18",180,"4.5",118,"12.8","9.0:1", "244/3996","0.9G","387 @ 3600","450 @ 6150"),
}

# Per-car ENGINE SOUND: which of TD6's 20 engine banks (cars/sound.zip) each car
# uses, written to BOTH drive.wav and rev.wav so the engine note is the car's
# across the whole RPM range (horn/reverb stay donor). The 20 banks are organized
# by car family/manufacturer (confirmed by RE'ing TD6.exe: engine =
# engine_table[carstruct+0x0d], table @0x49a7f8, banks Tvr/Jag/Mus/Vip/Nis/Sal/
# Cob/F15/Pro/As/Sta/Mo/Pon/Chev/Z28/Sti/Fb/Bus/Arm/98v). TD6's static per-car
# index byte reads as uninitialized defaults (set at runtime), so this maps by
# the table's family layout instead. Cars with no dedicated bank (European
# 4-cyl/exotics) are mapped by engine character. Unmapped codes keep the donor.
TD6_ENGINE_SOUND = {
    # confident (manufacturer/family)
    "cer": "TVR", "tus": "TVR", "grf": "TVR", "s12": "TVR", "cp4": "TVR",
    "xjr": "Jag", "xk1": "Jag", "cp1": "Jag",
    "flx": "Mus", "mcj": "Mus", "cp3": "Mus",
    "gts": "Vip", "chd": "Vip",
    "390": "Nis", "mgt": "Sal", "lit": "F15", "pwr": "Pro", "sup": "Sta",
    "pro": "Cob", "shl": "Cob", "g40": "Cob",
    "db7": "As",  "mcl": "As",
    "chr": "Mo",  "cp2": "Mo",
    # exotics — by engine character (refine by ear)
    "esp": "Fb",  "pan": "Fb",  "mam": "Fb",      # Panoz / Marcos Ford V8
    "lgt": "Sti", "lot": "Sti",                   # Lotus V8 race
    "toy": "Z28", "bmw": "Chev",                  # Toyota race V8TT / Pitbull V8
    "400": "98v", "atl": "98v",                   # Venturi V6
    "att": "Arm", "aud": "Arm", "eli": "Arm", "tur": "Arm",  # Audi / Lotus 4-cyl
    "sub": "Bus",                                 # Subaru flat-4 turbo
}

# Per-car wheel/body adjustments (carparam units, deltas from the raw TD6 data
# after the wheel-Y clamp). TD6 anchors wheels to the carparam 0x40 block (RE-
# confirmed in TD6.exe); some cars ship template/imperfect wheel data that does
# not fit their unique body, so we adapt the ASSET (the engine stays faithful).
# Keys (all optional, default 0 / rscale 1.0):
#   fz     front wheels Z delta  (+ = forward/front, - = rearward/back)  [0x44,0x4C]
#   rz     rear  wheels Z delta  (+ = forward,       - = back)           [0x54,0x5C]
#   out    track delta, all 4    (+ = outward,       - = inward)         [0x40..0x58 X]
#   out_f  track delta, front only (added to out)                        [0x40,0x48 X]
#   out_r  track delta, rear only  (added to out)                        [0x50,0x58 X]
#   dy     ride-height delta     (+ = body LOWER,    - = body UP)        [0x42..0x5A Y]
#   rscale wheel radius+width multiplier (e.g. 1.20 = 20% bigger)        [0x82,0x84]
TD6_WHEEL_ADJ = {
    "390": dict(dy=35, fz=-35, rz=-40),
    "att": dict(out=10),
    "aud": dict(out=50, out_r=-10, fz=-80, rz=-60),
    "bmw": dict(dy=-10),
    "cer": dict(fz=35, rz=10),
    "chd": dict(fz=40, rz=-20),
    "cp3": dict(out=15),
    "cp4": dict(fz=35, rz=20),
    "grf": dict(fz=65, rz=-65, out=15, rscale=1.20),
    "gts": dict(fz=-15, rz=15, dy=20),
    "lit": dict(dy=-30),
    "mcj": dict(fz=-10, rz=-10),
    "mcl": dict(fz=50, rz=40),
    "pan": dict(out=-70, fz=-35, rz=-10),
    "sub": dict(fz=20),
    "tus": dict(fz=-80, rz=-80, dy=10, out=-15),
    "xjr": dict(fz=-100, dy=10),
    "xk1": dict(dy=50),
}


# --- Performance normalization (S07) -------------------------------------
# Out of the box the TD6 roster feels categorically faster than the TD5 cars.
# carparam.dat is byte-identical TD5<->TD6, so the same physics-tuning fields
# drive both fleets; the TD6 data simply sits well above TD5's range in the
# acceleration / handling / braking fields (measured with measure_car_perf.py):
#
#   field (tuning off / file off)     TD5 band     raw TD6 band
#   grip      0x2C / 0xB8  handling   2200..2750   2375..5000
#   torque    0x68 / 0xF4  accel        50..180      55..240   <- main culprit
#   brake     0x6E / 0xFA  brake       400..750     450..2800
#   engbrake  0x70 / 0xFC  eng-brake   400..750     450..2200
#   speedlim  0x74 / 0x100 top speed   719..1159    635..1130
#   redline   0x72 / 0xFE  rev limit  6200..7600   6200..6900  (already in band)
#
# Fix: a per-field LINEAR MIN-MAX remap that maps each field's observed TD6
# [min,max] onto TD5's [min,max]. This lands the slowest TD6 car near the TD5
# slowest and the fastest near the TD5 fastest, distributes the rest
# proportionally, and — being monotonic — preserves the TD6 cars' ranking
# among themselves. redline is left faithful (already inside the TD5 band).
# Donor-param cars (aud/pro/xjr, which borrow a TD5 carparam) are NOT remapped.
#
# Each entry: field -> (file_offset, td5_min, td5_max). The td5 band is the
# 37-car reference fleet from original/cars/*.zip (measure_car_perf.py,
# 2026-06-04). The TD6 source [min,max] is computed live in main() over the
# full TD6 param set so a --codes subset run still uses the global remap.
PERF_REMAP_FIELDS = {
    "grip":     (0xB8,  2200, 2750),   # 0x8C+0x2C tire_grip_coeff (handling)
    "torque":   (0xF4,    50,  180),   # 0x8C+0x68 drive_torque_mult (acceleration)
    "brake":    (0xFA,   400,  750),   # 0x8C+0x6E brake_force
    "engbrake": (0xFC,   400,  750),   # 0x8C+0x70 engine_brake
    "speedlim": (0x100,  719, 1159),   # 0x8C+0x74 speed_limit (top speed)
}


def compute_td6_perf_ranges(param_zip, codes):
    """Scan every TD6 car param entry and return {field: (td6_min, td6_max)}
    for the PERF_REMAP_FIELDS. Computed over the full TD6 set (not a --codes
    subset) so the remap endpoints are stable regardless of which cars are
    being (re)converted."""
    vals = {name: [] for name in PERF_REMAP_FIELDS}
    with zipfile.ZipFile(param_zip) as pf:
        names = set(pf.namelist())
        for code in codes:
            entry = f"{code}param.dat"
            if entry not in names:
                continue  # donor-param car — excluded from the source band
            data = pf.read(entry)
            if len(data) < 0x10C:
                continue
            for name, (off, _lo, _hi) in PERF_REMAP_FIELDS.items():
                vals[name].append(struct.unpack_from("<h", data, off)[0])
    return {name: (min(v), max(v)) for name, v in vals.items() if v}


def remap_perf_fields(carparam, perf_ranges):
    """Linear min-max remap each perf field from its TD6 source range onto the
    TD5 target band, clamped to the band. Mutates the bytearray in place and
    returns {field: (before, after)} for the audit log."""
    audit = {}
    for name, (off, td5_lo, td5_hi) in PERF_REMAP_FIELDS.items():
        src = perf_ranges.get(name)
        if not src:
            continue
        td6_lo, td6_hi = src
        before = struct.unpack_from("<h", carparam, off)[0]
        if td6_hi == td6_lo:
            after = before  # degenerate source range -> leave untouched
        else:
            t = (before - td6_lo) / (td6_hi - td6_lo)
            after = int(round(td5_lo + t * (td5_hi - td5_lo)))
            after = max(td5_lo, min(td5_hi, after))
        struct.pack_into("<h", carparam, off, after)
        audit[name] = (before, after)
    return audit


def td6_names(code):
    """Return (display_underscored, short) for a TD6 car code."""
    if code in TD6_NAME_LUT:
        return TD6_NAME_LUT[code]
    return (code.upper(), code.upper())


SOUND_FILES = ["drive.wav", "rev.wav", "horn.wav", "reverb.wav"]


def verify_td6_himodel(data, code):
    """Sanity-check that himodel.dat is the expected TD6 indexed format.
    Returns (ok, message)."""
    if len(data) < 0x40:
        return False, "too small"
    render_type = struct.unpack_from("<h", data, 0)[0]
    cmd_count = struct.unpack_from("<i", data, 4)[0]
    vert_count = struct.unpack_from("<i", data, 8)[0]
    cmds_off = struct.unpack_from("<I", data, 0x2C)[0]
    verts_off = struct.unpack_from("<I", data, 0x30)[0]
    if render_type != 0x104:
        return False, f"render_type=0x{render_type & 0xffff:x} (expected 0x104)"
    if cmd_count < 1:
        return False, f"cmd_count={cmd_count}"
    if vert_count <= 0:
        return False, f"vert_count={vert_count}"
    # Stride must compute to exactly 32 bytes (pos+normal+uv).
    if vert_count and (len(data) - verts_off) % vert_count != 0:
        return False, "vertex region not a whole multiple of vert_count"
    stride = (len(data) - verts_off) // vert_count
    if stride != 32:
        return False, f"vertex stride={stride} (expected 32)"
    # Validate the command's index buffer.
    base = cmds_off
    idx_count = struct.unpack_from("<i", data, base + 0x0C)[0]
    idx_off = struct.unpack_from("<i", data, base + 0x14)[0]
    if idx_count % 3 != 0:
        return False, f"index_count={idx_count} not a multiple of 3"
    if idx_off + idx_count * 2 > len(data):
        return False, "index buffer overruns file"
    return True, f"rt=0x104 verts={vert_count} tris={idx_count // 3}"


def write_png(img, path):
    img.save(path, "PNG")


def convert_car(code, td6_dir, out_dir, donor_dir, param_zip, dry_run,
                perf_ranges=None):
    car_zip = os.path.join(td6_dir, "cars", f"{code}.zip")
    if not os.path.isfile(car_zip):
        return False, f"{code}: car zip not found ({car_zip})"

    with zipfile.ZipFile(car_zip) as zf:
        names = zf.namelist()
        if "himodel.dat" not in names or "carbody.tga" not in names:
            return False, f"{code}: missing himodel.dat/carbody.tga"
        himodel = zf.read("himodel.dat")
        carbody = zf.read("carbody.tga")
        envmodel = zf.read("envmodel.dat") if "envmodel.dat" in names else None

    ok, msg = verify_td6_himodel(himodel, code)
    if not ok:
        return False, f"{code}: himodel verify FAILED ({msg})"

    # carparam.dat from TD6 param.zip <code>param.dat. A few unfinished/clone TD6
    # cars (aud, pro, xjr) ship a mesh but no param entry — fall back to the
    # donor's carparam.dat so they still load and drive (approximate physics).
    param_entry = f"{code}param.dat"
    param_note = ""
    param_is_donor = False
    with zipfile.ZipFile(param_zip) as pf:
        if param_entry in pf.namelist():
            carparam = pf.read(param_entry)
        else:
            donor_param = os.path.join(donor_dir, "carparam.dat")
            if not os.path.isfile(donor_param):
                return False, f"{code}: {param_entry} missing and no donor carparam.dat"
            with open(donor_param, "rb") as f:
                carparam = f.read()
            param_is_donor = True
            param_note = " [param=DONOR]"
    if len(carparam) < 0x10C:
        return False, f"{code}: carparam.dat too small ({len(carparam)} bytes)"

    # --- Wheel-Y normalization (TD6 asset adaptation, NOT an engine change) ---
    # carparam 0x42/0x4A/0x52/0x5A = per-wheel vertical mount Y (signed int16),
    # read by RefreshVehicleWheelContactFrames @0x00403720 (RE-confirmed): a more-
    # negative value mounts the wheel lower, so ground-snap lifts the chassis and
    # the body floats high with the wheels hanging exposed. A few TD6 race cars
    # (tus/lit/pan) ship an anomalous -200 (the rest of the roster clusters at
    # -71..-153, median -120) and sit far too high. The byte-faithful TD5 engine
    # handles the value correctly — so we clamp the outlier in the ASSET to the
    # roster median, which tucks the wheels back into the arches.
    WHEELY_OUTLIER, WHEELY_TARGET = -160, -120
    carparam = bytearray(carparam)
    wy_fixed = None
    for w in range(4):
        off = 0x42 + w * 8
        v = struct.unpack_from("<h", carparam, off)[0]
        if v < WHEELY_OUTLIER:
            struct.pack_into("<h", carparam, off, WHEELY_TARGET)
            wy_fixed = v
    if wy_fixed is not None:
        param_note += f" [wheelY {wy_fixed}->{WHEELY_TARGET}]"

    # --- Per-car wheel/body adjustments (see TD6_WHEEL_ADJ) -------------------
    adj = TD6_WHEEL_ADJ.get(code)
    if adj:
        fz, rz = adj.get("fz", 0), adj.get("rz", 0)
        out, dy = adj.get("out", 0), adj.get("dy", 0)
        out_f, out_r = adj.get("out_f", 0), adj.get("out_r", 0)
        rscale = adj.get("rscale", 1.0)
        for w in range(4):
            xo, yo, zo = 0x40 + w * 8, 0x42 + w * 8, 0x44 + w * 8
            x = struct.unpack_from("<h", carparam, xo)[0]
            y = struct.unpack_from("<h", carparam, yo)[0]
            z = struct.unpack_from("<h", carparam, zo)[0]
            ox = out + (out_f if w < 2 else out_r)
            if ox:                        # outward(+)/inward(-) along |X|
                x += ox if x > 0 else -ox
            if dy:                        # body lower(+)/up(-)
                y += dy
            dz = fz if w < 2 else rz      # wheels 0,1 = front; 2,3 = rear
            if dz:
                z += dz
            struct.pack_into("<h", carparam, xo, x)
            struct.pack_into("<h", carparam, yo, y)
            struct.pack_into("<h", carparam, zo, z)
        if rscale != 1.0:                 # wheel radius (0x82) + width (0x84)
            for ro in (0x82, 0x84):
                v = struct.unpack_from("<h", carparam, ro)[0]
                struct.pack_into("<h", carparam, ro, int(round(v * rscale)))
        param_note += f" [adj {code}]"

    # --- Performance normalization to TD5's range (S07, see PERF_REMAP_FIELDS) -
    # Rescale the TD6 acceleration/handling/braking/top-speed fields into TD5's
    # band so the TD6 roster no longer feels categorically faster. Skipped for
    # donor-param cars (their carparam is already a TD5 car's).
    perf_audit = None
    if perf_ranges and not param_is_donor:
        perf_audit = remap_perf_fields(carparam, perf_ranges)
        if perf_audit:
            chg = ",".join(f"{k}:{b}->{a}" for k, (b, a) in perf_audit.items())
            param_note += f" [perf {chg}]"

    carparam = bytes(carparam)

    # Convert carbody.tga -> RGBA via the shared decoder (handles descriptor flip).
    # The TD6 body paint is a UNIFORM YELLOW placeholder (~[185,184,0]) that the
    # original game recolours at runtime; glass/lights/chrome/tyres are their own
    # colours. So we build a PAINT MASK from the yellow region and:
    #   * body  (yellow)     -> grayscale luminance  (so the port's tint can
    #                            MODULATE it to any hue), mask = white
    #   * fixed (everything   -> ORIGINAL colour, untouched by paint, mask = black
    #      else)
    # carskin keeps the fixed parts in colour; carmask.png drives the in-race
    # body-only re-bake and (via saturation) the frontend's body-only overlay.
    skin_color = parse_tga(carbody)
    if skin_color is None:
        return False, f"{code}: carbody.tga decode failed"
    arr = np.array(skin_color.convert("RGB")).astype(np.float32)
    R, G, B = arr[:, :, 0], arr[:, :, 1], arr[:, :, 2]
    # Yellow body: high R AND high G AND low B, with R~=G (excludes red lights,
    # blue glass, neutral chrome, black tyres).
    is_body = (R > 80) & (G > 80) & (B < R * 0.6) & (B < G * 0.6) & (np.abs(R - G) < 50)
    lum = (0.299 * R + 0.587 * G + 0.114 * B).astype(np.uint8)
    out = arr.astype(np.uint8).copy()
    gray3 = np.dstack([lum, lum, lum])
    out[is_body] = gray3[is_body]                       # body -> gray, fixed -> original
    alpha = np.full(out.shape[:2], 255, np.uint8)
    skin_img = Image.fromarray(np.dstack([out, alpha]), "RGBA")
    mask_img = Image.fromarray((is_body.astype(np.uint8) * 255), "L")

    car_out = os.path.join(out_dir, code)
    if dry_run:
        return True, f"{code}: OK [dry-run] ({msg}, skin {skin_img.size}, param {len(carparam)}B){param_note}"

    os.makedirs(car_out, exist_ok=True)

    # himodel.dat (raw TD6 — runtime-transcoded)
    with open(os.path.join(car_out, "himodel.dat"), "wb") as f:
        f.write(himodel)
    # carparam.dat
    with open(os.path.join(car_out, "carparam.dat"), "wb") as f:
        f.write(carparam)
    # envmodel.dat (future env-map; unused today)
    if envmodel is not None:
        with open(os.path.join(car_out, "envmodel.dat"), "wb") as f:
            f.write(envmodel)
    # carskin0..3.png (single TD6 paint; 1..3 are copies)
    for i in range(4):
        write_png(skin_img, os.path.join(car_out, f"carskin{i}.png"))
    # carmask.png — paint mask (white = body paint area, black = fixed parts).
    # Paint-independent, so a single file. Consumed by the in-race body re-bake.
    write_png(mask_img, os.path.join(car_out, "carmask.png"))
    # carhub0..3.png — donor hub if available, else 2x2 transparent stub so the
    # loader's hub step doesn't warn. The transcoded mesh never samples it.
    donor_hub = os.path.join(donor_dir, "carhub0.png")
    for i in range(4):
        dst = os.path.join(car_out, f"carhub{i}.png")
        if os.path.isfile(donor_hub):
            shutil.copyfile(donor_hub, dst)
        else:
            Image.new("RGBA", (2, 2), (0, 0, 0, 0)).save(dst, "PNG")
    # Engine sounds: per-car TD6 engine bank (cars/sound.zip) -> drive.wav + rev.wav
    # + reverb.wav (see TD6_ENGINE_SOUND); horn stays donor. Falls back to donor
    # when the car is unmapped or the bank/sound.zip is missing.
    #
    # reverb.wav is CRITICAL: the local player (slot 0) is the "reverb vehicle",
    # so td5_sound loads Reverb.wav (NOT Rev.wav) for the player's rev loop
    # (td5_sound.c:464). Leaving reverb.wav as the donor made every player car
    # share the SAME engine note above idle -> "same sound on all cars".
    BANK_TARGETS = ("drive.wav", "rev.wav", "reverb.wav")
    bank_wav = None
    bank = TD6_ENGINE_SOUND.get(code)
    if bank:
        sndzip = os.path.join(td6_dir, "cars", "sound.zip")
        if os.path.isfile(sndzip):
            with zipfile.ZipFile(sndzip) as sz:
                ent = next((n for n in sz.namelist()
                            if n.lower() == f"{bank.lower()}.wav"), None)
                if ent:
                    bank_wav = sz.read(ent)
    for snd in SOUND_FILES:
        if snd in BANK_TARGETS and bank_wav:
            with open(os.path.join(car_out, snd), "wb") as f:
                f.write(bank_wav)
            continue
        src = os.path.join(donor_dir, snd)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(car_out, snd))

    # config.nfo — the 17-line TD5 spec sheet. Line 0 = display name (shown in
    # the SELECT CAR title), line 1 = short name; both written with '_' for
    # spaces (the frontend converts back). Lines 2-16 are the STATS sub-screen
    # (LAYOUT, GEARS, PRICE, TIRES f/r, TOP SPEED, 0-60, 60-0, 1/4, ENGINE,
    # COMPRESSION, DISPLACEMENT, LATERAL ACC, TORQUE, HP). Real per-car stats
    # come from TD6_STATS (real-world specs + param-derived); cars not yet in
    # that table get safe defaults so the screen reads cleanly.
    disp, short = td6_names(code)
    us = lambda s: s.replace(" ", "_")
    st = TD6_STATS.get(code, {})
    nfo_lines = [
        us(disp),                          # 0  display name
        us(short),                         # 1  short name
        st.get("layout", "A"),             # 2  LAYOUT (letter A..)
        st.get("gears", "6"),              # 3  GEARS
        st.get("price", "N/A"),            # 4  PRICE
        us(st.get("tire_f", "N/A")),       # 5  TIRES front
        us(st.get("tire_r", "N/A")),       # 6  TIRES rear
        st.get("top", "0"),                # 7  TOP SPEED (MPH)
        st.get("accel", "0.0"),            # 8  0-60 (sec)
        st.get("brake", "0"),              # 9  60-0 (ft)
        st.get("quarter", "0.0"),          # 10 1/4 MILE (sec)
        st.get("engine", "A"),             # 11 ENGINE (letter)
        st.get("compression", "N/A"),      # 12 COMPRESSION
        st.get("displacement", "N/A"),     # 13 DISPLACEMENT
        st.get("lateral", "0.0G"),         # 14 LATERAL ACC
        us(st.get("torque", "N/A")),       # 15 TORQUE
        us(st.get("hp", "N/A")),           # 16 HP
    ]
    with open(os.path.join(car_out, "config.nfo"), "w", newline="\r\n") as f:
        f.write("\n".join(nfo_lines) + "\n")

    return True, f"{code}: OK ({msg}, skin {skin_img.size}, name '{disp}'){param_note}"


def main():
    ap = argparse.ArgumentParser(description="Port TD6 cars into TD5RE re/assets/cars/")
    ap.add_argument("--td6-dir", default="Test Drive 6",
                    help='TD6 install dir (contains cars/, default "Test Drive 6")')
    ap.add_argument("--out-dir", required=True,
                    help="Destination re/assets/cars dir (usually the worktree's)")
    ap.add_argument("--donor-dir", default="re/assets/cars/tvr",
                    help="TD5 car dir to source engine sounds + hub from")
    ap.add_argument("--codes", default=None,
                    help="Comma-separated car codes (default: all 39 TD6-new)")
    ap.add_argument("--all", action="store_true", help="Convert all 39 TD6-new cars")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    param_zip = os.path.join(args.td6_dir, "cars", "param.zip")
    if not os.path.isfile(param_zip):
        print(f"ERROR: param.zip not found at {param_zip}", file=sys.stderr)
        return 2

    if args.codes:
        codes = [c.strip() for c in args.codes.split(",") if c.strip()]
    else:
        codes = TD6_NEW_CODES

    # Performance-normalization endpoints are computed over the FULL TD6 set so a
    # --codes subset run still uses the same global remap as an --all run.
    perf_ranges = compute_td6_perf_ranges(param_zip, TD6_NEW_CODES)

    ok_n = 0
    fail = []
    for code in codes:
        ok, msg = convert_car(code, args.td6_dir, args.out_dir, args.donor_dir,
                              param_zip, args.dry_run, perf_ranges)
        print(("  " if ok else "X ") + msg)
        if ok:
            ok_n += 1
        else:
            fail.append(code)
    print(f"\nConverted {ok_n}/{len(codes)} cars -> {args.out_dir}")
    if fail:
        print(f"FAILED: {', '.join(fail)}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
