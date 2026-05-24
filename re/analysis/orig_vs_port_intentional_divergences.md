# Intentional Divergences — pre-seed for orig-vs-port verdict sweep (2026-05-24)

This reference lists known, deliberate divergences between TD5_d3d.exe (orig) and
td5re.exe (port). When an agent finds a divergence that matches one of these
classes, the verdict is **INTENTIONAL**, not **OVERSIGHT**. The agent should
cite the class tag in the verdict_evidence column.

Sources: `CLAUDE.md`, `MEMORY.md`, `re/analysis/l5_audit_reports/`,
`re/analysis/permanent_l4_residual.md`.

## Graphics / Surface backend

| Class tag | Description |
|---|---|
| `D3D11_BACKEND` | Original is DirectDraw + Direct3D 3 (16bpp primary surface, per-scanline blits). Port is a D3D11 wrapper (`ddraw_wrapper/`) with full-viewport quad submission. Any DDraw `Blt`, `Lock`, `BltColorFill`, `CrossFade16BitSurfaces`, primary/secondary surface logic, palette setup → INTENTIONAL. |
| `SURFBLIT` | DDraw 16bpp primary→secondary `Blt(0x1c)` or `Blt(0x11)` calls. Port draws each frame from scratch into the D3D11 backbuffer; no secondary surface exists. |
| `FONTSTR` | Per-glyph DDraw blit (BodyText 24x24, SmallText 12x12, MediumText 18x18) with control-code <0x20 subglyphs. Port consolidates into `fe_draw_text` glyph-strip via `s_font_glyph_advance` table. Per-glyph advance-sum measurement preserved. |
| `GALLERY` | Extras mugshot/gallery: orig batch-loads `pic*.tga` at init + crossfades per scanline. Port uses on-demand loader + fixed-interval `fe_draw_quad` advance. |
| `BUTTON_CACHE` | Phase 6 parity: `td5_frontend_button_cache.c` bakes 224×64 main-menu button surfaces CPU-side; orig does GPU-side. |

## Audio

| Class tag | Description |
|---|---|
| `M2DX_AUDIO` | Original uses `M2DX.dll` middleware (DXD3D + DXSound + CD audio). Port wraps DXSound directly via `td5_sound.c`. Any `DXSound::CanDo3D()`-gated 3-mode SFX cycle, CDPlay scheduling, etc., that the port simplifies → INTENTIONAL. |
| `TRANSLUCENT_HUD` | Port introduced `TRANSLUCENT_LINEAR_HUD` preset (`alpha_ref=1`) for `hud_submit_quad`; preserves orig 15+ non-HUD users of `LINEAR` (alpha_ref=0). Matches `M2DX.dll DXD3D::SetRenderState @ 0x10001770` original. |

## FMV

| Class tag | Description |
|---|---|
| `FMV_STUB` | Original loads EA TGQ codec for `movie/*.tgq`. Port (`td5_fmv.c`) is a stub — FMVs are skipped silently. No port equivalent. |

## Networking

| Class tag | Description |
|---|---|
| `DXPTYPE` | DirectPlay protocol architecturally divergent. Orig uses 3 transport types (1/2/4) with nested sub-opcodes inside type-1 payload; port flattens to 13 top-level handlers. Wire-incompatible — port peers cannot interop with orig peers. Lockstep counters live inside M2DX (not data segment); no local-echo ring, no host migration in port. Driver-name stride 0x3c vs 64. Any `RunFrontendConnectionBrowser`, `RunFrontendCreateSessionFlow`, `RunFrontendNetworkLobby` divergence → INTENTIONAL. |
| `DIRECTPLAY_UI` | Orig screens 4-15 of network flow are case-per-state; port collapses to single dispatch to `TD5_SCREEN_NETWORK_LOBBY`. |

## Replay / Save

| Class tag | Description |
|---|---|
| `M2DX_REPLAY` | Original delegates all replay I/O to `M2DX.dll`. Port must construct primitives in `td5_input.c` directly. Format wire-incompatible. |
| `SAVE_FORMAT` | Config.td5 / CupData.td5 XOR encryption keys preserved, but layout reconstructed (some fields renamed/relocated). |

## RNG / Determinism

| Class tag | Description |
|---|---|
| `MSVC_RAND_OVERRIDE` | Port ships `td5_msvc_rand.c` overriding mingw `rand()`/`srand()` with MSVC LCG (`0x343FD`, `0x269EC3`) for parity. Necessary substrate, but NOT sufficient — divergent rand-call patterns elsewhere can still produce different sequences. Functions whose only divergence is in RNG-dependent output (cardef seed → different AI car) → INTENTIONAL (with note). |
| `FPU_PC64_FWRAPV` | Port forces `_controlfp` to PC=64 and compiles with `-fwrapv` to match orig empirical FPU CW=0x077F and IMUL overflow wrap semantics. Code paths that depend on this are INTENTIONAL by design. |
| `F32_SPILL` | `TD5_F32_SPILL` macro on `rotation_matrix` intermediate FPU products explicitly `fstps` to float32 to mirror orig 0x42E1E0 spill behavior (GCC PR323 ignores volatile). |

## AI / Solo mode

| Class tag | Description |
|---|---|
| `SOLO_PEER_BIAS_EMULATION` | `td5_ai_update_track_offset_bias` synthesizes peer-push (+38/5 ticks) at the no-peer path when running solo PlayerIsAI. Closes "AI very unstable in TT solo" cascade. Orig never has solo path because it always has 6 racers. |
| `REVERSE_AWARE_RECOVERY` | Script flag-0x10 handler applies forward throttle (0x00FF) when `lspd<=-0x100`. Orig applies reverse throttle (0xFF00) which can't slow a reversing car. Port intentionally diverges to fix cascade. |
| `PHANTOM_PEER_GATED` | `td5_ai_setup_phantom_peer_for_solo0` and `--PhantomPeer=N` flag exist but default OFF (`PhantomPeer=0`). v1 emulation byte-identical at gate=0. |
| `INPUT_WRITEBACK_GATED` | `td5_input_update_player_control` skips actor+0x30C writeback for slot==0 && PlayerIsAI to avoid AI-cascade reset. |
| `WHEEL_DRIVE_SYMMETRY` | RWD airborne dispatch uses `wheel_drive[3]` (rear) symmetry; orig used `wheel_drive[0]`. Closes self-steer + slow-accel cascade. |

## Track / Physics

| Class tag | Description |
|---|---|
| `CHECKPOINT_CIRCUIT_GATE` | `adjust_checkpoint_timers` gated on `track_type != TD5_TRACK_CIRCUIT`. Orig runs unconditionally; port chose to skip on circuits (no checkpoint timer expected). |
| `FWD_TRACK_COMP_SQRT` | `fwd_track_comp` divides by `sqrt(sin²+cos²)` (matches orig FILD/FSQRT/__ftol), not `>>12`. Was a port bug, now byte-faithful. |
| `LATERAL_DIR_FIX` | `g_lateral_avoidance_direction` comparison in `find_offset_peer`: port uses `(abs_l >= abs_r)` matching orig SETGE. |
| `IRSS_RAND_ALIGNED` | `InitializeRaceSeriesSchedule` rand sequence byte-aligned to orig (21 calls); `tools/_probes/orig_rand_trace.js` keep-alive. |

## CRT / Toolchain

| Class tag | Description |
|---|---|
| `MINGW_CRT` | Functions in the `0x00448000+` address range (e.g., `__fpmath_init`, `_rand`, `_malloc`, `__ftol`, `__cfltcvt_init`, `__set_new_handler`) are MSVC CRT entries embedded in the orig binary. Port links against MinGW CRT (with `td5_msvc_rand.c` override for rand). These are INTENTIONAL — toolchain-provided, not part of the source port surface. |
| `STDLIB_INLINE` | Compiler-inlined memcpy/memset/strcpy variants in orig become libc calls in port. INTENTIONAL. |
| `ZLIB_REPLACE` | Original uses an embedded inflate routine; port uses zlib (`-DTD5_INFLATE_USE_ZLIB`). |

## Known regressions (NOT intentional — these are OVERSIGHT)

These are explicitly bugs in the port, documented in `permanent_l4_residual.md`.
Agent must classify any function whose divergence matches one of these as
**OVERSIGHT** with the regression ID cited:

| Reg ID | Function | Bug summary |
|---|---|---|
| REGR#1 | `RaceTypeCategoryMenuStateMachine` (0x004168B0) | button 3↔4 swap: port TimeTrials=7/Drag=9, orig Drag=7/Trials=9 |
| REGR#2 | `ScreenSoundOptions` (0x0041EA90) | SFX `^=1` (2-mode) vs orig 3-mode cycle; volume `*5` vs `*10` step |
| REGR#3 | `ConfigureActorProjectionEffect` (0x0040CBD0) | mode-1 SetProjectionEffectState param_3 vector choice ambiguous; port picks linear_velocity |
| REGR#4 | `UpdateStaticTracksideCamera` (0x00402950) | `g_camHeightSampleOfs[2]` never written; should be `g_cameraProfileVertOffset[2]` |
| REGR#5 | `UpdateSplineTracksideCamera` (0x00402AD0) | `s_splineTemplates[6][8]` values don't match orig stack pattern |
| REGR#6 | `UpdateFrontWheelSoundEffects` (0x0043F420) + `UpdateRearWheelSoundEffects` (0x0043F600) | wheel-anchor `+0x298` (hires) vs orig `+0xf0` (probe) — 1-tick lag |

## Verdict taxonomy quick reference

- **FAITHFUL** — port matches orig byte-for-byte or semantically equivalent (logic, constants, control flow, side effects all preserved).
- **INTENTIONAL** — divergence exists AND matches one of the class tags above (cite tag in evidence).
- **OVERSIGHT** — divergence exists, doesn't match an intentional class, and looks like a bug. Cite specific divergence (line, missing write, wrong constant, etc.).
- **NOT_PORTED** — function has no port equivalent at all. Sub-class: was-it-intentional? (CRT/M2DX/FMV → INTENTIONAL/NOT_PORTED dual-tag, otherwise plain NOT_PORTED).
- **CANNOT_DETERMINE** — divergence visible but classification requires runtime trace, Frida probe, or context the agent doesn't have.
