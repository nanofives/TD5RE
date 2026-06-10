# TD5RE Render Flow — End to End

How a race frame gets from simulation to the screen. The port keeps the original game's **software T&L** design: all vertex transform, lighting, clipping, and projection happen on the CPU in `td5_render.c`; the GPU only rasterizes pre-transformed XYZRHW vertices (FVF 0x1C4, 32-byte stride) submitted through the DDraw→D3D11 wrapper (`td5mod/ddraw_wrapper/`). Every claim below cites the source file/function it was read from in this worktree.

## 1. Frame flow: fixed 30 Hz sim, per-frame render

`td5_game_run_race_frame` (`td5mod/src/td5re/td5_game.c`) feeds a normalized frame delta ("1.0 = one 30 Hz simulation tick", td5_game.c:6214) into `g_td5.sim_time_accumulator` (:6243), then drains it in 0x10000 steps — max 4 ticks/frame for spiral-of-death protection (:3603-3626). Inside each tick: input poll, `td5_camera_cache_angles`, physics/AI/track, VFX tick, `update_race_order`, then `td5_camera_solve_tick_all()` (:4327) and the per-tick world-animation advances (sky rotation, billboard strobe, fog fade — deliberately moved out of the render loop so they are FPS- and viewport-count-independent, :4381-4395).

After the drain, render-side interpolation state is computed once per **render frame**: `g_subTickFraction = accumulator / TD5_TICK_ACCUMULATOR_ONE` (:4410, not recomputed while the pause menu is active), `td5_physics_apply_render_interpolation(g_subTickFraction)` lerps every actor's `prev_world_pos→world_pos` into `actor->render_pos` (:4424), and `td5_camera_finalize_all()` re-finalizes the camera with the fresh fraction (:4435).

**Camera model (2026-06 rewrite):** per-tick solve + per-frame interpolate. `td5_camera_solve_tick_all` (td5_camera.c:1754, "Per-sim-tick: solve every active view's camera pose (FPS-independent)") captures each mode's eye/target once per tick; `td5_camera_apply_view(view)` (td5_camera.c:1782) runs per render frame per viewport — it interpolates the solved pose, re-pins car-locked components to the body-mesh extrapolation, and builds the camera basis.

The render pipeline then runs once per frame (td5_game.c:4471-4727): `td5_render_begin_scene()` → single backbuffer clear → **per-viewport loop** (`vp < g_td5.viewport_count`): set viewport + hardware scissor → `td5_camera_apply_view(vp)` → `td5_render_configure_projection(w,h)` → SKY pass (fog off) → OPAQUE pass (fog on, `td5_render_begin_world_pass()` arms deferred-additive capture) → `td5_render_actors_for_view(vp)` → VFX → `td5_render_flush_translucent` / `_flush_projected_buckets` / `_flush_deferred_additive` → per-viewport HUD status text. After the loop: viewport/scissor reset to full screen, full-screen HUD overlays, pause overlay, race fade, `td5_hud_flush_text()`, audio tick, `td5_render_end_scene()`, `td5_plat_present(1)`. The present's effective sync interval is the caller's request AND the Display-options VSync toggle `g_backend.vsync` (td5_platform_win32.c:761-763).

## 2. World pass internals (td5_render.c)

`td5_render_actors_for_view` (:2237, mirrors RenderRaceActorsForView @0x40BD20) does, per viewport:

- **Span-window cull**: the track display-list walk is windowed ±64 spans (`VIEW_DIST_FWD/BACK_SPANS`, :2272) around the viewport player's **normalized** span (+0x82; raw branch spans ≥ ring must not be used as center — the London TD6 bug, :2284-2307), scaled by `view_distance * 0.85 + 0.15` (:2353). AI-spectator panes get ×0.5 distance; TD6 levels 12/22 cap at 0.65 (:2339-2352).
- **Sky** drawn first via `td5_render_draw_sky()` (:2415).
- **Track geometry**: each windowed span's display-list block goes to `td5_render_span_display_list` (:1938, RenderTrackSpanDisplayList @0x431270), which validates each sub-mesh and frustum-culls it by bounding sphere (`td5_render_is_sphere_visible`, :1827) before dispatch.
- **Vehicles**: per actor, `td5_render_apply_track_lighting(slot, actor)` (:2946) then `td5_render_prepared_mesh(mesh)` (:2957), plus shadow quad / brake lights / wheel billboards (statics around :4973-:5778).

**Mesh dispatch**: `td5_render_prepared_mesh` (:2109, RenderPreparedMeshResource @0x4314B0) walks the mesh's `TD5_PrimitiveCmd` list through the 7-entry `s_dispatch_table` (:493: tristrip ×2, projected tri, projected quad, billboard depth-sort, tristrip-direct, quad-direct), consuming vertices sequentially (`tri_count*3 + quad_count*4`). Handlers funnel through `clip_and_submit_polygon` (:962) → `append_projected_triangle` into the immediate batch (1024 verts / 4096 indices, :75-76). Projection is `project_vertex` (:555): reject `vz <= near_clip`, then `sx = vx*focal/vz + cx`. `flush_immediate_internal` (:675) remaps the color LUT, defers type-3 (additive) pages to the post-opaque composite, binds the texture page + blend preset, and calls `td5_plat_render_draw_tris`.

## 3. Wrapper: how draws reach D3D11

Two routes, one upload path:

- **Port-native** (the race path): `td5_plat_render_draw_tris` (td5_platform_win32.c:3989) — `Backend_StreamUpload` then binds `dynamic_vb`/`dynamic_ib`, `vs_pretransformed` (converts XYZRHW to NDC), applies `Backend_ApplyStateCache()`, and issues `DrawIndexed`.
- **COM emulation**: `Dev3_DrawPrimitive` / `Dev3_DrawIndexedPrimitive` (ddraw_wrapper/src/device3.c:566/:628, IDirect3DDevice3 vtable slots 28/29) — same `Backend_StreamUpload` + draw.

`Backend_StreamUpload` (ddraw_wrapper/src/d3d11_backend.c:744) is a **streaming ring buffer**: it appends verts (+16-bit indices) at running offsets `s_vb_ring_offset`/`s_ib_ring_offset` with `Map(WRITE_NO_OVERWRITE)`, falling back to `WRITE_DISCARD` only on wrap or first append, and returns base-vertex/start-index offsets for the draw. Buffers are 16 MB VB / 4 MB IB (:105-106) — several frames of geometry. This replaced a per-draw `Map(WRITE_DISCARD)` of a 128 KB buffer that exhausted the driver's rename ring at split-screen draw counts (2200-3500 draws/frame) and serialized the CPU on the GPU (50-100 ms spikes); full background in `re/analysis/split_screen_perf_diagnostic_2026-06-08.md`.

## 4. Pane system (split-screen)

`td5_game_get_pane_rect(views, v, w, h, ...)` (td5_game.c:6321) is the **single source** for pane rectangles — "the 3D viewport rects, the HUD pane layout and the divider lines so they cannot disagree" (:6318-6320). It row-majors over the grid from `td5_game_resolve_split_grid` (:6297; honors the committed Multiplayer-Options cols×rows, else 2=mode-dependent, 3=1×3, 4=2×2, 5-6=3×2). Consumers: `td5_game_init_viewport_layout` (:6335, fills `s_viewports`; count = humans + spectate panes) and td5_hud.c (:1859/:1869 HUD layout, :2605/:2614 dividers). A live resize re-runs both layouts when render dims change (td5_game.c:4489-4497).

## 5. Depth and fog model

Constants block, td5_render.c:83-137. `DEFAULT_NEAR_CLIP = 32.0f` (orig DAT_00473bbc; polygons with vz ≤ 32 are clipped). `NEAR_DEPTH_OFFSET = 64.0f` (orig subtracts 64 from vz before depth normalization). **Deliberate divergence**: `DEFAULT_FAR_CLIP`/`DEFAULT_FAR_CULL = 195000` and `DEPTH_NORMALIZE_INV = 1/195000` — the original normalized depth by 1/65479 with a draw window that never exceeded it; the port draws to the 195000 frustum far-cull, so the old range clamped everything distant to depth 1.0 (z-fight by draw order). Paired with a D16→**D32_FLOAT** depth buffer (d3d11_backend.c:834) so near precision survives the wider linear range. Fog: `td5_render_set_fog` gates on the per-track fog flag and routes the whole 6-render-state config of ApplyRaceFogRenderState @0x40AF50 into one `td5_plat_render_set_fog(enable, color, start, end, density)` call (td5_render.c:1536-1546); defaults start/end/density = 0.60/1.00/0.40 (:135-137). The backend feeds it to a fog constant buffer (`cb_fog`, d3d11_backend.c:687, bound at td5_platform_win32.c:4024). Sky renders fog-off, world fog-on, HUD fog-off (td5_game.c:4545/4553/4613).

## 6. Track lighting

`td5_render_apply_track_lighting(slot, actor)` (td5_render.c:4422), called per actor right before its mesh dispatch (:2946→:2957). TD6-migrated tracks short-circuit to `td5_render_set_override_daylight()` (no real light zones; :4430-4433). TD5 tracks: actor span (reverse-mirrored via `tl_reverse_mirror_span`) → `update_actor_light_zone` → per-track zone table (`td5_light_zone_track`) → dispatch on `zone->blend_mode` (`tl_apply_case0/1/2`, blend-from-start/end helpers, :4477+), committing ambient intensity + light direction vectors to the render globals consumed by `td5_render_compute_vertex_lighting` (:1715, ComputeMeshVertexLighting @0x43DDF0). Any failure path falls back to `tl_apply_fallback()`.

## 7. VFX and HUD placement

Both run **serially inside the per-viewport loop, after the opaque world** (td5_game.c:4581-4618): tire tracks/marks → ambient rain streaks → `td5_vfx_draw_particles(vp)` → translucent/projected-bucket/deferred-additive flushes → per-viewport HUD status text. Full-screen HUD (`td5_hud_render_overlays`), player-ID overlays, disconnect modal, pause overlay, and race-end fade draw after the loop at full-screen viewport/scissor (:4626-4670), then `td5_hud_flush_text()` (:4677).

## 8. Key entry points

| Function | File | Role |
|---|---|---|
| `td5_game_run_race_frame` | td5_game.c | Tick drain + whole render orchestration (:3602-4727) |
| `td5_camera_solve_tick_all` / `td5_camera_apply_view` | td5_camera.c:1754/:1782 | Per-tick camera solve / per-frame interpolate+basis |
| `td5_game_get_pane_rect` | td5_game.c:6321 | Single source for viewport/HUD/divider rects |
| `td5_render_begin_scene` / `td5_render_end_scene` | td5_render.c:1447/:1488 | Scene brackets, texture-cache aging, RENDERSTAT counters |
| `td5_render_actors_for_view` | td5_render.c:2237 | Per-viewport world pass: span cull, sky, track, actors |
| `td5_render_span_display_list` | td5_render.c:1938 | Track sub-mesh validation + sphere frustum cull |
| `td5_render_prepared_mesh` | td5_render.c:2109 | Command-list walk → 7-entry primitive dispatch table |
| `flush_immediate_internal` | td5_render.c:675 | Batch flush: LUT fixup, additive deferral, texture bind, draw |
| `td5_render_apply_track_lighting` | td5_render.c:4422 | Per-actor light-zone → vertex-lighting globals |
| `td5_plat_render_draw_tris` | td5_platform_win32.c:3989 | Port-native draw: ring upload + DrawIndexed |
| `Dev3_Draw(Indexed)Primitive` | ddraw_wrapper/src/device3.c:566/:628 | COM (IDirect3DDevice3) draw path |
| `Backend_StreamUpload` | ddraw_wrapper/src/d3d11_backend.c:744 | WRITE_NO_OVERWRITE VB/IB ring (DISCARD only on wrap) |
| `td5_plat_present` | td5_platform_win32.c:755 | Present; sync = request AND `g_backend.vsync` |

`td5_render_frame()` (td5_render.c:1427) is a documented no-op — sequencing is owned by td5_game.c.
