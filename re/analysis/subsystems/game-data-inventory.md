# Test Drive 5 -- Complete Game Data Inventory

Comprehensive inventory of all game data files shipped with TD5, including ZIP
archive contents, track numbering, car models, and size breakdowns.

**Total game data (original files only): 111.1 MB on disk (compressed ZIPs)**

---

## Table of Contents

1. [Top-Level File Tree](#top-level-file-tree)
2. [Level ZIPs -- Track Data](#level-zips)
3. [Track Number Mapping](#track-number-mapping)
4. [Car ZIPs](#car-zips)
5. [Shared Archives](#shared-archives)
6. [Front End Directory](#front-end-directory)
7. [Movie Files](#movie-files)
8. [Sound Directory](#sound-directory)
9. [Standalone Config Files](#standalone-config-files)
10. [Size Breakdown by Category](#size-breakdown)
11. [Missing / Gap Track Numbers](#missing-track-numbers)

---

## 1. Top-Level File Tree {#top-level-file-tree}

```
data/
├── Config.td5                      5,351 B     Binary config (game settings blob)
├── TD5.ini                             7 B     Window mode flag ("Window")
├── Cup.zip                       195,059 B     Installer/config archive
├── environs.zip                   49,051 B     Environment billboard textures (27 TGAs)
├── legals.zip                    248,145 B     Legal splash screens (2 TGAs)
├── loading.zip                 3,918,959 B     Loading screen images (21 TGAs)
├── static.zip                     72,736 B     Shared runtime textures + meshes (7 files)
├── traffic.zip                   350,822 B     Traffic vehicle models + skins (62 files)
│
├── level001.zip                4,681,912 B  ┐
├── level002.zip                4,825,867 B  │
├── level003.zip                5,921,962 B  │
├── level004.zip                4,734,366 B  │
├── level005.zip                5,176,003 B  │
├── level006.zip                3,815,339 B  │  20 level ZIPs
├── level013.zip                3,704,484 B  │  (track data archives)
├── level014.zip                3,987,606 B  │
├── level015.zip                3,887,395 B  │
├── level016.zip                3,984,765 B  │
├── level017.zip                3,870,479 B  │
├── level023.zip                4,243,058 B  │
├── level025.zip                1,122,775 B  │
├── level026.zip                1,175,065 B  │
├── level027.zip                1,176,653 B  │
├── level028.zip                1,015,893 B  │
├── level029.zip                1,190,117 B  │
├── level030.zip                  615,060 B  │
├── level037.zip                  914,671 B  │
├── level039.zip                1,178,133 B  ┘
│
├── cars/                                       37 car ZIP archives
│   ├── 128.zip ... xkr.zip
│
├── Front End/                                  Frontend UI resources
│   ├── MenuFont.tga              291,408 B
│   ├── frontend.zip              484,817 B
│   ├── Extras/
│   │   ├── Extras.zip            257,281 B
│   │   └── Mugshots.zip          885,212 B
│   ├── Sounds/
│   │   └── Sounds.zip             62,052 B
│   └── Tracks/
│       └── Tracks.zip             43,968 B
│
├── movie/
│   └── intro.tgq             24,903,200 B     FMV intro video
│
├── sound/
│   └── SOUND.ZIP              1,431,675 B     Shared ambient/collision SFX (25 WAVs)
│
├── assets/                                     [All extracted game assets — runtime directory]
└── td5_dump/                                   [Analysis dumps]
```

---

## 2. Level ZIPs -- Track Data {#level-zips}

Each level ZIP contains the complete track data for one raceable location.
Circuit tracks (pools 0-11) have 14 files including reverse-direction variants.
Point-to-point tracks (pools 12-18) have 9 files (no reverse variants, except level030).

### Per-Level Contents

#### Circuit Tracks (14 files each, bidirectional)

Levels: 001, 002, 003, 004, 005, 006, 013, 014, 015, 016, 017, 023, 030

| File | Description |
|------|-------------|
| `textures.dat` | Track geometry texture definitions (palettized 64x64) |
| `models.dat` | 3D trackside model container (PRR meshes) |
| `checkpt.num` | Checkpoint remapping matrix (96 bytes, 4x6 int32) |
| `levelinf.dat` | Environment config (circuit/weather/fog/traffic flags) |
| `forwsky.tga` | Forward sky panorama (256x256 @ 24bpp) |
| `backsky.tga` | Reverse sky panorama (256x256 @ 24bpp) |
| `strip.dat` | Forward track geometry (5-dword header + 24B/span) |
| `stripb.dat` | Reverse track geometry |
| `left.trk` | Left track boundary (forward) |
| `leftb.trk` | Left track boundary (reverse) |
| `right.trk` | Right track boundary (forward) |
| `rightb.trk` | Right track boundary (reverse) |
| `traffic.bus` | Forward traffic spawn table (4B records, -1 sentinel) |
| `trafficb.bus` | Reverse traffic spawn table |

#### Point-to-Point Tracks (9 files each, forward only)

Levels: 025, 026, 027, 028, 029, 037, 039

| File | Description |
|------|-------------|
| `textures.dat` | Track geometry texture definitions |
| `models.dat` | 3D trackside model container |
| `checkpt.num` | Checkpoint remapping matrix (96 bytes) |
| `levelinf.dat` | Environment config |
| `forwsky.tga` | Sky panorama |
| `strip.dat` | Track geometry |
| `left.trk` | Left track boundary |
| `right.trk` | Right track boundary |
| `traffic.bus` | Traffic spawn table |

**Note:** level028 and level039 have smaller sky TGAs (66,348 B vs the usual 196,652 B).

### Level ZIP Size Table (Compressed / Uncompressed)

| Level ZIP | Compressed | Uncompressed | Ratio | Type |
|-----------|------------|--------------|-------|------|
| level001.zip | 4,681,912 | 17,403,282 | 3.7x | Circuit |
| level002.zip | 4,825,867 | 15,103,782 | 3.1x | Circuit |
| level003.zip | 5,921,962 | 19,637,251 | 3.3x | Circuit |
| level004.zip | 4,734,366 | 17,108,159 | 3.6x | Circuit |
| level005.zip | 5,176,003 | 18,469,812 | 3.6x | Circuit |
| level006.zip | 3,815,339 | 14,820,997 | 3.9x | Circuit |
| level013.zip | 3,704,484 | 13,728,173 | 3.7x | Circuit |
| level014.zip | 3,987,606 | 15,027,160 | 3.8x | Circuit |
| level015.zip | 3,887,395 | 14,891,160 | 3.8x | Circuit |
| level016.zip | 3,984,765 | 13,706,292 | 3.4x | Circuit |
| level017.zip | 3,870,479 | 13,837,827 | 3.6x | Circuit |
| level023.zip | 4,243,058 | 14,737,791 | 3.5x | Circuit |
| level025.zip | 1,122,775 | 3,515,621 | 3.1x | P2P |
| level026.zip | 1,175,065 | 3,521,483 | 3.0x | P2P |
| level027.zip | 1,176,653 | 3,059,198 | 2.6x | P2P |
| level028.zip | 1,015,893 | 3,240,180 | 3.2x | P2P |
| level029.zip | 1,190,117 | 3,998,671 | 3.4x | P2P |
| level030.zip | 615,060 | 1,784,019 | 2.9x | Circuit* |
| level037.zip | 914,671 | 3,376,452 | 3.7x | P2P |
| level039.zip | 1,178,133 | 3,466,613 | 2.9x | P2P |

*level030 is the drag strip -- a special circuit with bidirectional data but the
smallest track of all.

---

## 3. Track Number Mapping {#track-number-mapping}

### Pool Index to Level ZIP to Track Name

The game uses a pool index (0-18 for regular tracks, plus drag strip) that maps
through `gScheduleToPoolIndex` (0x466894) and the pool-to-ZIP table at 0x466D50.
Track names come from `SNK_TrackNames` in Language.dll (at DLL VA 0x10007234).

| Pool | Level ZIP | Track Name | Type | Weather | Reverse? |
|------|-----------|------------|------|---------|----------|
| 0 | level001 | KESWICK, ENGLAND | Circuit | Rain | Yes |
| 1 | level002 | SAN FRANCISCO, CA, USA | Circuit | Clear | Yes |
| 2 | level003 | BERN, SWITZERLAND | Circuit | Snow | Yes |
| 3 | level004 | KYOTO, JAPAN | Circuit | Clear | Yes |
| 4 | level005 | WASHINGTON, DC, USA | Circuit | Clear | Yes |
| 5 | level006 | MUNICH, GERMANY | Circuit | Clear | Yes |
| 6 | level013 | HONOLULU, HAWAII, USA | Circuit | Clear (fog) | Yes |
| 7 | level014 | SYDNEY, AUSTRALIA | Circuit | Clear (fog) | Yes |
| 8 | level015 | TOKYO, JAPAN | Circuit | Clear | Yes |
| 9 | level016 | EDINBURGH, SCOTLAND | Circuit | Clear | Yes |
| 10 | level017 | BLUE RIDGE PARKWAY, NC, USA | Circuit | Clear (fog) | Yes |
| 11 | level023 | MOSCOW, RUSSIA | Circuit | Rain (fog) | Yes |
| 12 | level025 | CHEDDAR CHEESE, ENGLAND | P2P | Clear (fog) | No |
| 13 | level026 | JARASH, JORDAN | P2P | Clear (fog) | No |
| 14 | level027 | COURMAYEUR, ITALY | P2P | Clear | No |
| 15 | level028 | MAUI, HAWAII, USA | P2P | Clear | No |
| 16 | level029 | NEWCASTLE, ENGLAND | P2P | Clear (fog) | No |
| 17 | level037 | HOUSE OF BEZ, ENGLAND | P2P | Clear | No |
| 18 | level039 | MONTEGO BAY, JAMAICA | P2P | Clear (fog) | No |
| -- | level030 | DRAG STRIP | Drag | -- | Yes |

**Total: 20 level ZIPs = 12 circuit tracks + 7 P2P tracks + 1 drag strip**

### Schedule Index Order

The frontend schedule index (0-19) maps through `gScheduleToPoolIndex` to pool
indices, which then map to level ZIP numbers. The schedule order determines the
progression through the game's cup/championship mode. Track 20 (pool index for
drag strip) is loaded via the special level030 path.

---

## 4. Car ZIPs {#car-zips}

### 37 Car Archives in `data/cars/`

Every car ZIP contains an identical 19-file manifest (see
`car-zip-and-static-zip-contents.md` for full format docs).

| # | ZIP | Size (compressed) | Full Name (from config.nfo) |
|---|-----|-------------------|---------------------------|
| 1 | 128.zip | 569,177 | Ferrari 128 variant |
| 2 | 69v.zip | 520,830 | -- |
| 3 | 97c.zip | 612,973 | -- |
| 4 | atp.zip | 706,567 | -- |
| 5 | c21.zip | 607,229 | -- |
| 6 | cam.zip | 594,205 | Chevrolet Camaro |
| 7 | cat.zip | 591,625 | -- |
| 8 | chv.zip | 596,961 | Chevrolet |
| 9 | cob.zip | 513,199 | Shelby Cobra |
| 10 | cop.zip | 480,725 | Police car |
| 11 | crg.zip | 603,455 | -- |
| 12 | cud.zip | 672,697 | Cuda |
| 13 | day.zip | 603,513 | Dodge Daytona |
| 14 | fhm.zip | 673,224 | -- |
| 15 | frd.zip | 501,779 | Ford |
| 16 | gto.zip | 526,916 | Pontiac GTO |
| 17 | gtr.zip | 711,239 | Nissan GTR |
| 18 | hot.zip | 676,921 | Hot rod |
| 19 | jag.zip | 526,646 | Jaguar |
| 20 | mus.zip | 665,807 | Ford Mustang |
| 21 | nis.zip | 594,193 | Nissan |
| 22 | pit.zip | 725,073 | Dodge Viper (Pit Viper) |
| 23 | sky.zip | 401,581 | Skyline |
| 24 | sp1.zip | 660,384 | Special 1 ("The Mighty Maul") |
| 25 | sp2.zip | 653,293 | Special 2 |
| 26 | sp3.zip | 649,820 | Special 3 |
| 27 | sp4.zip | 660,052 | Special 4 |
| 28 | sp5.zip | 553,277 | Special 5 |
| 29 | sp6.zip | 520,332 | Special 6 |
| 30 | sp7.zip | 552,069 | Special 7 |
| 31 | sp8.zip | 602,754 | Special 8 |
| 32 | ss1.zip | 692,490 | Shelby SS1 |
| 33 | tvr.zip | 406,980 | TVR |
| 34 | van.zip | 689,080 | -- |
| 35 | vet.zip | 531,458 | Corvette |
| 36 | vip.zip | 673,800 | Dodge Viper |
| 37 | xkr.zip | 554,189 | Jaguar XKR |

### Per-Car File Manifest (19 files, identical across all 37 ZIPs)

| File | Typical Size | Format |
|------|-------------|--------|
| `himodel.dat` | 39-47 KB | PRR 3D mesh (vehicle body) |
| `carparam.dat` | 268 B | Binary tuning + physics parameters |
| `config.nfo` | ~140-145 B | ASCII stat card (17 fields) |
| `carskin0.tga` | ~98 KB | TGA 128x256 @ 24bpp (livery 0) |
| `carskin1.tga` | ~98 KB | TGA 128x256 @ 24bpp (livery 1) |
| `carskin2.tga` | ~98 KB | TGA 128x256 @ 24bpp (livery 2) |
| `carskin3.tga` | ~98 KB | TGA 128x256 @ 24bpp (livery 3) |
| `carhub0.tga` | ~5 KB | TGA 64x64 @ 8bpp indexed (wheel hub 0) |
| `carhub1.tga` | ~5 KB | TGA 64x64 @ 8bpp indexed (wheel hub 1) |
| `carhub2.tga` | ~5 KB | TGA 64x64 @ 8bpp indexed (wheel hub 2) |
| `carhub3.tga` | ~5 KB | TGA 64x64 @ 8bpp indexed (wheel hub 3) |
| `carpic0.tga` | ~229 KB | TGA 408x280 @ 16bpp (selection preview 0) |
| `carpic1.tga` | ~229 KB | TGA 408x280 @ 16bpp (selection preview 1) |
| `carpic2.tga` | ~229 KB | TGA 408x280 @ 16bpp (selection preview 2) |
| `carpic3.tga` | ~229 KB | TGA 408x280 @ 16bpp (selection preview 3) |
| `drive.wav` | 70-126 KB | Engine running loop |
| `rev.wav` | 55-93 KB | Idle loop (non-local player) |
| `reverb.wav` | 45-62 KB | Idle loop (local player, reverb) |
| `horn.wav` | 19-26 KB | Horn one-shot |

---

## 5. Shared Archives {#shared-archives}

### static.zip (72,736 B compressed)

Shared runtime textures and 3D meshes loaded every race.

| File | Uncompressed Size | Format | Purpose |
|------|-------------------|--------|---------|
| `sky.prr` | 50,784 B | PRR mesh (1152 verts) | Sky dome geometry |
| `wheel.prr` | 2,272 B | PRR mesh (36 verts) | Shared wheel model (all cars) |
| `static.hed` | 3,544 B | Binary header | Texture page directory (17 pages, 51 named entries) |
| `tpage0.dat` | 262,144 B | Raw ARGB32 256x256 | Car skin composite page 0 |
| `tpage1.dat` | 262,144 B | Raw ARGB32 256x256 | Car skin composite page 1 |
| `tpage2.dat` | 262,144 B | Raw ARGB32 256x256 | Car skin composite page 2 |
| `environ0.tga` | 196,652 B | TGA 256x256 @ 24bpp | Default environment reflection map |

**Total uncompressed: 1,039,684 B (1.0 MB)**

### traffic.zip (350,822 B compressed)

31 traffic vehicle models + 31 skin textures = 62 files.

| File Pattern | Count | Size Each | Format |
|-------------|-------|-----------|--------|
| `model0.prr` - `model30.prr` | 31 | 11,872 - 22,432 B | PRR mesh |
| `skin0.tga` - `skin30.tga` | 31 | 17,196 B each | TGA texture |

**Total uncompressed: ~1,085,600 B (1.0 MB)**

### environs.zip (49,051 B compressed)

27 environment/billboard TGA textures for per-track scenery objects.

| File | Size | Description |
|------|------|-------------|
| `TREE.TGA` | 9,004 B | Generic tree billboard |
| `BRIDGE.TGA` | 9,004 B | Generic bridge texture |
| `SUN.TGA` | 17,196 B | Sun/lens flare sprite |
| `MSUN.TGA` | 17,196 B | Alternate sun sprite |
| `SUNBKP.TGA` | 17,196 B | Sun backup/alternate |
| `ATRE1.TGA` | 9,004 B | Track A tree variant 1 |
| `KTRE1.TGA` | 9,004 B | Track K tree variant 1 |
| `MTRE1.TGA` | 9,004 B | Track M tree variant 1 |
| `MBRD1.TGA` | 9,004 B | Track M bridge variant |
| `MBWL1.TGA` | 9,004 B | Track M barrier/wall |
| `MTUN1.TGA` | 9,004 B | Track M tunnel |
| `ZTUN1.TGA` | 9,004 B | Track Z tunnel |
| `YSHP1.TGA` | 9,004 B | Track Y shape variant |
| `WTUN1.TGA` | 9,004 B | Track W tunnel |
| `WBRD1.TGA` | 9,004 B | Track W bridge |
| `UTUN2.TGA` | 9,004 B | Track U tunnel 2 |
| `UTUN1.TGA` | 9,004 B | Track U tunnel 1 |
| `SBRD1.TGA` | 9,004 B | Track S bridge |
| `QTUN1.TGA` | 9,004 B | Track Q tunnel |
| `NTUN1.TGA` | 9,004 B | Track N tunnel |
| `NBRD2.TGA` | 9,004 B | Track N bridge 2 |
| `NBRD1.TGA` | 9,004 B | Track N bridge 1 |
| `KTUN1.TGA` | 9,004 B | Track K tunnel |
| `ITUN1.TGA` | 9,004 B | Track I tunnel |
| `HTUN2.TGA` | 9,004 B | Track H tunnel 2 |
| `HTUN1.TGA` | 9,004 B | Track H tunnel 1 |
| `CTUN1.TGA` | 9,004 B | Track C tunnel |

**Total uncompressed: 267,300 B (261 KB)**

### legals.zip (248,145 B compressed)

| File | Size | Format | Purpose |
|------|------|--------|---------|
| `LEGAL1.TGA` | 614,444 B | TGA | Legal splash screen page 1 |
| `LEGAL2.TGA` | 614,444 B | TGA | Legal splash screen page 2 |

**Total uncompressed: 1,228,888 B (1.2 MB)**

### loading.zip (3,918,959 B compressed)

21 loading screen images -- one per track plus a duplicate.

| File | Size | Format |
|------|------|--------|
| `LOAD00.TGA` - `LOAD19.TGA` | 614,444 B each | TGA (track loading screens) |
| `Load04.tga` | 614,444 B | Duplicate of LOAD04 (mixed case) |
| `../load04.tga` | 921,644 B | Higher-resolution variant (path escape!) |

**Total uncompressed: 12,903,324 B (12.3 MB)**

Loading screen index corresponds to the schedule index (0-19), matching tracks
in cup progression order.

### Cup.zip (195,059 B compressed)

Installer/configuration archive. Contains DLLs and config for the game launcher.

| File | Size | Purpose |
|------|------|---------|
| `Config.td5` | 5,351 B | Binary game configuration blob |
| `CupData.td5` | 12,966 B | Cup/championship progression data |
| `Language.dll` | 49,152 B | Localization strings DLL (SNK_* exports) |
| `M2DX.dll` | 184,320 B | 3D rendering engine (DirectDraw/Direct3D) |
| `M2DXFX.dll` | 151,552 B | Rendering effects extension DLL |
| `settings.txt` | 1,706 B | ASCII help text for launcher settings |
| `TD5.ini` | 3,055 B | Game settings INI (video, audio, controls) |
| `Uninst.isu` | 29,454 B | InstallShield uninstall script |

**Total uncompressed: 437,556 B (427 KB)**

---

## 6. Front End Directory {#front-end-directory}

### data/Front End/ Root

| File | Size | Purpose |
|------|------|---------|
| `MenuFont.tga` | 291,408 B | Menu font glyph atlas (TGA) |
| `frontend.zip` | 484,817 B | Main frontend UI textures |

### frontend.zip Contents (58 files)

All TGA textures for menus, buttons, and UI elements.

| File | Uncompressed Size | Purpose |
|------|-------------------|---------|
| `explogo.tga` | 308,304 B | Publisher/developer logo |
| `Logo.tga` | 307,986 B | Game logo |
| `MainMenu.tga` | 308,304 B | Main menu background |
| `LanguageScreen.TGA` | 308,304 B | Language selection background |
| `LegalScreen.TGA` | 308,304 B | Legal screen background |
| `TrackSelect.tga` | 308,304 B | Track selection background |
| `NetMenu.tga` | 308,304 B | Network menu background |
| `RaceMenu.tga` | 308,304 B | Race menu background |
| `mainfont.tga` | 291,408 B | Main font atlas |
| `smalfont.TGA` | 177,504 B | Small font atlas |
| `BodyText.tga` | 133,584 B | Body text overlay |
| `OldBodyText.tga` | 133,584 B | Old body text (unused?) |
| `Language.tga` | 91,216 B | Language selector UI |
| `Untitled-1.tga` | 61,776 B | Unknown/debug asset |
| `smalltext.tga` | 34,368 B | Small text atlas |
| `smalltextb.tga` | 34,368 B | Small text atlas variant B |
| `CarSelTopBar.tga` | 20,256 B | Car selection top bar |
| `Controllers.TGA` | 15,440 B | Controller icons |
| `ControllersO.tga` | 15,440 B | Controller icons (alternate) |
| `Copy of Controllers.TGA` | 15,440 B | Controller icons (copy) |
| `SelectCompCarText.tga` | 11,612 B | "Select Computer Car" text |
| `Small TD5 Wht.TGA` | 11,344 B | Small TD5 logo (white) |
| `CarSelBar1.tga` | 10,604 B | Car selection bar |
| `AreYouSureText.TGA` | 8,504 B | "Are you sure?" dialog text |
| `NoControllerText.TGA` | 8,624 B | "No controller" error text |
| `TrackSelectText.TGA` | 8,064 B | Track selection label |
| `ResultsText.tga` | 7,784 B | Results screen text |
| `HighScoresText.TGA` | 7,264 B | High scores text |
| `ButtonBits.tga` | 6,704 B | Button UI fragments |
| `SelectCarText.tga` | 6,532 B | "Select Car" text |
| `3D Sound.tga` | 6,480 B | 3D sound icon |
| `inprog.tga` | 6,480 B | "In Progress" indicator |
| `Mono.tga` | 6,480 B | Mono audio icon |
| `Stereo.tga` | 6,480 B | Stereo audio icon |
| `QuickRaceText.tga` | 6,344 B | Quick race text |
| `RaceMenuText.TGA` | 6,304 B | Race menu text |
| `MainMenuText.TGA` | 6,064 B | Main menu text |
| `CarSelCurve.tga` | 5,292 B | Car selection curve graphic |
| `SplitScreen.tga` | 5,200 B | Split screen mode icon |
| `OptionsText.tga` | 5,104 B | Options text |
| `Player1Text.TGA` | 5,052 B | "Player 1" text |
| `Player2Text.TGA` | 5,052 B | "Player 2" text |
| `NetPlayText.TGA` | 5,344 B | Network play text |
| `ArrowButtons.TGA` | 4,240 B | Arrow button sprites |
| `VolumeBox.tga` | 3,792 B | Volume control box |
| `VolumeFill.tga` | 3,324 B | Volume bar fill |
| `InfoText.tga` | 3,224 B | Info text overlay |
| `JoypadIcon.tga` | 3,152 B | Joypad icon |
| `JoystickIcon.tga` | 3,152 B | Joystick icon |
| `KeyboardIcon.TGA` | 3,152 B | Keyboard icon |
| `ArrowExtras.tga` | 2,112 B | Arrow extra sprites |
| `tick.TGA` | 2,128 B | Checkmark/tick icon |
| `SnkMouse.TGA` | 1,764 B | Mouse cursor sprite |
| `ButtonLights.tga` | 1,616 B | Button highlight effects |
| `ArrowButtonz.TGA` | 1,536 B | Arrow button variant |
| `GraphBars.tga` | 1,384 B | Graph bar elements |
| `Positioner.tga` | 1,264 B | Position indicator |

### Front End/Extras/Extras.zip (10 files)

Band photos and gallery images for the Extras/Jukebox screen.

| File | Size | Content |
|------|------|---------|
| `Junkie XL.TGA` | 51,412 B | Band photo |
| `Gravity Kills.tga` | 51,412 B | Band photo |
| `Fear Factory.TGA` | 51,412 B | Band photo |
| `KMFDM.tga` | 51,280 B | Band photo |
| `PitchShifter.tga` | 51,280 B | Band photo |
| `Pic1.tga` | 63,040 B | Gallery image |
| `Pic2.tga` | 66,000 B | Gallery image |
| `Pic3.tga` | 124,980 B | Gallery image |
| `Pic4.tga` | 46,704 B | Gallery image |
| `Pic5.tga` | 66,304 B | Gallery image |

### Front End/Extras/Mugshots.zip (27 files)

Developer team mugshots + legal screen images for credits.

| File | Size | Content |
|------|------|---------|
| `Bob.tga` | 143,404 B | Developer mugshot |
| `Gareth.tga` | 143,404 B | Developer mugshot |
| `Snake.tga` | 143,404 B | Developer mugshot |
| `MikeT.tga` | 143,404 B | Developer mugshot |
| `Chris.tga` | 143,404 B | Developer mugshot |
| `Headley.tga` | 143,404 B | Developer mugshot |
| `Steve.tga` | 143,404 B | Developer mugshot |
| `Rich.tga` | 143,404 B | Developer mugshot |
| `Mike.tga` | 143,404 B | Developer mugshot |
| `Bez.tga` | 143,404 B | Developer mugshot |
| `Les.tga` | 143,404 B | Developer mugshot |
| `TonyP.tga` | 143,404 B | Developer mugshot |
| `JohnS.tga` | 143,404 B | Developer mugshot |
| `DavidT.tga` | 143,404 B | Developer mugshot |
| `TonyC.tga` | 143,404 B | Developer mugshot |
| `DaveyB.tga` | 143,404 B | Developer mugshot |
| `ChrisD.tga` | 143,404 B | Developer mugshot |
| `Slade.tga` | 143,404 B | Developer mugshot |
| `Matt.tga` | 143,404 B | Developer mugshot |
| `Marie.tga` | 143,404 B | Developer mugshot |
| `JFK.tga` | 143,404 B | Developer mugshot |
| `Daz.tga` | 143,404 B | Developer mugshot |
| `Legals1.tga` | 143,404 B | Legal credits image |
| `Legals2.tga` | 143,404 B | Legal credits image |
| `Legals3.tga` | 143,404 B | Legal credits image |
| `Legals4.tga` | 143,404 B | Legal credits image |
| `Legals5.tga` | 143,404 B | Legal credits image |

**Total uncompressed: 3,871,908 B (3.7 MB)**

### Front End/Sounds/Sounds.zip (6 files)

Frontend menu sound effects.

| File | Size | Purpose |
|------|------|---------|
| `crash1.wav` | 31,638 B | Menu crash/impact SFX |
| `Ping1.wav` | 4,562 B | Menu ping 1 |
| `ping2.wav` | 4,562 B | Menu ping 2 |
| `ping3.wav` | 4,562 B | Menu ping 3 |
| `whoosh.wav` | 8,934 B | Menu whoosh transition |
| `uh-oh.wav` | 13,650 B | Error/locked item SFX |

**Total uncompressed: 67,908 B (66 KB)**

### Front End/Tracks/Tracks.zip (20 files)

Track selection preview thumbnails.

| File | Size | Notes |
|------|------|-------|
| `trak0000.tga` - `trak0019.tga` | 35,152 B each (trak0019: 34,860 B) | Track preview thumbnails |

20 files = one per schedule index. `trak0019.tga` is slightly smaller (34,860 B
vs 35,152 B for all others).

**Total uncompressed: ~703,128 B (687 KB)**

---

## 7. Movie Files {#movie-files}

```
data/movie/
└── intro.tgq              24,903,200 B (23.7 MB)
```

Single FMV intro video in TGQ format (a proprietary FMV codec common in late-90s
PC games). This is by far the single largest file in the game data.

---

## 8. Sound Directory {#sound-directory}

```
data/sound/
└── SOUND.ZIP               1,431,675 B (1.4 MB)
```

### SOUND.ZIP Contents (25 files)

Shared ambient and collision sound effects loaded every race.

| File | Size | Purpose |
|------|------|---------|
| `rain.wav` | 121,294 B | Rain ambient loop |
| `bottom1.wav` | 75,436 B | Undercarriage impact 1 |
| `bottom2.wav` | 51,974 B | Undercarriage impact 2 |
| `bottom3.wav` | 53,000 B | Undercarriage impact 3 |
| `bottom4.wav` | 46,394 B | Undercarriage impact 4 |
| `hhit1.wav` | 42,536 B | Hard collision hit 1 |
| `hhit2.wav` | 40,254 B | Hard collision hit 2 |
| `hhit3.wav` | 35,474 B | Hard collision hit 3 |
| `hhit4.wav` | 46,848 B | Hard collision hit 4 |
| `lhit1.WAV` | 41,928 B | Light collision hit 1 |
| `lhit2.WAV` | 70,730 B | Light collision hit 2 |
| `lhit3.WAV` | 43,204 B | Light collision hit 3 |
| `lhit4.WAV` | 68,524 B | Light collision hit 4 |
| `lhit5.WAV` | 69,078 B | Light collision hit 5 |
| `scrapeX.wav` | 9,620 B | Scrape/grinding sound |
| `gear1.wav` | 13,628 B | Gear shift |
| `skidbit.wav` | 40,600 B | Tire skid/screech |
| `engine0.wav` | 86,206 B | Traffic engine variant 0 |
| `engine1.wav` | 86,206 B | Traffic engine variant 1 |
| `engine2.wav` | 86,206 B | Traffic engine variant 2 |
| `engine3.wav` | 86,206 B | Traffic engine variant 3 |
| `engine4.wav` | 86,206 B | Traffic engine variant 4 |
| `engine5.wav` | 86,206 B | Traffic engine variant 5 |
| `siren3.wav` | 169,738 B | Police siren (long loop) |
| `siren5.wav` | 14,094 B | Police siren (short) |

**Total uncompressed: 1,651,796 B (1.6 MB)**

---

## 9. Standalone Config Files {#standalone-config-files}

| File | Size | Purpose |
|------|------|---------|
| `data/Config.td5` | 5,351 B | Binary game configuration (graphics, audio, controls) |
| `data/TD5.ini` | 7 B | Contains "Window" -- window mode flag |

---

## 10. Size Breakdown by Category {#size-breakdown}

### On-Disk (Compressed) Sizes

| Category | File Count | Size | % of Total |
|----------|-----------|------|------------|
| Level ZIPs (track data) | 20 | 58.4 MB | 52.6% |
| Movie (intro.tgq) | 1 | 23.7 MB | 21.4% |
| Car ZIPs | 37 | 21.1 MB | 19.0% |
| Loading screens (loading.zip) | 1 | 3.7 MB | 3.4% |
| Front End (all) | 8 | 1.9 MB | 1.7% |
| Sound (SOUND.ZIP) | 1 | 1.4 MB | 1.2% |
| Traffic (traffic.zip) | 1 | 0.3 MB | 0.3% |
| Legal screens (legals.zip) | 1 | 0.2 MB | 0.2% |
| Cup.zip (installer data) | 1 | 0.2 MB | 0.2% |
| Static (static.zip) | 1 | 0.1 MB | <0.1% |
| Environs (environs.zip) | 1 | 0.05 MB | <0.1% |
| Config files | 2 | 0.005 MB | <0.1% |
| **Total** | **75 files** | **111.1 MB** | **100%** |

### Uncompressed Sizes (Estimated)

| Category | Uncompressed |
|----------|-------------|
| Level ZIPs | ~228 MB |
| Car ZIPs (37 x ~1.5 MB) | ~55 MB |
| Loading screens | 12.3 MB |
| Movie (already uncompressed stream) | 23.7 MB |
| Sound | 1.6 MB |
| Traffic | ~1.0 MB |
| Static | 1.0 MB |
| Front End (all ZIPs) | ~5.5 MB |
| Other | ~2.5 MB |
| **Total estimated uncompressed** | **~330 MB** |

---

## 11. Missing / Gap Track Numbers {#missing-track-numbers}

### Level ZIP Number Gaps

The level ZIPs use non-contiguous numbering. Present vs missing:

```
001-006: PRESENT (6 files) -- Tier 1 circuit tracks
007-012: MISSING (6 numbers) -- Gap between tier 1 and tier 2
013-017: PRESENT (5 files) -- Tier 2 circuit tracks
018-022: MISSING (5 numbers) -- Gap between tier 2 and tier 3
023:     PRESENT (1 file)   -- Moscow circuit (tier 3)
024:     MISSING             -- Gap
025-029: PRESENT (5 files)  -- P2P tracks block 1
030:     PRESENT (1 file)   -- Drag strip
031-036: MISSING (6 numbers) -- Gap
037:     PRESENT (1 file)   -- House of Bez P2P
038:     MISSING             -- Gap
039:     PRESENT (1 file)   -- Montego Bay P2P
040+:    MISSING             -- No further levels
```

### Analysis of Gaps

The numbering scheme suggests a **tier-based grouping** with 12-number blocks:

| Range | Block | Content |
|-------|-------|---------|
| 001-012 | Block 1 (circuits, tier 1) | 6 present, 6 empty slots (007-012) |
| 013-024 | Block 2 (circuits, tier 2-3) | 6 present, 6 empty slots (018-022, 024) |
| 025-036 | Block 3 (point-to-point) | 6 present, 6 empty slots (031-036) |
| 037-048 | Block 4 (P2P overflow?) | 2 present (037, 039), rest empty |

**Interpretation:** The numbering likely allocated 12 slots per track tier/type
during development. The gaps (007-012, 018-022, 031-036) represent tracks that
were planned but cut from the final release, or reserved slots that were never
used. The shipped game has exactly 20 playable routes (12 circuits that can be
raced both forward and reverse = 24 circuit variants, plus 7 one-way P2P tracks,
plus the drag strip = 32 total race configurations).

### Track Preview Coverage

The `Tracks.zip` in Front End contains `trak0000.tga` through `trak0019.tga`
(20 previews), confirming exactly 20 schedule slots in the final game. This
matches the 19 pool indices (0-18) plus the drag strip (pool 19 / level030).

---

## Cross-Reference Notes

- Full level ZIP file format documentation: `re/analysis/level-zip-file-formats.md`
- Car ZIP and static.zip format documentation: `re/analysis/car-zip-and-static-zip-contents.md`
- Per-track checkpoint/lighting data tables: `re/analysis/data-tables-decoded.md`
- 3D mesh format (PRR): `re/analysis/3d-asset-formats.md`
- Track names and localization: `SNK_TrackNames` array in `Language.dll` (DLL VA 0x10007234)
- Pool-to-ZIP mapping table: `0x466D50` in TD5_d3d.exe (int32[19])
- Schedule-to-pool mapping: `gScheduleToPoolIndex` at `0x466894` (byte[20])
