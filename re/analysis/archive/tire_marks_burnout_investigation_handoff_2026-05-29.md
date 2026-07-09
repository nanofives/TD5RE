# Tire Marks / Burnout Smoke — Investigation Handoff (2026-05-29)

**Branch:** `wave3-chain-c-cascade-investigation`
**Merge commit:** `d7ba202` (merge of worktree branch `fix-1780007606-189662-5471`, itself
committed as `c42e3ba`). Main tree **builds OK** (td5re.exe ~2,001,735 bytes). **NOT pushed.**
**Worktree** `.claude/worktrees/fix-1780007606-189662-5471` left in place (not torn down).

---

## 1. The bug (user symptoms)

"Tire marks on the road" never appeared in the port. Over the session the user also
reported (in order, as fixes landed): car froze at launch; marks through walls; marks too
small / faded too fast; no rear-wheel smoke; no launch marks; "accelerating during
countdown does nothing"; smoke looked weird; marks **not continuous**; marks **too faint /
not dark enough**; and finally marks look **"dented" (not a smooth curved line)** — which
is the one item left UNSOLVED.

---

## 2. ROOT CAUSE (the core finding) — slip-circle scf-bit→axle operand swap

The port reads actor `+0x31C` (front_axle_slip_excess) and `+0x320` (rear_axle_slip_excess)
to gate tire-track emitter acquisition. They were **always 0**, so the rolling-skid
emitter-acquire path never fired → no marks.

They were 0 because the slip-circle in `td5_physics_update_player`
(`UpdatePlayerVehicleDynamics @ 0x00404030`) had the **scf-bit→axle operands swapped**:

- **Orig** (`[CONFIRMED @ 0x404aed / 0x404c3d]`):
  - store `+0x31C` ← gated `scf & 1`, **LONGITUDINAL** slip (reads `+0x314`), **REAR** axle
    force pair + REAR wheel grip. Fires on RWD launch wheelspin.
  - store `+0x320` ← gated `scf & 2`, **LATERAL** slip (reads `+0x318`), **FRONT** axle
    force pair + FRONT wheel grip.
  - (Ghidra's struct names front/rear are INVERTED vs actual usage — go by offset.)
  - scf value at set = drivetrain (`tuning+0x76`): RWD=1=bit0, FWD=2=bit1, AWD=3.
- **Port (was):** FRONT-axle block gated `scf & 1` (td5_physics.c ~:2127), REAR-axle block
  gated `scf & 2` (~:2171) — **opposite**. So on a RWD car (scf=0x1) the port ran the
  FRONT grip-reduction (front is undriven → tiny force) and SKIPPED the REAR → rear never
  saturated → `r_comb < r_lim` → `rear_axle_slip_excess = 0`. Confirmed by the in-code
  `SLIPCIRC` log (`r_ex=0`).

**Fix:** swapped the force/grip operands so the longitudinal/`scf&1`/`+0x31C` block clamps
the REAR axle and the lateral/`scf&2`/`+0x320` block clamps the FRONT. scf gates, slip-axis
reads, and output fields were already correct — only the per-axle operands were wrong.

**Verified:** Frida capture of the ORIGINAL (track 5 / Viper, throttle held) showed `+0x31C
≈ 480–528` during launch with `+0x320 = 0`. After the port fix, the `SLIPCIRC` log shows
`f_ex = 480` at launch — **matches the original exactly.** Marks spawn.

---

## 3. All fixes applied (file / RE basis / what it fixed)

All in worktree branch, merged at `d7ba202`.

| # | Fix | File / location | RE basis | Symptom fixed |
|---|-----|-----------------|----------|---------------|
| 1 | Slip-circle operand swap (see §2) | `td5_physics.c` slip-circle block (~2103–2205) | `0x00404030`; stores `0x404aed`(+0x31C)/`0x404c3d`(+0x320) | No marks at all |
| 2 | Pass-B: clamp ONLY lateral force, not `*_long` (drive) | `td5_physics.c` both Pass-B clamps | orig Pass-B `@0x00404ad1` rescales only the lateral local `[EBP-0x10]`; the longitudinal local `[EBP-0x28]` is untouched to function exit | Car FROZE at launch (drive force clamped to ~0 when limit→0 on straight wheelspin) |
| 3 | Marks render as depth-tested ground decals | new `td5_render_submit_tire_mark()` using `TD5_PRESET_SHADOW` (z-test LEQUAL, z-write 0); rerouted in `td5_vfx.c` render | marks share opaque pass's linear `project_vertex` depth (unlike old smoke NDC issue) | Marks drew THROUGH walls |
| 4 | Fade gate `(lifetime & 8)==0` not `(control & 0x08)` | `td5_vfx.c` `td5_vfx_render_tire_tracks` fade block | `@0x43F265` `TEST CL,0x8` on lifetime counter (~half-rate fade) | Marks faded ~2× too fast |
| 5 | Smoke projected via `td5_render_transform_and_project` (world coords) | `td5_vfx.c` `td5_vfx_draw_particles`; added focal getters `td5_render_get_focal_length/center_x/center_y` to `td5_render.c/.h` | old path used focal `width*1.207` (~2.15× the renderer's `width*0.5625`) AND opposite X sign → off-screen/mirrored | No rear-wheel smoke (it spawned 466×/drive but projected off-screen) |
| 6 | Strip stitch: new segment trail ← prior segment lead | `td5_vfx.c` realloc block | orig `0x0043EE10` chains segments edge-to-edge | Marks not continuous (one-tick gap per segment → dotted) |
| 7 | Mark color = dark decal (black RGB, α=intensity×3) | `td5_vfx.c` render color pack | orig packs gray=intensity α=0xFF, **MODULATE** (darkens road) `@0x43F2A0` [UNCERTAIN blend] | Marks too faint / light-gray (was opaque `0xFF373737`) |
| 8 | `perp0` trail-edge perp (smooth trapezoid strip) | `td5_vfx.c` struct (+0x14/+0x18, reused pad), realloc, seed, render | orig stores explicit strip vertices → shared join edges | Attempt at "dented" — **did NOT visibly help** |
| 9 | Y-lift +24 world units on mark corners | `td5_vfx.c` render `wpos` build | orig lifts marks above road (`DAT_0045d6ac`) to avoid z-fight | Attempt at "dented" (z-fight under the new z-test) — **untested by user / inconclusive** |

---

## 4. Verified NOT bugs (left unchanged)

- **Countdown burnout.** Frida-confirmed: the orig does **NOT** call
  `UpdatePlayerVehicleDynamics @ 0x00404030` during the countdown (`preMoveCalls=1`, 0 calls
  while paused/`sim_tick==0`). Cross-confirmed by the orig paused branch `@0x00406881`
  skipping the dynamics dispatch (`404030/404EC0/403D90`). The port's `!g_game_paused` gate
  at `td5_physics.c:~1106` is **faithful**. The orig only revs the engine + sets the scf
  latch via the paused branch (no movement, no full slip dynamics) pre-GO. → "accelerating
  during countdown does nothing" is CORRECT orig behavior. DO NOT change.
- **Mark width `0x1A` (26).** Correct — orig `RenderTireTrackPool` computes perp =
  `width*cos12>>0xc` / `width*sin12>>0xc` (no extra width shift).
- **`SpawnRearWheelSmokeEffects @ 0x00401330`** is NOT the burnout smoke (fires only on
  surface_type 0x0a/0x0c). The real burnout/exhaust smoke is the tire-effects path
  (`UpdateRear/FrontTireEffects` 0x43F7E0/0x43F960, rate `rand()%50 < slip/2`).

---

## 5. STILL UNSOLVED — "dented marks"

User: *"tire marks even though they are continuous, they look dented, they don't look like
they are one line curved, more like dented lines."* Then after the perp0 fix: *"not fixed,
tire marks look the same."*

State: marks ARE spawning, continuous, dark, and depth-occluded; smoke IS visible. The
remaining problem is purely the **shape/quality** of the mark strip — it reads as
faceted/notched ("dented") rather than a smooth curved line.

**Attempts that did NOT resolve it:**
- Fix #8 (`perp0` shared join-edge / trapezoid strip) — user said "looks the same", so the
  visible dents are NOT (only) the per-segment join-perp mismatch.
- Fix #9 (Y-lift +24 to avoid z-fight under the new z-tested SHADOW preset) — built but the
  user hasn't confirmed; **z-fighting remains a live hypothesis** (the through-walls fix #3
  put marks coplanar with the road under a depth test; the earlier rewrite had REMOVED the
  Y-lift when it used a z-OFF preset, so coplanar + z-test → per-pixel z-fight → speckled
  "dented" fill).

**Hypotheses still open for the dented look (next session):**
1. **Z-fighting** (strongest, untested by user): marks under `TD5_PRESET_SHADOW` (z-test
   LEQUAL) are ~coplanar with the road. Fix #9 lifts +24 world-Y; if still dented, increase
   the lift, or give the SHADOW preset a slope-scaled depth bias (cf. the vehicle-shadow
   z-fighting fix `reference_shadow_polygon_offset_2026-05-26`).
2. **Perp direction precision/lag.** `perp = (-dz*w)/len` uses the *cumulative* motion
   (trail→current) seeded once and frozen; integer division at low speed quantizes the
   direction → jagged edges. The seed also uses the OLD slot's `dx/dz` (computed before
   realloc) → one-segment direction lag.
3. **Center+perp model is fundamentally lossy.** The port replaced the orig's explicit
   strip-vertex arrays with center+perp (overflow fix). A faithful re-port of
   `RenderTireTrackPool @ 0x0043F210` storing actual strip vertices would reproduce the
   orig's smooth shared-vertex strip exactly. This is the most thorough fix.
4. **Two parallel strips (wheels 2 & 3) close together** could read as a "dented band."

**Recommended next step:** get a CLEAR view first. 640×480 captured from behind the car was
inconclusive (`tools/capture_window.ps1 -Proc td5re -Out <png>`; marks are behind/under the
car). Bump `[Display] Width/Height`, or capture during a hard side-slide where the curved
trail is visible to the side, before guessing further. Then test the z-fight hypothesis
(it's the cheapest and most likely).

---

## 6. Pending before this can go to master

1. **Strip the investigation diagnostics** (committed on wave3, log-spammy):
   - `td5_vfx.c`: `dispatch ENTER` (in `td5_vfx_update_tire_track_emitters`), `ACQUIRE`
     (in `vfx_acquire_tire_track_emitter`), and the strip-builder desc-dump (in
     `td5_vfx_update_tire_tracks`, the `tire tracks update: ... descs[0..5]...` line).
   - `tire diag` line in `td5_vfx_render_tire_tracks`.
   - `rear_tire:` diag in `vfx_update_rear_tire_effects` (added by the earlier rewrite).
   - `SLIPCIRC` in `td5_physics.c` (~:2207) is useful; keep or gate it.
   - Search the diff for these and remove/`#ifdef` before a clean master merge.
2. **Run the mandatory multi-track handling sweep.** The slip-circle change (Fix #1/#2) now
   scales the REAR drive force on RWD wheelspin (Pass-A), which feeds yaw/velocity
   integration — it can shift the "drifts too early" feel (that bug was tuned around the
   OLD, buggy slip-circle: `reference_grip_load_weight_swap_2026-05-26`). Sweep
   Moscow/Newcastle/random and compare before master.
3. **Push wave3** when finalized (currently local only).
4. Resolve the "dented marks" (§5).

---

## 7. Key RE address reference

| Address | Function / item |
|---------|-----------------|
| `0x00404030` | `UpdatePlayerVehicleDynamics` (player slip-circle / drive / scf) |
| `0x00404aed` | store `[actor+0x31C]` (REAR/longitudinal slip-excess), EDX = `force_mag − demand` |
| `0x00404c3d` | store `[actor+0x320]` (FRONT/lateral slip-excess) |
| `0x00404ad1` | Pass-B rescale — orig clamps ONLY the lateral force `[EBP-0x10]` |
| `0x004049de` / `0x00404b2b` | slip-axis reads: `+0x314` (long) / `+0x318` (lat) |
| `0x0043EB50` | `UpdateTireTrackEmitters` (strip builder; realloc/stitch `0x43EE10`, AngleFromVector12 `0x43EC18` = (dx,dz)) |
| `0x0043F210` | `RenderTireTrackPool` (quad geometry, lifetime expire >600, fade-start >300 / `0x43F265` `&8`, color pack `0x43F2A0`) |
| `0x0043F7E0` / `0x0043F960` | `UpdateRear/FrontTireEffects` (acquire on `scf&bit`; burnout smoke `rand%50<slip/2`) |
| `0x0043F420` / `0x0043F600` | `UpdateFront/RearWheelSoundEffects` (rolling-skid acquire on slip-excess ≥ `0x3A99`/`0x2711`) |
| `0x00401330` | `SpawnRearWheelSmokeEffects` (surface 0x0a/0x0c only — NOT burnout) |
| `0x00406650` / `0x00406881` | `UpdateVehicleActor` + paused branch (skips dynamics dispatch during pause/countdown) |
| `0x0042B580` | `RunRaceFrame` (countdown sub-tick loop; no GO gate, gated only by sim-active + pause/replay) |

Actor fields: `+0x314` long_speed (ground), `+0x318` lateral_speed, `+0x31C` rear/long
slip-excess, `+0x320` front/lat slip-excess, `+0x33C` current_slip_metric (slip), `+0x364`
gear, `+0x370` surface_type_chassis, `+0x371..0x374` per-wheel emitter-id (0xFF sentinel),
`+0x375` slot_index, `+0x376` surface_contact_flags (scf), `+0x379` vehicle_mode, `+0x37C`
damage-lockout, `+0x33E` throttle. Tuning (`actor+0x1BC`): `+0x28`/`+0x2a` weight, `+0x2c`
tire-grip coeff, `+0x76` drivetrain, `+0x7c` slip-coupling.

---

## 8. How to continue

- Build (worktree or main): `cd td5mod/src/td5re && ./build_standalone.bat <pid>`
- Manual drive: set `[Trace] AutoThrottle=0`, launch `td5re.exe` (Newcastle = `[Game]
  DefaultTrack=5`). AutoThrottle=1 forces a launch burnout (deterministic marks).
- Frida orig capture: `python re/tools/quickrace/td5_quickrace.py --trace --trace-auto-exit
  --set race.track=5 --set race.car=0 ... --trace-max-frames 300 --max-runtime 90` (CSV has
  `front_slip`/`rear_slip`/`surface_flags` columns; labels use Ghidra's inverted names —
  read by offset). `frida_race_trace.js` has `AUTO_THROTTLE`.
- Memory: `memory/reference_tire_marks_slip_excess_never_written_2026-05-28.md`.
