# Hidden, Debug, and Cut Features in TD5_d3d.exe

## Overview

Systematic analysis of TD5_d3d.exe for hidden developer features, debug tools, cheat
protections, cut content, and secret functionality. All addresses are virtual addresses
in the PE image (base 0x00400000).

---

## 1. Debug/Developer Tools

### 1.1 ScreenPositionerDebugTool (0x415030) -- DEAD CODE

**Status:** Unreachable (zero callers)

A complete developer tool for positioning UI elements on screen. This is a multi-state
tool that was used during development to lay out the front-end screens.

**Function:** `0x415030` -- `0x41534B` (with jump table continuations at `0x415350+`)

**Behavior by state (DAT_00495204):**
- **State 0:** Loads `Front End\Positioner.tga` from `Front End\FrontEnd.zip`, initializes display
- **State 1:** Waits for rendering to complete
- **State 2:** Initializes a glyph position array (37 entries x 2 at `DAT_00495260`) from
  character data at `DAT_00466518`
- **State 3:** Interactive positioning mode -- renders glyph strip and responds to directional
  input (bit flags in `DAT_004951f8`):
  - Bit 0 (0x1): Move left
  - Bit 1 (0x2): Move right
  - Bit 9 (0x200): Move right by 8
  - Bit 10 (0x400): Move left by 8
  - Bit 18 (0x40000): Advance to next state
- **State 4:** Fine-tuning mode -- adjusts individual glyph X/Y positions
- **State 5:** Exports positions to `positioner.txt` with header `// Created by SNK_Positioner`

**Strings:**
- `"// Created by SNK_Positioner\n\n"` at `0x465910`
- `"positioner.txt"` at `0x465930`
- `"Front End\Positioner.tga"` at `0x465944`

**How to activate:** This function has no callers in the binary. To reach it, you would
need to patch a screen handler function pointer table to point to `0x415030`, or inject
a call. The front-end screen system uses `DAT_00495204` as a state variable, so hooking
into the screen dispatch at `FUN_00442170` (state handler at `0x442170`) would work.

**Patch approach:** In the main menu handler at `0x415490`, the switch on `DAT_00495204`
dispatches to different screens. Adding the positioner as an additional menu item would
require modifying the jump table at `0x415474`.

---

## 2. Anti-Cheat / Save File Encryption System

### 2.1 CupData.td5 -- XOR Encryption with CRC32 Validation

**Key string:** `"Steve Snake says : No Cheating! "` at `0x464084` (36 bytes including trailing spaces)

The game's save data (`CupData.td5`) is protected by a rolling XOR cipher using the
developer's anti-cheat message as the key, combined with CRC32 integrity checks.

**Encryption scheme:**
```
encrypted_byte = plaintext_byte XOR key[i % key_length] XOR 0x80
```
Where `key` = `"Steve Snake says : No Cheating! "` (including trailing spaces, 35 chars).

**Functions:**
- **Save (encrypt+write):** `FUN_004114F0` at `0x4114F0` -- XORs data from `DAT_00490BAC`
  with the key, writes to `CupData.td5`. **Zero callers** -- this is also dead code;
  saving may be handled differently in the shipping build or via a duplicate path.
- **Load (read+decrypt):** `FUN_00411590` at `0x411590` -- Reads `CupData.td5`, XOR-decrypts
  into buffer at `DAT_00490BAC`, calls `FUN_004112C0` to parse.
- **Validate (read+verify CRC):** `FUN_00411630` at `0x411630` -- Reads and decrypts, then
  computes CRC32 and compares against an expected value passed as `param_3`.

**CRC32 table:** Located at `DAT_00475160` (standard CRC32 lookup table).

**Data parsed from CupData (FUN_004112C0 at 0x4112C0):**
The decrypted save contains packed bitfields:
- Track/car unlock states
- Championship progress (`DAT_0049635C`)
- Race configuration (difficulty `DAT_00466E8C`, mirror mode `DAT_00463210`, etc.)
- Player car data (copied to `DAT_004AB108`, 0xC5C dwords = 12,656 bytes)
- Car availability states (6 entries at `DAT_004AADF4`)
- Hi-score tables

### 2.2 Config.td5 -- XOR Encryption

**Key string:** `"Outta Mah Face !! "` at `0x463F9C` (19 bytes including trailing spaces)

**Function:** `FUN_0040F8D0` at `0x40F8D0` (config save)

Same XOR+0x80 scheme as CupData but using a different key. The config stores:
- Controller configuration (2 players)
- Graphics settings (fog, mirror mode, split screen, etc.)
- Sound/music volumes
- Display options
- Input device mappings

**Config files per language:** `config.nfo`, `config.eng`, `config.fre`, `config.ger`,
`config.ita`, `config.spa` (at `0x4667B4`--`0x4667A8`)

---

## 3. Benchmark / Time Demo System

### 3.1 Benchmark Initialization (0x428D20)

**Gate variable:** `DAT_004AAF58` (the "time demo" flag)

```c
void FUN_00428D20(void) {
    DAT_004A2CF4 = FUN_00430CF0(1000000);  // Allocate 1M buffer for FPS samples
    DAT_004A2CF8 = 0;                       // Reset frame counter
}
```

Called from `FUN_0042AA10` (race initialization) only when `DAT_004AAF58 != 0`.

### 3.2 FPS Sample Recording (0x428D40)

```c
void FUN_00428D40(float fps_value) {
    buffer[DAT_004A2CF8] = fps_value;  // Store FPS sample
    DAT_004A2CF8++;                    // Increment frame count
}
```

Called from `FUN_0042B580` (race frame loop) every frame when `DAT_004AAF58 != 0`,
but skips the first frame (`DAT_004AAF40` gate). Also forces `DAT_00466E88 = 3.0`
(fixed time step for deterministic benchmarking).

### 3.3 Benchmark Report Generation (0x428D80)

**Function:** `FUN_00428D80` -- large function that generates a TGA screenshot with
system information and FPS graph overlay.

**Output includes:**
- "Test Drive 5" title
- DirectX API version
- Build number
- FPS caption from DLL
- Driver version
- Processor info
- Physical memory
- Operating system
- Screen mode
- Texture memory
- Buffer capabilities (WBuffer, Triple Buffer, Mip Mapping)
- **FPS graph:** Visual bar chart of frame times
- **Statistics:** Min/Max/Average FPS (divided by 3 for display)
- Saves to TGA file via `FPSName` (from DLL export)

**Log output:** `"Min FPS: %d\tMax FPS: %d\tAvg FPS: %d\n"` at `0x466BD4`

### 3.4 How the Time Demo is Activated

**From the Main Menu (0x415490), case 4, menu item 2:**

```c
if (*(int *)(app_exref + 0x170) != 0) {
    // Time Demo mode -- visible as 3rd menu button
    DAT_0049636C = 1;      // Set time demo session flag
    DAT_004AAF58 = 1;      // Enable benchmark recording
    FUN_0040DAC0();         // Start race setup
    FUN_00414A90();         // Save config & init
    return;
}
```

The `app_exref + 0x170` field in the DX_APP structure determines whether the "TIME DEMO"
button appears in the main menu (replacing the "TWO PLAYER" button). This flag is likely
set by the M2DX DLL based on command-line arguments or a registry setting.

**Localized strings for "Demo Mode":**
- English: `"DEMO MODE"` at `0x474370`
- French: `"MODE DEMO"` at `0x4741CC`
- German: `"DEMOSTAND"` at `0x474214`
- Italian: `"MODALITA' DEMO"` at `0x474308`
- Spanish: `"MODO DEMO"` at `0x474278`

**After race completion (FUN_00442170, case 3):**
The benchmark results are displayed as a full-screen TGA with FPS graph. The user presses
any key to return to the menu. `DAT_004AAF58` is reset to 0.

**To force-enable Time Demo:**
- Patch `app_exref + 0x170` to non-zero (requires knowing the DLL's app struct address)
- Or: Patch the conditional jump at `0x415BC7` (JE to JMP) to always enter time demo path
- Or: Directly set `DAT_004AAF58 = 1` at `0x4AAF58` and `DAT_0049636C = 1` at `0x49636C`

---

## 4. Cut / Disabled Features

### 4.1 ScreenPositionerDebugTool -- Developer UI Layout Tool (CUT)

As detailed in section 1.1, this is a complete tool with no callers. It was clearly used
during development to position front-end UI elements and output coordinate files.

### 4.2 CupData Save Function (0x4114F0) -- Potentially Dead

`FUN_004114F0` (encrypt and write CupData.td5) has zero callers in the binary. The
corresponding load function (`FUN_00411590`) IS called. This suggests either:
- Save functionality was moved to a different code path
- The function is called indirectly via a function pointer
- Save was intentionally disabled in this build

### 4.3 SNK_ViewReplay -- Replay Viewer

The string `?SNK_ViewReplay@@3PADA` at `0x461A98` indicates a "View Replay" feature
was planned/implemented. The DXInput system has both `WriteOpen` (record) and `ReadOpen`
(playback) functions. The `DAT_00466E9C` flag distinguishes record vs playback mode.

### 4.4 Music Test Screen

Strings indicate a music test/jukebox screen:
- `?SNK_MusicTest_MT@@3PADA` at `0x4611E0` -- Menu title
- `?SNK_MusicTestButTxt@@3PADA` at `0x4616AE` -- Button text
- `?SNK_NowPlayingTxt@@3PADA` at `0x46121C` -- "Now Playing" indicator
- `?SNK_SelectTrackButTxt@@3PADA` at `0x4611FC` -- Track selection button

This is a localized DLL import, suggesting it exists as a menu option in the front-end
flow (accessible from the options menu).

---

## 5. Developer Strings and Credits

### 5.1 Developer Attribution

- **Steve Snake** -- Primary developer. His anti-cheat message is used as the save file
  encryption key. "SNK" prefix on all game variables (SNK = Snake). Has a mugshot TGA
  (`Front End\Extras\Snake.tga`, `Front End\Extras\Steve.tga`).

### 5.2 Encryption Key Strings (Easter Eggs)

| String | Address | Usage |
|--------|---------|-------|
| `"Steve Snake says : No Cheating! "` | `0x464084` | CupData.td5 XOR key |
| `"Outta Mah Face !! "` | `0x463F9C` | Config.td5 XOR key |

### 5.3 Developer Mugshot TGAs (Credits/Extras Screen)

The credits screen loads developer photos from `Front End\Extras\Mugshots.zip`:

| File | Address |
|------|---------|
| `Front End\Extras\Steve.tga` | `0x465D28` |
| `Front End\Extras\Snake.tga` | `0x465D9C` |
| `Front End\Extras\Bob.tga` | `0x465DD4` |
| `Front End\Extras\Gareth.tga` | `0x465DB8` |
| `Front End\Extras\MikeT.tga` | `0x465D80` |
| `Front End\Extras\Chris.tga` | `0x465D64` |
| `Front End\Extras\Headley.tga` | `0x465D44` |
| `Front End\Extras\Rich.tga` | `0x465D0C` |
| `Front End\Extras\Mike.tga` | `0x465CF0` |
| `Front End\Extras\Bez.tga` | `0x465CD4` |
| `Front End\Extras\Les.tga` | `0x465CB8` |
| `Front End\Extras\TonyP.tga` | `0x465C9C` |
| `Front End\Extras\JohnS.tga` | `0x465C80` |
| `Front End\Extras\DavidT.tga` | `0x465C64` |
| `Front End\Extras\DaveyB.tga` | `0x465C2C` |
| `Front End\Extras\ChrisD.tga` | `0x465C10` |
| `Front End\Extras\Slade.tga` | `0x465BF4` |
| `Front End\Extras\Matt.tga` | `0x465BD8` |
| `Front End\Extras\Marie.tga` | `0x465BBC` |
| `Front End\Extras\JFK.tga` | `0x465BA0` |
| `Front End\Extras\Daz.tga` | `0x465B84` |

Plus legal screens: `Legals1.tga` through `Legals5.tga`

### 5.4 Band/Music Artist Screens

Referenced TGA files for soundtrack artists:
- `Front End\Extras\Fear Factory.tga` (`0x463DBC`)
- `Front End\Extras\Gravity Kills.tga` (`0x463D98`)
- `Front End\Extras\Junkie XL.tga` (`0x463D78`)
- `Front End\Extras\KMFDM.tga` (`0x463D5C`)
- `Front End\Extras\PitchShifter.tga` (`0x463D38`)
- `Front End\Extras\pic1.tga` through `pic5.tga` (`0x463D00`--`0x463C90`)

### 5.5 Embedded Default Player Names

At `0x46531C`, the binary contains default player/character names with associated data:
- Steve, Butch, Joanne, Philip, Susan, Nicole, Mela...

### 5.6 Copyright String

`"TEST DRIVE 5 COPYRIGHT 1998"` at `0x466808`

---

## 6. Feature Flags and Global Gates

### 6.1 Key Global Variables

| Address | Name/Purpose | Effect |
|---------|-------------|--------|
| `0x4AAF58` | Time Demo / Benchmark mode | Enables FPS recording, fixed timestep, benchmark report |
| `0x49636C` | Time Demo session flag | Set alongside 0x4AAF58 |
| `0x466E9C` | Replay playback mode | 0 = record (WriteOpen), non-0 = playback (ReadOpen) |
| `0x4AAF6C` | Single-car mode | Affects car placement and camera |
| `0x4AAF68` | Cop chase mode | Enables cop chase gameplay |
| `0x495254` | Unknown race mode flag | Affects race initialization |
| `0x4AAF74` | Race type (0=normal, 1=?, 2=?) | Controls car setup behavior |
| `0x4962A0` | Two-player mode | Enables split screen |
| `0x466E98` | Fog enabled | Checked against DXD3D::CanFog() |
| `0x4AAD8C` | Unknown option flag | Disabled in certain modes |
| `0x46320C` | Unknown display flag | Set from config, cleared in some modes |
| `0x4B0FA8` | Unknown race flag | Cleared in demo/time-trial modes |
| `0x495204` | Front-end state machine | Controls which screen/state is active |
| `0x4951F8` | Input button flags (front-end) | Bitmask for UI navigation |
| `0x4951E8` | Button press event | Set to 1 when a menu button is activated |
| `0x495240` | Selected menu item index | Which button was pressed |
| `0x4C3CE8` | Game state machine | 0=init, 1=front-end, 2=racing, 3=benchmark-display |
| `app_exref+0x170` | Time Demo enabled in DLL | Gates visibility of Time Demo menu button |
| `app_exref+0x180` | Quit flag | Non-zero = exit game |

### 6.2 Race Type Codes (DAT_0049635C)

| Value | Meaning |
|-------|---------|
| 0-7 | Various championship/cup types |
| 8 | Cop Chase |
| 0xFF | Quick Race (set to -1) |

---

## 7. How to Activate Hidden Features

### 7.1 Enable ScreenPositionerDebugTool

**Method: Binary patch**

The front-end screen dispatch table at addresses around `0x415474` contains function
pointers for each screen state. To make the positioner reachable:

1. Find an unused or duplicate entry in the screen handler table
2. Replace its pointer with `0x00415030`
3. Navigate to that screen state in-game

Alternatively, use a debugger to set `EIP = 0x415030` when on the front-end screen,
with `DAT_00495204 = 0` to start from the initialization state.

### 7.2 Force Time Demo / Benchmark Mode

**Method 1: Memory patch at runtime**
```
Set [0x004AAF58] = 1  (enable benchmark)
Set [0x0049636C] = 1  (time demo session)
```

**Method 2: Binary patch to always show Time Demo button**
At `0x415B97` area in MainMenuHandler case 4 / item 2, the check:
```asm
CMP dword ptr [app_exref + 0x170], 0
```
Patch the conditional to always branch into the time demo path.

**Method 3: Force via DLL**
The M2DX DLL's `DX_APP.field_0x170` controls this. Setting it non-zero before the
front-end initializes will show "TIME DEMO" as the 3rd main menu button.

### 7.3 Unlock All Cars/Tracks

The CupData save file at `CupData.td5` stores unlock states. The data is decrypted
and parsed at `FUN_004112C0`. Key fields after decryption at `DAT_00490BAC`:

- Car availability array: 6 DWORDs starting at offset `0x493E1C` in the save buffer,
  copied to `DAT_004AADF4`. Values: 0=locked, 1=player, 2=AI opponent, 3=not present
- Track index: byte at specific offsets in the first DWORD

To unlock everything, either:
1. Craft a valid CupData.td5 with all cars/tracks unlocked and correct CRC32
2. Patch `FUN_004112C0` to set all car availability to 1 after loading
3. Set memory at `0x4AADF4` (6 DWORDs) all to `0x01` at runtime

### 7.4 Bypass Save Encryption

The XOR encryption uses `"Steve Snake says : No Cheating! "` (with trailing space, 35 chars).
To decrypt CupData.td5 manually:
```python
key = b"Steve Snake says : No Cheating! "  # 35 bytes including trailing spaces
with open("CupData.td5", "rb") as f:
    data = f.read()
plaintext = bytes((b ^ key[i % len(key)] ^ 0x80) for i, b in enumerate(data))
```

To decrypt Config.td5:
```python
key = b"Outta Mah Face !! "  # 19 bytes including trailing space
# Same XOR ^ key[i] ^ 0x80 scheme
```

Config.td5 additionally has a CRC32 checksum as the first 4 bytes of the plaintext
(computed over the remaining data).

---

## 8. Summary Table

| Feature | Address | Status | Reachable? |
|---------|---------|--------|------------|
| ScreenPositionerDebugTool | `0x415030` | Complete, dead code | No (0 callers) |
| Benchmark/FPS System | `0x428D20`-`0x429500` | Complete, gated | Only via Time Demo menu |
| Time Demo menu button | `0x415BF2` | Complete, gated | Only if `app+0x170 != 0` |
| CupData XOR encryption | `0x4114F0` (save) | Save function has 0 callers | Save: no; Load: yes |
| Config XOR encryption | `0x40F8D0` (save) | Fully functional | Yes |
| CupData load+parse | `0x411590`/`0x4112C0` | Fully functional | Yes |
| Music Test screen | DLL import | Exists in menu system | Yes (via Options) |
| Replay recording | DXInput::WriteOpen | Functional | Yes (during races) |
| Replay playback | DXInput::ReadOpen | Functional | Via `DAT_00466E9C` |
| Developer mugshots | `0x465B84`-`0x465D9C` | Assets in Extras | Yes (credits screen) |
| Band/artist screens | `0x463D38`-`0x463DBC` | Assets in Extras | Yes (credits screen) |
| Cop Chase mode | `DAT_0049635C = 8` | Functional | Yes (race menu) |
| Demo Mode (attract) | `DAT_00466E9C != 0` | Replays input for AI demo | Via config/DLL |

---

## 9. Developer Notes

### "Steve Snake" Identity

Steve Snake (real name Steve) is the primary programmer of Test Drive 5 at Pitbull
Syndicate Ltd. The "SNK" prefix on all game globals (`SNK_MainMenu_MT`, `SNK_CarSelect_Ex`,
`SNK_TracksUnlocked`, etc.) is his developer tag. His anti-cheat message embedded as the
save encryption key is a classic 90s developer calling card.

### Code Architecture

The front-end is a state machine driven by `DAT_00495204` (current state) and
`DAT_004C3CE8` (game phase: 0=init, 1=menus, 2=racing, 3=benchmark-display). Each
screen handler is a single large function with a switch statement. The "SNK_" prefixed
imports from the language DLL provide all localized text strings.

### Naming Convention

All globals follow the pattern `SNK_<Name>` where SNK = Snake. Button text strings end
in `ButTxt`, menu titles end in `_MT`, and extended data ends in `_Ex`.
