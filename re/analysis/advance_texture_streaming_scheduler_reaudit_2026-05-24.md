# AdvanceTextureStreamingScheduler re-audit (0x0040b830) — 2026-05-24

## Verdict
- **CONFIRMED_D3D11_BACKEND (skip)** — the orig function is an on-demand texture-page streaming scheduler that is replaced *as a system* by the port's upfront-load + D3D11 LRU model. Both layers (`td5_asset_load_race_texture_pages` for upload, `td5_render_bind_texture_page` for cache binding) exist and cover the same end-to-end behavior. No 1:1 port of `AdvanceTextureStreamingScheduler` itself is needed.

## Orig behavior (from Ghidra @ 0x0040b830)
`AdvanceTextureStreamingScheduler(int page_id)` is called from two sites:

1. `BindRaceTexturePage` @ `0x0040B660` — when the renderer requests a page whose
   residency byte is 0 (not resident), the scheduler is invoked to make it
   resident before binding to the active D3D3 device.
2. `PreloadLevelTexturePages` @ `0x0040BAE0` — at race-load time, walks
   `g_textureCacheRuntimeCount[7]` pages and forces each into the cache one
   page at a time.

Internally the function:

- Reads the active slot count at `g_textureCacheRuntimeCount + 0x18`.
- If the cache is *not* full, advances the cursor (`+0x18` <- next slot).
- If the cache *is* full, walks the slot-id ring (`g_textureCacheRuntimeCount + 8`)
  back-to-front looking for an eviction victim, with two priorities:
  - any slot whose `age` byte (`+0x05`) is `0xFF` (saturated/coldest) wins
    immediately;
  - otherwise pick the slot with the highest current age (LRU).
- Picks the per-page transparency type (`+0x07` byte → switch on 0/1/2
  yielding IMAGETYPE 3/4/5).
- Calls `DXD3DTexture::LoadRGB(slot_ptr, page_id_offset, IMAGETYPE, dither_flag)`
  to perform the *actual GPU upload* of the page data.
- On success: clears the chosen slot's age byte, writes back the slot↔page id
  mappings, marks status=1, and returns. A `LogReport("Second Time Lucky")`
  fires when the second pass succeeds.
- On failure with `bVar3==true` (first attempt): retries — but first
  *shrinks* the cache by setting `[+0x10] = page_id_offset + iVar9 - 1`
  and mirrors that to the cursor and `g_appExref+0xDC`, then logs
  `"Retrying load: Resetting Max to %d"` and loops once more.
- On failure with `bVar3==false` (second attempt): `LogReport("Failed with %d")`
  and gives up.

So the orig has three responsibilities:
1. **Eviction policy** — saturate-or-LRU slot selection over a fixed-capacity
   ring (capacity 64 per `QueryRaceTextureCapacity`).
2. **On-demand GPU upload** — `DXD3DTexture::LoadRGB` is the page-decode +
   D3D3 surface upload, only fired when a page is actually needed.
3. **Cache shrink-on-fail retry** — under DDraw/AGP VRAM pressure the
   cache cap can be reduced and the load retried.

## Port behavior (from ddraw_wrapper + td5_asset + td5_render)

The port implements the same end-to-end *behavior* (you can bind any
texture page during rendering and get the correct pixels) but
restructured into two stages, neither of which is a 1:1 port of the
scheduler:

- **Upfront upload** — `td5_asset_load_race_texture_pages`
  (td5_asset.c:2245) opens `level%03d.zip`/`TEXTURES.DAT` at race-load and
  uploads *every* page to a dedicated D3D11 `Texture2D`. The wrapper
  (`td5mod/ddraw_wrapper/src/texture2.c`,
  `d3d11_backend.c`) calls `ID3D11Device_CreateTexture2D` once per page;
  there is no per-frame upload path.
- **Bind-time LRU cache** — `td5_render_bind_texture_page`
  (td5_render.c:2808) does the work the orig's `BindRaceTexturePage` +
  `AdvanceTextureStreamingScheduler` jointly do at bind time: searches
  the resident table for the page, otherwise picks a free slot or the
  oldest (highest `age`) slot. The "upload on miss" step is a no-op
  in the port because everything is already on the GPU — the cache slot
  is simply rebound to a different page id.
- **Per-frame aging** — `td5_render_advance_texture_ages`
  (td5_render.c:2752) increments the `age` byte for slots not touched
  this frame and clears `used_this_frame`, mirroring orig
  `AdvanceTexturePageUsageAges` @ `0x0040BA10` (already L4 FAITHFUL in
  the verdict CSV).
- **Capacity** — `TEXTURE_CACHE_SLOTS` is 600 (td5_types.h:115) vs orig's
  hard cap of 64; combined with the fact that D3D11 has effectively
  unlimited VRAM relative to a 1999 game, eviction almost never fires.

The two `LogReport` retry branches and the "shrink-cap-on-fail" path
have no equivalent because the failure mode (DDraw surface alloc fail
on a 64-slot cap) doesn't exist under D3D11.

## Diff analysis
Where they match:
- **LRU selection rule** — port's "find slot with greatest `age`" is the
  same rule as orig's saturated-age + highest-non-saturated tournament,
  modulo the `0xFF`-wins fast path (which is rare in practice and
  collapses to the same answer once everything has been touched).
- **Per-frame aging cadence** — both increment age once per `EndScene`,
  reset on `used_this_frame`.
- **Status byte semantics** — port's `s_texture_cache[i].status` and
  `used_this_frame` mirror the orig's `+0x04`/`+0x05` byte pair.
- **End user observable** — a page id requested by the renderer
  produces the correct pixels.

Where they diverge:
- **When the GPU upload happens** — orig: lazily on first bind, with a
  measurable cost the first time a new page is touched. Port: all
  textures resident from race-start (longer load, zero per-bind cost).
- **Cache cap** — 64 (orig) vs 600 (port); the orig's "shrink-cap-on-
  retry" path is structurally unreachable in the port.
- **Failure handling** — orig's two-pass retry + telemetry strings
  ("Second Time Lucky", "Retrying load: Resetting Max to ...",
  "Failed with %d") are absent. Port logs at bind-time only.
- **Tie-breaker** — orig walks slots back-to-front; port walks front-to-
  back. Determines which slot wins when two slots share `age==max`.
  Affects which texture handle gets rebound but does not change pixel
  output.

None of these diverge in a way that affects *visible* behavior under
the port's assumption (all pages preloaded, cache cap >> page count).
The two cases where they could diverge are:
1. A level with > 600 texture pages — none ship.
2. A custom mod that adds many pages — out of scope.

## Conclusion / recommendation
- Keep current classification: **NOT_PORTED / D3D11_BACKEND**.
- Bump the existing ARCH-DIVERGENCE doc-block at td5_asset.c:3043 to
  explicitly cross-reference `td5_render_bind_texture_page`
  (td5_render.c:2808) as the *behavioral* port of the bind-side half
  of `AdvanceTextureStreamingScheduler`, not just the upload-side. The
  current comment only credits the upload pipeline, which made the
  function look "missing" during the triage scan.
- Frame-pacing / texture pop-in risk: **none expected** because the
  port pre-uploads at race-load; in fact the port should pop *less*
  than the orig, not more.
- No Tier 1.5 / Tier 6 port required.
