# NPC Racer Group Table (gNpcRacerGroupTable)

> Address: `0x4643B8` | Size: 4264 bytes (0x10A8) | 26 groups x 164 bytes
> End address: `0x465460` (immediately followed by "%dKPH" format string)

## Structure (VERIFIED)

Each group is **0xA4 (164) bytes**: a 4-byte header + 5 sub-entries of 32 bytes each.

```c
struct NpcRacerEntry {            // 32 bytes (0x20)
    char     name[16];            // +0x00  NUL-terminated racer name
    uint32_t score;               // +0x10  best time (centiseconds/ms) or points
    uint32_t car_id;              // +0x14  external car index (into gExtCarIdToTypeIndex @ 0x463E24)
    uint32_t avg_speed;           // +0x18  average speed (integer, MPH or KPH per DAT_00466028)
    uint32_t top_speed;           // +0x1C  top speed (integer, MPH or KPH per DAT_00466028)
};

struct NpcRacerGroup {            // 164 bytes (0xA4)
    uint32_t         group_type;  // +0x00  scoring type (see below)
    NpcRacerEntry    entries[5];  // +0x04  top-5 high score entries (sorted best-first)
};

// gNpcRacerGroupTable: NpcRacerGroup[26] at 0x4643B8
// Total: 26 * 0xA4 = 4264 = 0x10A8 bytes
// Stride verification: disassembly at 0x41314D uses LEA+LEA+SHL pattern (index*5*8+index)*4 = index*164
// Save size verification: WritePackedConfigTd5 copies 0x42A dwords = 4264 bytes
```

**No missed fields.** The 4-byte header + 5x32-byte entries account for every byte. The "19 unknown header bytes" from the original open question was confirmed as a stride miscalculation artifact.

## Group Type Field

Decoded from `DrawPostRaceHighScoreEntry` (0x413080) switch statement and format strings at 0x465460-0x46549B:

| Value | Meaning | Score Format | Column Header | Score Semantics |
|-------|---------|-------------|---------------|-----------------|
| 0     | Race time | `%2.2d:%2.2d.%2.2d` (MM:SS.cc) | "Time" | Centiseconds, lower=better |
| 1     | Lap time | `%2.2d:%2.2d.%2.2d` (MM:SS.cc) | "Lap" | Centiseconds, lower=better |
| 2     | Points | `%d` (integer) | "Pts" | Points, higher=better |
| 4     | Time Trial (extended) | `%2.2d:%2.2d.%3.3d` (MM:SS.mmm) | "Time" | Milliseconds, lower=better |

Bit-mask semantics used in `TrackSelectionScreenStateMachine` (0x4276A0):
- `(group_type & 3) == 0` -- standalone race group (available in all modes: quick race, time trial, cop chase, drag)
- `(group_type & 3) != 0` -- cup/championship group (skipped in non-cup game modes 7-9)

## Score Field Interpretation

For types 0, 1 (time-based): value is total time in **centiseconds**.
- Example: Frank, group 0, score = 0x24A4 = 9380 cs = 1:33.80
- Qualifying: `player_time <= entries[4].score` (must beat 5th place)

For type 4 (Time Trial): value is total time in **milliseconds**.
- Example: Tim, group 19, score = 0x05FB40 = 392000 ms = 6:32.000
- Qualifying: `player_time <= entries[4].score`

For type 2 (Points): value is raw point total.
- Example: Amanda, group 20, score = 96 points
- Qualifying: `player_points >= entries[4].score` (reverse comparison)

High score insertion logic in `FUN_00413bc0` (0x413BC0, state 4): if the player qualifies, entries are shifted down (memcpy 32 bytes) and the new entry is inserted at the correct sorted position.

## Group Categories

### Track Groups (0-17): Per-Track High Scores

Groups 0-15 map directly to the 16 base tracks. Groups 16-17 are additional Era-exclusive tracks.
- `DAT_00466840` = 16 (number of base track groups accessible in non-cup modes)
- Maximum track index in cup mode: 0x12 = 18 (wraps to 0)
- Track lock flags: `DAT_004668B0[26]` -- 0=unlocked, 1=locked (default: groups 0-7 unlocked)
- Track completion flags: `DAT_004a2c9c[26]` -- set to 1 when player beats group_type==0 track

### Group 18: Bonus Track (Lap type, secret cars)

Uses car_ids 17-19 which map to Chevelle SS, Special 6, and Corvette C5 -- cars not normally in the first 16. This group appears in `AwardCupCompletionUnlocks` as the final unlock gate (car slot 0x20 or all-unlock flag).

### Group 19: Time Trial Cumulative

Type=4 (extended time format in milliseconds). Used exclusively when `g_selectedGameType == 7`. Scores are cumulative race times across all Time Trial attempts.

### Groups 20-25: Cup Completion High Scores

Assigned by `FUN_00413bc0` when `g_selectedGameType` is 1-6:
- `group = g_selectedGameType + 0x13` (i.e., gameType 1 -> group 20, gameType 6 -> group 25)
- These record best overall performance for each cup series

| Group | Cup Mode | Type | Score Meaning |
|-------|----------|------|---------------|
| 20 | Championship (gameType=1) | 2 (Points) | Total championship points |
| 21 | Era (gameType=2) | 0 (Time) | Total race time (cs) |
| 22 | Challenge (gameType=3) | 0 (Time) | Total race time (cs) |
| 23 | Pitbull (gameType=4) | 0 (Time) | Total race time (cs) |
| 24 | Masters (gameType=5) | 0 (Time) | Total race time (cs) |
| 25 | Ultimate (gameType=6) | 2 (Points) | Total championship points |

## Race-to-Group Mapping Table

Located at `0x464098` (embedded after the XOR key string "Steve Snake says : No Cheating!" at 0x464084). Indexed as `base[gameType * 0x10 + raceIndex]`. Value 99 (0x63) = end sentinel.

Used by `ConfigureGameTypeFlags` (0x410E80): `DAT_00490BA8 = table[g_raceWithinSeriesIndex + g_selectedGameType * 0x10 + 0x14]`

| Cup Mode | Race Sequence (group indices) |
|----------|-------------------------------|
| Championship (1) | 0, 1, 2, 3 |
| Era (2) | 4, 16, 6, 7, 5, 17 |
| Challenge (3) | 0, 1, 2, 3, 15, 8 |
| Pitbull (4) | 0, 1, 2, 3, 15, 8, 11, 13 |
| Masters (5) | 0, 1, 2, 3, 15, 8, 11, 13, 10, 12 |
| Ultimate (6) | 0, 1, 2, 3, 15, 8, 11, 13, 10, 12, 9, 14 |

Non-cup modes (7=Time Trial, 8=Cop Chase, 9=Drag Race) use `DAT_004a2c90` directly (player-selected track group).

## Cup Completion Unlock Rewards

From `AwardCupCompletionUnlocks` (0x421DC0). When player wins 1st place on a type=1 (Lap) group:

| Group | Unlocks Car Slot | Car (via gExtCarIdToTypeIndex) |
|-------|-----------------|-------------------------------|
| 4 | 0x15 | ext_id 21 -> sp2.zip (Special 2) |
| 5 | 0x11 | ext_id 17 -> chv.zip (Chevelle SS) |
| 6 | 0x18 | ext_id 24 -> atp.zip (A-Type Concept) |
| 7 | 0x19 | ext_id 25 -> nis.zip (Nissan 240SX) |
| 16 | 0x17 | ext_id 23 -> sp4.zip (Special 4) |
| 17 | 0x1A | ext_id 26 -> frd.zip (Ford Mustang) |
| 18 | 0x20 | ext_id 32 -> cat.zip (Cadillac Eldorado) *or* all-unlock if 32 cars already unlocked |

Car lock array: `DAT_00463E4C[37]` -- 0=unlocked, 1=locked. Default: slots 0-15 unlocked, 16-32 locked.
Accessible car count: `DAT_00463E0C` -- tracks highest unlocked slot+1 (default: 0x17 = 23).

When group_type == 0 (Time): sets `DAT_004a2c9c[group_index] = 1` (track completion flag, no car unlock).

## Serialization

The entire 26-group table (0x42A dwords = 4264 bytes) is saved/loaded as part of `Config.td5`:
- `WritePackedConfigTd5` (0x40FA00): copies from `gNpcRacerGroupTable` to save buffer
- `LoadPackedConfigTd5` (0x40FD50): restores from save buffer to `gNpcRacerGroupTable`
- XOR encrypted with key "Outta Mah Face!!!"

Also serialized alongside:
- Track lock flags (`DAT_004668B0`, 26 bytes)
- Car lock flags (`DAT_00463E4C`, 37 bytes)
- Track completion flags (`DAT_004a2c9c`, 26 bytes, via `WritePackedConfigTd5` at offset 0x0490851)

## Cross-References

| Address | Function | Usage |
|---------|----------|-------|
| 0x40FA26 | WritePackedConfigTd5 | Serialize entire table (0x42A dwords) |
| 0x40FD76 | LoadPackedConfigTd5 | Deserialize entire table |
| 0x413087 | DrawPostRaceHighScoreEntry | group_type for column headers |
| 0x413156 | DrawPostRaceHighScoreEntry | group_type for "Best" column check |
| 0x413197 | DrawPostRaceHighScoreEntry | group_type == 1 -> "Lap" header |
| 0x4131D0 | DrawPostRaceHighScoreEntry | group_type == 2 -> "Pts" header |
| 0x4132D7 | DrawPostRaceHighScoreEntry | group_type switch for score format |
| 0x413BC0 | FUN_00413bc0 (PostRaceHighScoreEntry) | Score insertion, qualification check |
| 0x414E51 | RunFrontendDisplayLoop | Frontend display reference |
| 0x421DC7 | AwardCupCompletionUnlocks | group_type for unlock logic |
| 0x4276AA | TrackSelectionScreenStateMachine | group_type & 3 for track filtering |

## External Car ID Mapping (gExtCarIdToTypeIndex at 0x463E24)

Maps the `car_id` field in NPC entries to car type index (into ZIP path table at 0x466FF0).

| ext_id | Car | ext_id | Car | ext_id | Car |
|--------|-----|--------|-----|--------|-----|
| 0 | Special 7 | 10 | Camaro SS | 20 | Dodge Viper |
| 1 | Jaguar XKR | 11 | Pitbull | 21 | Special 2 |
| 2 | Hot Rod | 12 | Special 1 | 22 | Chrysler 300 |
| 3 | Skyline GT-R R34 | 13 | Special 5 | 23 | Special 4 |
| 4 | Dodge Charger | 14 | Special 3 | 24 | A-Type Concept |
| 5 | Mustang GT | 15 | TVR Cerbera | 25 | Nissan 240SX |
| 6 | Jaguar E-Type | 16 | Dodge Daytona | 26 | Ford Mustang |
| 7 | '97 Corvette | 17 | Chevelle SS | 27 | Camaro Classic |
| 8 | Pontiac GTO | 18 | Special 6 | 28 | Ford F-150 |
| 9 | Nissan Skyline | 19 | Corvette C5 | 29 | Shelby Cobra |

## Dump: All 26 Groups (Default Values)

### Group 0 (type=0, Race Time) -- Track 0
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Frank    | 9380   | 1  | Jaguar XKR | 490 | 780 |
| 2 | Raymond  | 9400   | 2  | Hot Rod | 460 | 710 |
| 3 | Ben      | 9547   | 4  | Dodge Charger | 410 | 640 |
| 4 | Paul     | 9588   | 6  | Jaguar E-Type | 400 | 550 |
| 5 | Jeffrey  | 9604   | 9  | Nissan Skyline | 370 | 510 |

### Group 1 (type=0, Race Time) -- Track 1
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | George   | 8668   | 0  | Special 7 | 500 | 800 |
| 2 | Edna     | 8696   | 3  | Skyline GT-R R34 | 480 | 720 |
| 3 | Katie    | 8740   | 5  | Mustang GT | 420 | 650 |
| 4 | Martin   | 8812   | 7  | '97 Corvette | 410 | 580 |
| 5 | Guthrie  | 8871   | 8  | Pontiac GTO | 360 | 530 |

### Group 2 (type=0, Race Time) -- Track 2
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Ted      | 7389   | 3  | Skyline GT-R R34 | 480 | 770 |
| 2 | Dougal   | 7424   | 5  | Mustang GT | 470 | 712 |
| 3 | Jack     | 7457   | 1  | Jaguar XKR | 430 | 646 |
| 4 | Len      | 7486   | 2  | Hot Rod | 410 | 547 |
| 5 | Elvis    | 7501   | 7  | '97 Corvette | 350 | 521 |

### Group 3 (type=0, Race Time) -- Track 3
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Basil    | 9751   | 8  | Pontiac GTO | 482 | 776 |
| 2 | Sybil    | 9789   | 3  | Skyline GT-R R34 | 468 | 714 |
| 3 | Manuel   | 9843   | 2  | Hot Rod | 434 | 665 |
| 4 | Polly    | 9912   | 7  | '97 Corvette | 422 | 587 |
| 5 | Major    | 10076  | 5  | Mustang GT | 365 | 506 |

### Group 4 (type=1, Lap Time) -- Track 4 [Era R1] [Unlocks: Special 2]
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Cevin    | 3961   | 4  | Dodge Charger | 493 | 772 |
| 2 | Dwayne   | 4033   | 6  | Jaguar E-Type | 475 | 724 |
| 3 | Nivek    | 4101   | 3  | Skyline GT-R R34 | 440 | 666 |
| 4 | Alain    | 4153   | 0  | Special 7 | 411 | 587 |
| 5 | Paul     | 4202   | 1  | Jaguar XKR | 365 | 536 |

### Group 5 (type=1, Lap Time) -- Track 5 [Era R5] [Unlocks: Chevelle SS]
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Sascha   | 3600   | 7  | '97 Corvette | 493 | 791 |
| 2 | Enesch   | 3742   | 8  | Pontiac GTO | 465 | 743 |
| 3 | Gilbert  | 3801   | 10 | Camaro SS | 421 | 688 |
| 4 | Frederick| 3896   | 5  | Mustang GT | 410 | 554 |
| 5 | Albert   | 3952   | 6  | Jaguar E-Type | 392 | 524 |

### Group 6 (type=1, Lap Time) -- Track 6 [Era R3] [Unlocks: A-Type Concept]
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Norman   | 3406   | 14 | Special 3 | 488 | 776 |
| 2 | Philip   | 3452   | 12 | Special 1 | 454 | 714 |
| 3 | Tony     | 3497   | 6  | Jaguar E-Type | 445 | 695 |
| 4 | Michael  | 3556   | 0  | Special 7 | 440 | 567 |
| 5 | Grant    | 3598   | 3  | Skyline GT-R R34 | 376 | 566 |

### Group 7 (type=1, Lap Time) -- Track 7 [Era R4] [Unlocks: Nissan 240SX]
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Terry    | 3636   | 7  | '97 Corvette | 465 | 765 |
| 2 | Patricia | 3689   | 4  | Dodge Charger | 456 | 734 |
| 3 | Connor   | 3704   | 12 | Special 1 | 442 | 623 |
| 4 | Kathy    | 3799   | 1  | Jaguar XKR | 415 | 543 |
| 5 | Eric     | 3852   | 15 | TVR Cerbera | 368 | 554 |

### Group 8 (type=0, Race Time) -- Track 8
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Ernie    | 9728   | 9  | Nissan Skyline | 476 | 778 |
| 2 | Tiffany  | 9796   | 5  | Mustang GT | 456 | 756 |
| 3 | Peter    | 9843   | 2  | Hot Rod | 443 | 634 |
| 4 | Simon    | 9889   | 13 | Special 5 | 423 | 545 |
| 5 | Bianca   | 9940   | 8  | Pontiac GTO | 376 | 543 |

### Group 9 (type=0, Race Time) -- Track 9
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Trent    | 9006   | 12 | Special 1 | 487 | 787 |
| 2 | Matthew  | 9078   | 14 | Special 3 | 456 | 756 |
| 3 | Raymond  | 9143   | 6  | Jaguar E-Type | 442 | 654 |
| 4 | Marilyn  | 9265   | 7  | '97 Corvette | 412 | 543 |
| 5 | Christian| 9331   | 4  | Dodge Charger | 397 | 523 |

### Group 10 (type=0, Race Time) -- Track 10
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Irene    | 10281  | 15 | TVR Cerbera | 476 | 765 |
| 2 | Betty    | 10365  | 13 | Special 5 | 456 | 745 |
| 3 | Rob      | 10484  | 11 | Pitbull | 445 | 634 |
| 4 | Andrew   | 10505  | 10 | Camaro SS | 443 | 554 |
| 5 | Warren   | 10586  | 9  | Nissan Skyline | 376 | 523 |

### Group 11 (type=0, Race Time) -- Track 11
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Snake    | 7957   | 5  | Mustang GT | 467 | 743 |
| 2 | Bob      | 8012   | 4  | Dodge Charger | 434 | 734 |
| 3 | Gareth   | 8076   | 7  | '97 Corvette | 423 | 623 |
| 4 | Chris    | 8187   | 3  | Skyline GT-R R34 | 420 | 543 |
| 5 | Mike     | 8236   | 2  | Hot Rod | 376 | 556 |

### Group 12 (type=0, Race Time) -- Track 12
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Emma     | 9391   | 0  | Special 7 | 497 | 754 |
| 2 | Arnold   | 9456   | 4  | Dodge Charger | 454 | 734 |
| 3 | Dave     | 9509   | 7  | '97 Corvette | 434 | 645 |
| 4 | Charmaine| 9645   | 9  | Nissan Skyline | 423 | 534 |
| 5 | Holly    | 9704   | 12 | Special 1 | 354 | 523 |

### Group 13 (type=0, Race Time) -- Track 13
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Damon    | 7408   | 1  | Jaguar XKR | 454 | 765 |
| 2 | Lucy     | 7488   | 3  | Skyline GT-R R34 | 423 | 734 |
| 3 | Kevin    | 7537   | 6  | Jaguar E-Type | 422 | 654 |
| 4 | Elissa   | 7579   | 13 | Special 5 | 412 | 523 |
| 5 | Jodie    | 7603   | 14 | Special 3 | 386 | 521 |

### Group 14 (type=0, Race Time) -- Track 14
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Danielle | 7232   | 2  | Hot Rod | 487 | 754 |
| 2 | Jim      | 7265   | 4  | Dodge Charger | 456 | 734 |
| 3 | Narelle  | 7298   | 7  | '97 Corvette | 445 | 675 |
| 4 | Mick     | 7312   | 9  | Nissan Skyline | 434 | 545 |
| 5 | Poppy    | 7359   | 10 | Camaro SS | 376 | 532 |

### Group 15 (type=0, Race Time) -- Track 15
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Sarah    | 7269   | 6  | Jaguar E-Type | 493 | 776 |
| 2 | John     | 7299   | 8  | Pontiac GTO | 476 | 745 |
| 3 | Kyle     | 7332   | 4  | Dodge Charger | 444 | 634 |
| 4 | Arnold   | 7344   | 1  | Jaguar XKR | 434 | 565 |
| 5 | Dennis   | 7359   | 7  | '97 Corvette | 395 | 534 |

### Group 16 (type=1, Lap Time) -- Track 16 [Era R2] [Unlocks: Special 4]
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Slade    | 3609   | 9  | Nissan Skyline | 465 | 787 |
| 2 | Rodney   | 3712   | 14 | Special 3 | 445 | 756 |
| 3 | Alan     | 3787   | 6  | Jaguar E-Type | 423 | 654 |
| 4 | Marie    | 3865   | 15 | TVR Cerbera | 412 | 543 |
| 5 | Belinda  | 3902   | 3  | Skyline GT-R R34 | 375 | 532 |

### Group 17 (type=1, Lap Time) -- Track 17 [Era R6] [Unlocks: Ford Mustang]
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Craig    | 3739   | 15 | TVR Cerbera | 487 | 786 |
| 2 | Norman   | 3789   | 4  | Dodge Charger | 467 | 736 |
| 3 | Hatty    | 3821   | 13 | Special 5 | 456 | 656 |
| 4 | Danny    | 3853   | 8  | Pontiac GTO | 443 | 554 |
| 5 | Chris    | 3886   | 10 | Camaro SS | 389 | 543 |

### Group 18 (type=1, Lap Time) -- Bonus Track [Unlocks: Cadillac Eldorado / All Cars]
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Rob      | 3241   | 18 | Special 6 | 465 | 754 |
| 2 | Doug     | 3312   | 15 | TVR Cerbera | 445 | 734 |
| 3 | Samuel   | 3367   | 17 | Chevelle SS | 442 | 612 |
| 4 | John     | 3455   | 19 | Corvette C5 | 412 | 565 |
| 5 | Harvey   | 3567   | 12 | Special 1 | 365 | 523 |

### Group 19 (type=4, Time Trial) -- Cumulative Time Trial
| # | Name        | Score (ms) | Car ID | Car Name | Avg | Top |
|---|-------------|------------|--------|----------|-----|-----|
| 1 | Tim         | 392000     | 5  | Mustang GT | 265 | 654 |
| 2 | Uma         | 398936     | 2  | Hot Rod | 254 | 546 |
| 3 | Eric        | 421488     | 0  | Special 7 | 243 | 540 |
| 4 | Christopher | 445000     | 7  | '97 Corvette | 234 | 512 |
| 5 | Bruce       | 493104     | 6  | Jaguar E-Type | 212 | 487 |

### Group 20 (type=2, Points) -- Championship Cup (gameType=1)
| # | Name     | Score | Car ID | Car Name | Avg | Top |
|---|----------|-------|--------|----------|-----|-----|
| 1 | Amanda   | 96    | 4  | Dodge Charger | 487 | 754 |
| 2 | Maria    | 90    | 6  | Jaguar E-Type | 454 | 752 |
| 3 | Ving     | 88    | 3  | Skyline GT-R R34 | 423 | 643 |
| 4 | Rosanna  | 81    | 0  | Special 7 | 412 | 554 |
| 5 | Quentin  | 77    | 1  | Jaguar XKR | 343 | 543 |

### Group 21 (type=0, Time) -- Era Cup (gameType=2)
| # | Name      | Score  | Car ID | Car Name | Avg | Top |
|---|-----------|--------|--------|----------|-----|-----|
| 1 | Vincent   | 87055  | 8  | Pontiac GTO | 476 | 778 |
| 2 | Mia       | 88120  | 4  | Dodge Charger | 456 | 756 |
| 3 | Butch     | 89543  | 12 | Special 1 | 445 | 743 |
| 4 | Marcellus | 90765  | 3  | Skyline GT-R R34 | 434 | 712 |
| 5 | Greg      | 91987  | 15 | TVR Cerbera | 398 | 696 |

### Group 22 (type=0, Time) -- Challenge Cup (gameType=3)
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | John     | 47107  | 2  | Hot Rod | 478 | 756 |
| 2 | Paul     | 48430  | 8  | Pontiac GTO | 455 | 743 |
| 3 | Ringo    | 49587  | 4  | Dodge Charger | 454 | 625 |
| 4 | George   | 51432  | 10 | Camaro SS | 443 | 552 |
| 5 | Wilfred  | 52162  | 11 | Pitbull | 378 | 524 |

### Group 23 (type=0, Time) -- Pitbull Cup (gameType=4)
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | James    | 56598  | 12 | Special 1 | 486 | 773 |
| 2 | Scott    | 58655  | 13 | Special 5 | 464 | 763 |
| 3 | Stephen  | 59978  | 6  | Jaguar E-Type | 452 | 673 |
| 4 | Shirley  | 60736  | 7  | '97 Corvette | 441 | 552 |
| 5 | Duke     | 61879  | 9  | Nissan Skyline | 362 | 541 |

### Group 24 (type=0, Time) -- Masters Cup (gameType=5)
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Steve    | 76960  | 0  | Special 7 | 489 | 763 |
| 2 | Butch    | 78546  | 1  | Jaguar XKR | 453 | 752 |
| 3 | Joanne   | 79976  | 2  | Hot Rod | 434 | 677 |
| 4 | Philip   | 81548  | 3  | Skyline GT-R R34 | 431 | 559 |
| 5 | Susan    | 82376  | 4  | Dodge Charger | 368 | 545 |

### Group 25 (type=2, Points) -- Ultimate Cup (gameType=6)
| # | Name     | Score  | Car ID | Car Name | Avg | Top |
|---|----------|--------|--------|----------|-----|-----|
| 1 | Nicole   | 63236  | 5  | Mustang GT | 494 | 764 |
| 2 | Melanie  | 61542  | 6  | Jaguar E-Type | 475 | 753 |
| 3 | Natalie  | 60944  | 7  | '97 Corvette | 442 | 663 |
| 4 | Shaznay  | 59744  | 8  | Pontiac GTO | 432 | 542 |
| 5 | Mark     | 58797  | 9  | Nissan Skyline | 386 | 534 |

## Easter Eggs in NPC Names

- **Group 3**: Basil, Sybil, Manuel, Polly, Major -- characters from *Fawlty Towers*
- **Group 11**: Snake, Bob, Gareth, Chris, Mike -- developer names (Snake = Steve Snake)
- **Group 19**: Tim, Uma, Eric, Christopher, Bruce -- *Pulp Fiction* actors (Tim Roth, Uma Thurman, Eric Stoltz, Christopher Walken, Bruce Willis)
- **Group 20**: Amanda, Maria, Ving, Rosanna, Quentin -- *Pulp Fiction* cast (Amanda Plummer, Maria de Medeiros, Ving Rhames, Rosanna Arquette, Quentin Tarantino)
- **Group 21**: Vincent, Mia, Butch, Marcellus, Greg -- *Pulp Fiction* characters
- **Group 22**: John, Paul, Ringo, George, Wilfred -- The Beatles + Wilfred
- **Group 25**: Nicole, Melanie, Natalie, Shaznay, Mark -- All Saints (band members) + Mark
