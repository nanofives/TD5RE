# Test Drive 5 -- Decoded Data Tables

Cross-reference document for fifteen static data tables in TD5_d3d.exe.

---

## 1. Per-Track Checkpoint Metadata (0x46CF6C)

### Addressing

The pointer table at `0x46CF6C` is indexed by **level ZIP number** (not pool index).
`LoadTrackRuntimeData(level_zip_id)` accesses `*(ptr*)(0x46CF6C + level_zip_id * 4)`.

### Pool-to-Level-ZIP Mapping (0x466D50)

`gScheduleToPoolIndex` at `0x466894` maps the frontend schedule index to a pool index (byte array, 20 entries). The pool-to-level-ZIP table at `0x466D50` (int32[19]) then maps pool index to the level%03d.zip number.

| Pool | Level ZIP | Type | Weather |
|------|-----------|------|---------|
| 0  | level001 | Circuit | Rain |
| 1  | level002 | Circuit | Clear |
| 2  | level003 | Circuit | Snow |
| 3  | level004 | Circuit | Clear |
| 4  | level005 | Circuit | Clear |
| 5  | level006 | Circuit | Clear |
| 6  | level013 | Circuit | Clear (fog) |
| 7  | level014 | Circuit | Clear (fog) |
| 8  | level015 | Circuit | Clear |
| 9  | level016 | Circuit | Clear |
| 10 | level017 | Circuit | Clear (fog) |
| 11 | level023 | Circuit | Rain (fog) |
| 12 | level025 | P2P | Clear (fog) |
| 13 | level026 | P2P | Clear (fog) |
| 14 | level027 | P2P | Clear |
| 15 | level028 | P2P | Clear |
| 16 | level029 | P2P | Clear (fog) |
| 17 | level037 | P2P | Clear |
| 18 | level039 | P2P | Clear (fog) |

### Checkpoint Structure (24 bytes per track)

```c
struct CheckpointData {
    uint8_t  count;        // +0x00  Number of checkpoint stages (1-7)
    uint8_t  pad;          // +0x01
    uint16_t initial_time; // +0x02  Starting timer (ticks at 30fps, Hard difficulty)
    struct {
        uint16_t span_threshold; // Span index the actor must reach
        uint16_t time_bonus;     // Timer ticks added on reaching checkpoint
    } checkpoints[count];        // +0x04  Variable-length array
};
// Note: Last checkpoint always has time_bonus = 0 (finish line)
// Difficulty scaling: Easy = 1.2x, Normal = 1.1x, Hard = 1.0x (raw values)
```

### Full Checkpoint Table (all 18 used tracks)

Times shown as raw ticks. Divide by 30 for seconds at Hard difficulty.

#### Tier 1 Circuits (Pool 0-5)

**Pool 0 / level001 (Circuit, Rain)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 25,659 (0x643B) | ~855s |
| CP0 | span 869, bonus 15,360 | +512s |
| CP1 | span 1511, bonus 11,520 | +384s |
| CP2 | span 2061, bonus 15,360 | +512s |
| CP3 | span 2618, bonus 10,240 | +341s |
| CP4 | span 3074, bonus 0 | finish |

**Pool 1 / level002 (Circuit, Clear)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 17,979 (0x463B) | ~599s |
| CP0 | span 826, bonus 11,520 | +384s |
| CP1 | span 1429, bonus 5,120 | +171s |
| CP2 | span 1652, bonus 7,680 | +256s |
| CP3 | span 1926, bonus 15,360 | +512s |
| CP4 | span 2516, bonus 0 | finish |

**Pool 2 / level003 (Circuit, Snow)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 20,544 (0x503B) | ~685s |
| CP0 | span 768, bonus 17,920 | +597s |
| CP1 | span 1379, bonus 16,640 | +555s |
| CP2 | span 2090, bonus 16,640 | +555s |
| CP3 | span 2776, bonus 11,520 | +384s |
| CP4 | span 3221, bonus 0 | finish |

**Pool 3 / level004 (Circuit, Clear)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 17,979 (0x463B) | ~599s |
| CP0 | span 623, bonus 12,800 | +427s |
| CP1 | span 1175, bonus 15,360 | +512s |
| CP2 | span 1751, bonus 8,960 | +299s |
| CP3 | span 2181, bonus 8,960 | +299s |
| CP4 | span 2552, bonus 0 | finish |

**Pool 4 / level005 (Circuit, Clear)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 17,963 (0x463B) | ~599s |
| CP0 | span 747, bonus 7,680 | +256s |
| CP1 | span 1006, bonus 12,800 | +427s |
| CP2 | span 1533, bonus 14,080 | +469s |
| CP3 | span 1978, bonus 17,920 | +597s |
| CP4 | span 2754, bonus 0 | finish |

**Pool 5 / level006 (Circuit, Clear)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 16,699 (0x413B) | ~557s |
| CP0 | span 609, bonus 10,240 | +341s |
| CP1 | span 1029, bonus 10,240 | +341s |
| CP2 | span 1560, bonus 12,800 | +427s |
| CP3 | span 2140, bonus 16,640 | +555s |
| CP4 | span 2567, bonus 0 | finish |

#### Tier 2 Circuits (Pool 6-11)

**Pool 6 / level013 (Circuit, Clear, Fog)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 17,964 (0x463B) | ~599s |
| CP0 | span 556, bonus 17,920 | +597s |
| CP1 | span 1113, bonus 14,080 | +469s |
| CP2 | span 1663, bonus 14,080 | +469s |
| CP3 | span 2305, bonus 23,040 | +768s |
| CP4 | span 3060, bonus 0 | finish |

**Pool 7 / level014 (Circuit, Clear, Fog)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 20,539 (0x503B) | ~685s |
| CP0 | span 715, bonus 8,960 | +299s |
| CP1 | span 989, bonus 8,960 | +299s |
| CP2 | span 1212, bonus 14,080 | +469s |
| CP3 | span 1815, bonus 12,800 | +427s |
| CP4 | span 2508, bonus 0 | finish |

**Pool 8 / level015 (Circuit, Clear)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 15,420 (0x3C3B) | ~514s |
| CP0 | span 585, bonus 19,200 | +640s |
| CP1 | span 1271, bonus 20,480 | +683s |
| CP2 | span 1982, bonus 17,920 | +597s |
| CP3 | span 2593, bonus 17,920 | +597s |
| CP4 | span 3282, bonus 0 | finish |

**Pool 9 / level016 (Circuit, Clear)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 15,410 (0x3C3B) | ~514s |
| CP0 | span 466, bonus 8,960 | +299s |
| CP1 | span 896, bonus 12,800 | +427s |
| CP2 | span 1472, bonus 11,520 | +384s |
| CP3 | span 2024, bonus 12,800 | +427s |
| CP4 | span 2528, bonus 0 | finish |

**Pool 10 / level017 (Circuit, Clear, Fog)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 21,819 (0x553B) | ~727s |
| CP0 | span 901, bonus 10,240 | +341s |
| CP1 | span 1346, bonus 11,520 | +384s |
| CP2 | span 1873, bonus 7,680 | +256s |
| CP3 | span 2132, bonus 14,080 | +469s |
| CP4 | span 2755, bonus 0 | finish |

**Pool 11 / level023 (Circuit, Rain, Fog)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 15,419 (0x3C3B) | ~514s |
| CP0 | span 519, bonus 15,360 | +512s |
| CP1 | span 1099, bonus 11,520 | +384s |
| CP2 | span 1630, bonus 7,680 | +256s |
| CP3 | span 2050, bonus 8,960 | +299s |
| CP4 | span 2523, bonus 0 | finish |

#### Tier 3 Point-to-Point (Pool 12-18)

**Pool 12 / level025 (P2P, Clear, Fog)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 17,963 (0x463B) | ~599s |
| CP0 | span 651, bonus 14,080 | +469s |
| CP1 | span 1128, bonus 12,800 | +427s |
| CP2 | span 1599, bonus 14,080 | +469s |
| CP3 | span 2115, bonus 15,360 | +512s |
| CP4 | span 2606, bonus 0 | finish |

**Pool 13 / level026 (P2P, Clear, Fog)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 17,979 (0x463B) | ~599s |
| CP0 | span 660, bonus 15,360 | +512s |
| CP1 | span 1297, bonus 11,520 | +384s |
| CP2 | span 1840, bonus 12,800 | +427s |
| CP3 | span 2193, bonus 10,240 | +341s |
| CP4 | span 2601, bonus 11,520 | +384s |

**Pool 14 / level027 (P2P, Clear)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 10,299 (0x283B) | ~343s |
| CP0 | span 486, bonus 10,240 | +341s |
| CP1 | span 1057, bonus 8,960 | +299s |
| CP2 | span 1653, bonus 7,680 | +256s |
| CP3 | span 2167, bonus 10,240 | +341s |
| CP4 | span 2658, bonus 0 | finish |

**Pool 15 / level028 (P2P, Clear)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 17,972 (0x463B) | ~599s |
| CP0 | span 660, bonus 15,360 | +512s |
| CP1 | span 1297, bonus 11,520 | +384s |
| CP2 | span 1808, bonus 12,800 | +427s |
| CP3 | span 2337, bonus 10,240 | +341s |
| CP4 | span 2610, bonus 11,520 | +384s |

**Pool 16 / level029 (P2P, Clear, Fog)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 23,099 (0x5A3B) | ~770s |
| CP0 | span 827, bonus 12,800 | +427s |
| CP1 | span 1266, bonus 12,800 | +427s |
| CP2 | span 1662, bonus 19,200 | +640s |
| CP3 | span 2423, bonus 15,360 | +512s |
| CP4 | span 2989, bonus 0 | finish |

**Pool 17 / level037 (P2P, Clear)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 17,926 (0x463B) | ~598s |
| CP0 | span 738, bonus 7,680 | +256s |
| CP1 | span 1116, bonus 14,080 | +469s |
| CP2 | span 1739, bonus 11,520 | +384s |
| CP3 | span 2297, bonus 12,800 | +427s |
| CP4 | span 2924, bonus 0 | finish |

**Pool 18 / level039 (P2P, Clear, Fog)**

| Field | Value | Seconds (Hard) |
|-------|-------|-----------------|
| count | 5 | |
| initial_time | 16,699 (0x413B) | ~557s |
| CP0 | span 606, bonus 15,360 | +512s |
| CP1 | span 1122, bonus 12,800 | +427s |
| CP2 | span 1593, bonus 16,640 | +555s |
| CP3 | span 2070, bonus 15,360 | +512s |
| CP4 | span 2610, bonus 0 | finish |

### Drag Strip Checkpoint (level030, also at Pool special)

```
Count=1, initial_time=0x783B (30,779 ticks = ~1026s), CP0: span 204, bonus 0
```
This is used by drag race mode -- a single finish line checkpoint.

### Circuit Tracks (levels 7-12 unused gaps, levels 13-23 range)

The checkpoint pointer table at 0x46CF6C has entries for level ZIP IDs 7-12 and 19-22 that are not referenced by any pool index. These contain default/template data (count=5, initial_time varies, generic spans). They appear to be placeholder data for cut or unreleased tracks.

---

## 2. Span Type Suspension Response Table (0x46318C)

12-byte table: one byte per span type (0-11). Controls whether full suspension correction is applied.

```c
uint8_t gSpanTypeSuspensionFlag[12]; // at VA 0x46318C
```

| Span Type | Flag | Name | Suspension |
|-----------|------|------|------------|
| 0 | 1 | Straight | ENABLED -- full gravity-compensated vertical force |
| 1 | 1 | Gentle curve | ENABLED |
| 2 | 1 | Sharp curve | ENABLED |
| 3 | 0 | Chicane/S-curve | DISABLED -- bouncy handling |
| 4 | 1 | Crest/hill | ENABLED |
| 5 | 0 | Dip/valley | DISABLED -- bouncy handling |
| 6 | 0 | Crossover | DISABLED |
| 7 | 0 | Junction entry | DISABLED |
| 8 | 1 | Branch (fork) | ENABLED |
| 9 | 0 | Sentinel (start) | DISABLED |
| 10 | 0 | Sentinel (end) | DISABLED |
| 11 | 0 | Reserved | DISABLED |

Consumer: `IntegrateVehiclePoseAndContacts` (0x405E80). When flag=0, suspension correction is skipped, causing the vehicle to float/bounce on that span type.

---

## 3. Per-Car Balance Table (0x466F90)

8 entries x 12 bytes (3 x int32), indexed by car selection index (0-7). Applied in `InitializeRaceSession` (0x42AA10) and consumed by `InitializeRaceVehicleRuntime` (0x42F420). Only affects **player-controlled** slots. The sort order table at 0x466FC0 confirms max index = 7; offset 96 (what would be index 8) overlaps with the `cars\day.zip` string at 0x466FF0.

```c
struct CarBalanceEntry {
    int32_t torque_shape;   // Scales upper-gear torque curve points
    int32_t top_speed_mod;  // Scales rev limiter / max speed
    int32_t traction_mod;   // Scales base grip factor
};
// All values are signed offsets from baseline (0 = no change)
// Positive = stronger, negative = weaker (relative to AI reference physics)
```

| Index | torque_shape | top_speed_mod | traction_mod | Interpretation |
|-------|-------------|---------------|--------------|----------------|
| 0 | 0 | 0 | 0 | Baseline (no adjustment) |
| 1 | +114 (0x72) | -102 (0xFF9A) | -40 (0xFFD8) | More torque, slower top speed, less grip |
| 2 | -40 (0xFFD8) | +307 (0x0133) | -40 (0xFFD8) | Less torque, much faster top speed, less grip |
| 3 | -40 (0xFFD8) | -102 (0xFF9A) | +114 (0x0072) | Less torque, slower, much more grip |
| 4 | 0 | +1 | +6 | Near-baseline (negligible adjustment) |
| 5 | +7 | +2 | +4 | Slight all-around buff |
| 6 | +3 | +1 | +4 |  Slight buff |
| 7 | +2 | +5 | 0 | Slight speed buff |

### Sort Order Table (0x466FC0)

Immediately following the balance table at `0x466FC0` is a 10-entry int32 sort table used by the car selection screen for display ordering (maps display position to balance index):

```
{0, 1, 6, 7, 2, 4, 3, 1, 4, 2, 5, 0}
```
Note: 12 dwords present at this address; the final entries may include padding or wrap-around data.

### Application Details

- **Torque shaping**: Scales gear torque points from 3rd gear upward; linear weighting (top gear = full effect, lower gears = proportionally less)
- **Top speed**: Scales `carparam +0x74` by `(modifier + 0x800) >> 11`; also adjusts top-gear thresholds and low-speed grip inversely
- **Traction**: Adjusts base grip at `carparam +0x2C`; if positive also scales traction accumulator upward

Indices 0-3 represent the four "extreme" cars (large adjustments), while indices 4-7 are tightly clustered near baseline. This reflects the game's design: most cars perform similarly relative to AI, but a few outliers (very fast or very grippy) get rebalanced.

---

## 4. AI Rubber-Band Difficulty Presets

The rubber-band system in `ComputeAIRubberBandThrottle` (0x432D60) uses four parameters stored at `0x473D9C`:

```c
int32_t rb_behind_scale;     // 0x473D9C -- throttle boost when AI is behind player
int32_t rb_ahead_scale;      // 0x473DA0 -- throttle cut when AI is ahead of player
int32_t rb_behind_max_dist;  // 0x473DA4 -- max distance for behind scaling
int32_t rb_ahead_max_dist;   // 0x473DA8 -- max distance for ahead scaling
```

### Algorithm

```
span_delta = AI_span - player_span
if (delta < 0):  // AI behind player
    clamped = max(delta, -rb_behind_max_dist)
    adjust = rb_behind_scale * clamped / rb_behind_max_dist
else:             // AI ahead of player
    clamped = min(delta, rb_ahead_max_dist)
    adjust = rb_ahead_scale * clamped / rb_ahead_max_dist
throttle = 0x100 - adjust   // 0x100 = baseline (no rubber-banding)
```

When AI is **behind**, `adjust` is negative -> throttle > 0x100 (speed boost).
When AI is **ahead**, `adjust` is positive -> throttle < 0x100 (speed cut).

### Preset Table (12 configurations)

All presets are set in `InitializeRaceActorRuntime` (0x432E60) based on track type, traffic, and tier.

| # | Track Type | Traffic | Tier | behind_scale | ahead_scale | behind_max | ahead_max | Notes |
|---|-----------|---------|------|-------------|-------------|------------|-----------|-------|
| 0 | Circuit | Off | 0 | 0x8C (140) | 0xC8 (200) | 0x64 (100) | 0x37 (55) | Gentle catch-up, strong slow-down |
| 1 | Circuit | Off | 1 | 0x96 (150) | 0xC0 (192) | 0x64 (100) | 0x40 (64) | Balanced |
| 2 | Circuit | Off | 2 | 0xC8 (200) | 0x78 (120) | 0x64 (100) | 0x40 (64) | Strong catch-up, mild slow-down |
| 3 | P2P | Off | 0 | 0xA0 (160) | 0x96 (150) | 0x64 (100) | 0x50 (80) | Moderate symmetric |
| 4 | P2P | Off | 1 | 0xC8 (200) | 0xC0 (192) | 0x4B (75) | 0x4B (75) | Strong symmetric |
| 5 | P2P | Off | 2 | 0x10E (270) | 0x96 (150) | 0x41 (65) | 0x50 (80) | Very aggressive catch-up |
| 6 | P2P | On | 0 | 0xB4 (180) | 0xBE (190) | 0x4B (75) | 0x64 (100) | Traffic: wider distance window |
| 7 | P2P | On | 1 | 0xC8 (200) | 0xBE (190) | 0x3C (60) | 0x64 (100) | Traffic: tight behind window |
| 8 | P2P | On | 2 | 0xDC (220) | 0x64 (100) | 0x3C (60) | 0x40 (64) | Traffic: aggressive catch-up |
| 9 | Drag | any | any | 0x8C (140) | 0xC0 (192) | 0x64 (100) | 0x40 (64) | Drag race: moderate |
| 10 | Time Trial | - | - | 0x00 | 0x00 | 0x40 (64) | 0x40 (64) | DISABLED (no AI) |
| 11 | Default | - | - | 0x80 (128) | 0x80 (128) | 0x80 (128) | 0x80 (128) | Baseline (from 0x473D64) |

### Observations

- **Tier escalation**: Higher tiers have larger `behind_scale` values, making AI catch up more aggressively. This ensures races stay competitive at harder difficulties.
- **Circuit vs P2P**: Point-to-point tracks generally have tighter distance windows (`behind_max` / `ahead_max`), creating more intense rubber-banding in shorter races.
- **Traffic**: Traffic races have wider `ahead_max` windows (100 spans vs 55-80), giving AI more room to fall behind without aggressive catch-up -- traffic itself provides enough disruption.
- **Drag race**: Uses the same preset as Circuit Tier 0, Circuit Tier 1 (moderate catch-up).
- **Difficulty modifiers**: Before rubber-band presets are applied, the AI's base physics are also scaled by difficulty (Easy/Normal/Hard multipliers on torque, traction, max speed, and brake threshold).

---

## 5. Surface Friction and Damping Tables

### Surface Friction Table (0x4748C0)

16-entry `short[]` (uint16) indexed by surface type. Values in 8.8 fixed-point (0x100 = 1.0 = full grip).

| Index | Hex | Decimal | Fraction | Surface Name | Effect |
|-------|-----|---------|----------|-------------|--------|
| 0 | 0x0100 | 256 | 1.000 | Asphalt (standard) | Full grip |
| 1 | 0x0100 | 256 | 1.000 | Asphalt (variant) | Full grip |
| 2 | 0x00DC | 220 | 0.859 | Concrete | Reduced grip |
| 3 | 0x00F0 | 240 | 0.938 | Bridge deck | Slightly reduced |
| 4 | 0x00FC | 252 | 0.984 | Highway | Near-full grip |
| 5 | 0x00C0 | 192 | 0.750 | Gravel | Major grip loss |
| 6 | 0x00B4 | 180 | 0.703 | Grass | Worst grip |
| 7 | 0x0100 | 256 | 1.000 | Tunnel | Full grip |
| 8 | 0x0100 | 256 | 1.000 | Reserved A | Full grip |
| 9 | 0x0100 | 256 | 1.000 | Reserved B | Full grip |
| 10 | 0x00C8 | 200 | 0.781 | Dirt | Moderate grip loss |
| 11 | 0x0100 | 256 | 1.000 | Default C | Full grip |
| 12 | 0x0100 | 256 | 1.000 | Default D | Full grip |
| 13 | 0x0100 | 256 | 1.000 | Default E | Full grip |
| 14 | 0x0100 | 256 | 1.000 | Default F | Full grip |
| 15 | 0x0100 | 256 | 1.000 | Default G | Full grip |

### Surface Damping Table (0x474900)

16-entry `short[]` (uint16) indexed by surface type. Additive velocity damping (deceleration per tick):

| Index | Hex | Decimal | Surface Name | Effect |
|-------|-----|---------|-------------|--------|
| 0 | 0x0000 | 0 | Asphalt (standard) | No drag |
| 1 | 0x0000 | 0 | Asphalt (variant) | No drag |
| 2 | 0x0000 | 0 | Concrete | No drag |
| 3 | 0x0000 | 0 | Bridge deck | No drag |
| 4 | 0x0000 | 0 | Highway | No drag |
| 5 | 0x0002 | 2 | Gravel | Slight deceleration |
| 6 | 0x0000 | 0 | Grass | No drag (slippery, not slow) |
| 7 | 0x0000 | 0 | Tunnel | No drag |
| 8 | 0x0000 | 0 | Reserved A | No drag |
| 9 | 0x0000 | 0 | Reserved B | No drag |
| 10 | 0x0008 | 8 | Dirt | Significant deceleration |
| 11 | 0x0000 | 0 | Default C | No drag |
| 12 | 0x0000 | 0 | Default D | No drag |
| 13 | 0x0000 | 0 | Default E | No drag |
| 14 | 0x0000 | 0 | Default F | No drag |
| 15 | 0x0000 | 0 | Default G | No drag |

### Surface Behavior Summary

| Surface | Friction | Damping | Net Effect |
|---------|----------|---------|------------|
| Asphalt | 100% | 0 | Best overall -- full grip, no drag |
| Highway | 98.4% | 0 | Near-perfect (fast straights) |
| Bridge | 93.8% | 0 | Slightly slippery but no drag |
| Concrete | 85.9% | 0 | Noticeable grip loss |
| Dirt | 78.1% | 8 | Moderate grip loss + major speed drag |
| Gravel | 75.0% | 2 | Poor grip + slight speed drag |
| Grass | 70.3% | 0 | WORST grip but NO speed drag |
| Tunnel | 100% | 0 | Same as asphalt (visual-only variant) |

**Design note:** Grass is the slipperiest surface but does not slow you down, while dirt provides slightly more grip but decelerates significantly. This creates a trade-off: cutting across grass is fast but uncontrollable, while going through dirt is slower but more manageable.

---

## 6. Cheat Code Key Sequences (0x4654A4)

6 sequences at stride 0x28 (40 bytes), 0xFF-terminated. Typed on the **Options screen** in the frontend. Each sequence is an array of DirectInput keyboard scan codes.

### DirectInput Scan Code Mapping

| Code | Key | | Code | Key | | Code | Key |
|------|-----|-|------|-----|-|------|-----|
| 0x11 | W | | 0x1E | A | | 0x2E | C |
| 0x12 | E | | 0x1F | S | | 0x2F | V |
| 0x13 | R | | 0x20 | D | | 0x30 | B |
| 0x14 | T | | 0x21 | F | | 0x31 | N |
| 0x15 | Y | | 0x22 | G | | 0x32 | M |
| 0x16 | U | | 0x23 | H | | 0x39 | SPACE |
| 0x17 | I | | 0x24 | J | | 0x25 | K |
| 0x18 | O | | 0x26 | L | | | |
| 0x19 | P | | | | | | |

### Decoded Cheat Sequences

**Cheat 0: "I HAVE THE KEY"**
```
Raw:  17 39 23 1E 2F 12 39 14 23 12 39 25 12 15 FF
Keys: I  _  H  A  V  E  _  T  H  E  _  K  E  Y
```
- Target: `0x496298` XOR 1
- Effect: Unlock flag A (combine with cheat 2 to unlock all cars)

**Cheat 1: "CUP OF CHOICE"**
```
Raw:  2E 16 19 39 18 21 39 2E 23 18 17 2E 12 FF
Keys: C  U  P  _  O  F  _  C  H  O  I  C  E
```
- Target: `0x4962A8` XOR 8
- Effect: Unlock all cup tiers (Championship through Ultimate)

**Cheat 2: "I CARRY A BADGE"**
```
Raw:  17 39 2E 1E 13 13 15 39 1E 39 30 1E 20 22 12 FF
Keys: I  _  C  A  R  R  Y  _  A  _  B  A  D  G  E
```
- Target: `0x4962AC` XOR 2
- Effect: Unlock flag B (combine with cheat 0 to unlock all cars)

**Cheat 3: "LONE CRUSADER IN A DANGEROUS WORLD"**
```
Raw:  26 18 31 12 39 2E 13 16 1F 1E 20 12 13 39 17 31
      39 1E 39 20 1E 31 22 12 13 18 16 1F 39 11 18 13
      26 20 FF
Keys: L  O  N  E  _  C  R  U  S  A  D  E  R  _  I  N
      _  A  _  D  A  N  G  E  R  O  U  S  _  W  O  R
      L  D
```
- Target: `0x4AAF7C` XOR 1
- Effect: Nitro boost -- horn button doubles speed + vertical launch (0x6400 Y velocity), 15-frame cooldown

**Cheat 4: "REMOTE BRAKING"**
```
Raw:  13 12 32 18 14 12 39 30 13 1E 25 17 31 22 FF
Keys: R  E  M  O  T  E  _  B  R  A  K  I  N  G
```
- Target: `0x49629C` XOR 1
- Effect: Horn button freezes all opponents (zeroes velocity/speed of non-player actors)

**Cheat 5: "THAT TAKES ME BACK"**
```
Raw:  14 23 1E 14 39 14 1E 25 12 1F 39 32 12 39 30 1E
      2E 25 FF
Keys: T  H  A  T  _  T  A  K  E  S  _  M  E  _  B  A
      C  K
```
- Target: `0x4962B4` XOR 1
- Effect: Modifies NPC racer group unlock bits (likely unlocks retro/classic car variants)

### Notes

- All cheats are **toggles** (XOR operation) -- entering the same code again deactivates it
- Sound feedback: `DXSound::Play(4)` on activation, `DXSound::Play(5)` on deactivation
- Cheats are only accepted when the current frontend screen is `ScreenOptionsHub`
- Cheat 3 (nitro) is the longest at 34 characters; cheat 4 (remote braking) is the shortest at 14
- The space character (0x39) acts as a word separator in all sequences
- Cheats 0 + 2 must BOTH be active to unlock all cars in the car selection screen

---

## 7. AI Physics Template (0x473DB0)

128-byte template block copied to each AI actor's physics slot by `InitializeRaceActorRuntime` (0x432E60). Structure is `short[64]` (pairs of uint16 values). After copying, individual fields are scaled by difficulty and tier.

### Template Layout

```c
// Offset within 128-byte template (short index -> byte offset)
struct AIPhysicsTemplate {
    // Gear torque curve: 16 entries (offsets +0x00 to +0x1E)
    short gear_torque[16];    // 0xA0,0xC0,0xD8,0xE0,0xE4,0xE8,0xEC,0xF0,
                              // 0xF4,0xF8,0xFC,0x100,0x100,0x100,0x100,0x100
    // Drivetrain parameters (offsets +0x20 to +0x2E)
    int32_t param_20;        // 0x0002BF20 (180000)
    int32_t param_24;        // 0x00005DC0 (24000)
    short drag_coeff;        // +0x2C: 0x0190 (400)
    short param_2E;          // +0x2E: 0x0190 (400)
    // Speed/threshold fields (offsets +0x30 to +0x4E)
    short top_speed_raw;     // +0x30: 0x0DAC (3500)
    short param_32;          // +0x32: 0x0AF0 (2800)
    short pad_34;            // +0x34: 0x0000
    short gear_thresholds[7]; // +0x36: 0x09C4,0x073A,0x055A,0x03F2,0x02EE,0x0226
    short param_42;          // +0x42: 0x270F (9999)
    short pad_44;            // +0x44: 0x0000
    short brake_thresholds[6]; // +0x46: 0x1964..0x1964 (6500 x5), 0x270F, 0x270F
    // Brake/engine fields
    short pad_52;            // +0x52: 0x0000
    short pad_54;            // +0x54: 0x0000
    short surface_damping[5]; // +0x56: 0x10CC x5
    short param_60;          // +0x60: 0x0032 (50)
    short param_62;          // +0x62: 0x0028 (40)
    short param_64;          // +0x64: 0x001E (30)
    short param_66;          // +0x66: 0x2000 (8192)
    // Key scaled fields
    short drive_torque_mult; // +0x68: 0x0018 (24) -- torque: scaled by tier
    short param_6A;          // +0x6A: 0x0064 (100)
    short param_6C;          // +0x6C: 0x012C (300)
    short brake_force;       // +0x6E: 0x0BB8 (3000) -- scaled on Hard
    short engine_brake;      // +0x70: 0x0258 (600) -- scaled on Hard
    short param_72;          // +0x72: 0x0258 (600)
    short param_74;          // +0x74: 0x1CE8 (7400)
    short param_76;          // +0x76: 0x042A (1066)
    short param_78;          // +0x78: 0x0003
    short param_7A;          // +0x7A: 0x0050 (80)
    short param_7C;          // +0x7C: 0x00A0 (160)
    short param_7E;          // +0x7E: 0x00A0 (160)
};
```

### Key Fields and Scaling

| Byte Offset | Template Value | Scaled By | Description |
|------------|---------------|-----------|-------------|
| +0x2C | 0x190 (400) | Tier drag scale | AI drag coefficient -- higher = more drag |
| +0x68 | 0x18 (24) | Tier torque scale | AI drive torque multiplier |
| +0x6E | 0xBB8 (3000) | Hard only: 1.76x | Brake force |
| +0x70 | 0x258 (600) | Hard only: 1.56x | Engine brake (off-throttle decel) |

---

## 8. Per-Tier AI Physics Scaling

After copying the AI physics template, `InitializeRaceActorRuntime` applies two layers of scaling.

### Layer 1: Global Difficulty

| Difficulty | Torque Scale (+0x68) | Drag Scale (+0x2C) | Brake (+0x6E) | Engine Brake (+0x70) |
|-----------|---------------------|-------------------|---------------|---------------------|
| Easy | 1.00x (no change) | 1.00x | 1.00x | 1.00x |
| Normal | 1.41x (0x168/256) | 1.17x (300/256) | 1.00x | 1.00x |
| Hard | 2.54x (0x28A/256) | 1.49x (0x17C/256) | 1.76x (0x1C2/256) | 1.56x (400/256) |

### Layer 2: Track Type / Traffic / Tier

Applied on top of Layer 1. Scales torque (+0x68) and drag (+0x2C) fields.

| Track Type | Traffic | Tier | Torque Scale | Drag Scale | Max Speed Override (+0x74/+0x70) |
|-----------|---------|------|-------------|-----------|----------------------------------|
| Circuit | -- | 0 | 0x91/256 = 0.57x | 200/256 = 0.78x | brake_thresh=0x3A1 |
| Circuit | -- | 1 | 0xA0/256 = 0.63x | 0xEC/256 = 0.92x | brake_thresh=0x3A1 |
| Circuit | -- | 2 | 0xC3/256 = 0.76x | 0x104/256 = 1.02x | brake_thresh=0x433 |
| P2P | No | 0 | 0xAA/256 = 0.66x | 0x100/256 = 1.00x | brake_thresh=0x3A1 |
| P2P | No | 1 | 0xB4/256 = 0.70x | 0x100/256 = 1.00x | brake_thresh=0x3A1 |
| P2P | No | 2 | 0xDC/256 = 0.86x | 0x10E/256 = 1.05x | brake_thresh=0x433 |
| P2P | Yes | 0 | 0xB4/256 = 0.70x | 0x100/256 = 1.00x | brake_thresh=0x3A1 |
| P2P | Yes | 1 | 0xBE/256 = 0.74x | 0x10E/256 = 1.05x | brake_thresh=0x3B9 |
| P2P | Yes | 2 | 0xDC/256 = 0.86x | 0x122/256 = 1.13x | brake_thresh=0x433 |
| Drag | -- | -- | 0x91/256 = 0.57x | 0xB9/256 = 0.72x | brake_thresh=0x3A1 |

All scaling uses the pattern `(value * scale + rounding) >> 8`.

### Design Intent

The two-layer system means:
- **Easy Tier 0 Circuit AI**: torque = base * 1.00 * 0.57 = 0.57x baseline -- very slow
- **Hard Tier 2 Circuit AI**: torque = base * 2.54 * 0.76 = 1.93x baseline -- nearly 2x
- **Hard Tier 2 P2P+Traffic AI**: torque = base * 2.54 * 0.86 = 2.18x baseline -- fastest possible

This combined with the rubber-band system (Section 4) creates the difficulty curve: lower tiers have weak AI that rubber-bands gently, higher tiers have strong AI that rubber-bands aggressively.

---

## 9. NPC Racer Group Table (0x4643B8)

26 groups x 164 bytes (0xA4 stride), total 4264 bytes. Persisted in Config.td5 at offset 0x03E1. Each group represents a set of high-score entries for a particular cup race opponent bracket.

### Record Layout

```c
struct NpcRacerGroup {          // 164 bytes (0xA4)
    uint8_t  group_type;        // +0x00  0=Time, 1=Lap/Era, 2=Points, 4=TimeTrial-ms
    uint8_t  pad[3];            // +0x01  Alignment padding
    struct HighScoreEntry {     // 32 bytes (0x20) x 5 entries
        char     name[16];      // +0x00  Null-padded ASCII name
        uint32_t score;         // +0x10  Time (centiseconds), lap time, points, or ms
        uint32_t car_index;     // +0x14  Car index used by this NPC
        uint32_t avg_speed;     // +0x18  Average speed (display units)
        uint32_t top_speed;     // +0x1C  Top speed (display units)
    } entries[5];               // +0x04  Sorted best-to-worst
};
// Total: 4 + 5*32 = 164 bytes per group
```

### Group Type Semantics

| Type | Value | Score Unit | Display Format | Used By |
|------|-------|-----------|----------------|---------|
| 0 | Time | Centiseconds (MM:SS.cc) | `%2d:%2d.%2d` | Point-to-point races |
| 1 | Lap/Era | Centiseconds (MM:SS.cc) | `%2d:%2d.%2d` | Era cup (lap-based) |
| 2 | Points | Raw integer | `%d` | Championship / Ultimate scoring |
| 4 | TimeTrial | Milliseconds (MM:SS.mmm) | `%2d:%2d.%3d` | Time trial mode |

### Group Assignments

Groups are assigned to cup races via the Race-to-NPC-Group mapping table (Section 15).

| Group | Type | Header | Cup Usage | Top NPC | Score |
|-------|------|--------|-----------|---------|-------|
| 0 | Time (0) | Track | Championship R1 | Frank | 1:33.80 |
| 1 | Time (0) | Track | Championship R2 | George | 1:24.68 |
| 2 | Time (0) | Track | Championship R3 | Ted | 1:13.89 |
| 3 | Time (0) | Track | Championship R4 | Basil | 1:37.51 |
| 4 | Lap (1) | Era | Era R1 | Cevin | 0:39.61 |
| 5 | Lap (1) | Era | Era R5 | Sascha | 0:36.00 |
| 6 | Lap (1) | Era | Era R3 | Norman | 0:34.06 |
| 7 | Lap (1) | Era | Era R4 | Terry | 0:36.36 |
| 8 | Time (0) | Track | Challenge R6 | Ernie | 1:37.28 |
| 9 | Time (0) | Track | Ultimate R11 | Trent | 1:30.06 |
| 10 | Time (0) | Track | Masters R9 | Irene | 1:42.81 |
| 11 | Time (0) | Track | Pitbull R7 | Snake | 1:19.57 |
| 12 | Time (0) | Track | Masters R10 | Emma | 1:33.91 |
| 13 | Time (0) | Track | Pitbull R8 | Damon | 1:14.08 |
| 14 | Time (0) | Track | Ultimate R12 | Danielle | 1:12.32 |
| 15 | Time (0) | Track | Challenge R5 | Sarah | 1:12.69 |
| 16 | Lap (1) | Era | Era R2 | Slade | 0:36.09 |
| 17 | Lap (1) | Era | Era R6 | Craig | 0:37.39 |
| 18 | Lap (1) | Era | Bonus | Rob | 0:32.41 |
| 19 | TT (4) | TT | Time Trial | Tim | 6:32.000 |
| 20 | Pts (2) | Pts | Championship pts | Amanda | 96 |
| 21 | Time (0) | Track | Special long A | Vincent | 14:30.55 |
| 22 | Time (0) | Track | Special long B | John | 7:51.07 |
| 23 | Time (0) | Track | Special long C | James | 9:26.98 |
| 24 | Time (0) | Track | Special long D | Steve | 12:49.60 |
| 25 | Pts (2) | Pts | Ultimate pts | Nicole | 63236 |

### Cheat Code Interaction

When cheat 5 ("THAT TAKES ME BACK") is active (`0x4962B4 & 1`), the main loop iterates all 26 groups and sets bit 1 of `DAT_004A2C9C[group_index]` for groups with `group_type == 0`, or clears to bit 0. This marks track groups as "retro-completed."

### Display Function

`FUN_00413010` (0x413010) renders a group's 5 high-score entries. It reads:
- `group_type` to determine column headers (Time/Lap/Pts)
- `name[16]` rendered at x=0x10
- `score` formatted per type (switch on group_type: 0/1 = MM:SS.cc, 2 = integer, 4 = MM:SS.mmm)
- `car_index` mapped through `DAT_00463E24` to a display name from the car name array at `0x49B6CC` (stride 0xCC)
- `avg_speed` / `top_speed` formatted as MPH or KPH based on `DAT_00466028`

---

## 10. Car Lock Table (0x463E4C)

37-byte array. Each byte is a lock flag: 0 = unlocked, 1 = locked. Indexed by car index (0-36). Persisted in Config.td5 at offset 0x148E onward.

### Default Lock State

| Car Index | Lock | Notes |
|-----------|------|-------|
| 0-15 | 0 (unlocked) | Base game cars, available from start |
| 16-35 | 1 (locked) | Unlockable via cup completion or cheats |
| 36 | 0 (unlocked) | Special slot (always available) |

### Unlock Mechanism

Cars are unlocked by `AwardCupCompletionUnlocks` (0x421DA0) when the player finishes 1st in specific cup races. The unlock sets `DAT_00463E4C[car_index] = 0` and advances the max car counter at `DAT_00463E0C` if needed.

| NPC Group | Unlocked Car Index | Identity |
|-----------|-------------------|----------|
| 4 | 21 (0x15) | Special 2 |
| 5 | 17 (0x11) | Chevelle SS |
| 6 | 24 (0x18) | A-Type Concept |
| 7 | 25 (0x19) | Nissan 240SX |
| 16 | 23 (0x17) | Special 4 |
| 17 | 26 (0x1A) | Ford Mustang |
| 18 | 32 (0x20) | Cadillac Eldorado |

### Related Globals

| Address | Size | Description |
|---------|------|-------------|
| 0x463E0C | dword | Max unlocked car index (default: 0x17 = 23) |
| 0x4962B0 | dword | All-cars-unlocked flag (set when car 32 would overflow) |

### Car Index Remap Table (0x463E24)

40-byte lookup table mapping internal NPC car indices to display-order car indices. Used by the high-score display to resolve car names:

```
Index: 00 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19
Value: 07 02 17 33 22 31 32 34 18 14 01 15 13 09 11 05 00 35 08 03
Index: 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36
Value: 04 12 26 10 36 16 19 25 20 21 24 06 27 28 29 00 00
```

---

## 11. Track Lock Table (0x4668B0)

26-byte array. Each byte is a lock flag: 0 = unlocked, 1 = locked. Indexed by track pool index (0-25). The first 8 tracks are unlocked by default; the remaining 18 must be earned through cup progression.

### Default Lock State

| Track Index | Lock | Notes |
|-------------|------|-------|
| 0-7 | 0 (unlocked) | Starting tracks (available for Quick Race / Time Trial) |
| 8-25 | 1 (locked) | Unlocked by beating their corresponding NPC group |

### Unlock Mechanism

Track groups (those with `group_type == 0` in the NPC racer group table) unlock their associated track when `AwardCupCompletionUnlocks` sets `DAT_004A2C9C[group_index] = 1`. The track lock table at 0x4668B0 is checked by the track selection menu to determine which tracks are accessible.

The auto-demo feature (attract mode) also checks this table: it randomly picks a track from unlocked entries only (`(&DAT_004668B0)[track] != 0`).

---

## 12. Camera Preset Table (0x463098)

7 presets x 16 bytes, total 112 bytes. Accessed by the chase camera system in `FUN_00401450` (0x401450), indexed through `DAT_00482FD0[player_slot]` (the current camera mode per player).

### Record Layout

```c
struct CameraPreset {           // 16 bytes
    int16_t  height_offset;     // +0x00  Camera height above car (world units)
    int16_t  follow_distance;   // +0x02  Distance behind car (world units)
    int16_t  pitch_angle;       // +0x04  Camera look-down angle (in engine units)
    int16_t  fov_adjust;        // +0x06  Field of view adjustment
    uint32_t flags;             // +0x08  Camera behavior flags (0 for chase cams)
    uint32_t reserved;          // +0x0C  Unused (always 0)
};
```

### Preset Values

| Preset | Height | Distance | Pitch | FOV Adj | Flags | Description |
|--------|--------|----------|-------|---------|-------|-------------|
| 0 | 0 | 600 | 2100 | 510 | 0x00 | Far chase (widest view) |
| 1 | 0 | 550 | 1710 | 110 | 0x00 | Medium-far chase |
| 2 | 0 | 475 | 1500 | 310 | 0x00 | Medium chase |
| 3 | 0 | 400 | 1350 | 110 | 0x00 | Medium-close chase |
| 4 | 0 | 325 | 1200 | 240 | 0x00 | Close chase |
| 5 | 0 | 240 | 1550 | 110 | 0x00 | Very close chase |
| 6 | 1 | 0 | 0 | 0 | 0xFF38 | Bumper / hood cam (flag-driven) |

### Camera Initialization

Global `DAT_00463090` = 0x28 (40) is loaded as the base camera responsiveness. On preset change, the function copies follow_distance, pitch_angle, FOV, and two dwords at +0x08/+0x0C into per-player camera state arrays. A comparison against the previous preset's height field controls whether interpolation occurs (smooth transition) or the camera jumps instantly.

Preset 6 is special: `height_offset = 1` triggers a different code path (bumper cam), and `flags = 0xFF38` contains packed mode bits rather than chase-cam parameters. The zero distance/pitch/FOV confirms it bypasses the chase camera math entirely.

---

## 13. Force Feedback Terrain Coefficient Table (0x466AFC)

13-entry `int32[]` indexed by surface type byte from actor state at offset `+0x370`. Used exclusively by `FUN_004288E0` (0x4288E0) to scale force feedback vibration intensity per terrain.

### Values

| Index | Surface | Coefficient | Effect |
|-------|---------|-------------|--------|
| 0 | Asphalt (smooth) | 1 | Minimal vibration (effectively none) |
| 1 | Asphalt (rough) | 200 | Moderate road texture feedback |
| 2 | Concrete | 260 | Noticeable texture |
| 3 | Bridge deck | 180 | Moderate |
| 4 | Gravel | 100 | Light vibration |
| 5 | Ice / packed snow | 600 | Strongest vibration |
| 6 | Water / puddle | 50 | Very light splash feel |
| 7 | Rumble strip | 240 | Strong rumble (road edge) |
| 8 | Kerb (inner) | 400 | Heavy bump |
| 9 | Kerb (outer) | 400 | Heavy bump |
| 10 | Dirt | 200 | Moderate texture |
| 11 | Off-road | 360 | Strong uneven terrain |
| 12 | Grass | 190 | Moderate soft ground |

### Vibration Formula

```c
int steeringForce = abs(player_steering) * 2;  // clamped to 300,000
int baseMagnitude = (30 - steeringForce / 10000);
int vibration = baseMagnitude * terrainCoefficient[surfaceType];
```

The formula produces higher vibration when the player is steering gently (low force = high `baseMagnitude`), which means vibration is most noticeable on straights where steering input is minimal. Sharp turns suppress terrain feedback.

**Design note:** Ice/snow (600) produces 3x more feedback than asphalt (200), giving strong tactile cues that the surface is dangerous. Asphalt-smooth (1) is essentially zero feedback, representing a perfectly surfaced road.

---

## 14. Difficulty Track Pool Table (0x463E10)

3 tiers x 6 track indices, total 18 bytes. Used by `InitializeRaceSeriesSchedule` (0x40DAC0) for Strategy C (Championship, Challenge, Pitbull, Ultimate cups) to select random tracks appropriate to the current difficulty tier.

### Pool Contents

| Tier | Difficulty | Track Indices | Description |
|------|-----------|---------------|-------------|
| 0 | Easy | 10, 14, 7, 4, 11, 9 | Shorter / simpler circuits |
| 1 | Medium | 9, 6, 3, 13, 12, 0 | Mixed difficulty tracks |
| 2 | Hard | 22, 24, 16, 17, 19, 18 | Long / technical routes |

### Usage

For each empty race slot in the cup schedule, the game picks a random index (0-5) into the current tier's pool, ensuring no duplicates within the series. The track direction is randomized (0-3).

The Era cup (type 2) uses its own Strategy A (random from pool 0-7 or 8-15), and the Masters cup (type 5) uses Strategy B (pre-shuffled permutation). Only types 1, 3, 4, 6 use this table.

---

## 15. Race-to-NPC-Group Mapping Table (0x4640A4)

2D table indexed as `[gameType * 16 + raceIndex]`. Maps each race within a cup series to one of the 26 NPC racer groups. Sentinel value 0x63 (99) marks end of series.

### Decoded Mapping

```
Offset +0x00 (type 0, unused):  00 00 00 00 00 -- [padding for single race mode]
Offset +0x10 (type 1, Championship):  00  01  02  03  99
Offset +0x20 (type 2, Era):           04  16  06  07  05  17  99
Offset +0x30 (type 3, Challenge):     00  01  02  03  15  08  99
Offset +0x40 (type 4, Pitbull):       00  01  02  03  15  08  11  13  99
Offset +0x50 (type 5, Masters):       00  01  02  03  15  08  11  13  10  12  99
Offset +0x60 (type 6, Ultimate):      00  01  02  03  15  08  11  13  10  12  09  14  99
```

### Access Pattern

```c
// In ConfigureGameTypeFlags (0x410CA0):
int group = table[g_selectedGameType * 16 + g_raceWithinSeriesIndex + 0x14];
if (group == 99) {
    // Cup finished -- dispatch to end-of-cup handler
} else {
    DAT_00490BA8 = group;  // Set active NPC group for this race
}
```

### Cup Superset Property

Each progressive cup type is a strict superset of the previous:
- Championship: groups {0, 1, 2, 3}
- Challenge: + {15, 8}
- Pitbull: + {11, 13}
- Masters: + {10, 12}
- Ultimate: + {9, 14}

The Era cup uses its own distinct groups {4, 16, 6, 7, 5, 17} which are all Lap/Era type entries.

---

## Cross-Reference Index

### Table Dependencies

```
                  +--> [1] Checkpoint Table (per-track timing)
                  |
[Level ZIP ID] ---+--> [5] Surface Tables (per-surface friction/damping)
                  |         ^
                  |         |  (surface type from STRIP.DAT span records)
                  |         |
                  +--> Span Type -> [2] Suspension Response Flag
                  |
                  +--> [13] FF Terrain Coefficients (force feedback vibration)

[Car Selection] --+--> [3] Per-Car Balance (torque/speed/traction offset)
                  |
                  +--> [10] Car Lock Table (which cars are accessible)
                  |
                  +--> [7] AI Physics Template (copied per AI slot)
                           |
                           +--> [8] Per-Tier Scaling (difficulty x tier matrix)
                           |
                           +--> [4] Rubber-Band Presets (throttle bias per tick)

[Cup Selection] --+--> [15] Race-to-NPC-Group Mapping (which opponents per race)
                  |         |
                  |         +--> [9] NPC Racer Group Table (high scores + car assignments)
                  |
                  +--> [14] Difficulty Track Pool (track selection per tier)
                  |
                  +--> [11] Track Lock Table (which tracks are accessible)

[Camera Mode] ----+--> [12] Camera Preset Table (chase cam parameters)

[Frontend Input] ---> [6] Cheat Codes (unlock flags, toggle effects)
```

### Key Address Map

| Address | Table | Size | Entry Size | Count | Indexing |
|---------|-------|------|-----------|-------|---------|
| 0x46CF6C | Checkpoint pointers | 76B | 4B ptr | 19 | Level ZIP ID (1-18 valid) |
| 0x46CBB0-CF68 | Checkpoint records | 440B | 24B | 18+extras | Pointed to by 0x46CF6C |
| 0x46318C | Suspension flags | 12B | 1B | 12 | Span type (0-11) |
| 0x466F90 | Car balance | 96B | 12B | 8 | Car selection index (0-7) |
| 0x466FC0 | Car sort order | 48B | 4B | 12 | Display position |
| 0x473D9C | Rubber-band params | 16B | 4B | 4 | Single set (written per race) |
| 0x473DB0 | AI physics template | 128B | -- | 1 | Copied to each AI slot |
| 0x473D64 | Default throttle | 24B | 4B | 6 | Per AI slot |
| 0x4748C0 | Surface friction | 32B | 2B | 16 | Surface type byte |
| 0x474900 | Surface damping | 32B | 2B | 16 | Surface type byte |
| 0x4654A4 | Cheat sequences | 240B | 40B | 6 | Cheat index (0-5) |
| 0x4643B8 | NPC racer groups | 4264B | 164B | 26 | Group index (0-25) |
| 0x463E4C | Car lock flags | 37B | 1B | 37 | Car index (0-36) |
| 0x4668B0 | Track lock flags | 26B | 1B | 26 | Track pool index (0-25) |
| 0x463098 | Camera presets | 112B | 16B | 7 | Camera mode (0-6) |
| 0x466AFC | FF terrain coeffs | 52B | 4B | 13 | Surface type byte |
| 0x463E10 | Difficulty track pool | 18B | 1B | 3x6 | [tier][slot] |
| 0x4640A4 | Race-to-group map | 112B | 1B | 7x16 | [gameType*16 + raceIdx] |
| 0x463E24 | Car index remap | 40B | 1B | 40 | Internal car idx -> display idx |
| 0x463E0C | Max unlocked car | 4B | 4B | 1 | Scalar (default 0x17) |

### Consumer Functions

| Function | Address | Consumes Tables |
|----------|---------|----------------|
| `LoadTrackRuntimeData` | 0x42FB90 | [1] Checkpoint |
| `IntegrateVehiclePoseAndContacts` | 0x405E80 | [2] Suspension flags |
| `InitializeRaceSession` | 0x42AA10 | [3] Car balance |
| `InitializeRaceActorRuntime` | 0x432E60 | [4] Rubber-band, [7] AI template, [8] Tier scaling |
| `ComputeAIRubberBandThrottle` | 0x432D60 | [4] Rubber-band params |
| `UpdatePlayerVehicleDynamics` | 0x404030 | [5] Friction + Damping |
| `UpdateAIVehicleDynamics` | 0x404EC0 | [5] Friction (single probe) |
| `RunFrontendDisplayLoop` | 0x414B50 | [6] Cheat codes, [9] NPC groups (cheat toggle) |
| `DrawHighScorePanel` | 0x413010 | [9] NPC racer groups (display) |
| `AwardCupCompletionUnlocks` | 0x421DA0 | [9] NPC groups, [10] Car locks |
| `WritePackedConfigTd5` | 0x40F8D0 | [9] NPC groups, [10] Car locks, [11] Track locks |
| `LoadPackedConfigTd5` | 0x40FB60 | [9] NPC groups, [10] Car locks, [11] Track locks |
| `SetCameraPreset` | 0x401450 | [12] Camera presets |
| `UpdateForceFeedbackTerrain` | 0x4288E0 | [13] FF terrain coefficients |
| `InitializeRaceSeriesSchedule` | 0x40DAC0 | [14] Difficulty track pool |
| `ConfigureGameTypeFlags` | 0x410CA0 | [15] Race-to-NPC-group mapping |
