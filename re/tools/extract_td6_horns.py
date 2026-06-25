#!/usr/bin/env python3
"""Extract Test Drive 6 car horns and install them as the per-car horn sounds
used by TD5RE's regular-car horn (the press-the-horn-key honk).

Background
----------
The original TD5 binary NEVER played a regular-car horn — the horn control bit
(0x200000) only drove the cop siren + a remote-brake cheat, and although
`Horn.wav` was loaded into each car's audio slot (i*3+2) it was never played
(RE-confirmed @ 0x00440a30 / 0x00441a80). TD5RE wires that dead playback path
up so regular cars honk; this script supplies the horn AUDIO from TD6.

TD6 ships richer horns than the per-car TD5 `horn.wav` files:
  Test Drive 6/sound/Sound.zip  ->  HornLoop.WAV, HornLp2.WAV (generics) and
                                    ViperHorn / PontiacHorn / MoparHorn /
                                    AstonHorn / TankHorn (character horns)
  Test Drive 6/cars/sound.zip   ->  horn.wav (a ~33 ms blip — NOT used)
All are mono 22050 Hz 16-bit PCM — the exact format the port's horn channel
expects (TD5_SOUND_FREQ_22050), so they are byte-for-byte drop-ins.

What this does
--------------
Writes the mapped TD6 horn into each `re/assets/cars/<code>/horn.wav`
(the loose-file the port loads via td5_asset_open_and_read). A car whose code
is listed in CHARACTER_MAP gets that character horn; every other car gets
GENERIC_HORN. The raw TD6 horns are also dumped to
`re/assets/sound/td6_horns/` for reference.

re/assets/ is gitignored (shipped via the LAN release pipeline, regenerated
from original/*.zip by td5_asset_extractor.py). Re-run THIS script after any
re/assets regen, or the per-car TD5 horns will come back. This script (in
re/tools, git-tracked) is the durable, reviewable record — edit the map below
and re-run to retune.

Usage
-----
  python re/tools/extract_td6_horns.py
  python re/tools/extract_td6_horns.py --td6-dir "C:/.../Test Drive 6" \
                                       --assets-dir "C:/.../re/assets"
"""
import argparse
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Generic horn for any car not in CHARACTER_MAP. HornLp2 is the cleaner/longer
# (~0.5 s) of the two TD6 generic horns.
GENERIC_HORN = "HornLp2.WAV"

# TD6 character horn  ->  list of TD5RE car codes (re/assets/cars/<code> dirs).
# Edit freely and re-run. Identities from re/track_car_ids.md.
CHARACTER_MAP = {
    # Dodge Viper
    "ViperHorn.WAV":   ["vip"],
    # Aston Martin: '98 Vantage, Project Vantage, DB7
    "AstonHorn.WAV":   ["van", "atp", "db7"],
    # Mopar (Chrysler / Dodge / Plymouth): 'Cuda, '69 Charger, Daytona, CHR, CHD
    "MoparHorn.WAV":   ["cud", "crg", "day", "chr", "chd"],
    # Pontiac GTO
    "PontiacHorn.WAV": ["gto"],
    # Heavy "Pitbull" concept beasts -> deep air-horn.
    # NOTE: most speculative mapping — retune/remove if it sounds wrong.
    "TankHorn.WAV":    ["pit", "sp8", "sp1"],
}


def zip_entry_ci(z, name):
    """Case-insensitive lookup of a zip member; returns the real name or None."""
    lut = {n.lower(): n for n in z.namelist()}
    return lut.get(name.lower())


def read_td6_horn(td6_dir, wav_name):
    """Read a horn WAV's bytes from the TD6 Sound.zip (falls back to cars/sound.zip)."""
    for rel in (os.path.join("sound", "Sound.zip"),
                os.path.join("cars", "sound.zip")):
        zp = os.path.join(td6_dir, rel)
        if not os.path.exists(zp):
            continue
        with zipfile.ZipFile(zp) as z:
            entry = zip_entry_ci(z, wav_name)
            if entry:
                return z.read(entry)
    return None


def main():
    ap = argparse.ArgumentParser(description="Install TD6 horns as TD5RE per-car horns")
    ap.add_argument("--td6-dir", default=os.path.join(ROOT, "Test Drive 6"),
                    help="Path to the Test Drive 6 install (default: <repo>/Test Drive 6)")
    ap.add_argument("--assets-dir", default=os.path.join(ROOT, "re", "assets"),
                    help="Path to re/assets (default: <repo>/re/assets)")
    ap.add_argument("--dry-run", action="store_true", help="Report only; write nothing")
    args = ap.parse_args()

    cars_dir = os.path.join(args.assets_dir, "cars")
    if not os.path.isdir(cars_dir):
        print(f"ERROR: car assets dir not found: {cars_dir}", file=sys.stderr)
        return 1
    if not os.path.isdir(args.td6_dir):
        print(f"ERROR: TD6 install not found: {args.td6_dir}", file=sys.stderr)
        return 1

    # Resolve the set of horn WAVs we need and load their bytes once.
    needed = {GENERIC_HORN} | set(CHARACTER_MAP.keys())
    horn_bytes = {}
    for wav in sorted(needed):
        data = read_td6_horn(args.td6_dir, wav)
        if data is None:
            print(f"ERROR: TD6 horn '{wav}' not found in TD6 archives", file=sys.stderr)
            return 1
        horn_bytes[wav] = data
        print(f"  loaded TD6 {wav:16s} {len(data):6d} bytes")

    # Dump raw TD6 horns to a reference folder.
    ref_dir = os.path.join(args.assets_dir, "sound", "td6_horns")
    if not args.dry_run:
        os.makedirs(ref_dir, exist_ok=True)
        for wav, data in horn_bytes.items():
            with open(os.path.join(ref_dir, wav), "wb") as f:
                f.write(data)

    # Invert the character map: code -> horn.
    code_to_horn = {}
    for wav, codes in CHARACTER_MAP.items():
        for c in codes:
            code_to_horn[c] = wav

    # Install one horn per existing car dir.
    counts = {}
    missing_mapped = []
    car_codes = sorted(d for d in os.listdir(cars_dir)
                       if os.path.isdir(os.path.join(cars_dir, d)))
    for code in car_codes:
        wav = code_to_horn.get(code, GENERIC_HORN)
        counts[wav] = counts.get(wav, 0) + 1
        dst = os.path.join(cars_dir, code, "horn.wav")
        if not args.dry_run:
            with open(dst, "wb") as f:
                f.write(horn_bytes[wav])

    # Warn about mapped codes that have no car dir (typo / renamed car).
    for code in code_to_horn:
        if code not in car_codes:
            missing_mapped.append(code)

    print(f"\nInstalled TD6 horns into {len(car_codes)} car dirs"
          f"{' (dry-run, nothing written)' if args.dry_run else ''}:")
    for wav in sorted(counts):
        chars = ",".join(sorted(CHARACTER_MAP.get(wav, []))) if wav != GENERIC_HORN else "(generic)"
        print(f"  {wav:16s} x{counts[wav]:3d}   {chars}")
    if missing_mapped:
        print(f"\nWARNING: mapped codes with no car dir (ignored): {', '.join(missing_mapped)}")
    print(f"\nReference copies: {ref_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
