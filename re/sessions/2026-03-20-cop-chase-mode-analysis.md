# Cop Chase Mode (Game Type 8) - Complete Analysis

## Overview

Cop Chase is a solo game mode where the player drives against 5 police cars on a point-to-point track. The objective is to reach the finish line while ramming cops to accumulate points. Each cop has a health bar; depleting it "destroys" the cop and awards bonus points. The player also has a health bar -- when depleted, their vehicle stops being AI-updated (effectively arrested).

**Runtime flag:** `gWantedModeEnabled` (0x4AAF68) = 1
**Mode selector:** `DAT_004aaf74` = 4
**Game type:** `g_selectedGameType` (0x49635C) = 8
**Menu label:** `SNK_CopChaseButTxt` (from LANGUAGE.DLL)

---

## Initialization (`ConfigureGameTypeFlags` 0x410CCB)

When game type 8 is selected:
```
gSpecialEncounterEnabled = 0    -- actor-9 encounter system DISABLED
gTrafficActorsEnabled = 1       -- ambient traffic still runs
gWantedModeEnabled = 1          -- enables wanted damage/scoring/HUD
DAT_004aaf74 = 4                -- mode-class identifier
DAT_004b0fa8 = 0                -- checkpoint timer disabled
```

On first race (g_raceWithinSeriesIndex == 0):
```
DAT_0048d998 = 0                -- cumulative score counter reset
player.gap_01f8[0xd0..0xd3] = 0 -- per-race point accumulator reset
DAT_0048d98c = 0                -- related stats reset
```

Key difference from normal races: `gSpecialEncounterEnabled = 0` means the actor-9 NPC pursuit encounter is disabled. Cops are the main 5 AI slots (1-5), not traffic.

---

## Actor Placement (`InitializeRaceSession` 0x42ACF2)

In wanted mode, actors are placed on the track with cops AHEAD of the player:

| Slot | Role   | Span Offset from Track End | Lane |
|------|--------|---------------------------|------|
| 0    | Player | end - 3                   | 2    |
| 1    | Cop 1  | end + 25 (0x19)           | 2    |
| 2    | Cop 2  | end + 50 (0x32)           | 3    |
| 3    | Cop 3  | end + 75 (0x4B)           | 3    |
| 4    | Cop 4  | end + 100 (0x64)          | 2    |
| 5    | Cop 5  | end + 125 (0x7D)          | 3    |

Cops are placed far ahead (25-125 spans), meaning the player approaches them from behind as they progress through the track. The player starts near the track end marker and drives forward.

**Vehicle model:** All 6 slots load from `gCarZipPathTable` indexed by `gSlotCarTypeIndex`. The cop car model is `cars\cop.zip` (index 5 in the car zip table).

**Post-init override:**
```c
gSpecialEncounterEnabled = 0;  // Redundant re-disable
DAT_004b0fa8 = 0;              // No checkpoint gate
```

---

## Health / Damage System

### Initialization (`InitializeWantedHudOverlays` 0x43D2D0)

All 6 actors get 0x1000 (4096) health each, stored as an array of shorts starting at `DAT_004bead4`:
```
DAT_004bead4[0] = 0x1000  (player, slot 0)
DAT_004bead4[1] = 0x1000  (cop 1, slot 1)
DAT_004bead4[2] = 0x1000  (cop 2, slot 2)
... through slot 5
```

Additional state initialized:
- `DAT_004bead0` = 0 (cop kill counter)
- `DAT_004bf504` = -1 (last-hit actor, none)
- `DAT_004bf500` = 0 (marker intensity)
- `DAT_004bf508` = 0 (wanted message index)
- `DAT_004bf50c` = 0xFFFF (message display timer, starts maxed so first message doesn't play)
- `DAT_004bf518` = 0 (damage processing enabled)

### Damage Award (`AwardWantedDamageScore` 0x43D690)

Called from `ApplyVehicleCollisionImpulse` (0x407B00) when:
1. `gWantedModeEnabled != 0`
2. One of the two colliding actors is the player (slot matching `DAT_004bf51c`, always 0)
3. The other actor is slot < 6 (a racer, not traffic)

**Collision force thresholds:**
- Force 10,000-20,000: `sVar4 = 0x200` (light hit, -512 health)
- Force > 20,000: `sVar4 = 0x400` (heavy hit, -1024 health)
- Force < 10,000: No damage (too weak)

**Damage processing:**
```
if (target_actor != last_hit_actor) {
    // First hit on a new target:
    // Pick random wanted message (0-7), reset display timer
    DAT_004bf508 = rand() & 7
    DAT_004bf504 = target_actor
    DAT_004bf50c = 0       // Reset message timer
    return  // No damage on first contact (grace period)
}

if (target.health > 0) {
    target.health -= damage_amount

    if (damage_amount == 0x200):
        player.score += 10     // Light hit: +10 points
    else:
        player.score += 20     // Heavy hit: +20 points

    if (target.health <= 0):
        player.score += 50     // Kill bonus: +50 points
        cop_kill_count++       // DAT_004bead0
        target.health = 0      // Clamp

    if (target.health > 0x1000):
        target.health = 0x1000 // Clamp max
}
```

**Score storage:** Points accumulate at `gRuntimeSlotActorTable.slot[player].gap_01f8 + 0xD0` (a 32-bit int).

### Health Depletion Effects

When a cop's health reaches 0:
- `DAT_004bead0` increments (cop kill counter)
- In `UpdateRaceActors` (0x436EFB): `UpdateActorTrackBehavior` is SKIPPED for actors whose health is 0:
  ```c
  if ((gWantedModeEnabled == 0) || (*actor_health != 0)) {
      UpdateActorTrackBehavior(actor);
  }
  ```
  This means destroyed cops lose their AI steering -- they coast/drift to a stop.

**Player health:** The player (slot 0) also has health at `DAT_004bead4[0]`. If reduced to 0, the player's AI track behavior would also be suppressed, but since the player uses direct input (not AI steering), the practical effect is that no more damage can be scored. The race still continues to the finish.

---

## HUD Display

### HUD Flag Configuration (`InitializeRaceOverlayResources` 0x437805)

For `DAT_004aaf74 == 4` (cop chase):
```
HUD flags = 0x04 | 0x10 | 0x40 | 0x20 | 0x08 | 0x80 | 0x100
```
- Bit 0x04: Speedometer dial
- Bit 0x08: Gear indicator
- Bit 0x10: Wrong-way indicator
- Bit 0x20: Additional display
- Bit 0x40: Lap/checkpoint metric
- Bit 0x80: Position/score display (primary)
- Bit 0x100: Score display (secondary, cop chase specific)

Notably ABSENT: Bit 0x01 (race position) and bit 0x02 (timing splits).

### Damage Indicator (`UpdateWantedDamageIndicator` 0x43D4E0)

When `gWantedModeEnabled != 0` and the currently rendered actor is `DAT_004bf504` (last-hit cop):
- Renders a DAMAGE sprite (background bar) positioned relative to the actor's 3D world position, projected to screen
- Renders a DAMAGEB1 sprite (foreground fill) whose height is proportional to remaining health:
  ```
  fill_height = (0x1000 - actor_health) * sprite_size * scale * 0.00024414063
  ```
  The bar fills top-to-bottom as damage accumulates.

### Wanted Messages (`DrawRaceStatusText` 0x439BC8)

When `gWantedModeEnabled != 0` and `DAT_004b0bf4 != 0` (English locale):
- A timer (`DAT_004bf50c`) counts from 0 to 300 frames
- During the first 270 frames (0x10E): message displays continuously
- Frames 270-300: message blinks (odd/even frame toggle)
- Two lines of text are shown from the message pair table:
  - Line 1: `PTR_s_...00474038[DAT_004bf508 * 2]` (header)
  - Line 2: `PTR_s_...0047403C[DAT_004bf508 * 2]` (crime)

### 8 Wanted Message Pairs (table at 0x474038)

| Index | Line 1                     | Line 2                        |
|-------|----------------------------|-------------------------------|
| 0     | "SUSPECT IS WANTED FOR"    | "ARMED ROBBERY."              |
| 1     | "SUSPECT IS WANTED FOR"    | "SPEEDING."                   |
| 2     | "SUSPECT IS ARMED"         | "AND DANGEROUS."              |
| 3     | "SUSPECT IS WANTED"        | "FOR 1ST DEGREE MURDER."      |
| 4     | "SUSPECT IS WANTED"        | "FOR ILLEGAL LICENSE PLATES."  |
| 5     | "SUSPECT IS WANTED"        | "FOR GRAND THEFT AUTO."       |
| 6     | "SUSPECT IS WANTED"        | "FOR CHICKEN PLUCKING."       |
| 7     | "SUSPECT IS WANTED"        | "FOR SOFTWARE PIRACY."        |

A random message is selected each time the player first contacts a new cop.

### Tracked Actor Marker (`RenderTrackedActorMarker` 0x43CDE6)

When `gWantedModeEnabled != 0`, `DAT_004bf500 != 0`, and the rendered actor matches `DAT_004bf51c`:
- A pulsing translucent 3D marker is rendered around the tracked cop
- Marker intensity (`DAT_004bf500`) range: 0-0x1000
- Boosted by 0x400 each frame via `AdvanceGlobalSkyRotation` (called from `UpdateVehicleLoopingAudioState`)
- Decayed by 0x200 each sim tick via `DecayTrackedActorMarkerIntensity`
- The marker's scale (size 512, offset 64, separation 6) is proportional to intensity
- Two rotating billboard quads with sine-wave alpha pulsing

---

## Cop AI Behavior

Cop actors (slots 1-5) use the **standard race AI** system:
- `UpdateActorTrackBehavior` (0x434FE0): Standard track-following, lane steering, route planning
- `UpdateActorRouteThresholdState` (0x434AA0): Three-state throttle controller (coast/brake/accelerate)
- `ComputeAIRubberBandThrottle` (0x432D60): Distance-based difficulty scaling

**Key difference from regular races:** Cops are initialized AHEAD of the player on the track. Since they follow the same route AI as normal racers, they drive forward along the track. The player must catch up to them and ram them.

**No special pursuit AI:** Unlike later games, cops do NOT actively chase or pursue the player. They follow the track route at a speed determined by the standard AI difficulty scaling. The challenge comes from the player needing to catch them while managing their own health.

**When destroyed (health = 0):** `UpdateActorTrackBehavior` is skipped. The cop loses steering/throttle input and coasts to a halt. Their vehicle remains on-track as an obstacle.

---

## Encounter System Interaction

The `UpdateSpecialTrafficEncounter` (0x434DA0) function handles the special actor-9 NPC encounter. In wanted mode:
- `gSpecialEncounterEnabled = 0`, so the encounter gate conditions are never met
- When encounter cleanup fires (`gSpecialEncounterTrackedActorHandle = -1`), the `StopTrackedVehicleAudio()` call is SUPPRESSED if `gWantedModeEnabled != 0`
- This prevents the cop siren audio from being stopped by encounter teardown

### Audio
When `gWantedModeEnabled != 0` and `param_1 == DAT_004bf51c` (player's audio update):
- A looping siren/alert audio channel is activated
- `gTrackedVehicleAudioActive = 1`, `gTrackedVehicleAudioFadeTarget = 0x1000`
- The siren persists for the duration of the race

---

## Scoring System

### Per-Hit Scoring
Points are awarded in `AwardWantedDamageScore` on each qualifying collision:

| Condition                           | Points |
|-------------------------------------|--------|
| Light collision (force 10K-20K)     | +10    |
| Heavy collision (force >20K)        | +20    |
| Cop health reduced to 0 (kill)      | +50    |

Maximum theoretical per-cop: ~90 points (multiple hits + kill bonus)

### Score Accumulator
- Per-race score: `gRuntimeSlotActorTable.slot[0].gap_01f8 + 0xD0` (32-bit int)
- Cop kill count: `DAT_004bead0` (32-bit int)
- Reset to 0 at start of each cop chase series

### Post-Race Results (`BuildRaceResultsTable` 0x40AA59)
For the wanted mode player (`DAT_004bf51c`):
```c
result_table[player].metric = DAT_004bead0;  // Cop kill count
```
The results screen shows "PUNKTE" (Points) column instead of time/laps, using `SNK_PtsTxt` label. Format: `%3d` (simple integer).

The `DrawRaceDataSummaryPanel` (0x42214B) formats two `%3d` score lines for game type 8.

### Speed Bonus
`AccumulateVehicleSpeedBonusScore` (0x40A3D0) runs for ALL actors in ALL modes, including cop chase. Speed bonus accumulates at `actor + 0x2C8` based on current speed, providing a secondary scoring dimension that feeds into the race time metric.

---

## Win / Lose Conditions

### Race End
The standard `CheckRaceCompletionState` (0x409E80) applies:
- Player must reach the end of the track (point-to-point) or complete circuit laps
- When the player's `companion_state_1` is set to 1, the race ends
- The post-finish cooldown timer (0x3FFFFF ticks) then fires `BuildRaceResultsTable`

### No "Busted" Game Over
There is **no explicit "busted" or arrest condition**. If the player's health reaches 0:
- Their AI track behavior is suppressed (no effect since player uses direct input)
- No further damage can be scored against cops by the player
- The race continues until the finish line or the standard 45-second replay timeout

### Cop Kills Are Not a Win Condition
Destroying all 5 cops does not end the race. The player must still reach the finish.

---

## "Remote Braking" Cheat (`DAT_0049629C`)

Found in `UpdatePlayerVehicleControlState` (0x402E60):

```c
if ((DAT_0049629c != 0) && ((control_bits & 0x200000) != 0)) {
    // When horn button (bit 0x200000) is pressed:
    for each actor != current_player:
        actor.path_vec_x = 0;  // Zero X velocity
        actor.path_vec_y = 0;  // Zero Y velocity
}
```

**Effect:** When enabled, pressing the horn/siren button instantly zeros ALL other vehicles' velocity vectors, causing them to stop dead. This applies to ALL vehicle types (cops, racers, traffic).

**Activation:** `DAT_0049629c` is a configuration flag stored in the game options. It's referenced from `ProcessFrontendNetworkMessages` and `RunFrontendNetworkLobby`, suggesting it's a multiplayer lobby option (or possibly a hidden cheat code toggled through the options menu). The flag defaults to 0 (disabled).

**Cop Chase Interaction:** In cop chase mode, Remote Braking is devastating -- the player can stop all 5 cops with the horn, then ram them while stationary for easy damage scoring. However, since the horn also triggers the siren sound in wanted mode, there's no separate "chase horn" action.

---

## Key Global Variables

| Address      | Name/Purpose                                    | Size  |
|-------------|-------------------------------------------------|-------|
| 0x4AAF68    | `gWantedModeEnabled` - master wanted mode flag   | dword |
| 0x4BEAD0    | Cop kill counter (increments on health=0)        | dword |
| 0x4BEAD4    | Health array (6 shorts, 0x1000 max each)         | 12B   |
| 0x4BF500    | Marker intensity (0-0x1000, pulsing)             | dword |
| 0x4BF504    | Last-hit actor index (-1 = none)                 | dword |
| 0x4BF508    | Random wanted message index (0-7)                | dword |
| 0x4BF50C    | Message display timer (0-300 frames)             | dword |
| 0x4BF510    | Unused (always 0)                                | dword |
| 0x4BF514    | Unused (always 0)                                | dword |
| 0x4BF518    | Damage processing disable flag (always 0)        | byte  |
| 0x4BF51C    | Player slot for wanted scoring (always 0)        | dword |
| 0x49629C    | Remote braking cheat flag                         | dword |

## Key Functions

| Address    | Name                             | Role                                      |
|-----------|----------------------------------|-------------------------------------------|
| 0x410CCB  | ConfigureGameTypeFlags            | Sets wanted mode flags for type 8          |
| 0x42ACF2  | InitializeRaceSession             | Places cops ahead, enables wanted          |
| 0x43D2D0  | InitializeWantedHudOverlays      | Inits health array, HUD sprites            |
| 0x43D4E0  | UpdateWantedDamageIndicator       | Renders per-cop health bar                 |
| 0x43D690  | AwardWantedDamageScore            | Core damage/scoring on collision           |
| 0x43D7E9  | DecayTrackedActorMarkerIntensity  | Marker fade-out per tick                   |
| 0x43D7C9  | AdvanceGlobalSkyRotation          | Marker fade-in per frame                   |
| 0x43CDE6  | RenderTrackedActorMarker          | 3D pulsing marker around tracked cop       |
| 0x439BC8  | DrawRaceStatusText                | Wanted message display                     |
| 0x40AA59  | BuildRaceResultsTable             | Stores cop kill count as result             |
| 0x42214B  | DrawRaceDataSummaryPanel          | Formats cop chase score display             |
| 0x437805  | InitializeRaceOverlayResources    | Sets HUD flags (0x80|0x100 for cop chase)  |
