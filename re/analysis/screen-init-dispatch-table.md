# Screen Init Dispatch Table at 0x464104

Discovered via p-code analysis of `CALL dword ptr [ECX*4 + 0x464104]` at
instruction 0x410EB4 inside `ConfigureGameTypeFlags` (0x410CA0).

---

## 1. Table Layout

The dispatch table lives at **0x00464104** in the `.data` section.
It is indexed by `g_gameType` (`DAT_0049635C`) with DWORD-sized entries:

| Index | Address      | Value        | Target Function          |
|-------|-------------|-------------|--------------------------|
|   0   | 0x00464104  | 0x00000063  | Sentinel (value 99) -- not a function pointer |
|   1   | 0x00464108  | 0x00410F60  | `ScreenInit_SingleRace`   |
|   2   | 0x0046410C  | 0x00410FA0  | `ScreenInit_Championship` |
|   3   | 0x00464110  | 0x00410FF0  | `ScreenInit_DragRace`     |
|   4   | 0x00464114  | 0x00411030  | `ScreenInit_CopChase`     |
|   5   | 0x00464118  | 0x00411070  | `ScreenInit_Practice`     |
|   6   | 0x0046411C  | 0x004110A0  | `ScreenInit_Multiplayer`  |

Only indices 1-6 are valid function pointers. Index 0 is the value 99 (0x63),
which doubles as the sentinel byte ending the GameType 6 track-series row in the
adjacent race-series track table (see Section 4). Game types 7-9 never reach
the dispatch (see Section 3).

---

## 2. Decompiled Target Functions

### Index 1 -- ScreenInit_SingleRace (0x00410F60)

```c
void ScreenInit_SingleRace(void)
{
    if (DAT_004668ba != '\0') {
        DAT_00494bb4 = 2;       // Reset series race counter to 2
    }
    DAT_004668bb = 0;           // Clear "single race active" flag
    DAT_004668ba = 0;           // Clear "single race configured" flag
    if ((0x11 < DAT_00466840) && ((DAT_004962a8 & 7) == 0)) {
        DAT_004962a8 |= 1;     // Set unlock bit 0 (track unlock flag)
    }
}
```

**Purpose:** Finalizes Single Race series completion. Resets single-race state
flags. If enough tracks are unlocked (DAT_00466840 > 0x11 = 17) and no unlock
bits are set yet, grants unlock bit 0 in the progression flags at 0x4962A8.

### Index 2 -- ScreenInit_Championship (0x00410FA0)

```c
void ScreenInit_Championship(void)
{
    if (DAT_004668c0 != '\0') {
        DAT_00494bb4 = 2;       // Reset series counter
    }
    if (DAT_00466840 < 0x12) {
        DAT_00466840 = 0x12;    // Ensure at least 18 tracks unlocked
        DAT_004668c1 = 0;
        DAT_004668c0 = '\0';
        if ((DAT_004668ba == '\0') && ((DAT_004962a8 & 7) == 0)) {
            DAT_004962a8 |= 1;  // Set unlock bit 0
        }
    }
}
```

**Purpose:** Finalizes Championship series completion. Ensures unlock count is
at least 18 (0x12). Conditionally grants unlock bit 0. Cross-references single
race state (DAT_004668ba) before unlocking.

### Index 3 -- ScreenInit_DragRace (0x00410FF0)

```c
void ScreenInit_DragRace(void)
{
    if (DAT_004668bc != '\0') {
        DAT_00494bb4 = 3;       // Reset series counter to 3
    }
    DAT_004668bd = 0;           // Clear drag race sub-flags
    DAT_004668bc = 0;
    DAT_004668be = 0;
    if (DAT_00463e5f == '\0') {
        DAT_004962a8 |= 2;     // Set unlock bit 1 (car unlock flag)
    }
}
```

**Purpose:** Finalizes Drag Race series completion. Clears three drag-race state
bytes. Grants unlock bit 1 in the progression flags (unlocks content gated
behind the car-unlock flag) unless cop-chase sub-flag is already set.

### Index 4 -- ScreenInit_CopChase (0x00411030)

```c
void ScreenInit_CopChase(void)
{
    if (DAT_00463e5c != '\0') {
        DAT_00494bb0 = 5;       // Reset series lap/round counter to 5
    }
    DAT_00463e5e = 0;           // Clear cop chase sub-flags (5 total)
    DAT_00463e5c = 0;
    DAT_00463e60 = 0;
    DAT_00463e5f = 0;
    DAT_00463e62 = 0;
    if (DAT_004668be == '\0') {
        DAT_004962a8 |= 2;     // Set unlock bit 1 (car unlock flag)
    }
}
```

**Purpose:** Finalizes Cop Chase series completion. Clears five state bytes.
Grants unlock bit 1 unless drag-race sub-flag is set. This is the counterpart
to ScreenInit_DragRace -- both gate unlock bit 1, creating a mutual dependency
(either completing Drag Race or Cop Chase can grant it, whichever finishes
while the other's flag is clear).

### Index 5 -- ScreenInit_Practice (0x00411070)

```c
void ScreenInit_Practice(void)
{
    if (DAT_004668b8 != '\0') {
        DAT_00494bb4 = 2;       // Reset series counter
    }
    DAT_004668b9 = 0;           // Clear practice flags
    DAT_004668b8 = 0;
}
```

**Purpose:** Simplest handler. Finalizes Practice series completion. Clears two
practice state bytes. No unlock progression -- Practice mode does not contribute
to unlockable content.

### Index 6 -- ScreenInit_Multiplayer (0x004110A0)

```c
void ScreenInit_Multiplayer(void)
{
    if (DAT_004668bf != '\0') {
        DAT_00494bb4 = 2;       // Reset series counter
    }
    if (DAT_00463e67 != '\0') {
        DAT_00494bb0 = 9;       // Reset to 9 (max series length)
    }
    DAT_004668c2 = 0;           // Clear multiplayer sub-flags (11 total)
    DAT_004668bf = 0;
    DAT_00463e68 = 0;
    DAT_00463e67 = 0;
    DAT_00463e6a = 0;
    DAT_00463e69 = 0;
    DAT_00463e6b = 0;
    DAT_00463e6e = 0;
    DAT_00463e6d = 0;
    DAT_00463e70 = 0;
    DAT_00463e6f = 0;
    DAT_00466840 = 0x13;        // Set unlock count to 19
    if (DAT_00463e0c < 0x25) {
        DAT_00463e0c = 0x25;    // Ensure at least 37 unlockable items
    }
}
```

**Purpose:** Most complex handler. Finalizes Multiplayer series completion.
Clears 11 state bytes. Forces track unlock count to 19 (0x13) and ensures the
global unlock counter is at least 37 (0x25). This is the "completion reward"
handler that unlocks the most content.

---

## 3. Dispatch Context in ConfigureGameTypeFlags (0x410CA0)

### When is the dispatch reached?

The dispatch is guarded by two conditions at the end of ConfigureGameTypeFlags:

```c
if (DAT_00490ba8 != 99) {
    return 1;                    // Normal path -- more races in series
}
if (DAT_0048d988._2_2_ == 0) {  // word at 0x0048D98A
    (**(code **)(&DAT_00464104 + DAT_0049635c * 4))();  // Dispatch!
}
return 0;
```

**DAT_00490ba8** is the current track index loaded from the race-series track
table. It equals 99 only when the sentinel byte (0x63) at the end of a series
is reached. This means: the dispatch fires when the player has **completed all
races in a series** for the current game type.

The secondary guard on `DAT_0048D98A` (word at 0x0048D98A) prevents the dispatch
if a certain HUD/display state is active.

Assembly at 0x410EB4:
```
CALL dword ptr [ECX*0x4 + 0x464104]
```

ECX holds `g_gameType` (DAT_0049635C) at this point.

### Index-to-GameType mapping

The index used for the dispatch is `g_gameType` (0x0049635C), which is the same
variable used throughout ConfigureGameTypeFlags' main switch statement. The
mapping to TD5_GameType enum values:

| Index | TD5_GameType Enum        | Dispatch Target              |
|-------|--------------------------|------------------------------|
|   1   | GAMETYPE_SINGLE_RACE     | ScreenInit_SingleRace        |
|   2   | GAMETYPE_CHAMPIONSHIP    | ScreenInit_Championship      |
|   3   | GAMETYPE_DRAG_RACE       | ScreenInit_DragRace          |
|   4   | GAMETYPE_COP_CHASE       | ScreenInit_CopChase          |
|   5   | GAMETYPE_PRACTICE        | ScreenInit_Practice          |
|   6   | GAMETYPE_MULTIPLAYER     | ScreenInit_Multiplayer       |
|   7   | GAMETYPE_TIME_TRIAL      | (never dispatched)           |
|   8   | GAMETYPE_SURVIVAL        | (never dispatched)           |
|   9   | GAMETYPE_SPEED_TRAP      | (never dispatched)           |

**Why types 7-9 never dispatch:** These game types set special boolean flags
(DAT_004aaf6c for Time Trial, DAT_004aaf68 for Survival, DAT_00494bac for
Speed Trap) that cause the race-series track table lookup to be skipped. Since
the lookup is skipped, DAT_00490ba8 is never set to 99, so the dispatch path
is never entered. Types 7-9 are non-series modes (single-event races) that
do not have progression.

### What determines g_gameType?

The variable `g_gameType` at 0x0049635C is written by the calling code before
invoking ConfigureGameTypeFlags. The three callers (see Section 5) each set it:

- At 0x417585: `MOV [0x0049635C], EAX` -- set from the race-type menu selection
- At 0x416CF8: `MOV [0x0049635C], EDX` -- computed as `EAX + 3` (cup category offset)
- At 0x416D00: `MOV [0x0049635C], 9` -- hardcoded to Speed Trap (type 9)

---

## 4. Adjacent Race-Series Track Table at 0x4640A8

Immediately preceding the dispatch table is a 2D byte array used as the
**race series track schedule**. It is accessed via the formula:

```c
DAT_00490ba8 = s_Steve_Snake_says___No_Cheating__00464084
                    [DAT_00494bb8 + DAT_0049635c * 0x10 + 0x14];
```

Base pointer: `0x464084 + 0x14 = 0x464098`. Each game type gets a 16-byte row
(stride 0x10) starting at `0x464098 + game_type * 0x10`. The column index
`DAT_00494bb8` is the race-in-series counter.

| GameType | Row Start  | Track Indices (decimal)                    | Series Length |
|----------|------------|-------------------------------------------|---------------|
| 1 (Single Race)  | 0x4640A8 | 0, 1, 2, 3, **99**                      | 4 races       |
| 2 (Championship) | 0x4640B8 | 0, 0, 0, 0, 0, 0, 0, 4, 16, 6, 7, 5, 17, **99** | 13 races |
| 3 (Drag Race)    | 0x4640C8 | 0, 1, 2, 3, 15, 8, **99**               | 6 races       |
| 4 (Cop Chase)    | 0x4640D8 | 0, 1, 2, 3, 15, 8, 11, 13, **99**       | 8 races       |
| 5 (Practice)     | 0x4640E8 | 0, 1, 2, 3, 15, 8, 11, 13, 10, 12, **99** | 10 races   |
| 6 (Multiplayer)  | 0x4640F8 | 0, 1, 2, 3, 15, 8, 11, 13, 10, 12, 9, 14, **99** | 12 races |

The sentinel value **99 (0x63)** at the end of each row is the trigger for the
dispatch table. When `DAT_00490ba8 == 99`, the player has finished all scheduled
races and the series-completion handler fires.

> **Note:** The 99 sentinel at GameType 6's row end (0x464104) overlaps exactly
> with index 0 of the function pointer dispatch table. This is deliberate --
> the same byte serves as both "end of track series" and the unused index-0 slot
> of the dispatch table.

---

## 5. Callers of ConfigureGameTypeFlags

Three call sites reference `ConfigureGameTypeFlags` at 0x410CA0:

| Call Site  | Context                                              |
|-----------|------------------------------------------------------|
| 0x00417596 | Race type category menu -- sets `g_gameType` from menu selection, `DAT_00494bb8` (series counter) from saved state |
| 0x00416D0A | Cup championship sub-menu -- computes `g_gameType = selection + 3` (mapping cup categories to types 3-9); special-cases type 9 (Speed Trap) |
| 0x00422F5D | Race results screen -- re-invokes ConfigureGameTypeFlags after race completion, potentially incrementing `DAT_00494bb8` (advancing to next race in series) |

The first two callers set up the game type before a series begins.
The third caller (race results) re-evaluates after each race to check whether
the series is complete.

---

## 6. Nearby Dispatch Tables

### Checked: 0x4640C0 region

The region 0x4640A8-0x464103 is **not** a dispatch table -- it is the race-series
track table documented in Section 4. The bytes are track indices (0-17) and
sentinel values (99), not code pointers.

### No additional dispatch tables found near 0x464104

The bytes immediately after the dispatch table at 0x464120 are the ASCII string
`"CupData.td5\0"` followed by `"Lock(1) fail"`. These are string literals, not
function pointers. The dispatch table is exactly 7 DWORDs (28 bytes, 0x464104-
0x46411F), with only indices 1-6 being valid code pointers.

---

## 7. Relationship to Main Screen Function Table at 0x4655C4

The two tables serve different purposes in the game's architecture:

| Aspect | Screen Function Table (0x4655C4) | Screen Init Dispatch (0x464104) |
|--------|----------------------------------|--------------------------------|
| **Size** | 30 entries | 7 entries (1 sentinel + 6 ptrs) |
| **Index variable** | `g_frontendScreenIndex` | `g_gameType` (0x0049635C) |
| **Called from** | Frontend main loop (RunMainGameLoop) | ConfigureGameTypeFlags only |
| **Purpose** | Drives the currently-active UI screen | Series-completion reward logic |
| **Functions** | Full screen state machines (100s of lines each) | Short init stubs (5-20 lines each) |
| **When called** | Every frame while frontend is active | Once, when a race series ends |

The screen function table at 0x4655C4 controls **which screen is displayed**
(main menu, car select, results, etc.). The init dispatch table at 0x464104
controls **what happens when a game mode's race series is completed** --
specifically, it resets mode-specific state flags and grants progression unlocks.

The init dispatch table is downstream of the screen table: the race results
screen (entry 24 at 0x422480 in the screen table) calls ConfigureGameTypeFlags,
which in turn may invoke the init dispatch when the final race of a series is
done. This links the two tables in the flow:

```
Screen Table [24] RunRaceResultsScreen
    -> ConfigureGameTypeFlags (0x410CA0)
        -> [if series complete] Init Dispatch Table [g_gameType]
            -> Unlock rewards, reset state
```

---

## 8. Global Variable Corrections

The analysis reveals that the `td5_enums.h` header has an incorrect address
for `g_selectedGameType`:

| Variable | Header Says | Actual Address | Notes |
|----------|------------|----------------|-------|
| `g_selectedGameType` | 0x004AAF6C | **0x0049635C** | 0x004AAF6C is actually a boolean flag (1 = Time Trial active) |

The variable at 0x004AAF6C (`DAT_004aaf6c`) is set to 1 only in case 7
(Time Trial) and conditionally reset to 0 for non-Time-Trial types. It is a
mode-active boolean, not the game type selector. The true game type variable
is at 0x0049635C and holds values 1-9 matching the TD5_GameType enum.

---

## 9. Unlock Progression Summary

The dispatch handlers reveal a coherent unlock system:

| Unlock Flag | Bit in DAT_004962A8 | Granted By |
|------------|---------------------|------------|
| Track unlock (bit 0) | `\| 1` | Single Race (if >17 tracks) or Championship |
| Car unlock (bit 1)   | `\| 2` | Drag Race or Cop Chase (mutual exclusion gate) |

- **Practice** mode: No unlocks (just resets state).
- **Multiplayer** mode: Forces track count to 19 and global unlock counter to >= 37, acting as a "complete unlock" reward.
- **Championship** mode: Also ensures track count >= 18, creating a progression ladder.
- **Drag Race / Cop Chase** cross-gate: Each checks the other's flag before granting bit 1, preventing double-granting but ensuring either path works.

The `DAT_00494bb4` / `DAT_00494bb0` resets (to 2, 3, 5, or 9) likely set the
series race counters back to their starting values for replay.
