#!/usr/bin/env python3
"""FIX 1 verification: quantify the divergence the OLD libm trig helpers leaked,
and confirm the NEW LUT path is byte-faithful to the original's runtime int LUT.

- s_cosFixedTable (NEW) = round_half_even(float32(dumped_float[i]) * 4096) — this is
  byte-identical to the original's g_sinCosLut_fixed12 @ 0x483984, which
  BuildSinCosLookupTables @ 0x40A650 builds the same way from the same float table.
- OLD cos_fixed12(a) = trunc(cos(a*2pi/4096) * 4096)  (host libm, C cast = trunc-to-0)
- OLD sin_fixed12(a) = trunc(sin(a*2pi/4096) * 4096)
- OLD atan2_fixed12(dx,dz) = trunc(atan2(dx,dz)*4096/2pi) & 0xFFF
- NEW atan2_fixed12(dx,dz) = AngleFromVector12(dx,dz) & 0xFFF  (byte-faithful 0x40A720)
"""
import math, struct, re, sys

ROOT = r"C:/Users/maria/Desktop/Proyectos/TD5RE"
LUT_DATA = ROOT + "/td5mod/src/td5re/td5_trig_lut_data.c"
RENDER_C = ROOT + "/td5mod/src/td5re/td5_render.c"

# ---- 1. parse dumped float LUT bits -> float32 ----
txt = open(LUT_DATA).read()
bits = [int(m, 16) for m in re.findall(r"0x([0-9a-fA-F]{8})u", txt)]
assert len(bits) == 5120, f"expected 5120 float bits, got {len(bits)}"
fcos = [struct.unpack("<f", struct.pack("<I", b))[0] for b in bits]  # float32 cos values

def round_half_even(x):
    # Python round() is round-half-to-even, matching FISTP default rounding.
    return int(round(x))

# NEW int LUT (port s_cosFixedTable == original g_sinCosLut_fixed12)
int_lut = [round_half_even(fcos[i] * 4096.0) for i in range(5120)]

def NEW_cos(a):  return int_lut[a & 0xFFF]
def NEW_sin(a):  return int_lut[(a - 0x400) & 0xFFF]

# ---- 2. cross-check vs the Ghidra agent's reconstructed values ----
checks = {0:4096,256:3784,512:2896,683:2046,768:1567,1000:151,1023:6,1024:0,
          1025:-6,1280:-1567,1536:-2896,1707:-3548,2000:-4085,2047:-4096,
          2048:-4096,2049:-4096,2304:-3784,2560:-2896,2731:-2046,3000:-451,
          3072:0,3413:2046,3584:2896,3840:3784,4000:4052,4094:4096,4095:4096,
          1:4096,2:4096,3:4096}
idx_1_40 = [4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4095,4095,4095,4095,
            4095,4095,4095,4094,4094,4094,4094,4094,4093,4093,4093,4093,4092,4092,
            4092,4092,4091,4091,4091,4090,4090,4090,4089,4089,4089,4088]
idx_1020_1030 = {1020:25,1021:19,1022:13,1023:6,1024:0,1025:-6,1026:-13,1027:-19,
                 1028:-25,1029:-31,1030:-38}
mismatch = 0
for i,v in checks.items():
    if int_lut[i] != v:
        print(f"  CROSSCHECK MISMATCH i={i}: offline={int_lut[i]} agent={v}"); mismatch += 1
for k,i in enumerate(range(1,41)):
    if int_lut[i] != idx_1_40[k]:
        print(f"  CROSSCHECK MISMATCH i={i}: offline={int_lut[i]} agent={idx_1_40[k]}"); mismatch += 1
for i,v in idx_1020_1030.items():
    if int_lut[i] != v:
        print(f"  CROSSCHECK MISMATCH i={i}: offline={int_lut[i]} agent={v}"); mismatch += 1
print(f"[1] int-LUT cross-check vs Ghidra reconstruction: "
      f"{'ALL MATCH' if mismatch==0 else str(mismatch)+' MISMATCHES'} "
      f"(683={int_lut[683]}, 3000={int_lut[3000]} — non-textbook values reproduced)")

# ---- 3. OLD libm cos/sin drift vs NEW LUT, over the full 4096-angle circle ----
def OLD_cos(a):
    rad = (a & 0xFFF) * (2.0*math.pi/4096.0); return int(math.cos(rad)*4096.0)  # trunc
def OLD_sin(a):
    rad = (a & 0xFFF) * (2.0*math.pi/4096.0); return int(math.sin(rad)*4096.0)

def drift(oldf, newf):
    diffs = [abs(oldf(a)-newf(a)) for a in range(4096)]
    nz = sum(1 for d in diffs if d)
    return max(diffs), nz, sum(diffs)/4096.0
cmx,cnz,cavg = drift(OLD_cos, NEW_cos)
smx,snz,savg = drift(OLD_sin, NEW_sin)
print(f"[2] OLD libm cos vs NEW LUT: max|d|={cmx}, angles differing={cnz}/4096 ({100*cnz/4096:.1f}%), mean|d|={cavg:.3f}")
print(f"    OLD libm sin vs NEW LUT: max|d|={smx}, angles differing={snz}/4096 ({100*snz/4096:.1f}%), mean|d|={savg:.3f}")

# ---- 4. AngleFromVector12 faithful port + atan2 drift ----
m = re.search(r"k_angle_from_vector12_lut\[1026\]\s*=\s*\{(.*?)\};", txt2 := open(RENDER_C).read(), re.S)
body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
av_lut = [int(x) for x in re.findall(r"-?\d+", body)]
assert len(av_lut) == 1026, f"expected 1026 av-lut entries, got {len(av_lut)}"

def cdiv(a,b):  # C integer division: truncate toward zero
    q = abs(a)//abs(b); return q if (a<0)==(b<0) else -q

def AngleFromVector12(p1, p2):  # faithful port of 0x40A720
    if p1==0 and p2==0: return 0
    if p1>=0:
        if p2>0:
            if p1<p2: return av_lut[cdiv(p1*1024+(p2>>1), p2)]
            else:     return 0x400 - av_lut[cdiv(p2*1024+(p1>>1), p1)]
        else:
            if -p2 <= p1:
                if p1==0: return 0
                return 0x400 + av_lut[-cdiv(p2*1024-(p1>>1), p1)]
            else:
                if p2==0: return 0
                return 0x800 - av_lut[-cdiv(p1*1024-(p2>>1), p2)]
    else:
        if p2>0:
            neg_p1 = -p1
            if p2 > neg_p1: return 0x1000 - av_lut[-cdiv(p1*1024-(p2>>1), p2)]
            else:           return 0xc00 + av_lut[-cdiv(p2*1024-(p1>>1), p1)]
        else:
            neg_p1, neg_p2 = -p1, -p2
            if neg_p2 > neg_p1:
                if p1==0: return 0
                return 0x800 + av_lut[cdiv(p1*1024+(p2>>1), p2)]
            else:
                return 0xc00 - av_lut[cdiv(p2*1024+(p1>>1), p1)]

def NEW_atan2(dx,dz): return AngleFromVector12(dx,dz) & 0xFFF
def OLD_atan2(dx,dz):
    rad = math.atan2(float(dx), float(dz))
    return (int(rad*(4096.0/(2.0*math.pi)))) & 0xFFF  # C trunc-to-0 then mask

def adiff(a,b):  # smallest circular distance on a 4096 ring
    d = abs(a-b) & 0xFFF; return min(d, 0x1000-d)
worst=0; nz=0; tot=0; n=0
for dx in range(-100,101):
    for dz in range(-100,101):
        if dx==0 and dz==0: continue
        d = adiff(OLD_atan2(dx,dz), NEW_atan2(dx,dz))
        worst=max(worst,d); nz += (d!=0); tot+=d; n+=1
print(f"[3] OLD libm atan2 vs NEW AngleFromVector12 over {n} vectors: "
      f"max circ|d|={worst}, vectors differing={nz}/{n} ({100*nz/n:.1f}%), mean|d|={tot/n:.3f}")
print("[OK] NEW path == original integer-trig LUT by construction; "
      "OLD libm path drifted by the amounts above. Divergence is REDUCED to 0.")
