# TD5RE Save / Settings / Progress Architecture

All persistence is plain INI next to the exe. The original game's encrypted binaries (`Config.td5`, `CupData.td5`) are **retired**: imported once at first launch, then renamed `*.migrated` and never read again. Source of truth: `td5mod/src/td5re/td5_save.c` (+ `.h`), `main.c`; legacy binary layout: `re/analysis/formats/save-file-formats.md`.

## 1. The file set

**SETTINGS — `td5re.ini` (dev exe) / `td5re_release.ini` (release exe).** Filename selected by `TD5RE_RELEASE` (`TD5RE_INI_FILENAME`, main.c:153–157) so both exes coexist in one folder. Read once at launch: `td5_load_ini()` resolves the exe-relative path into `s_ini_path`, then `td5_ini_int/td5_ini_float/td5_ini_str` (GetPrivateProfileInt/StringA) fill `g_td5.ini.*` in WinMain (main.c:632+). Written back by `td5_ini_persist_options()` (main.c:230) via `WritePrivateProfileStringA` — in-place per-key edits, other keys untouched.

**STATE — owned by td5_save.c, read in `td5_save_init()` (module init, td5re.c:38):**
- `td5re_input.ini` — `[Devices]` per-player device index (9 players; 0=keyboard, ≥1=joystick), `[ForceFeedback]`, `[Assist] Catchup`, `[ControllerButtons]` raw dwords, `[ControllerActions]` per-action codes, `[KeyboardPlayer1/2]` DIK scancodes. Written by `cfgini_write_input()`, read by `cfgini_read_input()` (which also pushes scancodes to the live input layer via `td5_plat_input_set_keyboard_bindings`).
- `td5re_progress.ini` — `[Unlocks]` (MaxUnlockedCar, AllCarsUnlocked, CupTier, CarLocks 37×, TrackLocks 26× — note inverted senses: car 0=unlocked, track 1=unlocked), `[Cheats]`, `[Audio] MusicTrack`, `[HighScores.TrackNN]` 26 groups × 5 entries. Written by `cfgini_write_progress()`.
- `td5re_cup.ini` — "Continue Cup" resume: `[Cup]` header fields + masters/bitmask tail, `[Cars]` player + 5 AI car indices, `[Schedule]`/`[Results]` (30 dwords each), `[SlotState]`. The original's 12656-byte per-actor physics snapshot is **intentionally not persisted** — `cfgini_read_cup` zeroes `s_actor_table`; the next race re-grids.

**Who writes when:**
- `td5_save_write_config(NULL)` (td5_save.c:853) now means "write input+progress INIs" (its path arg is ignored). Called on Controller Options OK / binding save (td5_fe_menu.c:1785, 2385), post-race high-score flush (td5_fe_race.c:2236), name-entry commit (3170), cup won (3265).
- `td5_save_write_cup_data(NULL)` → `cfgini_write_cup` — called from `frontend_write_cup_data` (td5_fe_race.c:441; race-results save dialog at 2816), always preceded by `td5_save_sync_cup_from_game()`. `td5_save_delete_cup_data()` (td5_save.c:2054) removes the INI **and** any lingering legacy binary after a cup is won/abandoned.
- Cup read: Continue Cup button → `td5_save_load_cup_data(NULL)` (td5_fe_menu.c:191) → `cfgini_read_cup` → `td5_save_sync_cup_to_game()`. "Continue Cup" availability: `td5_save_is_cup_valid()` (valid INI `GameType>0`, or intact unmigrated binary).
- `td5_ini_persist_options()` is called on Game/Sound/Display Options OK (td5_fe_menu.c:1635, 1882, 2002), Quick Race OK — laps (td5_fe_race.c:668, 2073), paint-colour commit (1400/1406), and pause-menu close/quit/exit when a volume slider changed (`s_pause_options_dirty`, td5_game.c:3820/3838/3906).

## 2. Precedence: CLI > INI > built-in default

Every settings key has a `--Key=N` CLI override. The table is `CliOverride table[]` inside `td5_apply_cli_overrides()` (main.c:300–396), applied in WinMain **after** `td5_load_ini` (main.c:887). Keys are case-insensitive, values `atoi`'d; `Width`/`Height`/`Windowed` are WinMain locals handled via out-params; `--help`/`-h` prints the key list. Built-in defaults are the fallback args at each `td5_ini_int` call site (main.c:632+).

A second layer: at frontend init (td5_frontend.c:8541–8572) `g_td5.ini.*` values are pushed into the save module's runtime statics (`td5_save_set_speed_units/_sfx_volume/...`), so td5re.ini **overrides** anything imported from the legacy Config.td5. `td5_ini_persist_options` exists precisely to write in-game changes back so this boot-override doesn't mask them on the next launch.

## 3. One-time legacy import (Config.td5 / CupData.td5)

Both binaries are XOR-encrypted (`byte ^ key[i % len] ^ 0x80`, self-inverse) + CRC-32 (0xEDB88320, placeholder `0x10,0,0,0` before computing). Keys: `TD5_CONFIG_XOR_KEY` / `TD5_CUPDATA_XOR_KEY` (td5_types.h:146–147). Full byte layout in `re/analysis/formats/save-file-formats.md`.

- **Config.td5** (5351 B): in `td5_save_init()` (td5_save.c:661–687) — if input+progress INIs exist, read them; else if `Config.td5` decodes OK (`td5_save_load_config`), emit the two INIs and rename to `Config.td5.migrated`; else seed INIs from defaults. After load, cars + race tracks 0–19 are force-unlocked regardless of saved content (td5_save.c:689–696); cup tracks 20–25 stay lock-gated.
- **CupData.td5** (12966 B legacy, 12998 B extended): in `td5_save_load_cup_data()` (td5_save.c:1385) — if `td5re_cup.ini` is absent and `CupData.td5` exists, `cup_load_binary_file` decodes it, `cfgini_write_cup` emits the INI, binary renamed `CupData.td5.migrated`.
- **Extended format (import-time only now):** port-written binaries appended a 32-byte overlay at +0x32A6 — magic `"TD5RE_v1"` (`TD5_CUPDATA_OVERLAY_MAGIC`) + 6 LE car indices. `cup_deserialize_from_buffer` (td5_save.c:1201) detects it, fills `s_overlay_car_indices`, and defensively scrubs the three pointer-shaped actor fields (+0x1B0/+0x1B8/+0x1BC per 0x388 slot) so stale pointers can't be dereferenced before re-init; `td5_save_sync_cup_to_game` then pushes the indices into `g_td5.car_index`/`ai_car_indices[1..5]`. The INI path replicates this via the `[Cars]` section (`s_overlay_present=1` in `cfgini_read_cup`). The binary **serializers** (`config_serialize_to_buffer`, `cup_serialize_to_buffer`) are retained but `__attribute__((unused))` — never reached at runtime.

## 4. Persistence flow

All persistence is **event-driven, immediate** — there is no save-on-shutdown pass (`td5_save_shutdown` is a no-op; WinMain after the loop only calls `td5re_shutdown()` + `td5_plat_log_flush()`, main.c:1328–1331). Options write at OK; high scores/unlocks at race end; cup state at the results save dialog; pause-volume changes latch in `s_pause_options_dirty` and flush when the pause menu closes or on the quit/exit paths. State INIs are written whole-file via `cfgini_flush` (raw `td5_plat_file_write` then `td5_plat_ini_flush` to invalidate the Win32 profile cache, td5_save.c:1664).

## 5. Gotcha: clean shutdown vs kill

Log lines (frontend.log etc.) are buffered and flushed only on ERROR or every 256 lines (td5_platform_win32.c:4961); the full flush happens at clean shutdown (main.c:1331). **Killing the process loses buffered `[save]` log lines** (and skips event-driven persists that ride exit paths, e.g. pause EXIT GAME). When verifying headlessly, end runs with `CloseMainWindow`, not `Kill`. Settings written via the Win32 profile API are also subject to OS-side caching; treat presence-on-disk checks after a kill as unreliable.

## 6. Key entry points

| Function | File:Line | Role |
|---|---|---|
| `td5_save_init` | td5_save.c:627 | Module init: defaults → read INIs / one-time Config.td5 import → force-unlock |
| `td5_save_write_config` | td5_save.c:853 | Persist input + progress INIs (path arg ignored) |
| `td5_save_write_cup_data` / `_load_cup_data` | td5_save.c:1331 / 1385 | Cup INI write / read (+ one-time CupData.td5 import) |
| `td5_save_sync_cup_from_game` / `_to_game` | td5_save.c:1465 / 1502 | Bridge g_td5 + actor table ↔ save statics |
| `td5_save_is_cup_valid` / `_delete_cup_data` | td5_save.c:1588 / 2054 | Continue-Cup button gate / cup discard |
| `cfgini_write/read_input,progress,cup` | td5_save.c:1723–2052 | The actual INI serializers |
| `td5_load_ini`, `td5_ini_int/str/float` | main.c:159–196 | Settings INI path + readers |
| `td5_ini_persist_options`, `td5_ini_write_str` | main.c:230 / 224 | Settings write-back (options OK, pause, nickname) |
| `td5_apply_cli_overrides` | main.c:300 | `--Key=N` override table (CLI > INI > default) |
| `td5_save_test_cup_roundtrip` | td5_save.c:2371 | Dev self-test (`--TestCupRoundtrip=1`) |
