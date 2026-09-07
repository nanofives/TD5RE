/**
 * td5_trackgen_internal.h -- PRIVATE shared declarations of the auto-track generator modules.
 *
 * td5_trackgen.c (31k lines) was split 2026-09-06 into themed modules, listed
 * in srcs.txt as td5_trackgen.c + td5_tg_*.c. Everything here is internal to
 * those modules; the public contract stays in td5_trackgen.h. Definitions that
 * were file-scope `static` in the monolith and are used by more than one module
 * lost the `static` and are declared here; anything used by one module only
 * stayed static in that module. Order is the monolith order, so macro and type
 * dependencies keep resolving top-down. GENERATED once by the split tool, then
 * hand-maintained like any header.
 */

#ifndef TD5_TRACKGEN_INTERNAL_H
#define TD5_TRACKGEN_INTERNAL_H

/**
 * td5_trackgen.c -- procedural ("AUTO-GENERATED") track builder (PORT-ONLY).
 *
 * See td5_trackgen.h for the contract. Pipeline:
 *
 *   seeded RNG -> section picker -> centerline nodes -> curvature safety
 *              -> elevation profile -> spans + vertex rows -> STRIP.DAT
 *                                                          -> LEFT/RIGHT.TRK
 *                                                          -> LEVELINF.DAT
 *
 * WHY FINITE (Phase 1) AND NOT STREAMED: appending spans mid-race is blocked by
 * four independent things, none cosmetic --
 *   1. TD5_TrackProbe.span_index is int16 (td5_types.h), so span indices cannot
 *      grow without bound; an endless road needs an index rebase.
 *   2. LEFT/RIGHT.TRK route tables are sized to the ring at load and have no
 *      realloc path; appending spans without route bytes reads out of bounds
 *      (the failure documented at td5_track.c:3503).
 *   3. The minimap builds its segment table once at init from the whole ring
 *      (td5_hud.c), so a growing track needs a sliding/rescaling minimap.
 *   4. AI span arithmetic wraps at +/- span_count/2 (td5_ai.c smart_span_gap),
 *      which sign-flips on a track whose length changes underneath it.
 * Generating a long finite track up front sidesteps all four: every consumer
 * sees an ordinary point-to-point track with a known length.
 *
 * Coordinates are raw signed world units (the renderer divides by 256).
 */
#include "td5_trackgen.h"
#include "td5_track_registry.h"
#include "td5_jobs.h"          /* [S2c] parallel terrain pre-pass */
#include "td5_track.h"         /* streamed-scenery ingest (td5_track_scenery_*) */
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_tg_real_tex.h"   /* real TD5 texture pages (level014), opt-in */
#include "td5_tg_real_tex_city.h"  /* extra city facades: SF/Tokyo/Moscow */
#include "td5_tg_real_tex_r5flora.h"  /* [R5 item 18] tall Moscow park trees */
#include "td5_tg_real_tex_r7city.h"   /* [R7 item 4] more city facade variety */
#include "td5_tg_real_tex_r8var.h"    /* [R8 G1] facades, banners, guardrails */
#include "td5_tg_furniture_tex.h" /* real TD5 lamp/railing/banner pages     */
#include "td5_tg_real_tex_r11signs.h" /* [R11 SIGNS] direction arrow panels */
#include "td5_tg_props_tex.h"     /* [R9 INFRA] TD6 street-furniture pages  */
#include "td5re.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <direct.h>
#include <sys/stat.h>   /* [R14 GENPERF] build stamp: exe identity */
#define LOG_TAG "track"
/* Reserved identity. Level 90 is well clear of the shipped levels (0..40ish)
 * and of td5_trackgen.py's DEFAULT_CUSTOM_LEVEL_BASE (40) so a user's
 * hand-built tracks and the auto track can coexist. */
#define TD5_TG_LEVEL_NUM   90
/* Last custom slot, so manifest-loaded tracks keep the low slots. */
#define TD5_TG_SLOT        (TD5_CUSTOM_TRACK_SLOT_BASE + TD5_CUSTOM_TRACK_MAX - 1)
/* Name shown in the track selector. Defined once: the registry is re-set on
 * every regenerate, and three call sites drifting apart is how the feature
 * ended up with four different names in the first place. Must match the
 * screen-52 title in td5_frontend.c. (Merged from master a648b65d.) */
#define TD5_TG_TRACK_NAME  "AUTO TRACK STUDIO"
/* Span the start/finish line sits on. The grid places the six racers STAGGERED
 * BEHIND the start line, so starting at span 0 puts the back of the grid on
 * negative spans that do not exist on a point-to-point track (observed:
 * span=-9, no wheel contact). Leave road behind the line. */
#define TD5_TG_GRID_SPAN   24
/* STRIP.DAT pre-span block: 5 header DWORDs + 196 bytes of jump-table space. */
#define TD5_TG_PRE_SPAN_BYTES  196
#define TD5_TG_SPAN_OFFSET     (20 + TD5_TG_PRE_SPAN_BYTES)   /* 216 */
/* Vertex indices in a span record are u16, and each span emits 2*(lanes+1)
 * vertices, so the vertex table -- not the span count -- is the binding
 * ceiling. Keep a margin under 65535. */
/* Spans per shared origin block. Bounded by the int16 vertex offset: the last
 * row of a block sits TD5_TG_ORIGIN_BLOCK * span_length down-track from the
 * origin, plus half the widest road, and that must stay under 32767.
 * 16 * 1500 + ~7500 = 31500, inside 32767 (the emitter hard-checks anyway).
 * Shipped level001 uses runs of ~22 spans per origin. */
#define TD5_TG_ORIGIN_BLOCK   16
/* Span byte flags copied from shipped data (level001: every road span is
 * [type 1, attr 0x11, mask 0x09, packed 0x84]).
 *   attr  0x11 = primary surface 1 / alternate surface 1. Surface type selects
 *                GRIP (td5_track.c:3837); type 0 -- what this generator wrote
 *                at first -- is not the tarmac class shipped roads use.
 *   mask  bit N = lane N uses the alternate surface; shipped marks the two
 *                outer lanes (0x09 for a 4-lane road).
 *   packed high nibble 8 = span_height_offset, only ever read as a DIFFERENCE
 *                between neighbours, so a uniform value is what matters. */
#define TD5_TG_SURFACE_ATTR   0x11
#define TD5_TG_HEIGHT_NIBBLE  8
/* Per-span drivable surface byte + road texture page (biome-themed). Defined
 * after the biome table; forward-declared because the strip/road emitters that
 * use them are defined earlier. */
int tg_surface_attr(int si);
int tg_road_page(int si);
/* Texture page ids, in the order tg_emit_textures writes them. Declared here
 * because the mesh emitters (further up) reference them. */
#define TD5_TG_PAGE_ROAD   0
#define TD5_TG_PAGE_WALL   1   /* facade variant 0 */
#define TD5_TG_PAGE_GREEN  2
#define TD5_TG_PAGE_TREE   3
#define TD5_TG_PAGE_RAIL   4
#define TD5_TG_PAGE_GROUND 5
/* Facade variety: each RUN picks one of N wall pages so a street is not one
 * repeated building. Variant 0 is TD5_TG_PAGE_WALL; variants 1..N-1 live at
 * consecutive pages after GROUND.
 *
 * In real-texture mode the variants come from FOUR shipped city tracks, not one:
 * 0..4 are level014 (Sydney), 5..8 are masonry frontage from San Francisco and
 * Moscow, and 9..11 are Tokyo office curtain wall. The last group is the TOWER
 * class -- tg_facade_page_class hands it to runs tall enough to be a tower, so a
 * downtown block is glass high-rise while the street around it stays masonry.
 * With real textures off every variant is a procedural wall seeded by its index,
 * so the split costs nothing there. */
#define TD5_TG_WALL_VARIANTS    12
#define TD5_TG_WALL_TOWER_FIRST  9
#define TD5_TG_PAGE_WALL_EXTRA 6
/* Storefronts: the GROUND floor of a facade uses a shop page (glass/signage),
 * upper floors the wall page -- shops at street level, tower above, as the
 * shipped city does. Store variants live right after the wall variants: 0..2
 * from level014, 3..5 from San Francisco and Tokyo. */
#define TD5_TG_STORE_VARIANTS  6
#define TD5_TG_PAGE_STORE  (TD5_TG_PAGE_WALL_EXTRA + TD5_TG_WALL_VARIANTS - 1)
/* Thematic trees: a set of distinct tree/palm/conifer/topiary pages so each
 * biome mixes several species. Variant 0 reuses TD5_TG_PAGE_TREE; 1..N-1 live
 * after the store pages. */
#define TD5_TG_TREE_VARIANTS   10
#define TD5_TG_PAGE_TREE_EXTRA (TD5_TG_PAGE_STORE + TD5_TG_STORE_VARIANTS)
/* Props: people/statue/animal (alpha-keyed) + streetlamp glow (additive). */
#define TD5_TG_PROP_COUNT      7
#define TD5_TG_PAGE_PROP   (TD5_TG_PAGE_TREE_EXTRA + TD5_TG_TREE_VARIANTS - 1)
/* Water: one flat blue page for the sea/river plane beside coastal roads. */
#define TD5_TG_PAGE_WATER  (TD5_TG_PAGE_PROP + TD5_TG_PROP_COUNT)
/* Road surfaces: tarmac reuses PAGE_ROAD (variant 0); gravel/dirt/ice/cobble
 * live after WATER. Each also carries a grip class so the car drives to match. */
#define TD5_TG_ROAD_VARIANTS   5
#define TD5_TG_PAGE_ROAD_EXTRA (TD5_TG_PAGE_WATER + 1)
/* Feedback batch (2026-08-26): page slots RESERVED up front, one contiguous
 * group per work area, so several parallel changes can each fill their own page
 * without renumbering the derived chain above (every constant here is defined
 * off FB_BASE, never off its neighbour). Each page is filled by exactly one
 * emitter, called from the marked block in tg_emit_textures. */
#define TD5_TG_PAGE_FB_BASE   (TD5_TG_PAGE_ROAD_EXTRA + TD5_TG_ROAD_VARIANTS - 1)
#define TD5_TG_PAGE_SIDEWALK  (TD5_TG_PAGE_FB_BASE + 0)  /* paving slabs      */
#define TD5_TG_PAGE_CROSSING  (TD5_TG_PAGE_FB_BASE + 1)  /* zebra crossing    */
#define TD5_TG_PAGE_FENCE     (TD5_TG_PAGE_FB_BASE + 2)  /* sidewalk railing  */
#define TD5_TG_PAGE_TREELINE  (TD5_TG_PAGE_FB_BASE + 3)  /* continuous canopy */
#define TD5_TG_PAGE_TUNNEL    (TD5_TG_PAGE_FB_BASE + 4)  /* tunnel lining     */
#define TD5_TG_PAGE_SNOW      (TD5_TG_PAGE_FB_BASE + 5)  /* snow ground       */
#define TD5_TG_PAGE_HILL      (TD5_TG_PAGE_FB_BASE + 6)  /* distant hillside  */
#define TD5_TG_PAGE_BANNER    (TD5_TG_PAGE_FB_BASE + 7)  /* gantry legs       */
/* [FB r2] Real shipped TD5 street furniture (feedback items 10 and 12). The
 * banner word does not fit one 64x64 page: shipped TD5 splits it over two
 * consecutive pages laid side by side, so each banner needs a LEFT and a RIGHT
 * slot. Page ids of the originals are in td5_tg_furniture_tex.h. */
#define TD5_TG_PAGE_LAMPPOST  (TD5_TG_PAGE_FB_BASE + 8)  /* L001 p356 lamp    */
#define TD5_TG_PAGE_START_L   (TD5_TG_PAGE_FB_BASE + 9)  /* L001 p337 "STA"   */
#define TD5_TG_PAGE_START_R   (TD5_TG_PAGE_FB_BASE + 10) /* L001 p338 "RT"    */
#define TD5_TG_PAGE_FINISH_L  (TD5_TG_PAGE_FB_BASE + 11) /* L001 p369 "FIN"   */
#define TD5_TG_PAGE_FINISH_R  (TD5_TG_PAGE_FB_BASE + 12) /* L001 p370 "ISH"   */
/* ===================== [R3] RESERVED PAGE SLOTS =====================
 * SEAM CARVING for the round-3 feedback batch, same pattern that made round 1
 * survive five concurrent editors.
 *
 * THE PROBLEM THIS SOLVES. Every page id here is DERIVED from its neighbour, so
 * two agents each appending "one more page" both rewrite the same tail of the
 * chain and the #define block becomes one unresolvable conflict. That is exactly
 * what happened in round 2 (r2-city took PAGE_COUNT 42->52 while r2-furniture
 * took it 42->47, a 3-way hand-recombine).
 *
 * THE RULE. Each work area gets a PRE-RESERVED, FIXED-SIZE block measured off
 * TD5_TG_PAGE_R3_BASE -- never off its neighbour's block. Use only your own
 * block. Do NOT move TD5_TG_PAGE_COUNT, do NOT renumber another area's slots,
 * and do NOT append outside your range. Unused slots inside a block are free:
 * they cost one 64x64 page each and the loader cap is 1024, so we are nowhere
 * near a limit. Blocks are deliberately spaced and each is introduced by its own
 * comment, so two areas filling different blocks touch non-adjacent lines --
 * round 1's other lesson was that ADJACENT stubs merge into one conflict region
 * even when the edits are logically independent.
 *
 * Owners (see docs/AUTOTRACK_FEEDBACK_R3.md for the item numbers):
 *   CITY   items 1, 2     building side walls, back rows behind real streets
 *   BLOCK  items 3-6      intersections, 90-degree block turns, parks, houses
 *   BRANCH items 9, 10    avenue dividers, variable branch separation
 *   BRIDGE items 11-16    bridge deck/rail/structure, tunnel lining variety
 * ==================================================================== */
#define TD5_TG_PAGE_R3_BASE   (TD5_TG_PAGE_FB_BASE + 13)
/* --- CITY block (items 1, 2): 6 slots ------------------------------------- */
#define TD5_TG_PAGE_R3_CITY   (TD5_TG_PAGE_R3_BASE + 0)
#define TD5_TG_R3_CITY_N      6
/* --- BLOCK block (items 3-6): 10 slots. Widest reservation on purpose --
 * intersections, park greens/hedges and individual houses are all new art. --- */
#define TD5_TG_PAGE_R3_BLOCK  (TD5_TG_PAGE_R3_BASE + 8)
#define TD5_TG_R3_BLOCK_N     10
/* --- BRANCH block (items 9, 10): 4 slots (avenue dividers, kerb infill) --- */
#define TD5_TG_PAGE_R3_BRANCH (TD5_TG_PAGE_R3_BASE + 20)
#define TD5_TG_R3_BRANCH_N    4
/* --- BRIDGE block (items 11-16): 8 slots. Item 16 explicitly asks for tunnel
 * lining VARIETY, so several of these are tunnel variants. ------------------ */
#define TD5_TG_PAGE_R3_BRIDGE (TD5_TG_PAGE_R3_BASE + 26)
#define TD5_TG_R3_BRIDGE_N    8
/* Named slots inside the BRIDGE block. +0 is the bridge DECK surface (item 11:
 * a deck must not wear the tiling lane-marked road page); +1..+4 are extra
 * TUNNEL LINING variants (item 16a: "tunnel texture is always the same"), used
 * together with the existing TD5_TG_PAGE_TUNNEL as variant 0 -> 5 linings that a
 * per-run hash chooses between, so consecutive bores do not read identically. */
#define TD5_TG_PAGE_BRIDGE_DECK  (TD5_TG_PAGE_R3_BRIDGE + 0)
#define TD5_TG_PAGE_TUNNEL_VAR   (TD5_TG_PAGE_R3_BRIDGE + 1)  /* +1..+4 */
#define TD5_TG_TUNNEL_VARIANTS   5   /* base page + 4 variant pages */
/* ====================== ROUND-4 PAGE BLOCKS ==========================
 * Same rule as the R3 block above, measured off its OWN base so the two
 * rounds' reservations cannot interact. Use only your own block; do not move
 * TD5_TG_PAGE_COUNT and do not renumber another area's slots.
 *
 * Owners (see docs/AUTOTRACK_FEEDBACK_R4.md for the item numbers):
 *   FLOW   items 1, 5, 12   banner seam + legs, tree-line pages, map-edge safety
 *   CROSS  items 2,4,9,10,14 real intersections: markings, kerb breaks, joins
 *   CITY   items 3, 6, 13, 15 building sides, background rows, narrow streets
 *   BRANCH items 7, 8, 11   divider kerb faces, branch UV stability
 *   BRIDGE items 16-20      deck furniture, pillars, coastline, tunnel interlock
 * ==================================================================== */
#define TD5_TG_PAGE_R4_BASE   (TD5_TG_PAGE_R3_BASE + 36)
/* --- FLOW block (items 1, 5, 12): 6 slots. Item 5 asks for tree pages that
 * suit a CITY skyline, so several of these are canopy/skyline variants. ----- */
#define TD5_TG_PAGE_R4_FLOW   (TD5_TG_PAGE_R4_BASE + 0)
#define TD5_TG_R4_FLOW_N      6
/* +0: distant CITY skyline silhouette (item 5). A tree canopy behind a city
 * skyline reads wrong -- the far ridge in an urban biome should be blocky
 * building tops, not forest. Alpha-keyed like the treeline page it stands in
 * for. Slots +1..+5 stay reserved for future FLOW art. */
#define TD5_TG_PAGE_R4_SKYLINE (TD5_TG_PAGE_R4_FLOW + 0)
/* --- CROSS block (items 2, 4, 9, 10, 14): 8 slots. Perpendicular lane
 * markings and raised kerb breaks are both new art. --------------------- */
#define TD5_TG_PAGE_R4_CROSS  (TD5_TG_PAGE_R4_BASE + 8)
#define TD5_TG_R4_CROSS_N     8
/* --- CITY block (items 3, 6, 13, 15): 8 slots (side walls, narrow-street
 * surfaces, background massing). ---------------------------------------- */
#define TD5_TG_PAGE_R4_CITY   (TD5_TG_PAGE_R4_BASE + 18)
#define TD5_TG_R4_CITY_N      8
/* --- BRANCH block (items 7, 8, 11): 4 slots. Item 8 needs a KERB face page
 * so a planted median stops painting grass up its vertical walls. ------- */
#define TD5_TG_PAGE_R4_BRANCH (TD5_TG_PAGE_R4_BASE + 28)
#define TD5_TG_R4_BRANCH_N    4
/* Named slot inside the BRANCH block. +0 is the vertical KERB face used for the
 * two side walls of a planted / kerbed avenue median (item 8): a concrete kerb
 * so the median's walls are not painted with the grass page the TOP wears. */
#define TD5_TG_PAGE_BRANCH_KERB  (TD5_TG_PAGE_R4_BRANCH + 0)
/* --- BRIDGE block (items 16-20): 8 slots (real guardrail face, pier/pillar,
 * coastline strip where water meets land). ------------------------------ */
#define TD5_TG_PAGE_R4_BRIDGE (TD5_TG_PAGE_R4_BASE + 34)
#define TD5_TG_R4_BRIDGE_N    8
/* Named slots inside the BRIDGE block:
 *   +0 GUARDRAIL -- a real barrier face drawn for the parapet's UV convention
 *      (u across the panel HEIGHT, v along the road). The shared TD5_TG_PAGE_RAIL
 *      was authored for a horizontal armco and, sampled rotated on the sloped
 *      parapet ribbon, scrambled into a page that "is not a guardrail texture"
 *      (item 16b). This one has its rails and posts in the panel's own axes.
 *   +1 PIER -- smooth cast concrete for towers/piers. They shared the TUNNEL
 *      LINING page, whose damp/soot blotches read as a checkerboard on a bright
 *      exterior tower (item 18, "pillar has wrong texture").
 *   +2 COAST -- shore band where the bridge water meets the bank (item 20). */
#define TD5_TG_PAGE_R4_GUARDRAIL (TD5_TG_PAGE_R4_BRIDGE + 0)
#define TD5_TG_PAGE_R4_PIER      (TD5_TG_PAGE_R4_BRIDGE + 1)
#define TD5_TG_PAGE_R4_COAST     (TD5_TG_PAGE_R4_BRIDGE + 2)
/* ====================== ROUND-5 PAGE BLOCKS ==========================
 * Same rule as R3/R4, measured off its OWN base. Use only your own block; do
 * not move TD5_TG_PAGE_COUNT and do not renumber another area's slots.
 *
 * Owners (see docs/AUTOTRACK_FEEDBACK_R5.md for the item numbers):
 *   CROSS  items 2,3,4,12   per-intersection crossings, wrapped kerbs
 *   CITY   items 5,6,7,8,15 skyscraper scale, clipping, stretched facades
 *   BRIDGE items 13,14,17   clear ground under bridges, sloped coastline, rails
 *   STRUCT items 1,9,10     banner legs, tunnel bore walls, branch pavement
 *   FLORA  items 16,18      tree variety/ponds, non-stretched treeline
 * (The OOB item 11 lives in td5_track.c and needs no page block.)
 * ==================================================================== */
#define TD5_TG_PAGE_R5_BASE   (TD5_TG_PAGE_R4_BASE + 44)
/* --- CROSS block (items 2,3,4,12): 8 slots ------------------------------- */
#define TD5_TG_PAGE_R5_CROSS  (TD5_TG_PAGE_R5_BASE + 0)
#define TD5_TG_R5_CROSS_N     8
/* --- CITY block (items 5,6,7,8,15): 8 slots ------------------------------ */
#define TD5_TG_PAGE_R5_CITY   (TD5_TG_PAGE_R5_BASE + 10)
#define TD5_TG_R5_CITY_N      8
/* --- BRIDGE block (items 13,14,17): 8 slots (sloped shore, real guardrail,
 * under-bridge water bed). ------------------------------------------------ */
#define TD5_TG_PAGE_R5_BRIDGE (TD5_TG_PAGE_R5_BASE + 20)
#define TD5_TG_R5_BRIDGE_N    8
/* --- STRUCT block (items 1,9,10): 8 slots (banner leg face, tunnel lining,
 * branch pavement). ------------------------------------------------------- */
#define TD5_TG_PAGE_R5_STRUCT (TD5_TG_PAGE_R5_BASE + 30)
#define TD5_TG_R5_STRUCT_N    8
/* Named slot inside the STRUCT block. +0 is the SOLID leg-face concrete page for
 * the start/finish gantry uprights (item 1), replacing the bleeding 12% window
 * of the shared BANNER page. Slots +1..+7 stay reserved for future STRUCT art. */
#define TD5_TG_PAGE_R5_LEG    (TD5_TG_PAGE_R5_STRUCT + 0)
/* --- FLORA block (items 16,18): 12 slots. Widest reservation on purpose --
 * item 18 asks to EXPAND the tree library (varied canopies, tall trees,
 * ponds), so this area is expected to add the most new art. --------------- */
#define TD5_TG_PAGE_R5_FLORA  (TD5_TG_PAGE_R5_BASE + 40)
#define TD5_TG_R5_FLORA_N     12
/* ====================== ROUND-6 PAGE BLOCKS ==========================
 * Same rule as R3/R4/R5, measured off its OWN base. Use only your own block.
 *
 * Owners (see docs/AUTOTRACK_FEEDBACK_R6.md):
 *   CROSS  items 1,2,15,19   sidewalk stops at intersections, no grass crossings
 *   CITY   items 3,4,6,11,16 skyline scale, backdrop on road, occluder at 188
 *   BRANCH items 5,9,10      floor UV swim, see-through gap, span-570 lift
 *   TUNNEL item 8            entrance/lining art, bore intrusion, wall lights
 *   BRIDGE items 12,13,14,17 pillars to floor, alpha rails, water, variety
 *   FLORA  items 7,18        billboard vs treeline, oversized trees on road
 * ==================================================================== */
#define TD5_TG_PAGE_R6_BASE   (TD5_TG_PAGE_R5_BASE + 54)
#define TD5_TG_PAGE_R6_CROSS  (TD5_TG_PAGE_R6_BASE + 0)
#define TD5_TG_R6_CROSS_N     8
#define TD5_TG_PAGE_R6_CITY   (TD5_TG_PAGE_R6_BASE + 10)
#define TD5_TG_R6_CITY_N      8
#define TD5_TG_PAGE_R6_BRANCH (TD5_TG_PAGE_R6_BASE + 20)
#define TD5_TG_R6_BRANCH_N    8
/* TUNNEL block is wide: item 8 asks for correct entrance art, a better lining
 * AND emissive wall lights, so several lining/lamp variants are expected. */
#define TD5_TG_PAGE_R6_TUNNEL (TD5_TG_PAGE_R6_BASE + 30)
#define TD5_TG_R6_TUNNEL_N    12
/* BRIDGE block is wide: item 17 asks for pillar VARIETY sourced from the
 * shipped tracks, so this area is expected to add several new pier pages. */
#define TD5_TG_PAGE_R6_BRIDGE (TD5_TG_PAGE_R6_BASE + 44)
#define TD5_TG_R6_BRIDGE_N    12
#define TD5_TG_PAGE_R6_FLORA  (TD5_TG_PAGE_R6_BASE + 58)
#define TD5_TG_R6_FLORA_N     8
/* ====================== ROUND-7 PAGE BLOCKS ==========================
 * Same rule as R3-R6, measured off its OWN base. Use only your own block.
 *
 * Owners (see docs/AUTOTRACK_FEEDBACK_R7.md):
 *   GUARD  items 5,11,13,15,19  GLOBAL on-road geometry backstop (no new art)
 *   CROSS  items 1,2,14         perpendicular-street pavement, length, spacing
 *   CITY   items 3,4,7          plaza dressing, building variety, run-end continuity
 *   BRANCH items 6,10,12        pre-branch gap, verge scenery, median material
 *   BRIDGE items 9,16,17        tunnel portal art, pier base, water under deck
 *   FLORA  items 8,18           billboard page selection, ground/water validity
 * ==================================================================== */
#define TD5_TG_PAGE_R7_BASE   (TD5_TG_PAGE_R6_BASE + 68)
/* GUARD needs no art -- it validates what other emitters produced. Two slots
 * only, for an optional debug-overlay page if it wants to mark rejects. */
#define TD5_TG_PAGE_R7_GUARD  (TD5_TG_PAGE_R7_BASE + 0)
#define TD5_TG_R7_GUARD_N     2
#define TD5_TG_PAGE_R7_CROSS  (TD5_TG_PAGE_R7_BASE + 4)
#define TD5_TG_R7_CROSS_N     8
/* CITY block is wide: item 4 asks for more building variety in the city
 * pre-selection and item 3 for plaza dressing, so it adds the most new art. */
#define TD5_TG_PAGE_R7_CITY   (TD5_TG_PAGE_R7_BASE + 14)
#define TD5_TG_R7_CITY_N      16
/* [R7 item 4] The CITY block's first 12 slots carry EXTRA facade variety: the
 * original 12 wall variants (level014 + SF/Moscow/Tokyo) are joined by 12 more
 * mined from the shipped city levels (gen_trackgen_r7city_tex.py) -- 7 low-rise
 * masonry, 5 tower/office -- so the pre-selection is not one town repeated. They
 * sit in this reserved block, so the derived WALL/STORE/TREE page chain above is
 * untouched; tg_facade_page_class draws from the union. Slots +12..+15 are spare. */
#define TD5_TG_R7_WALL_LOW_N       7
#define TD5_TG_R7_WALL_TOWER_N     5
#define TD5_TG_PAGE_R7_WALL_LOW    (TD5_TG_PAGE_R7_CITY + 0)   /* +0..+6  */
#define TD5_TG_PAGE_R7_WALL_TOWER  (TD5_TG_PAGE_R7_CITY + 7)   /* +7..+11 */
#define TD5_TG_PAGE_R7_BRANCH (TD5_TG_PAGE_R7_BASE + 32)
#define TD5_TG_R7_BRANCH_N    8
/* BRIDGE block covers the tunnel PORTAL rework (item 9) as well as pier art. */
#define TD5_TG_PAGE_R7_BRIDGE (TD5_TG_PAGE_R7_BASE + 42)
#define TD5_TG_R7_BRIDGE_N    12
#define TD5_TG_PAGE_R7_FLORA  (TD5_TG_PAGE_R7_BASE + 56)
#define TD5_TG_R7_FLORA_N     8
/* ====================== ROUND-8 PAGE BLOCKS ==========================
 * Same rule as R3-R7, measured off its OWN base. Use only your own block.
 * Do NOT move TD5_TG_PAGE_COUNT and do NOT renumber another area's slots.
 *
 * Round 8 runs EIGHT areas (up from six), because the user's five untagged
 * design items were split into art breadth and generated shape rather than
 * folded into one over-subscribed area.
 *
 * Owners (see docs/AUTOTRACK_FEEDBACK_R8.md):
 *   GUARD   99991 3,7,8        precise height gate + per-run exemptions (no art)
 *   CROSS   99991 1,9          perpendicular-street reach rewrite, turn continuation
 *   CITY    99991 2,12         sidewalk run-end, no city backdrop inside a park
 *   BRIDGE  99991 4,10,11 + 777 13,17,18   portal page choice, piers, under-deck
 *   TERRAIN 99991 5 + 777 14,15,16         field extent, tree-line UV, snow ground
 *   VARIETY G1 + 99991 6       facades, depth, skyboxes, banners, guardrails
 *   SHAPE   G2,G3,G5,G4-part   height steps, long rejoining branches (picks TWO)
 *   BIOME   777 19 + G4-part   snow-coherent seeds, one-side sea, element inventory
 *
 * Budget check at reservation time: TD5_TRACK_TEXTURE_PAGE_LIMIT is 1024 and
 * this block takes the total from 325 to 435, so the cap is not in reach.
 * ==================================================================== */
#define TD5_TG_PAGE_R8_BASE   (TD5_TG_PAGE_R7_BASE + 66)
/* GUARD needs no art -- like R7 it validates what other emitters produced.
 * Two slots only, for an optional debug page marking rejects. */
#define TD5_TG_PAGE_R8_GUARD  (TD5_TG_PAGE_R8_BASE + 0)
#define TD5_TG_R8_GUARD_N     2
#define TD5_TG_PAGE_R8_CROSS  (TD5_TG_PAGE_R8_BASE + 4)
#define TD5_TG_R8_CROSS_N     8
#define TD5_TG_PAGE_R8_CITY   (TD5_TG_PAGE_R8_BASE + 14)
#define TD5_TG_R8_CITY_N      10
/* BRIDGE block is wide: item 4 is a tunnel-portal page SELECTION complaint, so
 * this area is expected to bring in alternative lining/portal art to choose
 * from rather than to re-texture the one page it already has. */
#define TD5_TG_PAGE_R8_BRIDGE (TD5_TG_PAGE_R8_BASE + 26)
#define TD5_TG_R8_BRIDGE_N    14
/* TERRAIN owns snow ground variety (777 item 16) and tree-line band variety
 * (777 item 14), both of which are "one page repeated" complaints. */
#define TD5_TG_PAGE_R8_TERRAIN (TD5_TG_PAGE_R8_BASE + 42)
#define TD5_TG_R8_TERRAIN_N    12
/* Allocation inside the reserved block. Slots 8..11 are unused headroom; the
 * block size stays 12 so no other area's base moves. */
#define TD5_TG_PAGE_R8_TREELINE (TD5_TG_PAGE_R8_TERRAIN + 0)  /* +0..+3, 4 variants */
#define TD5_TG_R8_TREELINE_N    4
#define TD5_TG_PAGE_R8_SNOWGND  (TD5_TG_PAGE_R8_TERRAIN + 4)  /* +4..+6, 3 variants */
#define TD5_TG_R8_SNOWGND_N     3
#define TD5_TG_PAGE_R8_SNOWMED  (TD5_TG_PAGE_R8_TERRAIN + 7)  /* snow median top    */
/* VARIETY is the widest block in the round by design: G1 asks for more building
 * variety AND day/night skyboxes AND start/finish banner variety AND guardrail
 * variety, and every one of those is new art rather than new geometry. */
#define TD5_TG_PAGE_R8_VARIETY (TD5_TG_PAGE_R8_BASE + 56)
#define TD5_TG_R8_VARIETY_N    24
/* [R8 VARIETY / G1] Layout INSIDE the 24-slot VARIETY block. Every one of the
 * five art axes G1 asks for is either new pages here or (skyboxes, depth) needs
 * none at all; the two spare slots at the top of the block stay spare.
 *
 *   +0..+5   WALL_LOW    6   more low-rise facades   (td5_tg_real_tex_r8var.h)
 *   +6..+9   WALL_TOWER  4   more tower facades
 *   +10..+13 BANNER      4   level003 start/finish word set (L,R,L,R)
 *   +14..+17 BANNER_NIGHT4   the shipped level001 set, palette-darkened
 *   +18..+21 RAIL        4   real photographic guardrails, alpha-keyed
 *   +22..+23 spare
 *
 * Skyboxes take no page: the sky is a FORWSKY.png copied into the level
 * directory (tg_install_sky), so day/night variety is a file CHOICE. Building
 * depth takes no page either -- it is massing, not art. */
#define TD5_TG_PAGE_R8V_WALL_LOW    (TD5_TG_PAGE_R8_VARIETY + 0)
#define TD5_TG_R8V_WALL_LOW_N       6
#define TD5_TG_PAGE_R8V_WALL_TOWER  (TD5_TG_PAGE_R8_VARIETY + 6)
#define TD5_TG_R8V_WALL_TOWER_N     4
#define TD5_TG_PAGE_R8V_BANNER      (TD5_TG_PAGE_R8_VARIETY + 10)
#define TD5_TG_PAGE_R8V_BANNER_NIGHT (TD5_TG_PAGE_R8_VARIETY + 14)
#define TD5_TG_R8V_BANNER_N         4   /* START_L, START_R, FINISH_L, FINISH_R */
#define TD5_TG_PAGE_R8V_RAIL        (TD5_TG_PAGE_R8_VARIETY + 18)
#define TD5_TG_R8V_RAIL_N           4
#define TD5_TG_PAGE_R8_SHAPE  (TD5_TG_PAGE_R8_BASE + 82)
#define TD5_TG_R8_SHAPE_N     10
#define TD5_TG_PAGE_R8_BIOME  (TD5_TG_PAGE_R8_BASE + 94)
#define TD5_TG_R8_BIOME_N     14
/* ====================== ROUND-9 PAGE BLOCKS ==========================
 * Same rule as R3-R8, measured off its OWN base. Use only your own block.
 * Do NOT move TD5_TG_PAGE_COUNT and do NOT renumber another area's slots.
 *
 * Owners (see docs/AUTOTRACK_FEEDBACK_R9.md):
 *   RAILFIX  8,12      orchestrator-caused double guardrails (no new art)
 *   TUNNEL   5,13      city tunnel -> UNDERPASS, portal SURROUND, bore ceiling
 *   TOPO     6,7,11    topographic continuity between neighbouring surfaces
 *   BRIDGE   9,10      over-deck structure lands on piers, pillar/beam, coast
 *   CITY     1,2,3,4   crossing massing, double pavement, park sides, rejoin
 *   INFRA    backlog   props/street furniture, ponds, deferred R8 items
 * ==================================================================== */
#define TD5_TG_PAGE_R9_BASE   (TD5_TG_PAGE_R8_BASE + 110)
/* RAILFIX needs no art -- it removes a duplicate, it does not add a rail. */
#define TD5_TG_PAGE_R9_RAILFIX (TD5_TG_PAGE_R9_BASE + 0)
#define TD5_TG_R9_RAILFIX_N    6
/* TUNNEL is the widest block in the round: item 13 reframes a city tunnel as an
 * UNDERPASS (a new element, not a re-texture) and item 5 needs a portal
 * SURROUND material distinct from the hillside plus a separate bore ceiling. */
#define TD5_TG_PAGE_R9_TUNNEL (TD5_TG_PAGE_R9_BASE + 8)
#define TD5_TG_R9_TUNNEL_N    16
/* Slots used, all measured off TD5_TG_PAGE_R9_TUNNEL. 7 of the 16 are spent;
 * the rest stay free for a later round rather than being filled speculatively.
 *
 * +0 and +1 are the two pages ITEM 5 is actually about. Three rounds re-textured
 * the portal FACE (+2's predecessors) while the band above the lintel and the
 * wings either side were TD5_TG_PAGE_HILL -- the hillside terrain page -- drawn
 * by tg_emit_fb_tunnel, a DIFFERENT emitter that no portal round ever touched.
 * +1 is that surround's own material.
 *
 * +3..+6 are ITEM 13's underpass, which is a new element rather than a
 * re-texture, and which is why this block was reserved wide. */
#define TD5_TG_PAGE_R9_BORE_CEIL   (TD5_TG_PAGE_R9_TUNNEL + 0)  /* item 5e */
#define TD5_TG_PAGE_R9_PORTAL_SURR (TD5_TG_PAGE_R9_TUNNEL + 1)  /* item 5a */
#define TD5_TG_PAGE_R9_PORTAL_FACE (TD5_TG_PAGE_R9_TUNNEL + 2)  /* item 5c */
#define TD5_TG_PAGE_R9_UP_ABUT     (TD5_TG_PAGE_R9_TUNNEL + 3)  /* item 13  */
#define TD5_TG_PAGE_R9_UP_SOFFIT   (TD5_TG_PAGE_R9_TUNNEL + 4)  /* item 13  */
#define TD5_TG_PAGE_R9_UP_DECK     (TD5_TG_PAGE_R9_TUNNEL + 5)  /* item 13  */
#define TD5_TG_PAGE_R9_UP_PARAPET  (TD5_TG_PAGE_R9_TUNNEL + 6)  /* item 13  */
#define TD5_TG_PAGE_R9_TOPO   (TD5_TG_PAGE_R9_BASE + 26)
#define TD5_TG_R9_TOPO_N      8
#define TD5_TG_PAGE_R9_BRIDGE (TD5_TG_PAGE_R9_BASE + 36)
#define TD5_TG_R9_BRIDGE_N    12
#define TD5_TG_PAGE_R9_CITY   (TD5_TG_PAGE_R9_BASE + 50)
#define TD5_TG_R9_CITY_N      10
/* INFRA owns the deferred backlog, including the 12 already-extracted breakable
 * street-furniture meshes in re/assets/props/ that nothing currently places. */
#define TD5_TG_PAGE_R9_INFRA  (TD5_TG_PAGE_R9_BASE + 62)
#define TD5_TG_R9_INFRA_N     14
/* One page per street-furniture tile in td5_tg_props_tex.h, in that header's
 * own order (its PAGES table is append-only for exactly this reason). The
 * assert below is the seam contract: if the header ever grows past the 14 slots
 * this area reserved, the build stops instead of quietly walking into the next
 * round's block. */
enum {
    TG_INFRA_CRATE = 0, TG_INFRA_CRATEFRG, TG_INFRA_CARDBOX,
    TG_INFRA_BINBODY,   TG_INFRA_BINLID,   TG_INFRA_PHONE,
    TG_INFRA_BENCH,     TG_INFRA_BENCHEND, TG_INFRA_CANOPY,
    TG_INFRA_WORKY,     TG_INFRA_REDTAPE,  TG_INFRA_SIGN,
    TG_INFRA_RICKSHAW
};
#define TD5_TG_INFRA_PAGE(i)  (TD5_TG_PAGE_R9_INFRA + (i))
typedef char tg_infra_pages_fit[(TD5_TG_PROPS_TEX_COUNT <= TD5_TG_R9_INFRA_N)
                                ? 1 : -1];
/* ====================== ROUND-11 PAGE BLOCKS =========================
 * Same rule as R3-R9, measured off its OWN base. Use only your own block.
 * Do NOT move TD5_TG_PAGE_COUNT out from under another area and do NOT
 * renumber another area's slots.
 *
 * Round 10 reserved NO page block (its one area was a placement setback, not
 * art), so R9_BASE + 78 -- the old TD5_TG_PAGE_COUNT -- is genuinely the next
 * free base rather than a gap left by a skipped round.
 *
 * Owners (see R11_FEEDBACK.md):
 *   SIGNS   16   direction signage on the approach to a curve
 * ==================================================================== */
#define TD5_TG_PAGE_R11_BASE  (TD5_TG_PAGE_R9_BASE + 78)
/* SIGNS: three mined arrow panels plus one procedural post. The R9 INFRA block
 * could not absorb these -- it reserved 14 slots and td5_tg_props_tex.h already
 * fills 13, so there was exactly one spare and this area needs four. */
#define TD5_TG_PAGE_R11_SIGN  (TD5_TG_PAGE_R11_BASE + 0)
#define TD5_TG_R11_SIGN_N     6
/* Slot layout inside the block. The three panels are in the SAME order as
 * td5_tg_real_tex_r11signs.h's SIGNS table (LEFT, RIGHT, STRAIGHT) and the
 * emitter indexes them by that order, so the two must not drift. Slots +4 and
 * +5 are unused headroom. */
#define TD5_TG_PAGE_R11_SIGN_LEFT     (TD5_TG_PAGE_R11_SIGN + 0)
#define TD5_TG_PAGE_R11_SIGN_RIGHT    (TD5_TG_PAGE_R11_SIGN + 1)
#define TD5_TG_PAGE_R11_SIGN_STRAIGHT (TD5_TG_PAGE_R11_SIGN + 2)
#define TD5_TG_PAGE_R11_SIGN_POST     (TD5_TG_PAGE_R11_SIGN + 3)
/* Seam contract with the header, the same shape as tg_infra_pages_fit: if the
 * mined set ever grows past the panel slots this area reserved, the build stops
 * instead of quietly walking into the next round's block. */
typedef char tg_r11_sign_pages_fit[(3 <= TD5_TG_R11_SIGN_N - 1) ? 1 : -1];
#define TD5_TG_PAGE_COUNT     (TD5_TG_PAGE_R11_BASE + 6)
#define TD5_TG_MAX_VERTICES   64000
#define TD5_TG_MAX_SPANS      3000
/* Down-track spans per MODELS.DAT display-list entry (entry = span >> 2).
 * Defined here (early) so the guard/meshtag code above the emitters can map a
 * span to its entry. */
#define TD5_TG_SPANS_PER_ENTRY 4
/* Minimum turn radius as a multiple of the road's half-width. Mirrors
 * td5_trackgen.py's CURVE_SAFETY_DEFAULT (1.5); the extra 1.2 is headroom so
 * the resampled centerline never lands exactly on the floor. */
#define TD5_TG_CURVE_SAFETY   (1.5 * 1.2)
/* Steepest allowed |dY/d(arc)|, mirroring td5_trackgen.py's max_grade.
 * Was briefly cut to 0.035 on an inverted reading of the airborne mask (see
 * the row-order note in tg_emit_strip); restored to the Python tool's value. */
#define TD5_TG_MAX_GRADE      0.12
#define TD5_TG_PI 3.14159265358979323846
extern unsigned int s_selfcheck_regen_seed;
/* Branch fork descriptors, produced by tg_build_strip and consumed by the header
 * jump-table write and by tg_emit_models (multi-fork). Each fork splits the ring
 * at span F into a main half [F+1..F+len] and a corridor appended after the ring
 * at [cbase..cbase+len-1], rejoining at span R. The loader (td5_track.c) already
 * iterates an N-entry jump table, so the whole chain is multi-fork. */
#define TD5_TG_BRANCH_MAX 8   /* [FORK KINDS] was 4; the plan ladder has 6 entries */
/* sep = per-fork separation scale in [0,1] (item 10): how far the branch bows
 * away from the main carriageway, as a fraction of the widest bow tg_branch_bow
 * allows. Small = a divided AVENUE (the two carriageways stay close, split only
 * by a central median); large = a road that genuinely diverges in two. Stored
 * on the fork so the strip rows, the road mesh, the clearance query and the
 * divider all read ONE value and cannot drift. */
/* [FORK KINDS] What shape a fork takes. From the census of the 147 shipped
 * forks (docs/plans/AUTOTRACK_ELEMENT_CATALOG.md section 5): 103 are
 * SYMMETRIC splits (8->4+4, 6->3+3, 4->2+2), 20 are SLIP roads (corridor
 * narrower: 8->6+2), 24 have the MAIN road narrower than the corridor
 * (8->2+6), 37 corridors are 8 spans or shorter (traffic ISLANDS), and every
 * one runs PARALLEL to or SHORTER than its main stretch -- the old single
 * bow-and-rejoin DETOUR is the one shape shipped tracks never use. */
typedef enum {
    TG_FORK_AVENUE = 0,   /* symmetric, tight separation, central divider   */
    TG_FORK_ISLAND,       /* symmetric, 4..8 spans, slim island             */
    TG_FORK_WIDE,         /* symmetric, wide separation, corridor gains lanes */
    TG_FORK_SLIP,         /* corridor 1-2 lanes peels off, main keeps the rest */
    TG_FORK_MAJOR,        /* main narrows to 1-2 lanes, corridor takes the rest */
    TG_FORK_KIND_COUNT
} TG_ForkKind;
typedef struct {
    int F, len, cbase, R;
    double sep;
    int lanes;            /* full count at F ([LANES])                       */
    int kind;             /* TG_ForkKind                                     */
    int main_lanes;       /* lanes(F+1): what the main ring keeps            */
    int br_lanes;         /* lanes(B0):  what the corridor takes             */
    double fm, fb;        /* main_lanes/lanes, br_lanes/lanes (0.5 symmetric) */
} TG_Fork;
const char *tg_fork_kind_name(int kind);
/* Stateless plan for fork ordinal `index`: kind, corridor length and
 * separation. Read by the centreline walk (tg_span_in_fork_run) BEFORE
 * s_forks exists and by the placement loop, so both see the same forks. */
void tg_fork_plan(int index, int *kind, int *len, double *sep);
int  tg_fork_count_planned(void);
int  tg_fork_kind_min_lanes(int kind);
int  tg_fork_window_ahead(int si, int within);
void tg_fork_split_lanes(int kind, int lanes, int *main_lanes, int *br_lanes);
/* Fork-aware carriageway geometry (fork fi = index into s_forks). Shifts are
 * lateral, +ve = left of travel; wscale multiplies the road's full width. */
double tg_fork_main_shift(int fi, double w);
double tg_fork_main_wscale(int fi);
double tg_fork_br_shift(int fi, int k, double w);
double tg_fork_br_wscale(int fi, int k);
int    tg_fork_br_lanes_at(int fi, int k);
extern TG_Fork s_forks[TD5_TG_BRANCH_MAX];
extern unsigned int s_fork_plan_seed;   /* [FORK KINDS] set by tg_srand */
extern int s_fork_count;
extern int s_ring_len;
extern int s_want_scenery;
extern int s_want_stream;
/* The rest of the stash needs TG_NodeList, so it lives just below that
 * typedef -- search s_stream_nl. */

/* [S0] Stop the COMPILER reordering two stores across this point. Not a CPU
 * fence and deliberately not pretending to be one: every publish it guards
 * writes the payload first and a validity flag second, and x86-64 does not
 * reorder store-store, so a compiler barrier is the whole requirement here.
 * Used where a lazily-filled build-scope cache is published to what will
 * become parallel readers. */
#if defined(__GNUC__)
#  define TG_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#else
#  define TG_COMPILER_BARRIER() ((void)0)
#endif
/* [S1] PER-ENTRY state that becomes PER-THREAD once the entry loop is
 * parallel. Single-threaded this changes nothing: the main thread simply gets
 * its own copy, and every one of these is already reset at the top of each
 * entry, so no value is ever carried between entries anyway.
 *
 * GCC __thread per-worker isolation on this project's job pool is not assumed:
 * td5_jobs_selftest_tls (td5_jobs.c) exists specifically to assert it. */
#if defined(__GNUC__)
#  define TG_TLS __thread
#else
#  define TG_TLS
#endif
/* [S1] Relaxed atomic OR for the element-inventory mask.
 *
 * Needed because ONE bit in this mask feeds a geometry decision -- the R11
 * sign emitter skips a span whose LAMP bit is set -- while tg_acct_range
 * writes ROWS THE ENTRY DOES NOT OWN (`si, si+1` at the WATER sites), so two
 * entries can hit the same 64-bit word. A plain |= is a read-modify-write, so
 * a lost update there could drop a LAMP bit and put a sign where none belongs.
 *
 * Relaxed is sufficient: the bits are independent flags, and the only bit read
 * during emit (LAMP) is set and tested for the same span inside a single
 * entry, i.e. by one thread in program order. Everything else is read after
 * the loop joins. */
#if defined(__GNUC__)
#  define TG_ATOMIC_OR64(p, v) __atomic_fetch_or((p), (v), __ATOMIC_RELAXED)
#else
#  define TG_ATOMIC_OR64(p, v) (*(p) |= (v))
#endif
/* [S2c] Relaxed atomic add for the one counter a parallel terrain worker can
 * touch (the overflow refusal count). Relaxed is enough: it is read after the
 * loop joins, and it only has to be non-zero for the caller to refuse. */
#if defined(__GNUC__)
#  define TG_ATOMIC_ADD_LONG(p, v) __atomic_fetch_add((p), (v), __ATOMIC_RELAXED)
#else
#  define TG_ATOMIC_ADD_LONG(p, v) (*(p) += (v))
#endif
/* Fork lookups live down in the [FB] block (three other work areas read them
 * from there); forward-declared here because the strip emitters above need
 * them to map an APPENDED corridor span back to its main-ring node. */
int tg_fork_of_main(int si);
int tg_fork_of_corridor(int si, int *k);
int tg_city_crossing_here(int si);   /* [R3 item 7] fence break gate */
extern signed char s_xhere_memo[TD5_TG_MAX_SPANS];
extern signed char s_xbase_memo[TD5_TG_MAX_SPANS];
extern int s_xmemo_armed;
void tg_xmemo_reset(int armed);
/* [R4 CROSS item 4] a zebra must not mark a PARK frontage, so tg_city_crossing_here
 * (defined above the park block) needs the park predicate forward-declared. */
int tg_block_is_park(int si, int left);
/* [R11 BIOME item 4] "Does the outskirts ramp OPEN this frontage?" -- the town
 * DENSITY axis. Asked by tg_side_built (here) and by tg_side_geom (with the rest
 * of that predicate's siblings); defined down with the biome block because it
 * needs the run bounds and the paved test. Deliberately NOT asked by
 * tg_facade_built: see the long note at its definition. */
int tg_town_ramp_open(int si, int left);
/* Same reason: the finish-line placer below has to keep the finish (and its
 * gantry) out of a tunnel run, and the tunnel test is defined with the tunnel
 * emitters much further down. */
int tg_span_in_tunnel(int si);
/* Per-biome bridge/tunnel weighting. The two run gates sit up here (the strip
 * pass needs them) but the biome table is defined with the biome block much
 * further down, so the accessors are forward-declared the same way. */
int tg_biome_bridge_pct(int si);
int tg_biome_tunnel_pct(int si);
/* [R3 item 15] the centreline walk (above the bridge block) clamps curvature on
 * bridge spans, so it needs the run gate before that gate is defined. */
int tg_span_in_bridge_run(int si);
/* [R9 RAILFIX] "Does the pedestrian kerb railing stand on this road edge?" --
 * the single definition of tg_city_emit_fence's placement, asked by the ROADSIDE
 * guardrail (which yields the edge) long before the city emitters are defined.
 * Declared here rather than duplicated so the two can never disagree. */
int tg_rail_kerbfence_here(int si, double sg);
/* Largest heading change per span allowed on a bridge deck, radians. 0.03 =>
 * turn radius >= ~50000 units, a sweeping curve rather than a hairpin, so a
 * deliberate bridge that lands on a tight section is straightened out. */
#define TD5_TG_BRIDGE_MAX_TURN 0.03
/* [R6 item 10] the centreline walk clamps curvature on FORK spans too -- a fork
 * on a sharp bend folds its shifted main half-carriageway and its bowed branch
 * corridor together, and a car driving the region is lifted (the "span 570
 * collision is LIFTING the car" report + the paired "avoid this kind of curve"
 * request). tg_span_in_fork_run gates it; forward-declared like the bridge one. */
int tg_span_in_fork_run(int si);
/* Largest heading change per span allowed across a fork's span range, radians.
 * 0.045 => turn radius >= ~33000 units, a sweeping curve. Sharper than this and
 * the quarter-road main shift + full-width branch bow overlap on the inside of
 * the bend. Looser than the bridge cap (a fork is a longer feature and a dead-
 * straight 120-span corridor reads worse than a gentle sweep) but still well
 * inside the fold threshold (measured folds began around 0.10 rad/span). */
#define TD5_TG_FORK_MAX_TURN 0.045
/* [R3 BLOCK] item 4: the centerline walk (above the biome block) needs to know
 * whether span si is a CITY cell to place "around the block" turns there. */
int tg_biome_span_is_city(int si);
/* [R5 item 15] True where a street-wall facade actually STANDS at span si (not
 * merely where the run/gap pattern says "built"). Forward-declared: it reads the
 * biome table and bridge runs, both defined below, but tg_side_built above needs
 * it. */
int tg_facade_stands(int si);
extern unsigned int s_gen_seed;
unsigned int tg_gen_seed(void);
/* ------------------------------------------------------------------------
 * [R14 GENPERF] Generation zone timers.
 *
 * Generation blocks the main thread for its whole duration, so the window sits
 * at "Not Responding" until it finishes and a user reasonably reads that as a
 * crash. Round 13 roughly doubled the wall clock (seed 20260901: 91 s on the
 * r12-integration build, 170 s on r13-integration) and NOTHING in the log said
 * where the time went -- the per-area reports count what was EMITTED, never
 * what it COST. Every optimisation attempt without this is a guess.
 *
 * Deliberately dumb: a microsecond accumulator per named zone, summed over the
 * whole build and printed once. QueryPerformanceCounter is ~25 ns, so the
 * per-mesh zones below cost single-digit ms across a full track -- under 0.01%
 * of the number they are measuring, which is what makes it safe to leave the
 * accumulation permanently on rather than behind a knob.
 *
 * TIMING CANNOT CHANGE OUTPUT: no zone reads a clock into a generator decision,
 * so MODELS.DAT stays byte-identical with the instrumentation in place. That is
 * verified in this round rather than asserted.
 * ------------------------------------------------------------------------ */
enum {
    TG_ZONE_CENTERLINE, TG_ZONE_STRIP, TG_ZONE_ROUTES, TG_ZONE_PREPASS,
    TG_ZONE_EMIT, TG_ZONE_CITYSCAN, TG_ZONE_GUARD_ROAD, TG_ZONE_GUARD_WATER,
    TG_ZONE_ASSEMBLE, TG_ZONE_TEX, TG_ZONE_REPORTS,
    /* [2026-09-04] The phases b2323ece measured and these zones did not, so
     * the report accounts for the whole call instead of ~95% of it. */
    TG_ZONE_BIOME, TG_ZONE_ELEVATION, TG_ZONE_LEVELINF,
    TG_ZONE_WRITE_STRIP, TG_ZONE_WRITE_MODELS, TG_ZONE_SKY,
    TG_ZONE_SIDEBUILD,
    /* [2026-09-04] NESTED inside TG_ZONE_EMIT, not disjoint from it: the
     * emitter buckets sum to 9707 ms of a 14870 ms "models emit", so a
     * quarter of the build was inside that loop and unattributed. These three
     * say WHICH part of the entry body it is. */
    TG_ZONE_ENTRY_L1, TG_ZONE_ENTRY_L2, TG_ZONE_GUARDVAL,
    TG_ZONE_COUNT
};
extern uint64_t s_tg_zone_us[TG_ZONE_COUNT];
extern long s_tg_zone_n[TG_ZONE_COUNT];
extern uint64_t s_tg_build_t0;
extern uint64_t s_tg_reports_t0;
extern uint64_t s_tg_guard_t0;
extern uint64_t s_tg_wet_t0;
#define TG_ZONE_BEGIN(z) const uint64_t z##_t0 = td5_plat_time_us()
#define TG_ZONE_END(z)   do { s_tg_zone_us[z] += td5_plat_time_us() - z##_t0; \
                              s_tg_zone_n[z]++; } while (0)
/* Second level: inside the per-span emit loop, one bucket per emitter. The
 * top-level zones say "88% is models emit" and stop there, which names a loop
 * rather than a cost. These name the emitter. Wrapping is a comma expression so
 * a call site changes from `f(...)` to `TG_SUB(z, f(...))` and keeps its value,
 * its short-circuiting and its place in the `if` exactly as written -- the
 * single-threaded generator makes the shared scratch temp safe. */
enum {
    TG_SUB_GROUND, TG_SUB_ROAD, TG_SUB_RAIL, TG_SUB_CITY, TG_SUB_BLOCK,
    TG_SUB_CROSS, TG_SUB_FLORA, TG_SUB_FCROSS, TG_SUB_PARKTREE,
    TG_SUB_SLOPEFLORA, TG_SUB_TERRAIN, TG_SUB_INFRA, TG_SUB_TRACK,
    TG_SUB_TUNNEL, TG_SUB_BRIDGE, TG_SUB_PROPS, TG_SUB_SIGN, TG_SUB_OTHER,
    TG_SUB_COUNT
};
extern uint64_t s_tg_sub_us[TG_SUB_COUNT];
extern long s_tg_sub_n[TG_SUB_COUNT];
extern uint64_t s_tg_sub_t0;
extern int s_tg_sub_r;
#define TG_SUB(z, expr) (s_tg_sub_t0 = td5_plat_time_us(), \
                         s_tg_sub_r = (expr), \
                         s_tg_sub_us[z] += td5_plat_time_us() - s_tg_sub_t0, \
                         s_tg_sub_n[z]++, s_tg_sub_r)
/* Third level: the children of the two emitters level 2 indicts. It needs its
 * OWN scratch temp -- these calls sit INSIDE a level-2 bucket, and a shared t0
 * would be overwritten by the inner timer and bill the outer one a few
 * nanoseconds instead of its true span. */
enum {
    TG_SUB2_XWALLS, TG_SUB2_XZEBRA, TG_SUB2_XFLANK, TG_SUB2_XINFILL,
    TG_SUB2_FARBAND, TG_SUB2_FARSHORE, TG_SUB2_PAVED, TG_SUB2_COUNT
};
extern uint64_t s_tg_sub2_us[TG_SUB2_COUNT];
extern long s_tg_sub2_n[TG_SUB2_COUNT];
extern uint64_t s_tg_sub2_t0;
extern int s_tg_sub2_r;
#define TG_SUB2(z, expr) (s_tg_sub2_t0 = td5_plat_time_us(), \
                          s_tg_sub2_r = (expr), \
                          s_tg_sub2_us[z] += td5_plat_time_us() - s_tg_sub2_t0, \
                          s_tg_sub2_n[z]++, s_tg_sub2_r)
/* Fourth level: inside tg_emit_far_band, which level 3 indicts. Same reason for
 * its own temp as level 3 had -- these sit inside a level-3 bucket. */
enum {
    TG_SUB3_GROUNDSIDE, TG_SUB3_DRYREACH, TG_SUB3_ROADCAP, TG_SUB3_ROADEDGE,
    TG_SUB3_COUNT
};
extern uint64_t s_tg_sub3_us[TG_SUB3_COUNT];
extern long s_tg_sub3_n[TG_SUB3_COUNT];
extern uint64_t s_tg_sub3_t0;
extern double s_tg_sub3_d;
/* void-valued calls (tg_ground_side, tg_road_edge) need a statement form. */
#define TG_SUB3V(z, stmt) do { s_tg_sub3_t0 = td5_plat_time_us(); stmt; \
                               s_tg_sub3_us[z] += td5_plat_time_us() - s_tg_sub3_t0; \
                               s_tg_sub3_n[z]++; } while (0)
#define TG_SUB3D(z, expr) (s_tg_sub3_t0 = td5_plat_time_us(), \
                           s_tg_sub3_d = (expr), \
                           s_tg_sub3_us[z] += td5_plat_time_us() - s_tg_sub3_t0, \
                           s_tg_sub3_n[z]++, s_tg_sub3_d)
/* [R14 GENPERF 2026-09-03] Fifth level: named CALLEE timers, nestable. The
 * four levels above name loops and emitters; these name the predicates and
 * sub-emitters the two remaining heavy zones (guardrail 6 ms/span, fb city
 * 4 ms/span, models prepass 10 s) are made of, plus every diagnostic report.
 * A depth-indexed t0 stack makes them safe to nest (a timed predicate may
 * call another timed predicate); the shared result temps are safe because an
 * inner timer finishes before the outer assignment reads its own value. */
enum {
    TG_T_RAIL_CLEARGAP, TG_T_RAIL_ONCW, TG_T_RAIL_EDGEWOULD, TG_T_RAIL_DECK,
    TG_T_RAIL_KERB, TG_T_RAIL_XSTREET,
    TG_T_CITY_PAVED, TG_T_CITY_SIDEWALK, TG_T_CITY_FENCE, TG_T_CITY_VERGE,
    TG_T_CITY_CROSSING, TG_T_CITY_XSTREET, TG_T_CITY_LAMP, TG_T_CITY_BACKROWS,
    TG_T_CITY_FORKBACK, TG_T_CITY_DIAG,
    TG_T_PRE_TURNMAP, TG_T_PRE_R8CROSS, TG_T_PRE_R10CROSS, TG_T_PRE_R11CITY,
    TG_T_PRE_R13JUNC, TG_T_PRE_R13FILL,
    TG_T_RPT_ACCT, TG_T_RPT_RAILEDGE, TG_T_RPT_R11GUARD, TG_T_RPT_R13MOUTH,
    TG_T_RPT_R12FLORA, TG_T_RPT_R9BRIDGE, TG_T_RPT_R8BRIDGE, TG_T_RPT_R12SPANQ,
    TG_T_RPT_R8TERRAIN, TG_T_RPT_R9TOPO, TG_T_RPT_R11XCURVE, TG_T_RPT_R12FCROSS,
    TG_T_RPT_R13BAND, TG_T_RPT_R11WATER, TG_T_RPT_R12TEX,
    TG_T_RPT_R14UP, TG_T_RPT_R14BAND, TG_T_RPT_R14COAST,
    TG_T_COUNT
};
extern uint64_t s_tg_t_us[TG_T_COUNT];
extern long s_tg_t_n[TG_T_COUNT];
extern uint64_t s_tg_t_t0[8];
extern int s_tg_t_depth;
extern int s_tg_t_ri;
extern double s_tg_t_rd;
#define TG_T_BEGIN()  (s_tg_t_t0[s_tg_t_depth++ & 7] = td5_plat_time_us())
#define TG_T_END(id)  (s_tg_t_us[id] += td5_plat_time_us() - s_tg_t_t0[--s_tg_t_depth & 7], \
                       s_tg_t_n[id]++)
#define TG_TI(id, expr) (TG_T_BEGIN(), s_tg_t_ri = (expr), TG_T_END(id), s_tg_t_ri)
#define TG_TD(id, expr) (TG_T_BEGIN(), s_tg_t_rd = (expr), TG_T_END(id), s_tg_t_rd)
#define TG_TV(id, stmt) do { TG_T_BEGIN(); stmt; TG_T_END(id); } while (0)
int tg_report_wanted(const char *own);
extern volatile int s_tg_progress;
void tg_zone_reset(void);
extern long s_xs_fill, s_xs_hit;
extern long s_xs_fill_terr, s_xs_hit_terr;
extern int s_xs_phase;
void tg_zone_report(uint64_t total_us);
void tg_srand(unsigned int seed);
/* Night-ness (feedback item 11: "street lamps should only be visible on night
 * time auto-generated tracks") is owned by td5_trackgen_is_night(), latched in
 * td5_trackgen_regenerate before the build so every emitter that asks during a
 * build gets the same answer. The placeholder seed-derived predicate that used
 * to live here was retired at the round-2 merge, per its own TODO. */

/* ==========================================================================
 * ELEMENT ACCOUNTING  (feedback R2 item 24)
 *
 * WHY THIS EXISTS. The build log used to record the seed, the span count, the
 * section mix, the biome runs, the fork positions, the texture-page count, the
 * MODELS.DAT size and the guardrail coverage -- everything EXCEPT what was
 * actually emitted into the world. So the only way to answer "what is standing
 * at span 900?" was to pull MODELS.DAT off disk and parse it offline. One
 * session burned eight wrong hypotheses that a per-element inventory would have
 * settled in one grep, which is what this replaces.
 *
 * CONTRACT FOR EMITTERS. One line, at the point the element is committed to a
 * buffer (not where it is merely considered -- a rejected candidate must not be
 * counted, or the log lies in the direction that hurts most):
 *
 *     tg_acct(TG_ACCT_TREE, si);                 // one element at span si
 *     tg_acct_n(TG_ACCT_TREE, si, 12);           // n elements at span si
 *     tg_acct_range(TG_ACCT_TUNNEL, si0, si1);   // one element spanning si0..si1
 *
 * All three are no-fail, no-alloc and safe before/after a build, so an emitter
 * never needs a guard around the call.
 *
 * WHAT THE REPORT ANSWERS. Per kind: how many, over how many spans, and the
 * contiguous span RUNS it occupies. The runs are what make "what is at span N"
 * a log question -- read down the report and every kind whose run list brackets
 * N is present there. Runs are logged rather than a per-span dump because a
 * 3000-span track with 20 kinds would otherwise be 60000 log lines; a kind that
 * is genuinely scattered is capped and summarised instead.
 * ========================================================================== */
typedef enum {
    TG_ACCT_BUILDING = 0,   /* facade wall cell / street-wall block */
    TG_ACCT_SIDEWALK,       /* pavement slab + kerb */
    TG_ACCT_SHOPFRONT,      /* ground-floor storefront command */
    TG_ACCT_FENCE,          /* roadside fence / railing (not a guardrail) */
    TG_ACCT_LAMP,           /* street lamp (post + head + glow) */
    TG_ACCT_CROSSING,       /* pedestrian crossing / side-street mouth */
    TG_ACCT_TREE,           /* tree billboard, or a tree-line band quad */
    TG_ACCT_PROP,           /* people / statues / animals / misc furniture */
    TG_ACCT_WATER,          /* sea / river plane */
    TG_ACCT_BRIDGE,         /* bridge deck / pier piece */
    TG_ACCT_TUNNEL,         /* tunnel bore piece */
    TG_ACCT_TERRAIN,        /* ground slab */
    TG_ACCT_FARBAND,        /* distant hillside / horizon band */
    TG_ACCT_BANNER,         /* start / finish gantry */
    TG_ACCT_GUARDRAIL,      /* armco / barrier */
    TG_ACCT_ROAD,           /* drivable road quad */
    TG_ACCT_CHECKPOINT,     /* LEVELINF checkpoint gate */
    TG_ACCT_BRANCH,         /* fork split / rejoin marker */
    /* [R3 BLOCK] appended (never renumbered): park green + individual houses.
     * Two new kinds rather than folding houses into TG_ACCT_BUILDING so the
     * element inventory can confirm the park/house emitters actually fired --
     * "confirm your emitter ran before hunting pixels". */
    TG_ACCT_PARK,           /* park lawn / hedge quad */
    TG_ACCT_HOUSE,          /* individual house in a park */
    TG_ACCT_STEPWALL,       /* side wall closing a run-to-run height step */
    /* [R4] PRE-RESERVED, one slot per work area, each on its OWN line with its
     * OWN comment. Round 3 pre-reserved pages but NOT accounting kinds, so two
     * areas appended to this enum and to k_acct_names and the union resolve
     * silently dropped a comma. RENAME YOUR OWN SLOT IN PLACE -- do not append
     * to the enum, do not append to k_acct_names, do not touch another area's
     * line. An unused reserved slot just reports 0 in the inventory. */
    TG_ACCT_R4_FLOW,        /* FLOW  (items 1, 5, 12)   rename in place */
    TG_ACCT_CROSSFURN,      /* CROSS (items 2,4,9,10,14): markings, kerb breaks, side buildings */
    TG_ACCT_FORKBACK,       /* CITY item 6: background massing behind a fork gore */
    TG_ACCT_R4_BRANCH,      /* BRANCH(items 7, 8, 11)   rename in place */
    TG_ACCT_COASTLINE,      /* BRIDGE(item 20) coastline strip; renamed in place */
    /* [R5] PRE-RESERVED, one slot per work area, each on its OWN line. Same
     * rule as R4: RENAME YOUR OWN SLOT IN PLACE. Do not append to the enum, do
     * not append to k_acct_names, do not touch another area's line. R4 still
     * produced one conflict here because two areas' lines were ADJACENT, so
     * these are spaced by a blank line as well. An unused slot reports 0. */
    TG_ACCT_R5_CROSS,       /* CROSS  (items 2,3,4,12)   rename in place */

    TG_ACCT_R5_CITY,        /* CITY   (items 5,6,7,8,15) rename in place */

    TG_ACCT_R5_BRIDGE,      /* BRIDGE (items 13,14,17)   rename in place */

    TG_ACCT_BRANCH_VERGE,   /* STRUCT item 10: flat verge band on a branch's outer edge */

    TG_ACCT_R5_FLORA,       /* FLORA  (items 16,18)      rename in place */
    /* [R6] PRE-RESERVED, one per area, own line, blank-line spaced.
     * RENAME YOUR OWN SLOT IN PLACE. Never append here. */
    TG_ACCT_R6_CROSS,       /* CROSS  (items 1,2,15,19)   rename in place */

    TG_ACCT_R6_CITY,        /* CITY   (items 3,4,6,11,16) rename in place */

    TG_ACCT_R6_BRANCH,      /* BRANCH (items 5,9,10)      rename in place */

    TG_ACCT_R6_TUNNEL,      /* TUNNEL (item 8)            rename in place */

    TG_ACCT_R6_BRIDGE,      /* BRIDGE (items 12,13,14,17) rename in place */

    TG_ACCT_R6_FLORA,       /* FLORA  (items 7,18)        rename in place */
    /* [R7] PRE-RESERVED, one per area, own line, blank-line spaced.
     * RENAME YOUR OWN SLOT IN PLACE. Never append here. */
    TG_ACCT_GUARD_REJECT,   /* GUARD  (items 5,11,13,15,19): meshes the on-road
                             * guard dropped for standing in the carriageway */

    TG_ACCT_R7_CROSS,       /* CROSS  (items 1,2,14)        rename in place */

    TG_ACCT_R7_CITY,        /* CITY   (items 3,4,7)         rename in place */

    TG_ACCT_R7_BRANCH,      /* BRANCH (items 6,10,12)       rename in place */

    TG_ACCT_R7_BRIDGE,      /* BRIDGE (items 9,16,17)       rename in place */

    TG_ACCT_R7_FLORA,       /* FLORA  (items 8,18)          rename in place */
    /* [R8] PRE-RESERVED, one per area, own line, blank-line spaced.
     * RENAME YOUR OWN SLOT IN PLACE. Never append here.
     * EIGHT areas this round, not six -- see the R8 page-block comment.
     *
     * HEADROOM WARNING for round 9: s_acct_mask below is a 64-bit presence
     * bitmap, one bit per kind. These eight take the count from 43 to 51, so
     * 13 bits remain. Round 9 fits; round 10 does not. Widening is not a
     * one-line change (the mask is also read by the inventory run extractor),
     * so plan it before the count reaches 64 rather than after -- an overflow
     * does not fail loudly, it silently reports a live emitter as "NONE
     * emitted", which is the one lie this inventory exists to prevent. */
    TG_ACCT_R8_GUARD,       /* GUARD  (99991 3,7,8)         rename in place */

    TG_ACCT_R8_CROSS,       /* CROSS  (99991 1,9)           rename in place */

    TG_ACCT_R8_CITY,        /* CITY   (99991 2,12)          rename in place */

    TG_ACCT_R8_BRIDGE,      /* BRIDGE (99991 4,10,11; 777 13,17,18) rename */

    TG_ACCT_R8_TERRAIN,     /* TERRAIN 99991 5; 777 14,15,16: extent + variety */

    TG_ACCT_R8_VARIETY,     /* VARIETY(G1; 99991 6)         rename in place */

    TG_ACCT_R8_LONGBRANCH,  /* [R8 SHAPE G5] long diverging-branch corridor */

    /* [R8 BIOME item 19] Spans owned by a SNOWY biome cell (ALPINE or the new
     * ALPTOWN) on a seed the snow-coherent path claimed. Zero on every non-snow
     * seed by construction, so its presence in the inventory is the proof that
     * the selection rule fired -- and its run list is the snow layout itself. */
    TG_ACCT_R8_SNOWRUN,
    /* [R9] PRE-RESERVED, one per area, own line, blank-line spaced.
     * RENAME YOUR OWN SLOT IN PLACE. Never append here.
     * The 64-bit headroom warning that used to live here is RETIRED: the mask
     * is now self-sizing off TG_ACCT_KIND_COUNT (see s_acct_mask), so adding a
     * kind grows the array instead of silently dropping a bit. */
    /* [R9 RAILFIX] One count per (span, side) road edge where the ROADSIDE
     * guardrail STOOD DOWN because another treatment already owns that edge
     * (the bridge parapet, or the pedestrian kerb railing). It is the
     * inventory proof that the hand-off fired -- the doubling it removes is
     * invisible to every mesh COUNT, which is why the R8 checks missed it. */
    TG_ACCT_R9_RAILYIELD,

    TG_ACCT_R9_UNDERPASS,   /* TUNNEL  (5,13)  item 13 city underpass runs  */

    TG_ACCT_R9_TOPO,        /* TOPO    (6,7,11)             rename in place */

    TG_ACCT_R9_BRIDGE,      /* BRIDGE  (9,10)               rename in place */

    TG_ACCT_R9_CITY,        /* CITY    (1,2,3,4)            rename in place */

    TG_ACCT_R9_INFRA,       /* INFRA   (backlog)            rename in place */
    /* [R10] PRE-RESERVED, one per area, own line. RENAME YOUR OWN SLOT IN
     * PLACE. Never append here. The mask is self-sizing off TG_ACCT_KIND_COUNT
     * (see s_acct_mask), so adding a kind grows the array, never drops a bit. */
    TG_ACCT_R10_CROSS,      /* CROSS  (99991 66) side-street sidewalk setback */
    /* [R11] PRE-RESERVED, one per area, own line. RENAME YOUR OWN SLOT IN
     * PLACE. Never append here. */
    TG_ACCT_R11_CITY,       /* CITY   (20260901 6,10) corner setback + turn gap */

    TG_ACCT_R11_SIGNS,      /* SIGNS  (20260901 16) curve direction signage  */
    /* [R12] PRE-RESERVED, one per area, own line. RENAME YOUR OWN SLOT IN
     * PLACE. Never append here. */
    TG_ACCT_R12_CROSS,      /* CROSS  (20260901 5) forest side road + fold   */
    /* [R13] PRE-RESERVED, one per area, own line. RENAME YOUR OWN SLOT IN
     * PLACE. Never append here. */
    TG_ACCT_R13_FILL,       /* FILL   (20260901 7b) gap-interior infill block */
    /* [R14] PRE-RESERVED, one per area, own line. RENAME YOUR OWN SLOT IN
     * PLACE. Never append here. */
    TG_ACCT_R14_UP,         /* OVERPASS (20260901 3) crossing surround floor + fill */
    TG_ACCT_R14_COAST,      /* COAST  (20260901 5b) tree line wrapping water */
    TG_ACCT_KIND_COUNT
} TG_AcctKind;
extern const char *const k_acct_names[TG_ACCT_KIND_COUNT];
/* [S2d] PER-THREAD element counters.
 *
 * Was one shared long[] incremented by every emitted element. Single-threaded
 * that is free; with the entry loop (or the terrain pre-pass) on the job pool
 * it is 16 threads writing the same few cache lines thousands of times per
 * build, which is the standing suspect for the threaded pre-pass measuring
 * SLOWER than serial (2576 ms -> 4392 ms).
 *
 * One cache-line-aligned row per thread, claimed on first write and never
 * released; reads sum the rows, which is exact because every counter is read
 * after the parallel region has joined. 40 rows covers the main thread plus
 * TD5_JOBS_MAX_WORKERS (32); a thread that cannot get a row falls back to row
 * 0, which is merely contended rather than wrong. */
#define TG_ACCT_SLOTS 40
typedef struct {
    long c[TG_ACCT_KIND_COUNT];
} __attribute__((aligned(64))) TG_AcctRow;
int tg_tslot(void);
long tg_acct_total(int kind);
/* Presence bitmap: bit k of s_acct_mask[si] = kind k stands at span si. One
 * word per span costs 24 KB at TD5_TG_MAX_SPANS and buys exact run extraction,
 * which a running min/max pair cannot give (a kind present at spans 0-40 and
 * 900-940 would otherwise report as one run 0..940).
 * WIDENED to 64 bits for round 4: the kind count reached 26 of the 32 bits an
 * unsigned int offers, and overflowing it would not fail loudly -- it would
 * silently shift a bit out and report a real emitter as "NONE emitted", which
 * is the one lie this inventory exists to prevent.
 *
 * [R9 INFRA] WIDENED AGAIN, and this time made SELF-SIZING. Round 8's eight
 * areas took the kind count to 51 of 64, so round 9 was the last one that fit
 * and round 10 would have overflowed -- silently, in exactly the way described
 * above. Rather than move to 128 bits and face the same deadline a few rounds
 * later, the word count is now DERIVED from TG_ACCT_KIND_COUNT, so adding a
 * kind can never overflow it again: the array grows when the enum does.
 *
 * The compile-time assert below is belt-and-braces -- it cannot fire while the
 * WORDS expression is derived, but it will fire loudly if someone later hard-
 * codes the word count back to a literal. Cost at 51 kinds is unchanged (one
 * word per span, 24 KB); it becomes 48 KB only once a 65th kind exists. */
#define TG_ACCT_MASK_WORDS  (((int)TG_ACCT_KIND_COUNT + 63) / 64)
extern unsigned long long s_acct_mask[TD5_TG_MAX_SPANS][TG_ACCT_MASK_WORDS];
typedef char tg_acct_mask_fits[(TG_ACCT_MASK_WORDS * 64 >= (int)TG_ACCT_KIND_COUNT) ? 1 : -1];
/* Atomic OR, not |= -- see TG_ATOMIC_OR64 for why one bit in here makes that
 * mandatory once entries run in parallel. */
#define TG_ACCT_MASK_SET(si, k)                                               \
    TG_ATOMIC_OR64(&s_acct_mask[(si)][(unsigned)(k) >> 6],                    \
                   1ull << ((unsigned)(k) & 63u))
#define TG_ACCT_MASK_TEST(si, k)                                              \
    ((s_acct_mask[(si)][(unsigned)(k) >> 6] >> ((unsigned)(k) & 63u)) & 1ull)
/* [R8 G1] PER-AXIS PAGE CENSUS. The element inventory above answers "did the
 * emitter fire"; it cannot answer the question a BREADTH complaint actually
 * asks, which is "how many DIFFERENT things are on screen". A page that is
 * defined and never selected is not a fix (R7 item 4), and a pool that is wired
 * up but always resolves to the same member is the same failure one level down.
 *
 * So each of the four art axes tallies, per BUILD, how many times every page id
 * it could have chosen was actually chosen. The dump lands next to the element
 * inventory, and the DISTINCT count in it is the before/after number the round
 * is judged on -- with the axis knobs off it prints the old pool, with them on
 * the new one, from the same code path.
 *
 * Depth is a fifth axis and is NOT a page, so it is censused by cell count. */
typedef enum {
    TG_VAR_FACADE = 0, TG_VAR_BANNER, TG_VAR_RAIL, TG_VAR_DEPTH,
    /* [R15 TEX item 2] The STOREFRONT pool, which until this round was the one
     * facade surface with NO census at all -- tg_var_note was called on the
     * wall page only, so "the same shop sign over and over" was invisible to
     * every report this generator emits. It is also the pool that carries TEXT,
     * which is what makes a repeat read as a repeat. */
    TG_VAR_STORE,
    TG_VAR_AXIS_COUNT
} TG_VarAxis;
#define TD5_TG_VAR_SLOTS 64     /* distinct ids tracked per axis before spill */
void tg_var_note(TG_VarAxis axis, int id);
/* ================= [R9 RAILFIX] ROAD-EDGE RAIL OWNERSHIP =================
 * The element inventory counts MESHES. That is precisely the measurement that
 * could not see the round-8 guardrail regression: "1178 rails over 589 spans"
 * was true, and said nothing about whether two of those rails stood on the SAME
 * ROAD EDGE. A doubling is not a total, it is a collision over a shared
 * resource, so it needs a UNIQUENESS check over that resource.
 *
 * The shared resource here is the (span, side) road edge. Three emitters can
 * put a linear barrier on one:
 *
 *   roadside    tg_emit_guardrail     -- the armco prism at road half width
 *   deck        tg_emit_bridge_rails  -- the bridge parapet on the deck edge
 *   kerb-fence  tg_city_emit_fence    -- the pedestrian railing on the pavement
 *
 * Each notes its claim at the point the geometry is committed. The report then
 * prints, per class, how many edges it owns, and -- the number this exists for
 * -- how many edges carry MORE THAN ONE class, broken down by which pair. That
 * count going to zero is the acceptance test; the mesh total is not evidence.
 *
 * A BITMASK per edge, not a counter: the streaming emitter may be asked for
 * overlapping span ranges, so the same emitter legitimately runs twice over one
 * span. Re-setting a bit is idempotent, so re-entry cannot fabricate a
 * duplicate, while two DIFFERENT classes on one edge always show. */
typedef enum {
    TG_RAIL_ROADSIDE = 0,       /* tg_emit_guardrail    */
    TG_RAIL_DECK,               /* tg_emit_bridge_rails */
    TG_RAIL_KERBFENCE,          /* tg_city_emit_fence   */
    TG_RAIL_CLASS_COUNT
} TG_RailClass;
extern const char *const k_rail_class[TG_RAIL_CLASS_COUNT];
extern unsigned char s_rail_edge[TD5_TG_MAX_SPANS][2];
extern unsigned char s_rail_would[TD5_TG_MAX_SPANS][2];
void tg_rail_edge_note(TG_RailClass c, int si, double lateral_sign);
void tg_rail_edge_would(int si, double lateral_sign);
void tg_acct_reset(void);
void tg_acct_n(TG_AcctKind kind, int si, int n);
void tg_acct(TG_AcctKind kind, int si);
void tg_acct_range(TG_AcctKind kind, int si0, int si1);
/* Cap on run entries printed per kind. A kind with more runs than this is
 * scattered rather than placed, and its exact run list is not what anyone reads
 * the log for -- the count and the first/last span are. */
#define TD5_TG_ACCT_MAX_RUNS 10
void tg_acct_report(int nspans);
extern int s_is_night;
int td5_trackgen_is_night(void);
/* ------------------------------------------------------- centerline ------- */
typedef struct {
    double x, y, z;      /* world units */
    double width;        /* full road width, world units */
    int    lanes;        /* 1..12: lane count of the SPAN that starts here */
    /* [LANES] lane management (see tg_row_points / tg_span_type_for):
     *   lane_base  high nibble of the span's packed byte 3, the walker's
     *              cross-span lane-index shift (shipped baseline 8);
     *   lane_side  which edge changed at the seam ON this node: +1 left,
     *              -1 right, 2 both, 0 no change. */
    int    lane_base;
    int    lane_side;
    /* [LANES] cumulative sideways jog the walk applied up to this node (a
     * one-sided lane change moves the centre by half a lane). The tangent
     * pass subtracts it so a jog never kinks the row direction. */
    double jx, jz;
    double tx, tz;       /* unit tangent (filled after the walk) */
} TG_Node;
/* Largest origin block the row table of tg_emit_span_range can hold; the
 * TD5RE_AUTOTRACK_BLOCK knob is clamped to it. */
#define TD5_TG_ORIGIN_BLOCK_MAX 20
typedef struct {
    TG_Node *v;
    int      count;
    int      cap;
} TG_NodeList;
/* [LANES] per-node lane management readers (defined in td5_trackgen.c). */
int tg_row_points(const TG_NodeList *nl, int node);
int tg_span_type_for(const TG_NodeList *nl, int si);
extern TG_NodeList s_stream_nl;
extern int s_stream_nspans, s_stream_lanes;
extern int s_stream_pending;
extern char s_stream_dir[256];
/* --- self-intersection guard -------------------------------------------
 * A random 2D walk WILL cross itself, and the engine localises a car to a
 * span by proximity -- so where the road overlaps, the span walker snaps to
 * the wrong span and spawn/progress/ground-probe all break (observed: car
 * placed at span 1791 of 1800 on the start line, airborne).
 *
 * Two roads overlap when their centerlines are closer than the sum of their
 * half-widths, so that -- plus a quarter-lane epsilon -- is the exact test.
 * Nodes within an ADJACENT-SKIP window of each other along the road are exempt
 * (a legal tight turn genuinely brings the road near itself); beyond that
 * window the heading budget below makes overlap geometrically IMPOSSIBLE, so
 * the check there is a pure backstop that never fires. The skip is DERIVED
 * from the budget at build time (tg_adjacent_skip) rather than hardcoded, so
 * the guarantee holds no matter how sharp the acute budget is set -- a
 * previously-hardcoded 25 would silently under-exempt once the budget rose.
 */

/* Widest road the generator can emit: the DUAL_LANE section caps lanes at 12
 * (see tg_build_centerline), so this bounds the too-close "need" distance and
 * therefore the derived adjacent-skip. */
#define TD5_TG_MAX_LANES 12
/* Heading budget, radians, measured from TD5_TG_AXIS_HEADING. This is what
 * makes the walk NON-TRAPPING: with |dev| <= limit every span advances the
 * axis coordinate by at least span_length*cos(limit) > 0, so that coordinate
 * is strictly increasing and two nodes far enough apart in span index can
 * never coincide -- self-intersection is geometrically impossible, not merely
 * rejected. Pure rejection sampling was tried first and traps: a self-avoiding
 * 2D walk paints itself into a cul-de-sac (observed: 1800 requested, then 300).
 *
 *   SPINE (~80 deg): straight/curve/dual-lane. Swings -80..+80 = a 160 deg
 *         switchback.
 *   ACUTE (~88 deg, tunable): tight sections only. Swings up to ~176 deg, a
 *         near-hairpin. cos(88 deg) is still > 0, so forward progress -- and
 *         the non-trapping proof -- survive; the price is a larger derived
 *         skip. A TRUE >=180 deg down-track hairpin is INCOMPATIBLE with a
 *         single-axis monotone guarantee (cos <= 0 there) and is deliberately
 *         NOT offered. TD5RE_AUTOTRACK_ACUTE_DEG (80..89) tunes the acute
 *         budget; the derived skip uses whichever limit is larger. */
#define TD5_TG_HEADING_LIMIT       1.396   /* spine, ~80 deg */
#define TD5_TG_ACUTE_HEADING_DEG   88      /* default acute budget, degrees */
#define TD5_TG_ACUTE_HEADING_MIN   80      /* never below the spine */
#define TD5_TG_ACUTE_HEADING_MAX   89      /* keep cos(limit) > 0 (non-trapping) */
/* Global axis the walk wanders about, radians. Deliberately +X (90 deg) rather
 * than +Z (0 deg): route byte[1] encodes the ABSOLUTE 12-bit heading as
 * heading = (byte * 0x102C) >> 8 (td5_ai.c:1280), and a byte < 4 is a junction
 * sentinel rather than a heading. With the axis at 0 the commonest heading
 * (straight ahead) would encode to byte 0..3 and be read as a sentinel; at
 * 90 deg the +/-80 deg spine band maps to bytes 7..120, clear of it. The
 * wider acute band reaches down toward byte 4 at its sharpest left-hand apex;
 * tg_emit_routes clamps to >=4 there, a small heading-fidelity loss on a
 * handful of apex spans rather than a sentinel collision. */
#define TD5_TG_AXIS_HEADING (TD5_TG_PI * 0.5)
const char *tg_section_name(TD5_TrackGenSection s);
int tg_build_centerline(const TD5_TrackGenSpec *spec, TG_NodeList *nl, int section_tally[TD5_TG_SECTION_COUNT]);
/* ===================== DELIBERATE BRIDGES =====================
 * Bridges are PLACED, not detected. The organic lift test alone can never
 * produce one: the elevation profile is 2..6 sine waves spread over ~1800
 * spans, so its wavelength is 300..900 spans and across the +/-8 span window
 * the local terrain test uses it is essentially a straight line. Measured
 * local convexity peaks near 160 -- against a 900 threshold that exists
 * because the deck is 780 tall. So the road never rises above its own
 * surroundings fast enough to be "on a bridge", and after the global-minimum
 * bug was fixed no bridge emitted at all.
 *
 * Fix: choose bridge RUNS the same stateless way tunnels choose theirs (hash
 * of si/RUN, no generator state), then drive the elevation into a hump over
 * each run so the road genuinely climbs and the deck has real clearance.
 *
 * The hump is a RAISED COSINE: zero value AND zero slope at both ends, so it
 * splices into the sine profile without a kink that would read as a ramp.
 *
 * Height is bounded by the grade cap, and that bound is the whole reason for
 * these numbers. Peak slope of the hump is H*PI/RUN per span, and
 * tg_apply_elevation rescales the ENTIRE profile if any span exceeds
 * max_grade (0.120 => 180 units per 1500-unit span). A hump too steep would
 * therefore flatten the whole track to fix itself.
 *
 * [R3 item 13] The user's report was "bridges are like small bumps": RUN 24 /
 * H 1300 was a short deck with a shallow rise. Longer AND taller reads as a
 * real crossing, but the two pull against the grade cap in opposite directions,
 * and lengthening is what buys the headroom to also raise it: peak slope is
 * H*PI/RUN, so stretching the run to 40 spans lets H climb to 2000 while the
 * slope stays inside the cap (2000*PI/40 = 157/span = grade 0.105 < 0.120). The
 * net effect is a deck ~1.7x longer with a ~1.5x taller crown -- i.e. bigger
 * pre-bridge approach ramps, exactly the two things asked for. Ceilinged at
 * 2000 in tg_apply_elevation's clamp, so a leftover-budget span can never push
 * the hump past what the grade permits.
 *
 * Emission deliberately does NOT depend on the lift threshold. The RANGE
 * decides, exactly as it does for tunnels, so a future grade or amplitude tweak
 * cannot silently delete every bridge again. */
/* [R8 item 18] "You can make longer tunnels and bridges."
 *
 * 40 was chosen in R3 as the longest run the GRADE CAP allowed at crown height
 * 2000: peak slope of the raised cosine is H*PI/RUN, and 2000*PI/40 = 157 units
 * per 1500-unit span = grade 0.105 against the 0.120 cap. Length and height pull
 * against that cap in OPPOSITE directions, which is the point -- lengthening is
 * free of it. At 56 spans the same 2000 crown peaks at 2000*PI/56 = 112/span
 * (grade 0.075), so the deck is 40% longer with MORE grade headroom than
 * before, not less. Deck length goes from 60000 to 84000 world units.
 *
 * The run count is what falls: runs are chosen by hashing si/RUN at a fixed
 * rate, so a longer RUN means fewer draws over the same track. That is the
 * intended trade (fewer, bigger crossings), but it is also why this is verified
 * by the element inventory on both seeds rather than assumed. */
#define TD5_TG_BRIDGE_RUN_R3  40       /* pre-R8 run length (A/B baseline)    */
#define TD5_TG_BRIDGE_RUN_R8  56       /* [R8 item 18] longer crossing        */
int tg_bridge_run_len(void);
#define TD5_TG_BRIDGE_RUN     (tg_bridge_run_len())
#define TD5_TG_BRIDGE_HEIGHT  2000.0   /* crown lift; bounded by max_grade */
#define TD5_TG_BRIDGE_CHASM   2500.0   /* how far the ground/river drops below */
/* Half-width of the river channel. Unlike the sea (which starts outboard of the
 * road edge) this crosses the CENTRELINE, which is why it is the water plane
 * that can end up over a bore -- see tg_water_span_clear. */
#define TD5_TG_BRIDGE_WATER_HALF 32000.0
int tg_span_in_bridge_run(int si);
/* [R6 CROSS item 15] Is span si within `clear` spans of any bridge run? A side
 * street opening right off a bridge end reads as detached from its surroundings
 * ("avoid adding crossing streets right after a bridge"), so the crossing
 * emitters use this to keep a clearance band around every run -- an explicit
 * gate in the same shape as the R4 bridge/tunnel interlock, not a rate tweak. */
#define TD5_TG_XBRIDGE_CLEAR 5
int tg_span_near_bridge(int si, int clear);
double tg_track_min_y(const TG_NodeList *nl);
/* ===================== [R8 SHAPE] MACRO RELIEF (G3) =====================
 * "there can be major height differences like on scotland, san francisco or
 * newcastle".
 *
 * The premise that has to be got right first: RANGE and GRADE are different
 * axes, and only the second one is dangerous. The grade cap (TD5_TG_MAX_GRADE
 * 0.12) exists because a steep local slope throws the car airborne -- it was
 * once cut to 0.035 on an INVERTED reading of the airborne mask and had to be
 * restored, so it is not a knob to lean on. But the cap bounds |dY| per SPAN,
 * not total height, and the profile is a sum of sines whose slope contribution
 * scales with FREQUENCY: a term of amplitude A over w waves contributes
 * A*2*PI*w/N per span. Halve w and the same slope buys twice the height.
 *
 * The shipped profile spends nearly the whole budget at HIGH frequency:
 * amp 6000 with waves drawn 2..6, i.e. up to grade 0.117 at waves=6 for a
 * peak-to-peak of only ~12000. That is undulation, not topography.
 *
 * So re-spend the budget. Scale the existing detail term DOWN and add a MACRO
 * term at 1..2 waves over the WHOLE track with a much larger amplitude, sized
 * so its slope contribution is constant regardless of how many waves it draws
 * (amp_eff = MACRO_AMP / waves). Net effect measured on both seeds: several
 * times the height range at a LOWER worst grade than before -- which is the
 * only version of this feature that is safe to ship.
 *
 * TD5RE_R8_SHAPE_RELIEF=0 restores the pre-R8 profile exactly (the macro term
 * draws its own RNG only when enabled, so OFF is byte-identical to R7). */
#define TD5_TG_R8_MACRO_AMP    18000.0  /* macro half-amplitude at 1 wave      */
#define TD5_TG_R8_DETAIL_SCALE 0.5      /* shrink the old high-frequency term  */
void tg_apply_elevation(const TD5_TrackGenSpec *spec, TG_NodeList *nl);
/* ------------------------------------------------------ byte emitters ----- */
typedef struct {
    unsigned char *b;
    size_t         len;
    size_t         cap;
    int            oom;
} TG_Buf;
void tg_buf_free(TG_Buf *buf);
int tg_buf_need(TG_Buf *buf, size_t extra);
void tg_put_u8(TG_Buf *buf, unsigned int v);
void tg_put_u16(TG_Buf *buf, unsigned int v);
void tg_put_u32(TG_Buf *buf, unsigned int v);
void tg_put_i32(TG_Buf *buf, int v);
void tg_install_sky(const char *dir, unsigned int seed);
int tg_write_file(const char *dir, const char *name, const unsigned char *data, size_t len);
int tg_round(double v);
/* ------------------------------------------------------- STRIP.DAT ------- */
/*
 * Layout (confirmed against td5_track.c's loader and td5_assetsrc.c's encoder):
 *   0x00 u32 span table byte offset      (= TD5_TG_SPAN_OFFSET, 216)
 *   0x04 u32 ring length                 (main-road span count)
 *   0x08 u32 vertex table byte offset
 *   0x0C u32 vertex count
 *   0x10 u32 total span count
 *   0x14 u32 branch jump-entry count     (0 -- no branches in Phase 1)
 *   0x18..0xD7  jump records / zero pad  (TD5_TG_PRE_SPAN_BYTES total)
 *   then span records (24 B each), then vertices (6 B each).
 *
 * Span record: type u8 | surface u8 | lane bitmask u8 | lanes|height u8 |
 *              left_vtx u16 | right_vtx u16 | link_next i16 | link_prev i16 |
 *              origin_x i32 | origin_y i32 | origin_z i32
 * Vertex: i16 x,y,z -- LOCAL offsets from that span's origin.
 *
 * Each span emits its own near row then far row (rows are duplicated at seams,
 * exactly as the Python emitter does -- the loader tolerates it).
 */
/* ===================== BRANCHES =====================
 * Full spec and provenance: docs/plans/AUTOTRACK_BRANCHES.md.
 *
 * DEFAULT OFF (TD5RE_AUTOTRACK_BRANCHES=1). A branch changes strip TOPOLOGY,
 * not just geometry, so a mistake corrupts the whole track rather than looking
 * wrong. Structurally complete but NOT verified in game.
 *
 * Design note: the main road keeps its verified shared-row emission untouched.
 * Junction and corridor spans instead get DEDICATED rows appended afterwards,
 * and their records are patched in place. That is also what native TD5 does --
 * level014's fork span owns its rows and the corridor start duplicates them --
 * so per-span lane counts are legal without disturbing the shared-row blocks
 * that fixed seam contact.
 *
 * Layout produced (ring = nspans = main road only):
 *   0..nspans-1     main road; [F-W..F] widened to main+branch, F is type 8
 *   nspans          PAD span -- exists solely so the corridor can start at
 *                   ring+1, because td5_track_branch_to_main_span REJECTS
 *                   span <= ring (td5_track.c:8185). td5_trackgen.py gets this
 *                   wrong (lo == ring) and must not be copied.
 *   nspans+1..      corridor: type 9, type 1 interior, type 10 linking to R
 *   jump record     (lo=nspans+1, hi=last, base=F+1) so main = span - lo + base
 */
#define TD5_TG_BRANCH_FORK_SPAN  600   /* fixed, so a test can drive to it */
#define TD5_TG_BRANCH_LEN         40   /* corridor spans */
#define TD5_TG_BRANCH_WIDEN        6   /* approach spans widened before F */
int tg_branches_enabled(void);
int tg_span_in_fork_clear(int si);
double tg_fork_region_max_curve(const TG_NodeList *nl, int F, int L, int ring);
int tg_append_row(TG_Buf *verts, int *vtx_count, const TG_Node *n, int lanes, double width, double shift, int ox, int oy, int oz);
void tg_patch_span(TG_Buf *spans, int si, int type, int lanes, int lvi, int rvi, int link_next, int link_prev, int ox, int oy, int oz);
void tg_append_span(TG_Buf *spans, int type, int attr, int lanes, int lvi, int rvi, int link_next, int link_prev, int ox, int oy, int oz);
/* NATIVE-FAITHFUL FORK (matches level014). A real TD5 fork does NOT widen the
 * road: a constant-width road SPLITS DOWN THE MIDDLE into two half-width
 * carriageways, which run apart and then merge back into a full-width road at a
 * type-11 rejoin (level014: 6 lanes -> 3 main + 3 branch; rejoin 4+4 -> 8).
 *
 * So here the `lanes`-wide road splits into a MAIN (left) half on sub-lanes
 * [0, lanes/2) and a BRANCH (right) half on [lanes/2, lanes). Each half is
 * lane_count/2 wide, centred a quarter-width off the road centreline:
 *   main centre   = +width/4  (left of travel)
 *   branch centre = -width/4  (right of travel), plus an outward bow
 * The fork span itself and the rejoin span stay FULL width (they are where the
 * two halves share the road); everything between is the two half carriageways.
 *
 * Because the two halves never occupy the same ground (they are opposite halves
 * that only bow further apart), the collision walker cannot flicker between
 * them, and because the rejoin merges back to the road's OWN width there is no
 * forward lane-drop to strand -- both problems the widen-and-converge version
 * had. */
#define TD5_TG_BRANCH_BOW  1.20   /* extra outward sag of the branch, x width */
/* MINIMUM corridor length, spans. A fork shorter than this cannot taper: the
 * bow below is a half sine over `len` spans, so its peak LATERAL RATE is
 * width*BOW*PI/len per span. At the shipped 4-lane width (6000) with BOW 1.20
 * that is 22600/len units per 1500-unit span -- 2827 units/span at len=8, i.e.
 * the branch centre jumps sideways nearly TWICE the span length per step. The
 * two half-carriageways then diverge faster than they advance, the gore quad
 * that fills between them turns inside out, and the strip rows cross: the
 * "very small branches like the one in the last race caused glitches" report.
 * The shortest fork in the old length table was exactly 8.
 *
 * 24 spans is the length at which the derived bow below stops being clamped for
 * a 2-lane-wide half carriageway, so it is the shortest fork that can reach a
 * full-width separation without exceeding the rate limit. */
#define TD5_TG_BRANCH_MIN_LEN  24
/* Peak lateral movement of the branch centre per span, as a fraction of the
 * span length. 0.35 = ~19 degrees of divergence, which the gore can fill and
 * the AI can follow. The bow amplitude is DERIVED from this and the corridor
 * length instead of being a flat 1.20, so a long fork bows out fully and a
 * short one bows out only as far as it can taper. */
#define TD5_TG_BRANCH_RATE   0.35
int tg_branch_min_len(void);
/* ===================== [R8 SHAPE] LONG DIVERGING BRANCH (G5) ==============
 * "you should add major branches that goes in completely different ways for
 * longer and then come back like on sydney".
 *
 * The fork table already ships a short chicane, a canonical split and a "long"
 * route -- but the long one is 120 spans and its centre never gets further than
 * TD5_TG_BRANCH_BOW (1.20 x width = 7200 units) from the main road, so it reads
 * as a wide lay-by rather than an alternate route.
 *
 * TWO axes, and the safety argument is that they move in the SAME direction as
 * the existing rate limit rather than against it. TD5_TG_BRANCH_RATE (0.35 of a
 * span length of lateral movement per span) is what actually keeps the gore
 * fillable and the strip rows from crossing; tg_branch_bow already derives the
 * usable amplitude from it. Lengthening the corridor RAISES the amplitude that
 * rate permits: at len 260 the rate allows 7.24 x width. So the binding
 * constraint on divergence today is the flat BOW ceiling, not the rate, and
 * raising the ceiling for LONG corridors only costs lateral rate 0.145 of a
 * span -- BELOW what the current 120-span fork already runs at (0.126) by a
 * factor well inside the measured fold threshold.
 *
 * The remaining hard ceiling is the int16 vertex offset: corridor spans carry a
 * per-span origin, so |lateral| + half the branch width + one span step must
 * stay under 32767. At bow 3.0 that is 18000 + 1500 + 3000 + ~2100 = 24600,
 * roughly 8000 units of margin.
 *
 * TD5RE_R8_SHAPE_LONGBRANCH=0 restores the 120-span / 1.20-bow fork. */
#define TD5_TG_R8_LONG_LEN   260     /* corridor spans for the long fork      */
#define TD5_TG_R8_LONG_MIN   200     /* len >= this counts as a LONG corridor */
#define TD5_TG_R8_BOW_LONG   3.75    /* bow ceiling (x width) for a long fork.
                                      * 3.75 not 3.0: at the shipped 6000-unit
                                      * road that is 25500 units of carriageway
                                      * separation, just inside the LAT_MAX
                                      * int16 clamp below, and it costs a peak
                                      * lateral rate of 0.181 span/span -- under
                                      * half TD5_TG_BRANCH_RATE and 1.4x what the
                                      * shipped 120-span fork already runs at. */
#define TD5_TG_R8_LONG_APRON 4000.0  /* ground kept OUTBOARD of a bowed branch */
/* Furthest a corridor point may sit from the main centreline, world units. The
 * strip writes it as an int16 offset from a per-span origin, so this must stay
 * clear of 32767 with room for the far row's origin delta and rounding. */
#define TD5_TG_R8_LAT_MAX    28000.0
int tg_r8_longbranch_enabled(void);
int tg_branch_len_for(int index);
int tg_branch_count_max(void);
int tg_branch_is_long(int len);
int tg_span_in_fork_run(int si);
/* Lateral centre of the MAIN (left) half carriageway -- constant. */
#define TD5_TG_MAIN_SHIFT(w)   ((w) * 0.25)
double tg_branch_bow(int len, double width);
/* ---- variable branch separation (item 10) ----
 * Each fork gets a separation scale in [0,1] that multiplies the outward bow.
 * A small repeating ladder keyed to the fork index guarantees VARIETY on any
 * track with several forks -- one tight avenue, one medium split, one wide
 * split -- rather than every fork looking the same. It is a closed form (no
 * shared mutable) so tg_carriageway_reach, the strip builder, the road mesh and
 * the divider all resolve the SAME value for a given fork without coordination.
 *
 * The floor is not zero: at sep 0 the branch's left edge would meet the main's
 * right edge exactly (a divided road with no median at all). The tightest entry
 * keeps a thin median so an avenue still reads as two carriageways, not one. */
#define TD5_TG_BRANCH_SEP_MIN   0.16   /* tightest avenue: a slim central median */
#define TD5_TG_AVENUE_SEP_MAX   0.34   /* sep <= this reads as an AVENUE         */
double tg_fork_sep_for(int fork_index);
int tg_fork_is_avenue(int fork_index);
double tg_branch_shift_s(int k, int len, double width, double sep);
double tg_branch_shift(int k, int len, double width);
double tg_branch_wscale_s(int k, int len, int base_lanes, double sep);
int tg_branch_lane_gain_s(int k, int len, int base_lanes, double sep);
/* ===================== CARRIAGEWAY QUERY =====================
 * THE authority on "is this ground drivable road?". One function the whole
 * generator asks, instead of each work area re-deriving the fork geometry.
 *
 * ROOT CAUSE it exists for (feedback: "implement safeguards that avoid other
 * geometry being rendered over the branches"). Flora, terrain skirts, facades,
 * props and guardrails each grew their OWN copy of the branch arithmetic --
 * tg_flora_branch_reach and tg_ground_branch_clear are two surviving examples.
 * Every copy has to be updated in lockstep whenever the corridor changes shape,
 * and they were not: both of those still assume the corridor is a FIXED half
 * carriageway (width*0.25 out from its centre), which stopped being true the
 * day the corridor learned to widen. Scenery sized against the old assumption
 * then lands on the widened part of the branch. Route every clearance decision
 * through here and that entire class of bug is one function deep.
 *
 * CONVENTIONS -- identical to the strip rows and tg_road_edge:
 *   si       MAIN-RING span index. `nl` only has main-ring nodes; an appended
 *            corridor span has none of its own, so ask about the main span the
 *            corridor runs beside (tg_fork_of_corridor maps one to the other).
 *   lateral  world units across the road, POSITIVE = LEFT of travel.
 *   side     +1 = left of travel, -1 = right. No corridor ever bows left.
 *   margin   extra clearance, world units. 0 asks the bare geometric question;
 *            TD5_TG_CARRIAGEWAY_MARGIN is the house verge.
 *
 * COVERAGE: the full-width main road everywhere, plus -- over a fork -- the
 * MAIN half carriageway, the bowed BRANCH half carriageway at its CURRENT
 * (tapering) width, and the GORE wedge between them. Those three are contiguous
 * in lateral by construction (the gore exists to fill the wedge), so one
 * outward reach per side describes all of it: the test is a single compare, no
 * sqrt, and the only loop is over the <= TD5_TG_BRANCH_MAX fork table.
 *
 * ADOPTION, one call. Something placed at an absolute lateral:
 *     if (tg_on_carriageway(nl, si, lat, TD5_TG_CARRIAGEWAY_MARGIN)) return;
 * Something placed at a setback measured from the MAIN ROAD EDGE (the usual
 * case -- trees, kerbs, facades, terrain skirts):
 *     gap = tg_carriageway_clear_gap(nl, si, side, gap,
 *                                    TD5_TG_CARRIAGEWAY_MARGIN);
 * Both are pure functions of (nl, s_forks), so a streaming range emitter can
 * ask about any span in isolation.
 */
#define TD5_TG_CARRIAGEWAY_MARGIN 300.0   /* house verge between road and prop */
double tg_road_half_width(const TG_NodeList *nl, int si);
double tg_carriageway_reach(const TG_NodeList *nl, int si, double side);
int tg_on_carriageway(const TG_NodeList *nl, int si, double lateral, double margin);
double tg_carriageway_clear_gap(const TG_NodeList *nl, int si, double side, double gap, double margin);
void tg_validate_geometry_safety(const TG_NodeList *nl, int nspans);
/* Mesh-record byte format, shared by every MODELS.DAT writer (tg_emit_box_mesh,
 * tg_emit_billboard_mesh, tg_write_quad_mesh, tg_emit_road_quad_taper) AND by the
 * R7 guard below, which parses the assembled records back out. */
#define TD5_TG_MESH_DISK_SIZE  0x38
#define TD5_TG_CMD_SIZE        16
#define TD5_TG_VTX_SIZE        44
/* ===================== [R7 GUARD] ON-ROAD GEOMETRY BACKSTOP =====================
 * "You need to add an additional way to check for things being rendered on the
 * road." (R7 item 15.)
 *
 * ROOT CAUSE this exists for: for FOUR rounds running, individual scenery
 * emitters (a building, a far-band skyline, a fork backdrop, a grass apron) have
 * put geometry on the drivable surface because each one is separately
 * responsible for asking tg_carriageway_clear_gap and one forgot. Patching the
 * offending emitter each round has failed four times, because the safety depends
 * on every current AND FUTURE emitter remembering to ask.
 *
 * So this is NOT another per-emitter patch. It is a POST-EMIT VALIDATION PASS
 * that runs over the ASSEMBLED mesh bytes of every entry, parses each mesh's
 * world footprint back out (the 0x38 header is uniform across every writer --
 * box, billboard, road quad, quad-mesh -- so this sees geometry regardless of
 * which emitter produced it), tests it against the SAME carriageway authority
 * the emitters are supposed to consult, and REJECTS anything standing clearly in
 * the road. A future emitter that forgets the clearance is caught here for free;
 * the failure direction is inverted from "scenery ships on the road" (silent) to
 * "a road-surface piece a new emitter forgot to mark exempt goes invisible"
 * (loud, and self-correcting).
 *
 * REJECT vs REPORT: this REJECTS clear overlaps (drops the mesh bytes so the
 * player never sees a building in the road) AND logs every rejection with span,
 * penetration depth and vertex count, so the owning emitter can still be fixed
 * properly afterwards. Rejecting-only would leave the road looking right while
 * masking the emitter bug; reporting-only would leave the user still driving
 * through buildings. The log line + the guard-rejects inventory count are the
 * class-level evidence (a per-generation number, not one screenshot).
 *
 * EXEMPTIONS. Some geometry legitimately occupies the carriageway envelope and
 * MUST NOT be rejected: the road quads themselves, the fork gore/divider, the
 * ground skirt (underlaps the road), tunnel bores (enclose it), bridge decks
 * (ARE the road), water/coast (below/beside), and start/finish gantries (legs at
 * the road edge, beam overhead). Those emitters MARK the byte range they wrote
 * as exempt (tg_guard_ex_mark) at their call sites in tg_emit_models; everything
 * else is validated. Two further natural filters keep false positives down: the
 * test ignores vertices well ABOVE the road (an overhead gantry beam) or well
 * BELOW it, and it only rejects on a vertex that penetrates PAST the road edge by
 * more than TD5_TG_GUARD_PEN -- so edge-hugging kerbs, sidewalks, guardrails and
 * edge-to-edge crossing decals (whose vertices sit AT the road edge) survive
 * without needing an explicit exemption.
 *
 * ===================== [R8 GUARD] PRECISION PASS =====================
 * R8 items 3 (`floating tiles on top of the road` @187), 7 (`tiles spilling over
 * the road` @627) and 8 (`background texture over the road` @686), seed 99991.
 * Three objects still stood in the road on a build where this guard already
 * dropped 44 meshes, so the guard's TEST -- not its ambition -- was wrong.
 *
 * MEASURED ROOT CAUSE (not the inherited one). The R7 test above samples
 * VERTICES: a mesh is on the road when one of its own vertices lands inside the
 * carriageway envelope. That is exactly blind to the shape that produces all
 * three complaints: a WIDE QUAD WHOSE TWO ENDS LIE OUTSIDE THE ROAD ON OPPOSITE
 * SIDES. A skyline backdrop band, a plaza floor slab and a fork-back apron are
 * all tens of thousands of units wide; when one crosses the carriageway, every
 * one of its four vertices is far OUTSIDE it, penetration computes as negative
 * at each, and the mesh ships. (The second-hand diagnosis for these items was
 * "the height gate and the byte-range exemptions" -- both were wrong: crossing
 * and plaza geometry are NOT in the exempt set at all, and the offenders sit at
 * road level, not above it. See the DIAG dump this file can emit.)
 *
 * THE FIX IS AREA COVERAGE, NOT WIDTH. Each mesh is walked QUAD BY QUAD and the
 * quad's LATERAL INTERVAL is intersected with the carriageway interval, so a
 * quad that straddles the road is measured by how much road it COVERS, not by
 * how deep its corners poke in. The old vertex penetration is kept and the two
 * are maxed, so every mesh R7 rejected is still rejected and the new test can
 * only ever be a superset.
 *
 * A wider test needs SHARPER exemptions or it starts eating decks and gantries,
 * so the single exempt bit becomes a KIND with a POLICY CLASS:
 *   EXEMPT  road / gore / skirt / branch road / tunnel bore / bridge deck +
 *           rails / water / coast / gantry / end wall. Authored across the
 *           carriageway. Now SPAN-SCOPED: the mark records the span it was
 *           emitted for, and the licence only covers intrusions within
 *           TD5_TG_GUARD_EX_SPANS of it, so a deck exemption cannot cover a
 *           stray mesh emitted later in the same entry.
 *   DECAL   the zebra/crossing road paint. It is authored EDGE TO EDGE across
 *           the road on purpose, so it cannot be judged by coverage -- but it
 *           is only legal FLUSH WITH THE ROAD SURFACE (it is lifted
 *           TD5_TG_CROSS_LIFT = 20 raw). A crossing slab that floats above the
 *           tarmac is rejected, which is item 3's own wording as a rule.
 *   SCENERY everything else. Rejected on any road coverage, unless it clears
 *           the road entirely overhead (dy > TD5_TG_GUARD_OVERHEAD -- lamp arms
 *           and signs) or is buried under it.
 * The kind is also what makes the acceptance evidence readable: rejections are
 * counted PER KIND, so "did this change start eating road/deck/gantry/tunnel"
 * is a number in the log, not a screenshot.
 */
/* [R10 SPAN66] Outermost tarmac for the FURNITURE class -- carriageway plus any
 * side-street asphalt. Owned by the crossstreet emitter (see the block comment
 * there); forward-declared so the guard tests the SAME street that was laid. */
double tg_footway_reach(const TG_NodeList *nl, int si, double side);
int tg_r10_xstreet_guard(void);

/* ===================== [R15 OCC] LATERAL OCCUPANCY AUTHORITY =================
 * "this building is on top of a street" / "on top of a sidewalk" / "next to no
 * road at the end of the crossing street" (R15 items 7, 8a, 4).
 *
 * ROOT CAUSE, measured by reading every reach helper in the generator: there
 * are authorities for the MAIN ROAD (tg_carriageway_reach) and, since R10, for
 * SIDE-STREET ASPHALT (tg_footway_reach) -- but the raised PAVEMENT is in no
 * envelope at all, and tg_footway_reach is consulted only for the FURNITURE
 * policy class. So a massing emitter asking "how far out is taken here" had
 * nowhere to ask, and tg_city_emit_backrows answered it privately by
 * re-deriving tg_xstreet_reach_at with its OWN sw. Where its answer and the
 * street's disagree, the reveal building lands on the street it should close.
 *
 * Deliberately NOT a mesh-vs-mesh overlap engine. Every element in this
 * generator is placed as a LATERAL OFFSET from the centreline at a (span,
 * side), so the honest shared question is "what is the outermost lateral
 * already spoken for here", and the answer composes from authorities that
 * already exist. A pure function of them -- no ledger, no reservation order,
 * nothing to keep in sync -- which is why it cannot drift the way a second
 * model would.
 *
 * ONE AUTHORITY, TWO CONSUMERS, the rule the R10 block comment states:
 *   - PLACEMENT: tg_city_emit_backrows stands its rows beyond tg_occ_reach.
 *   - ENFORCEMENT: the on-road guard keeps its OWN narrower envelope. It is
 *     NOT widened to include pavement, because a facade, a back row and a
 *     pavement arm legitimately BOUND a street and stand ON the kerb -- exactly
 *     the false positive the R10 comment warns about. Massing is corrected
 *     where it is PLACED; the guard still catches anything reaching the road.
 * Mask bits so a caller says which surfaces it may not stand on: a back row may
 * not stand on road, street or pavement; a railing only cares about road.
 * ========================================================================== */
#define TG_OCC_ROAD    1u      /* main carriageway + fork branch corridor   */
#define TG_OCC_STREET  2u      /* side-street / forest-lane asphalt         */
#define TG_OCC_PAVE    4u      /* the raised sidewalk slab                  */
#define TG_OCC_ALL     (TG_OCC_ROAD | TG_OCC_STREET | TG_OCC_PAVE)

/* Outermost lateral (from the CENTRELINE, not the kerb) already spoken for at
 * (si, side) by any of the masked surfaces. 0 when nothing is. Defined in
 * td5_tg_streets.c beside tg_footway_reach, which it generalises. */
double tg_occ_reach(const TG_NodeList *nl, int si, double side,
                    unsigned int mask);

#define TD5_TG_GUARD_PEN       700.0    /* min intrusion past the road edge to reject */
#define TD5_TG_GUARD_OVERHEAD 1800.0    /* a vertex this far above road Y is overhead */
#define TD5_TG_GUARD_UNDER     800.0    /* a vertex this far below road Y is underground */
#define TD5_TG_GUARD_WINDOW     40      /* nearest-node search half-width, spans */
#define TD5_TG_GUARD_EX_MAX    768      /* exempt byte ranges per entry */
/* [R8] A road decal may sit this far off the tarmac and no further. The zebra is
 * lifted 20 raw; 150 leaves room for the road's own within-span camber without
 * licensing anything a driver would read as floating. */
#define TD5_TG_GUARD_FLUSH     150.0
/* [R8] How far from its own span an EXEMPT mark's licence reaches. Bridge decks,
 * bores and gantries are emitted per span but legitimately reach a span or two
 * either way (a 4-span entry's geometry is assembled into one buffer), so this
 * is deliberately loose -- it exists to stop a licence covering an unrelated
 * mesh, not to second-guess the deck emitters. */
#define TD5_TG_GUARD_EX_SPANS   8
/* [R8] Mesh kinds, marked at the emit sites in tg_emit_models. Order is only
 * used for the log breakdown. */
enum {
    TG_GK_OTHER = 0, TG_GK_SKIRT, TG_GK_ROAD, TG_GK_BRANCHROAD, TG_GK_TUNNEL,
    TG_GK_GANTRY, TG_GK_ENDWALL, TG_GK_DECK, TG_GK_WATER, TG_GK_COAST,
    TG_GK_DECAL, TG_GK_CITY, TG_GK_BLOCK, TG_GK_CROSS, TG_GK_FLORA,
    TG_GK_PARKTREE, TG_GK_TERRAIN, TG_GK_BUILDING, TG_GK_PROP, TG_GK_RAIL,
    TG_GK_BRANCHSIDE, TG_GK_COUNT
};
extern const char *const k_guard_kind_name[TG_GK_COUNT];
/* [R10 SPAN66] FURNITURE is SCENERY judged against a WIDER envelope: it obeys
 * every scenery rule (coverage, penetration, the overhead and buried gates), but
 * the tarmac it may not stand on includes a side street's asphalt as well as the
 * carriageway. It is a separate class rather than a wider tg_carriageway_reach
 * because a facade, a back row and a pavement arm legitimately BOUND a side
 * street -- widening the shared authority would have rejected the buildings that
 * make the street a street. */
enum { TG_GKC_SCENERY = 0, TG_GKC_EXEMPT = 1, TG_GKC_DECAL = 2,
       TG_GKC_UNDER = 3, TG_GKC_FURNITURE = 4 };
extern size_t s_guard_ex_lo[TG_ACCT_SLOTS][TD5_TG_GUARD_EX_MAX];
extern size_t s_guard_ex_hi[TG_ACCT_SLOTS][TD5_TG_GUARD_EX_MAX];
extern unsigned char s_guard_ex_kind[TG_ACCT_SLOTS][TD5_TG_GUARD_EX_MAX];
extern int s_guard_ex_si[TG_ACCT_SLOTS][TD5_TG_GUARD_EX_MAX];
/* [S2i] One CACHE LINE per slot, not one int. As a plain int[] the first 16
 * slots share a single line, and this counter is incremented on every guard
 * mark by every worker -- so the array that was meant to remove contention
 * would have reintroduced it as false sharing. */
typedef struct { int v; } __attribute__((aligned(64))) TG_SlotInt;
extern TG_SlotInt s_guard_ex_nx[TG_ACCT_SLOTS];
#define s_guard_ex_n(t) (s_guard_ex_nx[(t)].v)
extern long s_guard_rejects;
extern long s_guard_residual;
extern long s_guard_rej_kind[TG_GK_COUNT];
extern long s_guard_ex_scope_hits;
extern long s_guard_unsorted;
extern long s_guard_unsafe;
int tg_guard_enabled(void);
int tg_guard_report_only(void);
void tg_guard_ex_reset(void);
void tg_guard_mark(size_t lo, size_t hi, int kind, int si);
/* [PICK MESHTAG] dev-only sidecar (td5_tg_guard.c). Compiled out of RELEASE:
 * the macros below match the #else branch of the block that defines them. */
#ifndef TD5RE_RELEASE
void tg_meshtag_reset(int nentries);
void tg_meshtag_set(int entry, int slot, int kind);
void tg_meshtag_fallback(int entry, int slot, size_t off);
void tg_meshtag_write(const char *dir);
#else
#define tg_meshtag_reset(n)          ((void)0)
#define tg_meshtag_set(e, s, k)      ((void)0)
#define tg_meshtag_fallback(e, s, o) ((void)0)
#define tg_meshtag_write(dir)        ((void)0)
#endif

/* ===================== [R9 CITY] PAVEMENT PROVENANCE MARKS =================
 * Round 9 item 2 ("the right track on span 150 branch has double sidewalk") is a
 * UNIQUENESS defect: two emitters each lay a legal-looking pavement on the SAME
 * span-side. R9's method rule says a count going up is not evidence of correct
 * placement, so the acceptance test cannot be "how many pavements did we emit" --
 * it has to be "does any span-side carry more than one SEPARATED pavement band".
 *
 * Answering that needs to know which assembled meshes are pavements laid ALONG
 * the road, and which emitter laid each one. Page alone is not enough: the corner
 * ARMS (tg_block_emit_arm) and the crossing decals share TD5_TG_PAGE_SIDEWALK and
 * legitimately add lateral off a mouth, so a page-only sweep reports false doubles
 * at every side street. So the four along-road pavement emitters mark their byte
 * range here, exactly the way tg_guard_mark records kinds, and the sweep measures
 * only marked ranges. Marks are per-entry byte offsets and are read BEFORE the
 * guard's compaction moves anything (see the call site in tg_emit_models). */
enum {
    TG_PVS_CITY = 0,    /* tg_city_emit_sidewalk    -- raised main-road slab   */
    TG_PVS_VERGE,       /* tg_city_emit_verge_band  -- flat main-road band     */
    TG_PVS_BRANCH,      /* tg_emit_branch_sidewalk  -- raised branch slab      */
    TG_PVS_BRVERGE,     /* tg_emit_branch_verge     -- flat branch band        */
    TG_PVS_ARM,         /* tg_block_emit_arm -- kerb turning down a side street.
                         * NOT an along-road band: an arm legitimately adds
                         * lateral at a mouth, so it is recorded as a per-side
                         * FLAG (item 4's evidence) and deliberately kept out of
                         * the item-2 uniqueness test, which would otherwise
                         * report a false double at every side street. */
    TG_PVS_COUNT
};
extern const char *const k_pave_src_name[TG_PVS_COUNT];
#define TD5_TG_PAVE_MARK_MAX 512
extern TG_SlotInt s_pave_mark_nx[TG_ACCT_SLOTS];
#define s_pave_mark_n(t) (s_pave_mark_nx[(t)].v)
void tg_pave_mark_reset(void);
void tg_pave_mark(size_t lo, size_t hi, int src, int si);
int tg_pave_src_of(size_t off, int *pmark_si);
float tg_rd_f32(const unsigned char *p);
unsigned int tg_rd_u32(const unsigned char *p);
unsigned int tg_rd_u16(const unsigned char *p);
size_t tg_guard_mesh_len(const unsigned char *b, size_t off, size_t cap);
/* Nearest main-ring node to world (wx,wz), searched in a window around the
 * entry's spans so a far-band vertex reaching tens of thousands of units OUT
 * still resolves to the span it stands beside, not a curve several spans away. */
/* [R11 BRIDGE item 7a] How far a vertex may be from the nearest node in the
 * search window and still be judged AS road geometry.
 *
 * The search is over a +/-TD5_TG_GUARD_WINDOW span window and had NO distance
 * bound, so it always returned a node -- the best one available, however far
 * away. That was harmless while nothing reached far: every mesh the guard had
 * ever seen sat within a few thousand units of some node in its own window.
 *
 * MEASURED FAILURE. Extending the overpass arms to the drawn edge (30000)
 * broke it. On seed 20260901 the entry holding span 1130 has window 1088..1171,
 * and the deck's far corners -- 33000 units out to the side, over open ground --
 * were attributed to nodes 1089 and 1167, i.e. THE TWO ENDS OF THE WINDOW,
 * simply because nothing nearer was allowed to be considered. Their lateral in
 * those spans' frames then landed inside the carriageway, the mesh read as
 * covering 6000 units of road at a span 40 away from its own, the span-scoped
 * exemption (TD5_TG_GUARD_EX_SPANS = 8) refused to license it, and the guard
 * REJECTED the deck: three "the guard is eating the track" warnings, all three
 * marked @1130. THERE WAS NO ACTUAL OVERLAP -- two earlier fixes that shortened
 * the arm against another carriageway found nothing to shorten against,
 * which is what pointed at the attribution rather than the geometry.
 *
 * 12000 is chosen against the thing being tested, not tuned: the widest
 * carriageway on this generator is a 4-lane avenue plus a branch corridor,
 * comfortably under 6000 of reach per side, so a vertex more than 12000 from
 * every node in its window cannot be over any carriageway and has no business
 * being normalised into one's frame. Returning "no node" makes the callers skip
 * it, which they already do for an out-of-ring index.
 *
 * TD5RE_R11_GUARD_NEAR=0 restores the unbounded search for an A/B. */
#define TD5_TG_GUARD_NEAR_MAX 12000.0
int tg_guard_nearest_node(const TG_NodeList *nl, int lo, int hi, double wx, double wz);
/* [R8] Worst road intrusion this mesh makes, and the height band it makes it in.
 * `cover` is the metric the R8 pass adds: how much CARRIAGEWAY WIDTH the mesh's
 * footprint covers, which is what catches a wide quad whose own corners are
 * outside the road on both sides. `pen` is R7's vertex penetration, kept so the
 * new test is a strict superset (a narrow post standing in the middle of the
 * road covers little width but penetrates deeply). */
typedef struct {
    double intr;        /* max(cover, pen) at the worst quad, world units */
    double dy_lo;       /* that quad's lowest  point above the road surface */
    double dy_hi;       /* that quad's highest point above the road surface */
    int    si;          /* span it intrudes on */
} TG_GuardHit;
/* ============ [R9 BRIDGE item 10] OVER-WATER AUDIT (see the call site) ========
 * The river rectangle and its surface are owned by the bridge block much
 * further down, so both are forward-declared rather than re-derived here -- the
 * whole point is that the audit tests the SAME water the emitter laid. */
#define TD5_TG_WATER_DROP    1200    /* how far below the road the surface sits  */
#define TD5_TG_WATER_BEACH   8100    /* gap from the road edge to the shoreline  */
#define TD5_TG_WATER_EXTENT  50000   /* how far out to sea the plane reaches     */
int    tg_water_span_clear(int si);
double tg_bridge_water_surf_y(const TG_NodeList *nl, int si);
double tg_sea_level_y(const TG_NodeList *nl, int si);
/* [R17 WATER item 1] GLOBAL water level -- ONE absolute surface height for the
 * whole track, a LOW PERCENTILE of the route's node elevations so the sea sits
 * in the terrain's low band; paired with a route floor clamp in
 * tg_apply_elevation so the road stays above it. Defined in td5_trackgen.c. */
double tg_water_level_y(const TG_NodeList *nl);
double tg_water_side(int si);
int    tg_biome_for_span(int si);
int    tg_biome_span_has_water(int si);
extern int s_r9_wet_total;
extern int s_r9_wet_kind[TG_GK_COUNT];
extern int s_r9_wet_first_span;
extern double s_r9_wet_worst;
/* How far a non-exempt mesh may rise above a water surface before it counts as
 * standing ON the water. Generous: a verge prop whose base sits a few units
 * proud of a shoreline is not what the report is about, a building is. */
#define TD5_TG_R9_WET_LIFT 250.0
extern int s_r9_wet_rejected;
/* Per-span water table, built ONCE per track. The first version of this audit
 * asked tg_span_in_bridge_run / tg_water_side / tg_sea_level_y per (mesh, span)
 * and generation stopped finishing: those are run-walking accessors, and the
 * product is tens of millions of calls. The table is the same information,
 * sampled once, and it also collapses the scan to the handful of spans that
 * actually carry water. */
#define TD5_TG_R9_WET_MAX 3000
extern int s_r9_wet_ready;
void tg_r9_water_table_build(const TG_NodeList *nl);
/* ==== [R14 COAST item 5a] "on the LEFT side of the bridge, polygons collide
 * with the water below and with the coastline" ================ SECTION:r14coast
 *
 * The R9 over-water audit above answers a DIFFERENT question: does a mesh stand
 * ABOVE the surface (ytop > surf + LIFT) inside the river rectangle. A polygon
 * that COLLIDES with the water is one that STRADDLES the surface -- some of its
 * vertices above it and some below -- and the R9 audit is blind to that by
 * construction, because it only ever looks at the highest vertex and it exempts
 * every kind that legitimately meets the water (SKIRT, COAST, DECK, RAIL).
 * Interpenetration is exactly a defect BETWEEN two of those exempt surfaces, so
 * the audit that would catch it has to include them.
 *
 * Report-only, and deliberately so: this is the measurement, not the fix.
 * Straddle depth is reported per kind and per span with the lateral sign, so
 * "the LEFT side" is a number rather than a reading of a screenshot.
 * TD5RE_R14_COAST_REPORT=1 turns it on; it costs nothing when off. */
#define TD5_TG_R14_STRADDLE 60.0   /* ignore a z-fight-scale overlap */
void tg_r14_coast_report(void);

/* [R15] Round-15 reporting. FOUR functions rather than the one the work was
 * written against: after the trackgen split the counters are file-statics in
 * four different modules, and a static cannot be read across a translation
 * unit -- so each owning module reports its own. All four are O(1) (they only
 * divide running sums), which is why none is TG_TV-wrapped or span-gated.
 * tg_store_page_reset is per-BUILD state for the item-2 anti-repeat, reset
 * beside tg_acct_reset for the same reason. */
void tg_r15_sky_report(void);      /* item 3      -- td5_tg_terrain.c */
void tg_r15_city_report(void);     /* 1,2,5,6,8b,4,7,8a -- td5_tg_city.c */
void tg_r15_streets_report(void);  /* items 7 + 9 -- td5_tg_streets.c */
void tg_r15_pair_report(void);     /* items 10/11 -- td5_tg_guard.c   */
void tg_store_page_reset(void);
/* Validate one entry's assembled meshes against the carriageway and drop the
 * ones standing in the road. Rewrites `meshes` and `moff`/`*pnmesh` in place
 * (compacting left). Returns the number rejected. Corridor-only entries (past
 * the ring) carry only exempt branch road and are skipped.
 *
 * [R10] "COMPACTING LEFT IS ALWAYS SAFE: WE ONLY REMOVE" WAS NOT TRUE, and it
 * cost a silent STATUS_HEAP_CORRUPTION kill of the whole generator. It holds
 * only while moff[] is in ASCENDING byte order -- one forward write cursor over
 * offsets that step backwards overtakes the read position and walks off the end
 * of the allocation. An R9 emitter did record two offsets out of order (see the
 * R10 note above tg_r9_waterguard_enabled), so the walk now runs over a sorted
 * permutation and the cursor is bounded against the buffer regardless. */
/* >= TG_MAX_MESHES_PER_ENTRY (4 spans * 96 = 384); a literal so it does not
 * depend on TD5_TG_SPANS_PER_ENTRY, which is defined further down. */
#define TD5_TG_GUARD_KEPT_MAX 512
/* [R13 FACES item 6] The frontage census lives with the facade emitter, but the
 * number it has to report is what SURVIVES this pass -- a building dropped here
 * is a building the driver never sees, and on seed 20260901 span 1200 that is
 * exactly what turned a capped run end into a flat card. Forward-declared so the
 * drop is recorded where it happens instead of being inferred afterwards. */
void tg_r13_faces_dropped(int si);
int tg_guard_validate_entry(const TG_NodeList *nl, int ring, int s0, int ns, TG_Buf *meshes, size_t *moff, int *pnmesh);
void tg_selfcheck_ranges(const TG_NodeList *nl, int block);
int tg_emit_strip(const TG_NodeList *nl, TG_Buf *out, int *out_spans);
int tg_emit_routes(const TG_NodeList *nl, int nspans, int lateral, TG_Buf *out);
/* ------------------------------------------------- FINISH LINE + RUN-OFF ---
 * Reported: "fix track end, there should be a finish banner and around 100 span
 * after the end".
 *
 * VERIFIED where the finish actually comes from before changing anything. A
 * generated track is a faithful TD5 point-to-point, so s_td6_finish_span is 0
 * and advance_pending_finish_state takes the CHECKPOINT path
 * (td5_game.c:9152): the race ends for an actor the tick its checkpoint_index
 * reaches s_active_checkpoint.checkpoint_count, and those thresholds are the
 * LEVELINF checkpoint spans this function writes. So the finish line IS the last
 * checkpoint span -- there is no separate finish field to set.
 *
 * The old placement was `nspans * (i+1) / (cp_count+1)`, i.e. the last
 * checkpoint at 80% of the span count, and `nspans` here is the TOTAL emitted
 * count including every fork's pad and corridor tail -- so the finish landed on
 * an arbitrary interior span that moved when the branch layout changed, with
 * nothing on screen marking it. Whatever road happened to be left past it was
 * an accident of that arithmetic, not a run-off.
 *
 * Now the finish is PLACED: ring_len - runoff, so there are exactly `runoff`
 * spans of road past the line for the car to slow down on, and the earlier
 * checkpoints are spread evenly between the grid and the finish. This needs no
 * extra spans -- the run-off is road that already existed and used to sit
 * uselessly past an interior finish -- so TD5_TG_MAX_SPANS, TD5_TG_MAX_VERTICES
 * and TD5_TG_ORIGIN_BLOCK are all untouched by it.
 */
#define TD5_TG_RUNOFF_SPANS  100
int tg_finish_span(int ring);
int tg_emit_levelinf(const TD5_TrackGenSpec *spec, int nspans, TG_Buf *out);
/* ==========================================================================
 * MODELS.DAT -- road surface mesh
 *
 * Emitting ANY MODELS.DAT switches OFF the procedural ribbon renderer (it is
 * gated on "zero display-list entries", td5_render_mesh.c:2663), so the road
 * surface must come from here instead. Hence TD5RE_AUTOTRACK_SCENERY defaults
 * to OFF: until this is verified, the shipped path stays the ribbon.
 *
 * Byte format is a C port of re/tools/mesh_tool.py (_pack_mesh, build_dat) --
 * the emitter the existing Python-generated levels are validated against --
 * rather than a fresh reading of the parser.
 *
 * Container ("format A strict", td5_track_parser.c:79-119):
 *   u32 entry_count
 *   (u32 block_offset, u32 block_size) * entry_count   -- offset ABSOLUTE
 *   blocks, contiguous; block 0 must start at 4 + count*8 (no padding)
 * Block:
 *   u32 sub_count (1..256)
 *   u32 mesh_off[sub_count]   -- BLOCK-relative; 0 = empty slot
 *   packed mesh records
 * Mesh record is 0x38 bytes, then commands (16 B each), then vertices (44 B).
 * (TD5_TG_MESH_DISK_SIZE / _CMD_SIZE / _VTX_SIZE are defined up by the R7 guard,
 * which parses the same record.)
 * ========================================================================== */
/* Down-track sub-quads per span. A single 1500-unit quad shimmers at distance;
 * the Python emitter uses 3 for the same reason. */
#define TD5_TG_ROAD_SUBDIV     3
void tg_put_f32(TG_Buf *buf, double v);
void tg_road_edge(const TG_NodeList *nl, int si, double f, double shift, double wscale, double *lx, double *ly, double *lz, double *rx, double *ry, double *rz);
/* ================== [R13 JUNCTION] BEND-FOLD AUTHORITY =====================
 * Round 13 item 3, span 709: "improve the algorithm for folding geometry on
 * close curves when intersections happen right after, to avoid DOUBLE GROUND
 * and BUILDINGS OVERLAPPING INTO THE ROAD". Rounds 11 and 12 both fixed
 * junction FURNITURE at a bend and it came back, so this round measured the
 * surfaces instead (tg_r13_junc_report).
 *
 * MEASURED, seed 20260901: 165 of 1987 spans emit a ground slab whose OUTER
 * edge runs BACKWARDS along the road, and 25 emit a side-street mouth that
 * does. At the complaint neighbourhood spans 705-707 the ground slab's outer
 * edge is 3.87x the inner edge long IN REVERSE, and span 708 -- the mouth of
 * the junction the user is standing in at 709 -- lays a 19500-long street quad
 * at -2.03. Those are bowties: the surface past the crossing point lies on top
 * of what the neighbouring spans laid, which is the doubled ground.
 *
 * WHY. Every lateral surface here is a ruled quad between the cross-section at
 * node si and the one at node si+1, each a STRAIGHT ray off its own kerb point.
 * On a bend those two rays are not parallel: they converge on the inside at the
 * centre of curvature. Past that point the quad has crossed itself. Nothing in
 * the generator knew where that point was, so every emitter picked its reach
 * from a table (12000 of verge, 19500 of street, a biome's block depth) and the
 * bend was free to be tighter than the reach.
 *
 * This states the limit ONCE, from the emitters' own frame rather than from a
 * curvature model, so ground, street and massing cannot disagree about where a
 * bend folds. `keep` is the share of the inner edge's along-road length that
 * survives at the outer edge -- 1 on a straight, 0 exactly at the convergence
 * point, negative once folded -- and it is LINEAR in the reach, so the bound is
 * exact and needs no search:
 *
 *     keep(d) = 1 + d * ((far_out - near_out) . inner_unit) / inner_len
 *
 * Returns 1e30 where the rays are parallel or diverge (a straight, and the
 * OUTSIDE of every bend), which is why straight track is untouched. `ang`
 * is the emitter's own skew, so a skewed side street is judged on the bearing
 * it is actually laid at. */
#define TD5_TG_R13_KEEP  0.0    /* de-overlap only: the degenerate tip is legal */
/* [R14 JUNCTION item 4] THE SURVIVING SHARE THE CAP AIMS FOR.
 *
 * R13 set this to 0 and flagged the consequence it did not measure: keep == 0
 * is the CONVERGENCE POINT itself, so the cap stops the quad exactly where its
 * two rays MEET. The outer edge is then a single degenerate point sitting at
 * the centre of curvature -- and on a bend tight enough, that centre is not out
 * in the scenery, it is over the OPPOSITE carriageway. So the R13 clamp removed
 * the bowtie (a fold is by definition gone once keep >= 0) and left the tip,
 * which is why `mass_on_road` moved the WRONG way while every fold counter went
 * to zero. Both halves of that trade are reported together by
 * tg_r14_junc_report; neither number is allowed to move alone.
 *
 * A keep above 0 stops the quad SHORT of the convergence point, so its outer
 * edge is a real segment on the same side of the road as its inner edge.
 *
 * MEASURED, AND THE HYPOTHESIS IS WRONG. R14 swept this on seed 99991 with the
 * geometry probe below reporting the real footprint rather than two corners:
 *
 *   config              mass_on_road  mass_on_road_geom  fold/tip  sev g/x fold
 *   R13_FOLD=0                     2                190     25/165     129/25
 *   shipped (keep 0)              12                190     17/173        0/0
 *   keep 0.14                     12                190     17/173        0/0
 *   keep 0.29                     13                190     17/173        0/0
 *   keep 0.50                     16                190     15/175        0/0
 *
 * mass_on_road_geom is 190 in EVERY configuration, worst intrusion 2990-2996
 * throughout. Raising keep does not remove one intruding block; it makes the
 * two-corner count and ground_on_road WORSE (12->16 and 62->78) by pulling the
 * ground in. So the tip is not what puts mass on the road, R13's "one-constant
 * fix" is not one, and this stays at 0 -- bit-identical to R13 -- rather than
 * shipping a change that trades a real regression for no measured gain.
 *
 * Kept as a live instrument (TD5RE_R14_KEEP, needs TD5RE_R14_KEEP_ON=1) so the
 * next round can re-run the sweep in one command instead of rebuilding. */
#define TD5_TG_R14_KEEP  0.0f
double tg_r14_keep(void);
double tg_r13_fold_reach(const TG_NodeList *nl, int si, double side, double ang);
/* Shortest reach the fold cap may impose. A hairpin's convergence point can sit
 * a few hundred units off the kerb, and collapsing a verge or a street to that
 * trades a doubled surface for a bare one. Below this the cap yields: the fold
 * is then confined to a strip narrower than one pavement, which is not what the
 * complaint is about. */
#define TD5_TG_R13_FLOOR 2600.0
/* A block still has to be a block. Below this the cap yields and the massing
 * keeps a minimum body rather than degenerating into the zero-thickness sheet
 * the FACADE_MASS pass exists to remove. */
#define TD5_TG_R13_MIN_DEPTH 900.0
double tg_r13_fold_cap(const TG_NodeList *nl, int si, double side, double ang, double want, const char *knob);
int tg_emit_road_quad_taper(const TG_NodeList *nl, int si, double u_scale, double shift_near, double shift_far, double wscale_near, double wscale_far, int page, TG_Buf *blk);
int tg_emit_road_quad(const TG_NodeList *nl, int si, int lanes, double shift_near, double shift_far, double wscale, int page, TG_Buf *blk);
int tg_emit_road_mesh(const TG_NodeList *nl, int si, int lanes, TG_Buf *blk);
int tg_emit_box_mesh(TG_Buf *blk, double cx, double cy, double cz, double hx, double hy, double hz, double fx, double fz, int page, double tile, unsigned int color);
/* A camera-facing billboard: the natural primitive for a tree, where a box
 * reads as a hedge slab. Two things differ from the opaque box path:
 *   - the mesh header field at 0x02 is a BILLBOARD TAG, not a page id: 1 or 2
 *     makes the renderer face the quad at the camera (td5_render_mesh.c:1720);
 *   - `origin` carries the WORLD position in 24.8, where opaque geometry
 *     leaves it zero (td5_render_mesh.c:1666), so the vertices below are LOCAL
 *     offsets about that origin -- x across, y up from the base.
 * The sampled page still comes from the command, as everywhere else.
 *
 * [UNCERTAIN] The transparent key index for an alpha-keyed page is not
 * documented in what I read; palette index 0 is reserved for it here. If the
 * engine keys on something else the surround will show as a solid block rather
 * than cutting out -- visible immediately, and diagnosable. */
/* Declared here, defined with the TREES table it consults: some borrowed TD5
 * foliage pages hold only ONE HALF of a mirrored pair, and this writer is the
 * single place that can rebuild the whole tree from one. */
int tg_tree_page_is_half(int page);
int tg_emit_billboard_mesh(TG_Buf *blk, double wx, double wy, double wz, double half_w, double height, int page, int tag);
/* ===================== FACADE WALLS =====================
 * Shipped TD5 buildings are NOT closed boxes with a UV-wrapped masonry texture.
 * A survey of level014 (Sydney, 1743 meshes) found exactly ONE 6-quad box:
 * every building front is a FLAT array of road-facing quads, and every shipped
 * vertex UV is inside [0,1] -- pages are mapped ONCE per quad, never wrapped. A
 * wide/tall frontage is built by REPEATING GEOMETRY (many whole-page cells side
 * by side), not by stretching one page across a big face. That is why our boxes
 * read as "cut off in the middle": tg_emit_box_mesh sets UV = 2*half/tile > 1,
 * slicing the facade page mid-window at each edge.
 *
 * The facade path below imitates the shipped look: a grid of whole-page cells
 * laid flat along the road into a continuous street wall, so the texture is what
 * sizes the building (one page image per cell) and nothing is ever cut. */
/* Worst case, both sides: front grid (4 cols x 8 floors) + two run-end corner
 * prisms per side (5*floors + 2 quads each, see tg_facade_push_cap). 232 at the
 * tallest tower run; 256 leaves headroom. The arrays are stack doubles, so this
 * is ~40 KB of frame -- fine, but do not grow it casually. */
#define TD5_TG_FACADE_MAXQUAD 320
/* Half a texel of a 64x64 page -- the only page size the TEXTURES.DAT container
 * carries (TD5_TG_TEX_DIM, defined with the page emitters far below).
 *
 * Why every cell is inset by it: scenery samples LINEAR + WRAP (d3d12_backend.c
 * sampler table), so a UV of exactly 1.0 lands on the texel boundary and the
 * bilinear tap blends texel 63 with texel 0 -- the OPPOSITE edge of the same
 * page. On the procedural pages that is a grey-on-grey smear, but the real
 * borrowed frontages (k_real_wall_*, photographic level014 pages) have a strong
 * coloured column/row at one edge, so every cell boundary drew a coloured line
 * across the building. That is the "colored lines on buildings" report. Insetting
 * the cell by half a texel keeps every tap strictly inside the page and costs
 * half a texel of image at the seam. */
#define TD5_TG_FACADE_UV_INSET (0.5 / 64.0)
/* A run this many floors tall counts as a TOWER: it stands above the back rows,
 * so it needs its own back wall (see the MASS pass in tg_emit_street_wall).
 * CITY runs are 2..4 floors normally and 3..8 in a tower cluster, so 5 selects
 * the towers and nothing else. */
#define TD5_TG_FACADE_TALL_ROWS 5
/* Hard ceiling on a run's floor count -- a quad-budget bound, see the note at
 * the clamp in tg_side_geom. */
#define TD5_TG_FACADE_MAX_ROWS 10
int tg_write_quad_mesh(TG_Buf *blk, const double *px, const double *py, const double *pz, const double *uu, const double *vv, int n, const int *seg_page, const int *seg_nq, int nseg);
int tg_write_quad_mesh_col(TG_Buf *blk, const double *px, const double *py, const double *pz, const double *uu, const double *vv, const unsigned int *col, int n, const int *seg_page, const int *seg_nq, int nseg);
void tg_facade_push_grid(double bx, double by, double bz, double ax, double ay, double az, double ux, double uy, double uz, int cols, int rows, int r0, int r1, double *px, double *py, double *pz, double *uu, double *vv, int *pn);
/* Is a facade wall present at span si on this side? Spans group into
 * SUPERBLOCKS, and each superblock carries ONE side street whose START and
 * WIDTH both come from the superblock hash.
 *
 * The first cut divided a fixed 13-span period into "leading gap, then run",
 * which put a gap boundary on every 13-span beat: the gap WIDTH varied but its
 * position did not, so the street read as a metronome. Varying the start as
 * well, and letting runs cross superblock boundaries (a run that ends one
 * superblock joins the run that starts the next), gives gaps of 3..8 spans at
 * moving positions and run lengths anywhere from ~4 to ~30 spans.
 *
 * The two sides break at different spans (the +777 offset) so a street is never
 * gapped on both sides at once. */
#define TD5_TG_FACADE_PERIOD 22
/* Spans from the start line that are forced to a solid frontage -- see below. */
#define TD5_TG_FACADE_START_RUN 60
/* STREET vs AVENUE. A real city has a hierarchy: most side streets are narrow
 * and meet the road on one side only (a T junction), and every few blocks an
 * AVENUE crosses it -- wider, and continuing through BOTH kerbs as one
 * crossroads. The first cut had a single uniform gap of 3..8 spans, always
 * offset by 777 spans between the two sides so the kerbs could never open
 * together, which is exactly what stopped any crossing from reading as a
 * crossing: there was never a street on the far side to continue into.
 *
 * One superblock in four is an avenue. The avenue test uses the span's own
 * index, not the per-side one, so both kerbs agree on which blocks are avenues;
 * only minor streets keep the per-side offset. Where an avenue block abuts a
 * street block the left kerb changes partition, which can leave a one- or
 * two-span sliver of wall or gap at the seam -- an alley, which a city has. */
#define TD5_TG_AVENUE_IN     4       /* 1 superblock in N is an avenue */
void tg_facade_block(int si, int left, unsigned int *block, unsigned int *phase, unsigned int *gs, unsigned int *gl, int *avenue);
/* ===================== [R8 CROSS item 9] TURN CONTINUATION =====================
 * "implement 'continuation' of a perpendicular street, where you make a sharp
 * turn, that turn could be coming from an existing street."
 *
 * A generated bend currently leaves the frontage running unbroken around the
 * outside of the corner, so the road reads as bending in a void: there is no
 * reason for the turn to be there. A real street grid explains it -- you were on
 * a street, the street carries straight on, and your route turns off it. So at a
 * SHARP bend the outside frontage opens and a street runs out of that opening
 * along the INCOMING heading: the straight you arrived on, continued.
 *
 * The whole element is built by MACHINERY THIS AREA ALREADY OWNS. Rather than a
 * new mesh kind, a turn marks two things:
 *   1. tg_facade_built returns 0 on the outside of the bend, which is the
 *      existing definition of a street opening -- so the cross-street
 *      carriageway, the pavement + railing arms, the pedestrian crossing, the
 *      reveal row and the R8 flanking massing ALL lay themselves there, already
 *      agreeing with each other because they read one predicate.
 *   2. tg_block_arm_skew returns the signed angle from that side's outward
 *      normal to the incoming tangent, which is the existing hook every one of
 *      those emitters already routes its outward direction through (it is what
 *      makes DIAGONAL side streets diagonal). Pointing it at the incoming
 *      heading is what turns a perpendicular side street into a continuation.
 * No emitter below is edited for item 9; the two predicates do all of it.
 *
 * The map is precomputed once per build (tg_turn_map_build, called from
 * tg_emit_models) because both predicates are called with a span index and no
 * node list, and because a per-call rescan would be quadratic.
 *
 * BOXING IN. Nothing here touches the carriageway, the strip or the routes: it
 * only OPENS a frontage (removing mass, never adding it near the road) and
 * points existing outward-running scenery along a different bearing. Everything
 * the opening then emits starts at the kerb and runs away from the road, exactly
 * as an ordinary side street does. Default ON; TD5RE_R8_CROSS_TURN=0 disables. */
#define TD5_TG_R8_TURN_BASE   3      /* spans either side used to read the bend */
#define TD5_TG_R8_TURN_SIN 0.34      /* |sin| of the heading change to qualify  */
#define TD5_TG_R8_TURN_GAP   16      /* min spans between two continuations     */
/* [R11 CITY item 10] spans either side of a continuation that must already be
 * unbroken frontage. 2 is the minimum that makes stranding impossible: with the
 * opening at si, spans si-1 and si+1 must be built, so neither neighbour can be
 * a gap edge and no one-span block can be left between two mouths. */
#define TD5_TG_R11_TURN_CLEAR 2
/* Steepest angle any side street may lean off its side's outward normal. Stated
 * here rather than beside tg_block_arm_skew because BOTH of that function's two
 * branches have to respect it, and until [R14 item 4] only one of them did.
 *
 * The decorative-diagonal branch has been capped at this since R8, with the
 * reason written down: "capped well under 45 deg so a leaning arm can never
 * double back across the main carriageway (which would put a side street on top
 * of the road)". The TURN CONTINUATION branch returns s_turn_skew[] instead, and
 * that value is the full angle from the outward normal to the incoming heading,
 * bounded only by the `dot < 0.30` rejection above -- which admits up to ~72.5
 * deg. So the one construction this file says must never happen was reachable
 * by the one path that was never asked to obey the rule, and it was reachable
 * ONLY at a sharp bend, because that is the only place continuations exist.
 * That is the "road intersections placed on a curve" half of item 4. */
#define TD5_TG_DIAG_MAX_DEG   28.0    /* steepest diagonal side street, deg */
/* The continuation's OWN ceiling, and NOT TD5_TG_DIAG_MAX_DEG, even though both
 * are the same quantity (degrees off that side's outward normal) and the larger
 * value is the looser rule. The two branches sample opposite ends of it:
 *
 *   decorative diagonal - skew is drawn from a hash, 0 is the common case, and
 *     28 deg is a lean applied to an otherwise square street.
 *   turn continuation   - skew is 90 deg MINUS the turn the road makes over the
 *     map's own +/-3 span window, so it runs the other way: a 90 deg corner
 *     gives skew 0, and the `dot < 0.30` test above admits everything up to
 *     ~72.5 deg, i.e. a bend of only ~17.5 deg. Capping this branch at 28 deg
 *     would demand a 62 deg corner and refuse essentially every continuation on
 *     every seed -- that is not a safeguard, it is deleting the feature.
 *
 * So the ceiling cuts the near-parallel tail -- the arms that leave the kerb
 * almost ALONG the road and so lay their carriageway, pavements, crossing and
 * flanking massing back over it -- while leaving the honest corner-turn
 * continuations the feature exists for.
 *
 * Derived from TD5_TG_DIAG_MAX_DEG rather than tuned, so the two branches stay
 * one decision: an ordinary diagonal may lean 28 deg off the normal, i.e. it
 * always meets the road at >= 62 deg. A continuation is held to meeting the
 * road at least as steeply as that same 28 deg -- the mirror of the ordinary
 * rule, and far more permissive than it, since 62 deg of lean would be refused
 * outright as an ordinary street.
 *
 * MEASURED, seed 99991, 99 candidates: skews run 45.9 to 72.4 deg (median
 * 59.5) and NOT ONE is under 45 deg, which is why the ordinary 28 deg ceiling
 * could not be reused -- it would have refused all 99. This ceiling refuses 44
 * and keeps 55. TD5RE_R14_TURN_SKEW_MAX overrides it; =90 disables in effect. */
#define TD5_TG_R14_SKEW_MAX_DEG (90.0 - TD5_TG_DIAG_MAX_DEG)
/* [R11 CITY item 10] The gap guard below reads the run/gap pattern around a
 * candidate bend, and tg_facade_built is defined just under the map (it consults
 * the map, which is why it comes after). Reading it here is safe and not
 * circular: the guard only looks TD5_TG_R11_TURN_CLEAR spans either side, and
 * TD5_TG_R8_TURN_GAP keeps any already-marked continuation 16 spans away, so no
 * entry the guard reads has been written by this pass. */
int tg_facade_built(int si, int left);
extern signed char s_turn_side[TD5_TG_MAX_SPANS];
extern float s_turn_skew[TD5_TG_MAX_SPANS];
extern int s_r14_turn_cand;
extern int s_r14_turn_refused;
extern double s_r14_turn_worst;
extern int s_r14_turn_opened;
void tg_turn_map_build(const TG_NodeList *nl, int nspans);
int tg_turn_open(int si, int left);
double tg_turn_bend(int si);
int tg_facade_built(int si, int left);
int tg_side_built(int si, int left);
int tg_facade_isolated(int si, int left);
int tg_r11_corner_stands(int si, int left);
/* [R16 CITY item 1] Does the raised PAVEMENT / street grid (as opposed to a
 * building WALL) stand on side `left` at span si? Like tg_r11_corner_stands but
 * blind to the outskirts density ramp -- used only by JUNCTION furniture
 * (pavement arms, crossing base) so a ramp-retracted frontage still wraps the
 * sidewalk down its side street. See the definition in td5_tg_city.c. */
int tg_r16_pave_corner_stands(int si, int left);
unsigned int tg_facade_run_id(int si, int left);
/* DOWNTOWN GRADIENT. Height was a per-run hash alone, so a "city" was a random
 * jumble of 2..4-storey blocks with the odd tower and no sense of place. A real
 * city has a centre: frontages climb toward a core and fall away to suburbs.
 *
 * The cycle is evaluated at the SUPERBLOCK, never at the span -- keying it to
 * the span would change height inside one building and saw-tooth its roofline --
 * and it is a raised cosine rather than a step so the skyline ramps into the
 * core over several blocks instead of jumping at one street corner. */
#define TD5_TG_DISTRICT_BLOCKS 12    /* superblocks per core-to-core cycle */
#define TD5_TG_DOWNTOWN_FLOORS  5    /* extra floors at the core */
int tg_city_district_floors(unsigned int block);
int tg_page_is_r8_variety(int page);
int tg_facade_page_class(unsigned int gh, int rows);
/* ===================== TREES =====================
 * A palette of tree/palm/conifer/topiary SPECIES. Each entry is a billboard
 * size (raw = world_units*256) plus a procedural silhouette shape used ONLY
 * when real textures are switched off. Real mode -- the default since
 * 2026-08-27, see tg_real_textures_enabled -- fills each variant page from a
 * shipped TD5 foliage page (level ids in the comments); index 0 of those pages
 * is the transparent key. Biomes reference these by index (tree_set), mixed.
 *
 * `half` marks a page that holds only ONE HALF of a mirrored pair, which is
 * what the "trees cut in half" report was seeing: the shipped level placed two
 * quads sharing a vertical axis, we drew one of them alone. MEASURED from the
 * key coverage of the borrowed pages in td5_tg_real_tex.h (64x64 index maps,
 * index 0 = key), per page: how far the non-key content reaches at each edge.
 *   whole (keyed margin on BOTH edges, so the silhouette closes off):
 *     0 cols 2..59   1 cols 2..61   5 cols 8..57   6 cols 1..63 (col63 = 1
 *     texel)   7 cols 7..58   8 cols 1..62 (col63 = 0)
 *   half (content FLUSH at column 63 -- a straight vertical cut -- with a wide
 *   keyed margin on the left, and a silhouette that only widens toward that
 *   edge, i.e. the edge IS the tree's axis):
 *     2 cols 13..63, col63 filled on 55 of 64 rows
 *     3 cols 37..63, col63 on 45  (the whole page is one flank of the cone)
 *     4 cols 38..63, col63 on 49
 *     9 cols  7..63, col63 on 41
 * The same measurement over the PROP pages found none half (only prop3, the
 * monument, reaches both edges, and it reaches BOTH -- a full-page image), so
 * this is a trees-only problem. Procedural pages draw whole trees, so the
 * mirroring in tg_emit_billboard_mesh is real-texture mode only. */
enum { TG_TREE_DECID = 0, TG_TREE_CONIFER, TG_TREE_PALM, TG_TREE_TOPIARY,
       TG_TREE_WILLOW };
typedef struct { int w, h, shape, half; } TG_TreePage;
extern const TG_TreePage k_tree_pages[TD5_TG_TREE_VARIANTS];
int tg_tree_slot(int v);
int tg_flora_tree_slot(int v);
int tg_real_textures_enabled(void);   /* defined with the page emitters */
int tg_tree_page_is_half(int page);
/* ---- branch-corridor clearance for verge scenery ----
 * Trees are placed off the MAIN RING only, so where a branch corridor runs
 * alongside they land on the branch carriageway. The corridor bows up to
 * TD5_TG_BRANCH_BOW (1.2) road widths into the -ve lateral, far past the
 * 800..3200 tree setback, which is why the trees were standing in the road.
 * Group E owns the fork data; this only READS it. */
#define TD5_TG_FLORA_BRANCH_MARGIN  600.0   /* verge left between road and trunk */
double tg_flora_gap_clear(const TG_NodeList *nl, int si, double side, double gap);
/* ---- [R12 FLORA] plant ledger + tree-line coverage ledger ----------------
 * R12 items 4 ("some billboard trees fight over which one draws in front") and
 * 9 ("no tree background for a few spans") are both claims about WHAT WAS
 * EMITTED, so both are answered by recording every plant and every tree-line
 * band decision as the level is built and reporting on the whole track
 * afterwards (tg_r12_flora_report). Recording is unconditional and costs one
 * struct store per tree; the REPORT is opt-in.
 *
 * Why a ledger and not a per-emitter print: four separate emitters plant tree
 * billboards (near verge, park canopy, R9 slope, R7 branch verge) and item 4 is
 * a claim about a PAIR, which no single emitter can see. */
#define TD5_TG_R12_FLORA_MAX 6000
typedef struct {
    int    si, page;
    double side, cx, cz, tw, th;
    const char *kind;
} TG_R12FloraRec;
extern TG_R12FloraRec s_r12_flora[TD5_TG_R12_FLORA_MAX];
extern int s_r12_flora_n;
/* Why did the tree-line band not stand on span si? One code per side. */
enum {
    TG_R12_BAND_OK = 0, TG_R12_BAND_OFF, TG_R12_BAND_GRID, TG_R12_BAND_END,
    TG_R12_BAND_BRIDGE, TG_R12_BAND_BIOME, TG_R12_BAND_SLOTS
};
extern unsigned char s_r12_band_why[TD5_TG_MAX_SPANS];
extern int s_r12_band_seen;
/* [R12 FLORA item 4] "some billboard trees fight over which one draws in front."
 *
 * MECHANISM, measured off the ledger above (TD5RE_R12_FLORA_REPORT=1, seed
 * 20260901): 1279 tree billboards, of which 49 PAIRS stand closer together than
 * a quarter of the narrower canopy's width and 9 pairs closer than 300 raw --
 * e.g. si=952 park/near d=123 on canopies 4185 and 3328 wide, si=321
 * near/branch d=199, si=132/135 r9slope/r9slope d=182. Every kind of pair is
 * present (park/near 15, near/near 14, near/branch 9, r9slope/r9slope 6,
 * park/park 5), i.e. FOUR emitters plant into the same verge with no shared
 * view of what is already there, and the near-verge band (gap 800..3200) and
 * the park canopy band (2600..5800) overlap outright.
 *
 * It is NOT depth-buffer precision and NOT a sort instability, both of which
 * were checked first: tree billboards are mesh tag 1, which takes the immediate
 * FOLIAGE_CUTOUT path (alpha-tested OPAQUE, z-write ON) and never enters the
 * 4096-bucket billboard depth sort, and the depth target is D32_FLOAT with 1/z
 * distribution, which resolves well under a raw unit at treeline distance. What
 * actually happens is geometric: camera-facing quads are all PARALLEL planes, so
 * two trunks 123 raw apart carry two 4000-wide canopies that overlap over
 * essentially their whole area, and which one is in front flips the instant the
 * camera crosses the pair's bisector -- twice a second at racing speed. The two
 * pages swap places, which is exactly "they fight over which draws in front".
 *
 * So the fix is spacing, and it belongs in ONE place rather than in each of the
 * four emitters: a trunk may not stand inside a quarter of an already-planted
 * canopy's width, measured against every plant within +/-3 spans. A quarter is
 * the measured gap in the population -- ordinary forest density (one tree per
 * span, 1500 apart, d/narrow 0.3..0.5) is untouched, and 3.8% of plants are
 * rejected. Rejected means DROPPED, not nudged: a nudge would only re-run the
 * same lottery against the next neighbour, and one tree in 26 is invisible in a
 * forest whereas a moved trunk can land on a verge that was cleared for a
 * reason. Default ON; TD5RE_R12_FLORA_SPACE=0 restores the coincident plants. */
#define TD5_TG_R12_SEP_FRAC 0.25
extern int s_r12_flora_rejects;
int tg_r12_flora_accept(int si, const char *kind, int page, double side, double cx, double cz, double tw, double th);
void tg_flora_diag(const TG_NodeList *nl, int si, const char *kind, int page, double side, double tw, double th, double cx, double cz);
/* ===================== PROPS =====================
 * Roadside billboards beyond trees: spectators, statues, animals and streetlamp
 * glows -- all one camera-facing quad, differing only in page/size/tag. Each
 * entry: billboard size (raw), the mesh billboard tag (1 camera-facing, 2
 * additive), the page TYPE (1 alpha-keyed, 3 additive) for the real page, a Y
 * lift (streetlamp glows float), and a procedural silhouette kind. Real mode
 * fills the pages from shipped foliage/figure pages (level ids in comments). */
enum { TG_PROP_PERSON = 0, TG_PROP_STATUE, TG_PROP_ANIMAL, TG_PROP_LAMP };
/* prop-page indices (into k_prop_pages) used by biomes */
enum { PP_PERSON0 = 0, PP_PERSON1, PP_LION, PP_MONUMENT, PP_SHEEP, PP_DEER, PP_LAMP };
/* People are HALF the size they were until 2026-08-26. The scale anchor in this
 * file is TD5_TG_LANE_WIDTH = 1500 raw per lane: at a real 3.65 m lane that is
 * 411 raw per metre, so the old 600x1400 spectator stood 3.4 m tall and 1.5 m
 * wide -- 0.93 of a lane width tall, where a 1.8 m human beside a 3.65 m lane
 * is 0.49. 300x700 puts them at 1.70 m tall / 0.73 m wide, which is 0.47 of a
 * lane. Only the PEOPLE were rescaled; the statue/animal/lamp entries are what
 * the report asked to leave alone. */
typedef struct { int w, h, tag, type, y_off, kind; } TG_PropPage;
extern const TG_PropPage k_prop_pages[TD5_TG_PROP_COUNT];
int tg_prop_slot(int i);
/* ===================== ROAD SURFACES =====================
 * Each biome drives on one surface: a GRIP class written into the strip's
 * surface byte (so the car really slides on ice / drags on cobble) plus a
 * matching texture. Grip classes are the shipped values (td5_physics.c grip
 * table): 1 tarmac 1.0, 4 gravel 0.98, 3 dirt 0.94, 5 cobble 0.75, 6 ice 0.70. */
enum { RS_TARMAC = 0, RS_GRAVEL, RS_DIRT, RS_ICE, RS_COBBLE };
typedef struct { int grip_class; int page_var; int proc_kind; } TG_RoadSurf;
extern const TG_RoadSurf k_road_surf[TD5_TG_ROAD_VARIANTS];
int tg_road_slot(int v);
/* ===================== BIOMES =====================
 * A biome owns a RUN of spans and drives what stands beside the road: how
 * dense the props are, how tall, how far back, and which texture page. That is
 * what makes a stretch of city read differently from open fields without
 * needing separate emitters per biome -- every prop is still a box.
 *
 * Not yet driven by biome: the section mix (straight/curve/acute weighting).
 * The section picker runs during the centerline walk, before any of this, so
 * per-biome cornering needs the picker to know its own position first. */
/* Facade/tree sizes below are the SHIPPED-track measurements (level014 city,
 * level005 rural), in raw 24.8 units == world_units * 256. A survey found a
 * facade page-cell is ~8.4 x 11.5 wu (2150 x 2950 raw), a city frontage ~3
 * floors tall, buildings sit ON the curb (setback ~0), and city trees are big
 * and sparse while rural trees are small and dense. */
#define TD5_TG_TREESET_MAX 4
typedef struct {
    const char *name;
    int    density;      /* trees: prop if (hash>>28) <= this, so 0..15 */
    int    cell_w, cell_h;      /* facade page-cell world size, raw */
    int    floors_min, floors_extra; /* facade height in cells */
    int    depth;        /* building depth for side returns, raw */
    int    sidewalk;     /* setback from road edge, raw (~0 = on the curb) */
    int    tree_set[TD5_TG_TREESET_MAX]; /* tree-variant indices to mix */
    int    tree_n;       /* how many entries of tree_set are used (0 = facade) */
    int    page;         /* facade WALL page for box biomes */
    int    tower_mask;   /* (hash>>3 & mask)==0 -> a taller run (tower cluster) */
    int    billboard;    /* 1 = camera-facing trees, 0 = facade wall */
    int    ground_page;  /* page for the terrain slab under/around the road */
    /* Prop layer (Phase 2). people = spectator density 0..15 (0 none);
     * lamp = 1 for streetlamp glows; statue/animal = a k_prop_pages index or
     * -1. Props are emitted for every biome, additional to trees/facades. */
    int    prop_people, prop_lamp, prop_statue, prop_animal;
    int    water;        /* 1 = a sea plane on the seaward side of the run */
    int    road_surf;    /* index into k_road_surf: drivable surface + grip */
    /* ---- [R2 item 23] biome character: adjacency axes + feature weighting ----
     * climate  0 = warm/tropical, 1 = temperate, 2 = cold/alpine
     * urbanity 0 = wilderness, 1 = rural, 2 = edge-of-town, 3 = dense urban
     * w_bridge / w_tunnel  percent weighting the bridge/tunnel emitters scale
     *          their own gates by -- 100 = "as often as the global rate", 0 =
     *          never here, 200 = twice as often here.
     * repeat_max  how many CONSECUTIVE 150-span cells this biome may occupy.
     *          This is how "cities should have urban and suburban areas longer"
     *          is expressed without changing the fixed run grid that the water
     *          and prop emitters key their per-run state off. */
    int    climate, urbanity;
    int    w_bridge, w_tunnel;
    int    repeat_max;
} TG_Biome;
extern const TG_Biome k_biomes[];
/* Biomes drawable by a NORMAL (non-snow-coherent) seed. Deliberately 7, not
 * ARRAY_COUNT(k_biomes): see the ALPTOWN note above. */
#define TD5_TG_BIOME_COUNT 7
/* Biomes drawable once a seed has been declared snowy (adds ALPTOWN). */
#define TD5_TG_BIOME_COUNT_SNOW 8
/* [R11 BIOME] How many biome ROWS the table actually has -- the correct size for
 * anything INDEXED BY a biome index (tg_biome_cell_index can return 7). The two
 * COUNTs above are draw MODULI, not array bounds, and using one as a bound is
 * an out-of-bounds write on every snow seed. Kept as its own name so the
 * distinction is stated once instead of being rediscovered. */
#define TD5_TG_BIOME_KINDS ((int)(sizeof(k_biomes) / sizeof(k_biomes[0])))
#define TD5_TG_BIOME_ALPINE  4    /* index of ALPINE in k_biomes */
#define TD5_TG_BIOME_RUN   150
/* Biome per 150-span CELL. The cell grid is deliberately unchanged from the old
 * hash version: the water and prop emitters derive per-run state from
 * (span / TD5_TG_BIOME_RUN), so making runs variable-length would silently
 * desynchronise them. Length comes from repeating a cell instead. */
#define TD5_TG_BIOME_CELLS ((TD5_TG_MAX_SPANS / TD5_TG_BIOME_RUN) + 2)
/* ==========================================================================
 * ONE-SIDE SEA  ([R8] BIOME, seed 777 item 19, third clause: "add one side sea
 * like on sydney")
 *
 * The machinery is already here -- COAST carries water=1, tg_water_side picks a
 * side and holds it, tg_emit_water lays the plane, R4's coastline strip joins
 * it to the bank and R7 FLORA's SHORE_VERGE keeps palms out of it. What is
 * missing is DURATION: COAST is pinned to repeat_max 1, so the sea is never
 * more than one 150-span cell, and Sydney's sea is alongside you for minutes.
 *
 * COAST's own table row says why it was pinned, and says what has to happen
 * first: "the water emitter keys its sea level and its seaward side off the
 * 150-span cell, so a COAST spanning two cells could step the sea surface or
 * flip which side it is on. Lift this only together with tg_biome_run_bounds
 * adoption there."  So that is done first, below, in tg_water_side and
 * tg_sea_level_y: both now key off the MERGED run rather than the raw cell.
 * With COAST still at repeat_max 1 a merged run IS one cell, so that adoption
 * is a provable no-op on every existing seed -- it changes nothing until the
 * cap is lifted.
 *
 * The cap lift itself is knob-gated and DEFAULT OFF for round 8. Not because it
 * is unsound but because seed 99991 contains a COAST run, so lifting the cap
 * would move 99991's biome boundaries -- and 99991 is the reference layout the
 * other seven R8 areas are quoting span numbers against this round. Turning it
 * on is a one-variable change once the round closes.
 *
 * td5_env_flag_on returns 1 for an UNSET variable, which is the wrong default
 * here, so this reads through td5_env_int with an explicit 0 default instead.
 * ========================================================================== */
#define TD5_TG_COAST_REPEAT_SEA 3   /* ~1.5 km of sea alongside, Sydney-scale */
extern int s_biome_snow_seed;
void tg_biome_layout(unsigned int seed, int nspans_hint);
int tg_biome_cell_index(int si);
void tg_biome_run_bounds(int si, int *out_a, int *out_b);
/* Width of the dithered transition band, in spans, either side of a cell
 * boundary. 20 spans is ~70 m of road at the scale derived above: long enough
 * that the changeover reads as a gradient at racing speed, short enough that a
 * 150-span run still has a solid ~110-span core of its own character. */
#define TD5_TG_BIOME_BLEND 20
int tg_biome_for_span(int si);
int tg_biome_bridge_pct(int si);
int tg_biome_tunnel_pct(int si);
int tg_biome_span_is_city(int si);
int tg_biome_index_is_city(int idx);
/* ==========================================================================
 * [R11 BIOME item 4] OUTSKIRTS -- THE WILDERNESS-TO-TOWN EDGE
 *
 * THE REPORT: "the transition park/green/snowy/scenery -> city should pass
 * through a SMALL TOWN band first, then city."
 *
 * WHAT THE USER ACTUALLY DROVE, which is what this implements. The round-11
 * seed (20260901) is snow-coherent and contains NO CITY and NO INDUSTRIAL at
 * all -- its runs are FOREST / ALPINE / ALPTOWN / FIELDS / ALPINE / ALPTOWN /
 * ALPINE / ALPTOWN / ALPINE / ALPTOWN. So the "city" in the report is ALPTOWN,
 * the snowy TOWN biome, and the transitions being complained about are the
 * ALPINE -> ALPTOWN edges at spans 450, 1050, 1650 and 1950. The adjacency
 * rules already guarantee a graduated SEQUENCE of biomes (Rule 2 makes a
 * wilderness->city step illegal, which is why INDUSTRIAL or ORIENTAL always
 * stands between open country and a CITY); what is missing is a gradient
 * WITHIN the town, so the town's first span is as dense as its core.
 *
 * So the fix is not a new biome and it is emphatically NOT a change to the draw
 * modulus: TD5_TG_BIOME_COUNT is the modulus of every seed's layout, and this
 * round's feedback plus seven sibling branches are all pinned to the current
 * grid. Instead the town's BUILT-UP CHARACTER ramps in across its leading
 * spans, so you drive wilderness -> sparse low outskirts -> town.
 *
 * WHAT RAMPS AND WHAT SNAPS. This is the whole design, and it is a direct
 * consequence of what R11 GUARD just landed one function above: the raised
 * pavement, the kerb height, the pedestrian railing and the ownership of the
 * road edge now change on ONE span at a paved/unpaved boundary, precisely
 * because a half-built kerb is not a transition, it is a flicker.
 *
 *   SNAPS (untouched here): pavement presence and width, kerb, kerb railing,
 *   roadside barrier hand-off, ground/verge page, road surface and grip. All of
 *   these live downstream of tg_scenery_biome_index, which this does not touch.
 *
 *   RAMPS (this block): how MUCH town there is -- how many frontages stand, how
 *   tall they are, and how tall the blocks behind them are.
 *
 * Density and height are exactly the quantities you CAN half-build: a town's
 * edge really does have detached low buildings with gaps between them, and
 * scaling them is not a flicker because the ramp is monotone in distance and
 * quantised to the block grid (below), so nothing alternates.
 *
 * WHY THIS IS NOT ROUTED THROUGH tg_facade_built. That predicate is the
 * run/gap PATTERN, and it is the single definition of "there is a SIDE STREET
 * here" -- the cross-street carriageway, the pavement arms, the back rows, the
 * reveal row and the pedestrian ZEBRA all read it, and tg_crossing_base fires
 * on its built->gap EDGE. Thinning frontages through it would therefore invent
 * a side street and paint a zebra crossing at every gap the ramp opened, which
 * is R11's items 9 and 15 rather than a fix for item 4. The correct seam is
 * tg_side_built / tg_side_geom -- "does a wall actually STAND here" -- which is
 * where the fork-corridor clearance and the R6 no-lone-stub rule already live,
 * and which no crossing reads. So the ramp removes BUILDINGS and leaves the
 * street grid, the pavement and the crossings exactly where they were.
 * ========================================================================== */

/* Length of the outskirts band, in spans. Three facade superblocks: the ramp is
 * quantised to TD5_TG_FACADE_PERIOD (see tg_town_ramp) so that every span of one
 * building shares one ramp value, which makes three blocks the shortest band
 * that can actually show a gradient rather than a single step. 66 spans is
 * ~230 m at the scale the biome block derives. */
#define TD5_TG_TOWN_RAMP (3 * TD5_TG_FACADE_PERIOD)
double tg_town_ramp(int si);
int tg_town_ramp_open(int si, int left);
int tg_scenery_biome_index(int si);
int tg_span_surface_is_tarmac(int si);
/* TUNNEL EXCEPTION -- the one reason a span's surface is not its biome's.
 * Defined further down; forward-declared here because the two accessors below
 * sit above the tunnel block. */
int tg_span_in_tunnel(int si);
int tg_surface_attr(int si);
int tg_road_page(int si);
/* A continuous street wall of flat facade cells lining span si -- one mesh,
 * both sides. A side is built per tg_facade_built (runs separated by side
 * streets). Consecutive built spans share their near/far endpoints, so the
 * facades ABUT into an unbroken wall the way a shipped city block does.
 *
 * Single- vs multi-sided is POSITIONAL, exactly as in the shipped data: a
 * run-INTERIOR span shows only its road-facing plane (single-sided); a run-END
 * span (its neighbour on that side is a gap) also gets a RETURN cap turning the
 * corner, so the wall does not read as a paper edge (multi-sided). Setback and
 * base height are keyed to the RUN, not the span, so a wall stays straight and a
 * block shares a rough height while individual buildings still step. */
typedef struct {
    int    built;
    double bx, by, bz, ax, ay, az, H, depth;
    double lx0, lz0, lx1, lz1;
    int    cols, rows, cap_near, cap_far;
    /* [R8 item 6] Whole cells of DEPTH for this run. Per SIDE, not per mesh:
     * the two sides of one span belong to different runs and so may be
     * different depths, and `depth` above is just dcols * the cell width. */
    int    dcols;
} TG_SideGeom;
/* ===================== CITY PAVEMENT GEOMETRY =====================
 * Values the biome table does not carry, derived from it here rather than added
 * to the shared struct: the facade SETBACK and the SLAB that carries it have to
 * agree exactly or the wall floats over the kerb, so they read one function.
 * Raw = world_units * 256; a lane is TD5_TG_LANE_WIDTH (1500) raw. */

/* Kerb rise of a city pavement. A step, not a wall: 130 raw is ~0.5 wu, low
 * enough that the slab does not read as a plinth and high enough that the kerb
 * face is still a visible line at speed. */
#define TD5_TG_KERB_H       130.0
/* NO BUILDING EVER TOUCHES THE ROAD. This is the one place that gap is decided,
 * for every facade emitter (street wall, corner returns, back rows) and for the
 * pavement that fills it, so the two cannot disagree and no per-biome table
 * value can undercut it.
 *
 * k_biomes carries `sidewalk` only as the facade setback, and the CITY value
 * (350 raw) came from the shipped measurement "buildings sit on the curb" --
 * less than a quarter of a lane. That is a kerb, not a pavement: too narrow to
 * walk on, to carry a railing, or to read as a gap at all from the car, which is
 * what made the front row look like it was growing out of the asphalt. The floor
 * is 900 raw, 0.6 of a lane, which reads as a pavement from the road and still
 * leaves the frontage close enough to feel like a street canyon. */
#define TD5_TG_SIDEWALK_MIN 900.0
double tg_city_sidewalk_w(const TG_Biome *b);
/* [R10 WIDEWALK] Road-width-scaled pavement width. tg_city_sidewalk_w is the
 * per-BIOME floor/base and stays the boolean gate ("is there a raised pavement
 * on this biome"); this is the WIDTH the two geometry sites lay -- the main-road
 * slab (tg_city_emit_sidewalk) and the facade setback (tg_side_geom) -- so the
 * facade FRONT keeps landing on the slab BACK edge.
 *
 * The complaint: on a wide 4-lane avenue (half-road 3000 raw) the 900-raw floor
 * slab is ~30% of the half-road, foreshortens under a chase cam and reads as "no
 * sidewalk". Scaling the width with the half-road makes a wide avenue carry a
 * wide pavement while a normal 2-lane still lands on the 900 floor. Buildings
 * move further back as a consequence (accepted).
 *
 * Returns 0.0 wherever the biome has no pavement (base <= 0) so the "is there
 * pavement here" answer never changes -- only the width where there already is
 * one. tg_road_half_width takes the WIDER of the span's two ends, so the slab
 * and both facade ends (set0/set1) derive one width for the span and nothing
 * skews. Gated by TD5RE_R10_WIDEWALK (default ON); with it off this returns the
 * base width and the geometry is byte-identical to before. */
#define TD5_TG_WIDEWALK_FACTOR 0.40    /* pavement width = half-road * this ...  */
#define TD5_TG_WIDEWALK_MAX   2000.0   /* ... clamped to a sane ceiling (raw)    */
double tg_city_sidewalk_w_at(const TG_NodeList *nl, int si, const TG_Biome *b);
/* Width of the FLAT verge band outside the city: "elevated sidewalks are fine,
 * but when outside the city the sidewalks can be just another texture". Tree
 * biomes get a painted-looking margin drawn on the ground rather than a slab
 * with a kerb face -- no geometry to climb, nothing to catch a wheel on, and one
 * quad per side instead of two. Biomes that HAVE a pavement get none. */
#define TD5_TG_VERGE_W      700.0   /* band width, raw                        */
#define TD5_TG_VERGE_LIFT    16.0   /* clear of the ground skirt, see below   */
double tg_verge_band_w(const TG_Biome *b);
double tg_city_kerb_h(const TG_Biome *b);
int tg_facade_cols_for(double len, double cell_w, int cap);
double tg_facade_floor_h(const TG_Biome *b);
double tg_facade_depth(const TG_Biome *b);
double tg_facade_run_depth(const TG_Biome *b, int si, int left, int floors, int *dcols_out);
int tg_facade_floors(int si, int left, const TG_Biome *b);
/* [R11 CITY item 6] "Does span si lay a pedestrian-pavement ARM on side s?" --
 * the single definition of tg_block_emit_intersection's placement, factored out
 * so the FRONTAGE below can end short of an arm instead of standing on it.
 * Returns a bitmask: 1 = arm at the near node (the previous span's far end),
 * 2 = arm at the far node (the next span's near end), 0 = no junction on that
 * side. Forward-declared: the gates it reads live with the junction emitters
 * much further down, but tg_side_geom below needs the answer. */
int tg_r11_arm_side(const TG_NodeList *nl, int si, int s);
/* Largest share of one span's frontage the two corner setbacks may take between
 * them. A single setback is one sidewalk width (900 of a 1500-raw span, 60%), so
 * this only ever binds on a one-span block flanked by streets on both sides,
 * where it splits what is left rather than inverting the segment. */
#define TD5_TG_R11_CORNER_KEEP 0.65
/* Shortest frontage a corner setback may leave standing, raw. Below this the
 * wall reads as a splinter rather than a building end, so the setback yields. */
#define TD5_TG_R11_CORNER_MIN 400.0
void tg_side_geom(const TG_NodeList *nl, int si, int left, const TG_Biome *b, TG_SideGeom *g);
/* ===================== [R13 FACES] RUN-END RETURN CENSUS =====================
 * [R13 item 6 -- "at the end of the bridge these buildings have NO SIDE FACES"]
 *
 * Recorded at the EMIT SITE, not re-derived from the model. The R11 report
 * already prints tg_side_geom's cap_near/cap_far, and on the reported span they
 * read 1 -- so a model-side sweep would have declared the frontage closed while
 * the strip on disk carried a single vertical normal per building. What is
 * counted here is what tg_emit_street_wall actually PUSHED, so the number cannot
 * disagree with MODELS.DAT.
 *
 *   emitted   -- this side of this span really wrote frontage quads.
 *   capn/capf -- the model's run-end flags for it.
 *   rets      -- return (side face) quads were pushed for it.
 *
 * A RUN is a maximal stretch of consecutive SURVIVING spans on one side, i.e.
 * one building as the strip carries it. Its two ends are the two places a driver
 * meets it end-on, so both must carry a corner return. The acceptance number is
 * MISSING_SIDE_FACES: run ends with no return. FLAT_RUNS is the worst case of
 * the same measure -- a building open at BOTH ends, which is the flat card the
 * report names. Survival is what makes this a real measure rather than a restated
 * intention: the emitter's own cap flags read correct on the reported span. */
typedef struct {
    unsigned char emitted, capn, capf, rets;
    short rows, dcols;
} TG_R13Face;
extern TG_R13Face s_r13_face[TD5_TG_MAX_SPANS][2];
extern unsigned char s_r13_drop[TD5_TG_MAX_SPANS];
/* A frontage side that is on the STRIP: emitted and not eaten afterwards. The
 * drop is per SPAN (one mesh carries both sides), so it takes both with it. */
#define TG_R13_STANDS(si, s) (s_r13_face[(si)][(s)].emitted && !s_r13_drop[(si)])
void tg_r13_faces_reset(void);
void tg_r13_faces_dropped(int si);
void tg_r13_faces_report(int nspans);
/* Scenery beside span si, or 0 (no-op success) if this span gets none. One mesh
 * at most, deterministic from si -- NOT the shared RNG, which the centerline
 * walk has already consumed, so scenery cannot perturb track shape. Tree biomes
 * keep the camera-facing billboard; box biomes now lay a flat facade wall
 * (tg_emit_street_wall) instead of a UV-tiled 6-sided box. */
/* [R5 item 15] Where does a street wall actually STAND? Mirrors the emit gate in
 * tg_building_for_span below: past span 0, not on a bridge deck (the deck is
 * cleared), and in a facade biome (tg_city_sidewalk_w > 0 -- CITY / INDUSTRIAL,
 * not the tree-billboard biomes). tg_side_built uses this so caps and step walls
 * treat a bridge span or a non-facade neighbour as a run boundary. */
/* [R11 item 7c] "the overpass must NOT touch other buildings" -- the deck's own
 * footprint clears the frontage. Declared here because the massing gates read it
 * well before the underpass geometry that defines it. */
int tg_up_clear_span(int si);
int tg_facade_stands(int si);
/* [R7 item 18] Ground/water validity + base-Y for a verge tree billboard.
 * Defined after the terrain cross-section (tg_ground_side); forward-declared here
 * so both tree emitters can share it. Returns 0 when the trunk is over water or a
 * coastline (skip the tree), else writes the plant point and sampled ground Y. */
int tg_flora_plant(const TG_NodeList *nl, int si, const TG_Biome *b,
                          double side, double gap, double tw,
                          double *cx, double *cz, double *base_y);
/* [R12 CROSS item 5] "Is a FOREST side road laid at this span, and on which side
 * / how far out?" -- the single definition of the forest crossing. The verge-tree
 * emitter just below must not plant a trunk in the new carriageway, so it is
 * forward-declared here, the same way tg_city_crossing_here is. */
int tg_r12_fcross_at(const TG_NodeList *nl, int si,
                            double *pside, double *preach);
/* [R14 FCROSS items 1c/1d] The three questions the REST of the file is allowed
 * to ask about a forest crossing. Everything else about the element stays inside
 * the R14 block beside its emitter; these are the narrow seams:
 *
 *   _clear_side  "is a forest crossing lying on this (span, side)?" -- the
 *                one-span-either-side window, because a billboard or a crate is
 *                a FOOTPRINT and a piece placed at the shoulder span still
 *                overhangs the mouth. Trees and animals ask this.
 *   _occupies    the same question in the frame every placement helper already
 *                uses (nearest edge of a footprint, measured out from the main
 *                road edge) -- deliberately the signature of tg_xstreet_occupies,
 *                so the two sit side by side at each placement site instead of
 *                one growing a forest special case.
 *   _pave_stop   "must the pavement RUN END here?" -- asked by
 *                tg_pavement_side_width, the single width authority both the
 *                raised city slab and the out-of-town verge band already share.
 *                That is the whole coupling to the pavement layer: no city
 *                junction gate is widened and no city emitter learns about
 *                forests. */
int tg_r14_fcross_clear_side(const TG_NodeList *nl, int si, double side);
int tg_r14_fcross_occupies(const TG_NodeList *nl, int si, double side,
                                  double inner_d);
int tg_r14_fcross_pave_stop(const TG_NodeList *nl, int si, double side);
int tg_building_for_span(const TG_NodeList *nl, int si, TG_Buf *blk);
int tg_building_verge_tree(const TG_NodeList *nl, int si, TG_Buf *blk);
int tg_side_blocked(int si, double side);
int tg_side_corridor_here(const TG_NodeList *nl, int si, double side);
/* [R7 item 6] The FLAT PAVEMENT (city sidewalk slab / out-of-town verge band) on
 * the RIGHT needs suppressing only where the branch carriageway actually reaches
 * into that lateral. tg_side_blocked keys on the whole fork-CLEAR region, which
 * opens TD5_TG_BRANCH_WIDEN+2 spans BEFORE the fork splits (span 311 for a fork
 * at F=319). On those APPROACH spans the road is merely widening -- the branch
 * has not peeled off yet, so tg_carriageway_reach there is exactly the plain road
 * half width -- yet the blanket block dropped the kerb anyway, leaving the
 * "see-through gap in the right sidewalk before the branch" (item 6, span 318).
 * Render the pavement wherever the branch does not overlap this lateral, and drop
 * it only where the pavement would otherwise land on the bowed corridor. Same
 * spirit as item 7: keep the element continuous until the run actually ends.
 * TD5RE_R7_PRE_BRANCH_PAVE=0 restores the blanket fork-clear drop for an A/B.
 *
 * [R8 CITY item 2] R7's rule is still BINARY -- it drops the whole slab the
 * instant the corridor pokes ONE unit past the plain road edge. Measured on seed
 * 99991 (r8city-diag, span 143): reach=3104 against half=3000, so the branch had
 * bowed out 104 raw units into a 900-wide pavement and 796 units of it were
 * still clear ground -- and the entire kerb vanished from 143 onward while the
 * left kerb ran on. That is the user's "on span 143 the sidewalk stop spawning
 * here": not a biome edge (R7 item 7) and not the blanket fork-clear block (R7
 * BRANCH item 6, which this side already passes), but the successor threshold
 * being a hairline.
 *
 * So the authority is a WIDTH, not a flag: the pavement keeps whatever lateral
 * the corridor has not taken (sw minus the overlap) and is dropped only once
 * what is left is too narrow to read as a kerb. The slab therefore TAPERS out
 * over the spans where the branch peels away instead of ending on one span, and
 * the run ends where the branch actually occupies the verge. tg_road_half_width
 * is the plain carriageway, tg_carriageway_reach the main+branch envelope, both
 * measured from the centreline, so their difference is exactly the bite the
 * corridor takes out of this side's verge.
 *
 * Returns the usable width (0.0 = emit nothing on this side). Both the raised
 * city slab and the out-of-town verge band ask this one function, so the two
 * never disagree about where a pavement run ends.
 * TD5RE_R8_CITY_PAVE_TAPER=0 restores R7's all-or-nothing drop for an A/B.
 *
 * [R9 CITY item 2] R8's taper is right at the FORK MOUTH and wrong once the
 * corridor has pulled clear, and that is what the user reads as "the right track
 * on span 150 branch has double sidewalk". Measured on seed 99991 (r9city
 * uniqueness sweep, spans 150/151/162/163 and span 150 again on seed 777):
 *
 *     si=150 side=R  bands=2  [2999..3332 city-slab] [3488..4467 branch-slab]
 *
 * Two SEPARATED pavements on one span-side. The mechanism is not the taper's
 * arithmetic -- it is that this slab is laid on the FULL road edge
 * (tg_city_edge_frame asks tg_road_edge for the plain carriageway), while over a
 * fork the MAIN carriageway is narrowed to its LEFT half and shifted away. So the
 * moment the branch separates, the slab is standing in the middle of the GORE,
 * attached to no carriageway at all, with the branch's own kerb a few hundred
 * units further out. R7's binary drop hid this by killing the slab at the first
 * unit of overlap; R8 made it taper and thereby exposed it.
 *
 * The stopping rule is therefore CONTIGUITY, not width: the main road's outer
 * pavement is legitimate only while it still MEETS the branch's own pavement, so
 * that the two read as one widening kerb across the mouth (which is exactly the
 * span-143 continuity R8 was asked to restore). Once a gap opens between them the
 * outer pavement is the branch's job and this slab is a stranded island. The
 * branch lays its kerb from tg_carriageway_reach outward, so the gap is
 * reach - (half + remaining), and TD5_TG_PAVE_JOIN is the same "these two read as
 * one pavement" tolerance the uniqueness sweep merges bands with -- one number,
 * so the emitter and its acceptance test cannot disagree.
 *
 * On seed 99991 this keeps the taper over spans 143-149 (where the slabs still
 * touch, i.e. the R8 fix) and stops it at 150, which is the span the user named.
 * TD5RE_R9_CITY_PAVE_JOIN=0 restores R8's taper-to-a-sliver for an A/B. */
#define TD5_TG_PAVE_MIN_W   250.0   /* narrower than this is a sliver, not a kerb */
#define TD5_TG_PAVE_JOIN    150.0   /* == TD5_TG_R9_BAND_JOIN, see sweep (A)      */
double tg_pavement_side_width(const TG_NodeList *nl, int si, double side, double sw);
/* ===================== [R10 SPAN66] SIDE-STREET OCCUPANCY ==================
 * Declared here, DEFINED beside tg_city_emit_crossstreet -- the emitter whose
 * footprint it reports. See the block comment there for the measurement that
 * made it necessary. `inner_d` is the NEAREST edge of a footprint, measured out
 * from the MAIN ROAD EDGE, exactly the frame every placement helper in this file
 * already uses for a setback. */
int tg_xstreet_occupies(const TG_NodeList *nl, int si, double side,
                               double inner_d);
void tg_xstreet_audit(const TG_NodeList *nl, int si, double side,
                             double inner_d, const char *what, int placed);
extern long s_r10_prop_skipped;
extern long s_r14_fcross_animal_moved;
extern long s_r14_fcross_side_hit;
extern long s_r14_fcross_prop_skip;
extern long s_r14_fcross_pave_stop;
extern long s_r14_fcross_worn;
int tg_prop_one(const TG_NodeList *nl, int si, int pp, double side, double gap, TG_Buf *m, size_t *moff, int *pn);
extern long s_r13_animal_kept;
extern long s_r13_animal_town;
int tg_emit_props(const TG_NodeList *nl, int si, const TG_Biome *b, TG_Buf *m, size_t *moff, int *pn, int cap);
/* ===================== WATER =====================
 * A flat sea/river plane on the SEAWARD side of a coastal run: it starts past a
 * beach strip (so palms on the verge stand on the sand, not in the water) and
 * runs far out, a bit below the road. Shipped coastal tracks (level012 sea,
 * level021 coast) do exactly this -- a big flat blue mesh grid below road level.
 * Raw = world*256. */
/* (WATER_DROP / WATER_BEACH / WATER_EXTENT moved up above the R9 over-water
 * audit in the guard block, which tests the same sea footprint this lays.) */
/* World units per page repeat. The sea is textured by WORLD POSITION, not by
 * cell, so the page tiles on a single global grid (see tg_emit_water). */
#define TD5_TG_WATER_TILE    6000.0
int tg_biome_span_has_water(int si);
double tg_water_side(int si);
double tg_sea_level_y(const TG_NodeList *nl, int si);
int tg_water_span_clear(int si);
double tg_r11_sea_outer(const TG_NodeList *nl, int si);
int tg_emit_water(const TG_NodeList *nl, int si, double side, TG_Buf *m, size_t *moff, int *pn);
/* Tunnels come in runs so a whole stretch is enclosed, not isolated spans.
 *
 * [R8 item 18] 20 -> 32. A bore has no grade cap to respect (it follows the
 * road, it does not shape it), so the only thing 20 was buying was a shorter
 * stretch of the one thing the user asked to see more of. 32 spans is ~48000
 * world units of enclosed road. The BRIDGE/TUNNEL interlock below is the real
 * constraint on going further: a tunnel run yields to any bridge run within
 * CLEAR spans, and both runs just got longer, so tunnel COUNT is the number to
 * watch in the element inventory rather than to assume. */
#define TD5_TG_TUNNEL_RUN_R3  20      /* pre-R8 bore length (A/B baseline) */
#define TD5_TG_TUNNEL_RUN_R8  32      /* [R8 item 18] longer bore          */
int tg_tunnel_run_len(void);
#define TD5_TG_TUNNEL_RUN  (tg_tunnel_run_len())
/* [R4 item 19] Spans of ordinary ground a tunnel run must keep clear of any
 * bridge run, on either side. A deck runs into a bore with zero clearance today
 * (seed 99991: bridge 1320-1359 overlaps tunnel 1340-1359), so this both forbids
 * the OVERLAP and guarantees a flat approach band between the two -- ~10 spans
 * (~15000 world units) is a full bridge approach length. */
#define TD5_TG_BRIDGE_TUNNEL_CLEAR 10
/* ==========================================================================
 * [R9 TUNNEL item 13]  BORE  vs  UNDERPASS
 *
 * "the tunnels in the city should represent underpasses below highways, build
 *  it like that, and current tunnels should only be happening in mountains
 *  biome"
 *
 * This is the round's reframe, and it is very probably also the answer to
 * ITEM 5, which is now on its FOURTH round. R6 built a portal page, R7 built a
 * projecting concrete portal facade, R8 replaced the banded page with a flat
 * one and verified the selection at all 12 mouths. Every one of those was a
 * real fix and the complaint came back each time -- because the element was
 * wrong, not the art. A bored mountain portal was being built in the middle of
 * a town, and no portal texture can make a mountain portal look right where
 * there is no mountain. On seed 99991 there is not one ALPINE cell in 1987
 * spans, and BOTH bores (480-499 ORIENTAL, 1520-1539 INDUSTRIAL) are urban.
 *
 * So the run gate keeps its hash EXACTLY as it was -- the same runs are chosen,
 * so this is not a rate change -- and what gets BUILT on a chosen run is now
 * decided by the biome:
 *
 *   ALPINE (cold AND wilderness)  -> BORE      a real tunnel through rock
 *   urbanity >= 1 (everything settled) -> UNDERPASS
 *   FOREST                        -> NONE      nothing to bore, nothing to span
 *
 * See tg_tunnel_kind for why the mountain test is climate AND urbanity rather
 * than climate alone -- that distinction was forced by a frame, not chosen.
 * FIELDS, FOREST and COAST carry no bore any more, which is the strict reading
 * of "only in mountains"; TD5RE_R9_UNDERPASS=0 restores the old biome-blind
 * bore everywhere for an A/B.
 *
 * WHY tg_span_in_tunnel KEEPS ITS MEANING. Twenty-odd call sites ask it, and
 * every one of them means "is the road ENCLOSED here" -- dry tarmac inside the
 * bore, no sea plane over the roof, no gantry, no buildings, no flora, the fork
 * gore on bore-median concrete. An underpass encloses the road for about two
 * spans out of twenty and a city carries straight on over the top of it, so
 * NONE of those consumers wants to fire on an underpass run. Narrowing this one
 * predicate to BORE therefore leaves all of them saying what they already said,
 * and the underpass gets its own predicate rather than borrowing this one.
 * ====================================================================== */
enum { TG_TUN_NONE = 0, TG_TUN_BORE, TG_TUN_UNDERPASS };
int tg_span_in_tunnel(int si);
/* The one span of an underpass RUN that carries the crossing.
 *
 * An underpass is not a 20-span enclosure. A road passes over ours, and either
 * side of it the town simply continues -- which is the whole point of item 13,
 * and why this returns a single span rather than a range. Centre of the run, so
 * it lands where the old bore's midpoint was and the approach is symmetric.
 * Returns -1 when the run containing si is not an underpass. */
/* ============ [R12 OVERPASS items 14a + 11b] WHERE A CROSSING MAY LAND ======
 * "A highway ran straight into the water. The overpass should either CURVE to
 *  avoid the bridge, OR run alongside the road at a higher level and finish
 *  past the coastline."  and  "there is no background geometry behind it."
 *
 * ONE ROOT, TWO REPORTS. R11 item 7a extended each arm from a flat 13000 to
 * `half + tg_far_reach()` -- 33000-36598 on this seed -- so that the deck lands
 * on the drawn edge instead of stopping in mid-air. That was right about the
 * length and silent about the GROUND it now flies over. The arm is a single
 * straight lateral sweep at one height, and two kinds of span have nothing
 * under them to land on:
 *
 *   WATER. A water biome's ground has fallen THROUGH sea level by
 *   TD5_TG_SHORE_END (9600) from the road edge, and the bridge crossings dig a
 *   gorge with a river plane in it. An arm of 33000 is guaranteed to end over
 *   open water on such a span -- literally "a highway ran straight into the
 *   water", and the same reason there is nothing behind the deck: what is
 *   behind it is sea, so the deck is silhouetted against an empty plane with no
 *   massing of any kind (measured: the R11 clear band suppresses the frontage
 *   for 3 spans, and on a water span there is no backdrop to see through it).
 *
 *   A BRIDGE RUN. The road there is itself a deck over a river. A second deck
 *   flying across it is the unreadable pile-up the item describes, and it is
 *   also where the coastline/gorge geometry lives.
 *
 * WHY RELOCATE RATHER THAN CURVE OR PARALLEL. Both alternatives the item offers
 * are new ELEMENTS, not fixes. "Curve the deck" means replacing the single quad
 * with a swept multi-segment ribbon plus a swept parapet, and the guard's own
 * quad-framing rule (R11: a quad whose corners straddle more than
 * TD5_TG_GUARD_EX_SPANS has no frame to be judged in) would then have to be
 * re-derived per segment. "Run alongside at a higher level" is an entire
 * parallel elevated carriageway with its own piers, ramps and massing gates.
 * Relocation reuses machinery that is already here and already trusted: a
 * crossing already owns one span of a TD5_TG_TUNNEL_RUN-span run, and the run
 * is 40 spans long, so there is ample slack to put the crossing where it can
 * actually cross. It is "curve to avoid the bridge" expressed in the one degree
 * of freedom this element has.
 *
 * INVARIANT PRESERVED. Every caller asks tg_underpass_span(si) and compares to
 * si (see tg_up_clear_span and the emit site), so the answer only has to be the
 * SAME for every span of a run. The walk below is symmetric about the run centre
 * and bounded by the run, so it is still a pure function of si and a run still
 * carries exactly one crossing -- or none, when the whole run is water or deck,
 * which is the correct answer rather than a stub over the sea.
 *
 * TD5RE_R12_UP_SHORE=0 restores the R11 centre-of-run placement for an A/B. */
#define TD5_TG_UP_BRIDGE_CLEAR 4    /* spans of bridge approach kept crossing-free */
#define TD5_TG_UP_SLIDE_MAX   14    /* how far along its run a crossing may slide  */
int tg_underpass_span(int si);
/* Clear interior height of the bore, and the thickness of wall/roof slabs.
 * Named because the mountain massing above the tunnel (tg_emit_fb_tunnel) has
 * to stack on top of the roof and cannot guess these. */
#define TD5_TG_TUNNEL_HEIGHT  2600.0
#define TD5_TG_TUNNEL_WALL_T   300.0
/* ================= [R12 TUNNEL item 6] PORTAL FRAME vs BUTTRESS ============
 * "the vertical pillars and the horizontal beam are not connected -- extend the
 *  beam so it reaches the OUTER EDGES of the pillars"
 *
 * The mouth is built by TWO emitters that never agreed on where the portal is:
 *
 *   tg_emit_tunnel_swept   the FRAME -- header + jambs, flat quads standing
 *                          `proud` (1600) out along the road from the mouth
 *                          node, jamb width JW past the opening edge
 *   tg_emit_fb_tunnel      the BUTTRESSES -- solid boxes hugging the opening,
 *                          half-width 800 at lateral offset side_x + 800, and
 *                          half-DEPTH 2200 along the road
 *
 * The "pillars" of the report are the BUTTRESSES (the frame's own jambs are
 * buried inside them: a 1600-deep quad inside a 2200-deep box). So the two
 * numbers the user is comparing are the buttress and the HEADER, and they
 * disagree on BOTH axes:
 *
 *   lateral  header reaches side_x + JW (1400); buttress outer face is at
 *            side_x + 800 + 800 = 1600. The beam stops 200 short of each
 *            pillar's outer edge -- verbatim the report.
 *   depth    the buttress front face stands at 2200 while the frame plane is
 *            at 1600, so the beam is recessed 600 BEHIND the pillars it is
 *            meant to sit on. At a driver's eye height that recess is the
 *            "visible gap": you see past the end of the header, over the top
 *            of the pillar, to whatever is behind.
 *
 * Fix by DERIVING both from one statement of where the portal is, instead of
 * three independent literals:
 *
 *   TD5_TG_PORTAL_PROUD      the frame plane (unchanged 1600 -- it exists so
 *                            the Group-C hill mass at the node cannot occlude
 *                            the facade, and must NOT be reduced)
 *   TD5_TG_PORTAL_WING       lateral reach of BOTH the buttress and the frame
 *                            past the opening edge -> beam ends exactly on the
 *                            pillar's outer edge
 *   buttress half-depth      pulled back to just behind the frame plane, so the
 *                            pillar's front face and the beam are flush. This
 *                            is what R9 item 5b asked for in the first place
 *                            ("the buttress reaches the frame plane"); 2200
 *                            overshot it, and the overshoot is the gap.
 *
 * Pulling the buttress back also EXPOSES the frame's jamb quad, which has been
 * invisible inside the box since R9. That is a second win from the same edit:
 * the pillar's front face now wears the portal FACE page, the same page as the
 * header, so the frame reads as one object rather than as a concrete beam
 * floating in front of two stone wings.
 *
 * TD5RE_R12_TUNNEL_PORTAL_JOIN=0 restores the R11 numbers for the A/B; the
 * measurement below logs whichever set is live, so "before" and "after" are two
 * runs of one build. TD5RE_R12_TUNNEL_DIAG=1 turns the measurement on.
 * ========================================================================= */
#define TD5_TG_PORTAL_PROUD   1600.0   /* frame plane, out along the road   */
#define TD5_TG_BUTT_INSET      800.0   /* buttress inner offset past side_x */
#define TD5_TG_BUTT_HALFW      800.0   /* buttress half width               */
/* Outer edge of a pillar, measured past the opening edge. ONE definition, read
 * by the buttress emitter and by the frame's jamb width. */
#define TD5_TG_PORTAL_WING    (TD5_TG_BUTT_INSET + TD5_TG_BUTT_HALFW)
#define TD5_TG_PORTAL_WING_R11 1400.0  /* the jamb width that fell 200 short */
/* Frame-to-pillar clearance: the pillar stops this far BEHIND the frame plane
 * so the two are flush without co-planar z-fighting. */
#define TD5_TG_PORTAL_SKIN      80.0
#define TD5_TG_BUTT_DEEP_R11  2200.0   /* the depth that overshot the frame  */
double tg_portal_butt_deep(void);
/* Fork whose MAIN half-carriageway covers a main-ring span. Owned by the branch
 * area and defined further down, forward-declared here because the bore has to
 * know whether a branch runs alongside it. READ ONLY from the tunnel code. */
int tg_fork_of_main(int si);
void tg_tunnel_bore(const TG_NodeList *nl, int si, double *half, double *shift);
/* [TUNNEL SUBDIV 2026-08-30] Sub-segments per span, and the largest heading
 * change any ONE sub-segment may span. The swept bore was emitted as a single
 * flat piece per span (~1560 units deep): vertices are shared so there are no
 * gaps, but on a curve each piece is a CHORD, so the surface kinks at every
 * span boundary. Viewed down the bore at a grazing angle that C1 break reads as
 * a staircase along the wall/roof junction -- confirmed with
 * TD5RE_TUNNEL_SEGCOLOR=1: every staircase step landed exactly on a segment
 * colour boundary, while the lining texture itself stayed smooth. */
#define TD5_TG_TUNNEL_SUBDIV_MAX 8
int tg_emit_tunnel(const TG_NodeList *nl, int si, TG_Buf *blk, int *added);
/* ==========================================================================
 * [R9 TUNNEL item 13]  CITY UNDERPASS -- a road passing OVER ours.
 *
 * "the tunnels in the city should represent underpasses below highways, build
 *  it like that"
 *
 * This is a new ELEMENT, not a re-texture, which is the whole reason the round
 * reserved this area the widest page block. What a bore and an underpass have
 * in common is only that the road is briefly covered. Everything else differs,
 * and the differences are the point:
 *
 *   bore                              underpass
 *   ----------------------------      ------------------------------------
 *   20 spans of enclosure             ONE crossing, ~2 spans of cover
 *   cut into a hillside               carried on abutments in flat ground
 *   nothing above it but rock         a HIGHWAY, with its own deck and rails
 *   portal facade at each end         open ends; you see daylight through
 *
 * The pieces, in emit order:
 *   ABUTMENTS   retaining walls either side of our carriageway, carrying the
 *               deck and holding back the ground the highway is built on
 *   DECK        the crossing road itself, built as a quad mesh rather than a
 *               box so its SOFFIT, its TOP and its FASCIAE can be three
 *               different materials -- which is also item 5's "walls and
 *               roofing should have different texture" satisfied by
 *               construction rather than by a page swap
 *   PARAPETS    a low wall along each deck edge, so from below the thing
 *               overhead reads as a road and not as a slab
 *
 * GUARD. TD5RE_R7_GUARD exists precisely to reject meshes that sit over the
 * carriageway, and a deck crossing above our road is the exact silhouette it
 * hunts. The caller marks this whole element TG_GK_DECK (an EXEMPT kind) for
 * the same reason the bridge deck and the bore are marked -- authored to be
 * there. Without that mark the guard eats the underpass and item 13 ships
 * invisible.
 * ====================================================================== */
#define TD5_TG_UP_CLEAR    2600.0   /* clear height under the soffit  */
#define TD5_TG_UP_THICK     420.0   /* deck structural depth          */
#define TD5_TG_UP_HALFDEEP 1500.0   /* half the deck's width along OUR road */
#define TD5_TG_UP_REACH   13000.0   /* R9 reach each side (A/B baseline)   */
#define TD5_TG_UP_PARAPET   620.0   /* parapet height above the deck   */
/* ================== [R11 BRIDGE item 7] THE OVERPASS, MEASURED ==============
 * "the highway overpass must be LONGER on the sides, extending to the end of
 *  the drawn area. It must NOT touch other buildings. Its support pillars must
 *  be exactly as wide as the highway itself."
 *
 * Three separate defects, all measurable off the constants above and all
 * confirmed against the emitted geometry (see tg_r11_up_diag, which logs the
 * numbers on every underpass span so the before/after is a log diff, not a
 * frame impression).
 *
 * 7a REACH. The crossing ran 13000 each side of the bore centre. The drawn
 *    ground beside the road is the verge skirt (tg_verge_reach, 12000 from the
 *    road edge) and then the background band, which reaches
 *    TD5RE_AUTOTRACK_TERRAIN_REACH / TD5_TG_FAR_REACH = 30000 from the same
 *    edge. So the deck stopped roughly 1000 units past the skirt and some
 *    18000 short of where the scenery actually ends -- it read as a stub, which
 *    is the report. Reach is now DERIVED from those two, not a constant: the
 *    bore half-width plus the far-band reach, so it lands on the drawn edge and
 *    keeps landing there if the terrain reach is ever raised by its knob.
 *
 * 7b PILLARS. The abutment box was emitted with half-depth `dp + 1900` along
 *    OUR road, i.e. 6800 wide, while the deck it carries is `2*dp` = 3000 wide.
 *    The support was 2.27x the width of the thing it supports, which is exactly
 *    "the pillars are the wrong width". They now take `dp`, so pillar width ==
 *    highway width to the unit. (The 1900 was there to make the wall read as a
 *    retaining embankment; a pillar flush with its own deck reads as a pier,
 *    which is what was asked for.)
 *
 * 7c BUILDINGS. Nothing ever told the town the highway was there. The deck is
 *    3000 deep along our road -- two span-lengths at 1500 -- and it now sweeps
 *    laterally straight through the frontage line, the backrows and the
 *    fork-back massing, so facades intersected the deck and the parapets. The
 *    fix is a CLEARANCE BAND in the same shape as tg_span_near_bridge: the
 *    spans the deck's own footprint covers stand no buildings. It is stated
 *    once (tg_up_clear_span) and read by every massing gate, so the frontage
 *    cannot come back through a route that did not know about it.
 *
 * Knobs, all default ON: TD5RE_R11_UP_REACH, TD5RE_R11_UP_PIER,
 * TD5RE_R11_UP_CLEAR. TD5RE_R11_UP_DIAG=1 adds the per-span measurement lines.
 * ========================================================================= */
#define TD5_TG_UP_CLEAR_SPANS 1     /* deck half-depth 1500 / span 1500 = 1 */
/* [R12 item 11c] Extra spans either side of the deck footprint that carry no
 * street intersection. A junction is not just its zebra: the crossing quad, the
 * corner pavement arms and the side street itself run on for several spans, so
 * the band a junction needs to be legible is wider than the deck. */
#define TD5_TG_UP_XCLEAR      3
double tg_far_reach(void);           /* defined with TD5_TG_FAR_REACH */
/* Lateral reach of one ARM, measured from the bore centre: the bore half-width
 * the abutments stand on, plus the background band's own reach, so the deck
 * ends exactly where the drawn ground does -- CAPPED so it never flies over
 * another leg of the ring.
 *
 * BOTH halves are needed and it took three measured rounds to establish that,
 * because the item hides TWO different failures behind one symptom.
 *
 *  - REAL reach-over. Seed 99991: a 33000 arm from span 1530 lands on the road
 *    at span 1495, 35 spans away. The guard rejects the deck and it is RIGHT to
 *    -- a motorway overpass crosses THIS road, it does not sail over the back
 *    straight. That is what the walk below stops.
 *  - Guard MIS-ATTRIBUTION, which looks identical in the log. Seed 20260901:
 *    the deck at 1130 was rejected at spans 1089 and 1167 with NO overlap
 *    anywhere -- the guard's own quad framing was mixing two spans' frames.
 *    That one is fixed in tg_guard_mesh_scan, not here; a cap can never fix it,
 *    and the first two attempts wasted a round trying (tg_topo_road_cap's ray
 *    and an earlier version of this walk both found nothing to shorten).
 *
 * The walk samples the arm and stops one step before the slab's footprint comes
 * within another carriageway's own reach (tg_carriageway_reach -- the same
 * authority the guard judges by, so the two cannot disagree). "Another" is an
 * INDEX separation on the main ring, plus a distance test for fork corridors,
 * whose nodes live past s_ring_len and have no meaningful index. */
#define TD5_TG_UP_ARM_STEPS    48
/* Deliberately the numbers TOPO's C3 cap uses (TD5_TG_TOPO_SELF_SPANS /
 * TD5_TG_TOPO_ROAD_MARGIN, declared further down with the terrain code), so
 * "another leg of the ring" means one thing on this track. Restated here only
 * because this emitter is compiled above those definitions. */
#define TD5_TG_UP_SELF_SPANS   14
#define TD5_TG_UP_ROAD_MARGIN 600.0
double tg_up_reach(const TG_NodeList *nl, int si, int is_left, double half, double dp);
int tg_up_clear_span(int si);
int tg_up_xclear_span(int si);
/* ================= [R14 OVERPASS item 3] THE CROSSING SURROUND ==============
 * "Wherever there is an overpass, always fill the nearby area with buildings and
 *  floor. Here the floor ends early and there is nothing behind it but
 *  background."  (seed 20260901, span 528; the crossing is at 530)
 *
 * MEASURED FIRST (tg_r14_up_report, all six crossings on the seed). Three causes
 * were plausible and the numbers pick exactly one of them:
 *
 *   NOT the deck outrunning the drawn world. `OVER = arm - ground horiz` is
 *   <= 0 at every crossing and every side: R11 item 7a already tied the arm to
 *   `half + tg_far_reach()`, and the ground chain reaches the same 30000. In
 *   PLAN the floor stops exactly where the deck does.
 *
 *   NOT a R13 fold clamp. tg_r13_fold_reach binds on 2 of 12 crossing sides and
 *   its answer there is 30336 -- outside the 30000 the ground already reaches,
 *   so it never truncates anything at a crossing.
 *
 *   IT IS THE FLOOR'S ELEVATION, not its extent. The skirt is flat only to
 *   tg_verge_reach (12000); past that the far band SINKS toward the track floor
 *   to push its cut edge onto the horizon. The deck does not sink -- it is one
 *   flat plane at road + TD5_TG_UP_CLEAR. Measured soffit-to-ground under the
 *   deck at 0/25/50/75/100% of the arm, span 530 left:
 *       2600  2643  15715  33795  33795
 *   The floor is under the deck for the first quarter of it and then falls 30000
 *   units away. That is "the floor ends early ... the deck stands over a void",
 *   verbatim, and it is an ELEVATION reading of a complaint that sounds like an
 *   extent one. Reporting the extent again is how it would have survived a
 *   fourth round.
 *
 * THE FIX IS TO HOLD THE FLOOR FLAT UNDER THE CROSSING, and it is a change to
 * the existing cross-section rather than a new element: the skirt's outer point
 * runs out to tg_far_reach instead of tg_verge_reach on the spans a deck covers,
 * tapered back to the ordinary verge over TD5_TG_R14_UP_TAPER spans so no two
 * adjacent slabs step laterally (the R8 item 11 defect). It costs ZERO new
 * meshes -- the same slab, further points -- and it inherits the R9 topo road
 * cap and the R13 fold cap for free, because tg_ground_side applies both to
 * whatever profile it is handed. The reach is taken from tg_far_reach, the SAME
 * authority the arm takes its length from, so the deck and the floor under it
 * cannot disagree about where the crossing ends.
 *
 * THE CLEAR CORRIDOR STAYS. tg_up_clear_span is why nothing stands on the deck's
 * own three spans, and that is correct -- a facade there intersects the deck
 * (R11 item 7c). Measured, it costs 22 wall sides over six crossings. What the
 * surround gets instead is r13-fill's block, on the spans just OUTSIDE the deck
 * footprint whose frontage happens to be open: see CASE C in tg_r13_fill_here.
 * At the reported crossing that is spans 526-527 left, an open run the gap-
 * interior rule refused because a corner stands two spans away.
 *
 * Knobs, both default ON: TD5RE_R14_UP_FLOOR (the flat floor),
 * TD5RE_R14_UP_FILL (the surround massing). Accounted TG_ACCT_R14_UP.
 * ========================================================================= */
/* Spans of skirt either side of a crossing that ramp between the widened floor
 * and the ordinary verge. One more than the no-intersection band, so the ramp
 * has finished by the time the ordinary town furniture resumes. */
#define TD5_TG_R14_UP_TAPER  (TD5_TG_UP_XCLEAR + 1)
int tg_r14_up_dist(int si, int win);
int tg_emit_underpass(const TG_NodeList *nl, int si, TG_Buf *blk, size_t *moff, int *nmesh, int maxmesh);
/* How far above the LOCAL terrain line counts as ELEVATED. Single definition:
 * both the bridge deck and the guardrail gate key off it, so "on a bridge"
 * cannot mean two different things in two places.
 *
 * 900 is a GEOMETRY fact, not a tuning dial: the deck is 780 tall and the pier
 * is lift*0.5, so below roughly this much lift the deck sits in the road and
 * the pier is too short to see. Do not lower it to make bridges appear.
 *
 * MEASURED CONSEQUENCE (3 seeds, 1800 spans each): against the local terrain
 * line, this generator's elevation almost never reaches 900 of local
 * convexity -- seed 123456789 peaks near 160, seed 42 near 200, and seed 777
 * does not reach even 100. So bridges now essentially never emit, and that is
 * CORRECT: the previous 87%-of-all-spans bridging was an artifact of measuring
 * against the global track minimum, which made every hill a bridge.
 *
 * To make bridges a real feature again they must be PLACED DELIBERATELY, the
 * way tunnels and branch forks are (a chosen span range, with the elevation
 * driven to suit), not conjured by lowering this threshold. */
#define TD5_TG_BRIDGE_MIN_LIFT 900.0
/* Half-width, in spans, of the window the local terrain line is taken over.
 * ~8 spans either side = the length of road a deck would plausibly carry. */
#define TD5_TG_LIFT_WINDOW 8
double tg_local_ground_y(const TG_NodeList *nl, int si);
void tg_bridge_run_bounds(const TG_NodeList *nl, int si, int *s0, int *s1);
int tg_r8_bridge_water(void);
int tg_bridge_run_is_water(const TG_NodeList *nl, int si);
double tg_bridge_water_y(const TG_NodeList *nl, int si);
double tg_bridge_water_surf_y(const TG_NodeList *nl, int si);
double tg_bridge_gorge_phase(const TG_NodeList *nl, int si);
int tg_bridge_struct_enabled(void);
/* [R6 item 17] "All bridges have pillars every few spans and they all look the
 * same. I want more variety." Give each crossing a STYLE, keyed on the run index
 * (like the tunnel lining variant) so a whole run is one style end to end and
 * the four bridges on a track differ from one another:
 *   0 CONCRETE  box-girder: paired square legs + a tower pair at the crown.
 *   1 STEEL     slimmer, taller paired legs on a tighter pitch, extra cross
 *               bracing, slim tall towers -- reads as a steel-truss viaduct.
 *   2 MASONRY   one FAT round-ish pier on a wide pitch with an arch cap under
 *               the deck and NO towers -- reads as a stone viaduct.
 * TD5RE_AUTOTRACK_BRIDGE_VARIETY=0 forces style 0 (the pre-R6 single look). */
#define TD5_TG_BRIDGE_STYLES 3
int tg_bridge_style(int si);
int tg_bridge_pier_page_for(int style);
int tg_bridge_pier_pitch(int style);
/* Clearance from the deck SURFACE down to the top of anything hanging under it.
 * Derived, not chosen: the girder is a 170 half-height box centred 260 below the
 * road, so its underside is at road - 430. 480 clears that with a margin, which
 * is why a pier topped out here can never break the road surface. */
#define TD5_TG_BRIDGE_UNDER 480.0
/* How far outboard of the drivable edge the parapet line sits. MOVED UP here in
 * R9 (it used to be declared with tg_emit_bridge_kerb_panel, below) because it
 * is now the single lateral authority the SUBSTRUCTURE reads too -- see
 * tg_bridge_column_lateral. */
#define TD5_TG_BRIDGE_RAIL_OUT 120.0
int tg_r9_bridge_tie(void);
double tg_bridge_column_lateral(const TG_NodeList *nl, int si, double side);
int tg_bridge_pier_here(const TG_NodeList *nl, int si);
int tg_bridge_gantry_here(const TG_NodeList *nl, int si);
/* [R9 item 9] Two NEW structural materials, so "the pillars" and "the horizontal
 * beam" stop sharing the pier's cast concrete:
 *   +0 PYLON -- the ABOVE-deck verticals (gantry legs, tower legs). Painted
 *      steel with vertical ribs and rivet lines; deliberately warmer and darker
 *      than the pier page so the column visibly changes material where it
 *      leaves the water, the way a real bridge does.
 *   +1 BEAM  -- the HORIZONTAL members (gantry cross-beam, tower cross-member).
 *      A lattice/flange girder drawn for a member seen side-on and lengthwise,
 *      which the pier page (a vertical column texture) never was.
 * TD5RE_R9_BRIDGE_TEX=0 puts both back on the pier page for an A/B. */
#define TD5_TG_PAGE_R9_PYLON (TD5_TG_PAGE_R9_BRIDGE + 0)
#define TD5_TG_PAGE_R9_BEAM  (TD5_TG_PAGE_R9_BRIDGE + 1)
#define TD5_TG_PAGE_R9_SHORE (TD5_TG_PAGE_R9_BRIDGE + 2)
int tg_r9_bridge_tex(void);
int tg_emit_bridge(const TG_NodeList *nl, int si, TG_Buf *blk, int *added);
int tg_railfix_on(void);
int tg_rail_deck_here(const TG_NodeList *nl, int si);
/* ============ [R13 RAIL item 5b] THE BRIDGE APPROACH RAMP IS THE CROSSING ===
 * "Between the road and the bridge, the GUARDRAILS and the SIDEWALK
 *  DISAPPEAR."   -- reported against span 1155, and R12 item 14b reported this
 *  class fixed by CAPPING both runs at a hand-off. The first job was to decide
 *  whether that cap misses this hand-off (a missing END FACE) or whether the
 *  report is about a real missing LENGTH. It is the LENGTH, so the R12 cap is
 *  not extended here: it closes cross-sections, and nothing measured below is
 *  an open cross-section.
 *
 * MEASURED (tg_r13_rail_mouth_report, seed 20260901, all 14 mouths). What every
 * mouth has in common is a RAMP: the road leaves local ground and pitches down
 * into the gorge for several spans before the deck starts. Nothing owns that
 * ramp, and it shows up in two different ways depending on the biome:
 *
 *   UNPAVED mouths (160, 800, 1000). The roadside gate's elevation rule is
 *   `lift >= +900` and a ramp into a gorge has NEGATIVE lift, so it never
 *   fires; a straight approach fails the bend rule too. The only thing that
 *   rails the ramp at all is the +/-3 span dilation pad reaching back out of
 *   the deck. Measured at mouth 160: spans 148-156 carry no rail on either edge
 *   and no pavement on either side -- a NINE-SPAN, 13500-unit bare descent --
 *   and the rail appears only at 157.
 *
 *   PAVED mouths (1320). The pavement and its kerb railing run to 1314 and then
 *   the LEFT side loses both for spans 1315-1318, gets a roadside guardrail for
 *   two of those four, and gets the pavement back for exactly one span (1319)
 *   before the deck. Attributed to ONE sub-predicate by the report: sw, the
 *   pavement width, tg_side_blocked and tg_city_crossing_here are all constant
 *   across the window and only tg_facade_built(si, LEFT) goes 1,0,0,0,0,1. It
 *   is a FRONTAGE GAP -- the city block model opening a side street -- that
 *   happens to land on the ramp. And because the side street runs on every span
 *   of a frontage gap, that gap is also laying a street off the side of a road
 *   which is by then 206 units below local ground and falling.
 *
 * So the root is not the pavement emitter and not the rail emitter: it is that
 * a ramp is treated as ordinary road by every roadside rule, while structurally
 * it is part of the crossing. ONE predicate, stated once, with three
 * consequences that follow from it:
 *
 *   1. a ramp span always carries a barrier (the guardrail gate returns 1);
 *   2. a frontage gap may not break the pavement or its kerb railing on a ramp,
 *      so a run that reaches the ramp reaches the deck;
 *   3. no side street opens onto a ramp -- the same statement as (2) seen from
 *      the other side, and the reason (2) does not lay a kerb across a street.
 *
 * The test is "the road has measurably left local ground within reach of a
 * deck", not "N spans from a deck": the ramp's LENGTH is set by the bridge's
 * own elevation profile and varies from 3 spans to 10 on one seed, so a fixed
 * count would either miss the long ones or rail flat road at the short ones.
 * |lift| because a deck is reached by climbing on some mouths and by the ground
 * falling away on others, and both are ramps.
 *
 * Held in a mask rather than recomputed, so it is a PURE function of si like
 * tg_span_in_bridge_run -- the pavement and kerb-fence gates take no node list
 * and all three consequences must read the identical set of spans or they will
 * disagree at the ends, which is the failure mode R9 RAILFIX documents for the
 * roadside/kerb-fence hand-off.
 *
 * TD5RE_R13_APPROACH=0 restores the unowned ramp for an A/B. */
#define TD5_TG_R13_APPROACH      10    /* furthest a ramp may reach from a deck */
#define TD5_TG_R13_APPROACH_LIFT 25.0  /* road has measurably left the ground   */
int tg_r13_approach_span(int si);
void tg_r13_approach_build(const TG_NodeList *nl, int nspans);
extern long s_r11_rail_on_cross;
extern long s_r11_rail_on_xstreet;
int tg_r11_cross_diag(void);
int tg_r11_xguard(void);
/* Item 8 geometry. SLIM: a gore this narrow already reads as a median (the
 * shipped 0.32 island covers most of it), so it is left byte-identical. MAX: a
 * gore wider than this is a genuine road split with scenery between the two
 * carriageways, not a median, and is left alone at the other end. INSET: clear
 * air kept between a raised face and the tarmac beside it -- deliberately
 * larger than TD5_TG_GORE_OVERLAP (240), which is how far the FLOOR underlaps
 * that same tarmac. */
#define TD5_TG_R11_MEDIAN_SLIM   900.0
#define TD5_TG_R11_MEDIAN_MAX   2600.0
#define TD5_TG_R11_MEDIAN_INSET  300.0
/* Height of a FILLED median above the gore floor. The kerbed treatment's own
 * 150 clears the road by 146, which is a pavement kerb (TD5_TG_KERB_H = 130) --
 * enough to be a step, not enough to answer "it needs a real height
 * difference" from a car. 300 puts the top 296 above the tarmac, a bit over
 * twice a kerb, which reads as a median at driving speed and still lets you see
 * the oncoming carriageway over it. */
#define TD5_TG_R11_MEDIAN_H      300.0
/* [R16 MEDIAN] "avoid really small medians ... should be longer by default" and
 * "always median with height or guardrails".
 *  - MIN_FORK_LEN: a fork shorter than this (in spans) carries NO median at all
 *    -- a handful-of-spans stub reads as a barely-visible flush stripe, not a
 *    divided avenue, so it should "either extend or not exist".
 *  - MIN_H: floor the island height so a surviving median always stands proud
 *    enough to read; the kerbed treatment's own 150 was the "barely visible"
 *    stub the report named. Purely vertical, no lateral change. */
#define TD5_TG_MEDIAN_MIN_FORK_LEN 8
#define TD5_TG_MEDIAN_MIN_H      220.0
int tg_r11_median_rise(void);
/* [R16 MEDIAN] 1 unless fork `fork_index` is too short to carry a median (see
 * tg_median_fork_long_enough in td5_tg_branch.c). Fork length is a per-fork
 * constant, so every span of a fork gets the same answer -- the placement
 * predicate and the emitter cannot disagree. TD5RE_MEDIAN_MIN_RUN=0 for A/B. */
int tg_median_fork_long_enough(int fork_index);
int tg_r12_median_cap(void);
int tg_r12_geom_diag(void);
extern long s_r12_median_caps;
extern long s_r12_median_runs;
int tg_r12_median_fill(double gw0, double gw1);
int tg_r12_median_at(const TG_NodeList *nl, int si, int br_lanes);
/* [R17 MEDIAN item 2] 1 where the gore floor on MAIN span si is MEDIAN-WIDTH
 * (an avenue or a fill-eligible gore) yet NO raised island stands on it, so the
 * flush gore reads as "a median without a height difference". Used at the gore
 * emit site to pave that strip as road instead of ground. See the definition in
 * td5_tg_bridge.c. */
int tg_gore_reads_as_median(const TG_NodeList *nl, int si, int br_lanes);
/* [MEDIAN/BRIDGE 2026-09-07] 1 unless si is on a bridge run the median cannot
 * cover end to end. See the definition in td5_tg_bridge.c for why a partial
 * median cannot simply be extended. */
int tg_median_bridge_uniform(const TG_NodeList *nl, int si, int br_lanes);
/* [R11 CROSS items 9+15] "A street crosses this road edge" -- defined with the
 * guardrail block far below, asked here so the zero-rail invariant can tell an
 * intentional gap at a crossing from an edge that lost its only barrier. */
int tg_r11_street_crosses_here(const TG_NodeList *nl, int si, double sg);
int tg_rail_edge_report(const TG_NodeList *nl, int nspans);
/* Base offset of the parapet FOOT relative to the deck surface, and its height.
 * [R5 item 17b] Was +40 (parapet floated 40u above the deck); with the deck
 * kerb only reaching deck level that left a thin open slot at the barrier foot
 * you could see the river through -- the residual half of the "gap between the
 * guardrails and the road" report. Drop the foot just BELOW the deck (-20) so
 * the rail overlaps the kerb and seals the edge. The rail is a vertical quad and
 * the kerb a horizontal one meeting only along the outer edge line, so the small
 * overlap intersects rather than co-planar z-fights. */
#define TD5_TG_BRIDGE_RAIL_BASE (-20.0)
#define TD5_TG_BRIDGE_RAIL_H    420.0
int tg_emit_bridge_rails(const TG_NodeList *nl, int si, TG_Buf *m, size_t *moff, int *pn);
/* The river surface spanning the full width under a bridge run, recording its
 * own mesh offset. ONE quad with world-projected UVs, for the same reason as
 * tg_emit_water: a cell grid seamed and marched, a world projection is
 * continuous across spans and across the two water emitters (same tile size, so
 * a river and a sea meeting at a biome edge stay on one texture grid). */
/* [R11 WATER] Outward reach of the river and its coastline beside node si --
 * see the definition below TD5_TG_FAR_REACH, whose value it reads. */
double tg_r11_wet_reach(const TG_NodeList *nl, int si);
int tg_emit_bridge_water(const TG_NodeList *nl, int si, TG_Buf *m, size_t *moff, int *pn);
/* Lateral half-width of the coastline band. The river is ±BRIDGE_WATER_HALF
 * (32000) but the seam the user sees is the near-deck one; a ±12000 band covers
 * the gorge pull-back (GORGE_INSET 9000) plus a margin without a huge tilted
 * slab reaching to the far water edge. */
#define TD5_TG_COAST_HALF 12000.0
/* One side's terrain CROSS-SECTION at span si: a chain of (distance from the
 * road edge, drop below the road) points, innermost first. Consecutive points
 * make one quad, so a plain verge is 2 points / 1 quad and a beach is 3 points /
 * 2 quads. One function, so the skirt in tg_emit_ground, the far terrain in
 * tg_emit_fb_terrain and (since R9) the coastline band cannot disagree about
 * where the skirt ended or how low. The struct lives here rather than with
 * tg_ground_side because the coastline is emitted earlier in the file. */
#define TD5_TG_GROUND_MAXPT 3
typedef struct {
    double d[TD5_TG_GROUND_MAXPT];
    double dy[TD5_TG_GROUND_MAXPT];
    int    n;
} TG_GroundProf;
void tg_ground_side(const TG_NodeList *nl, int si, int is_left,
                           double water_side, TG_GroundProf *p);
/* ONE span-side's whole ground surface, near skirt and far band concatenated
 * into a single outward polyline: `d` = horizontal distance from the road edge,
 * `dy` = drop BELOW the road edge (positive down, TG_GroundProf's convention).
 * Built by tg_topo_chain, which lives with the far-band constants further down.
 *
 * `horiz` is R8's number, kept so before/after is comparable. `surf` is the arc
 * length along the same polyline -- the distance the ground actually covers --
 * and is the number R9 TOPO reports, because a falling side spends its reach
 * vertically and a horizontal extent cannot see that.
 *
 * [R14 COAST item 5a] Hoisted above the coastline emitter for the same reason
 * TG_GroundProf above it was: the coast band reaches PAST the near skirt, so
 * the near cross-section is not the authority for the ground it lands on --
 * this chain is, and the band has to be able to ask it. */
#define TD5_TG_TOPO_MAXPT 8
typedef struct {
    double d[TD5_TG_TOPO_MAXPT];
    double dy[TD5_TG_TOPO_MAXPT];
    int    n;
    int    closed;      /* C2: far end closed by a wall, the sea or a gorge */
    double horiz;       /* outward extent ACROSS THE MAP                    */
    double surf;        /* outward extent ALONG THE SURFACE                 */
    double drop;        /* total fall from the road edge to the outer point */
    double end_grade;   /* grade of the last segment -- the run-out test    */
    double gap;         /* C1: worst unexplained lateral discontinuity      */
    double road_cap;    /* C3: where a neighbouring carriageway intervenes  */
} TG_TopoChain;
void tg_topo_chain(const TG_NodeList *nl, int si, int is_left,
                          TG_TopoChain *c);
double tg_topo_drop_at(const TG_TopoChain *c, double d);
int tg_emit_bridge_coast(const TG_NodeList *nl, int si, TG_Buf *m, size_t *moff, int *pn);
/* GROUND. Without this the road is a ribbon in a void: buildings and trees
 * stand on nothing and every frame reads as objects floating in blue. One wide
 * flat slab per display-list ENTRY (not per span -- per-span slabs would
 * overlap into heavy overdraw for no gain), sitting just below the road
 * surface and extending well past both verges.
 *
 * Textured from the biome's ground page, so FIELDS/FOREST get vegetation and
 * CITY/INDUSTRIAL get concrete. Cosmetic only: driving off the road still puts
 * you on nothing, because collision comes from the STRIP, not from this. */
#define TD5_TG_GROUND_WIDTH   24000.0   /* old (A/B): flat verge reach outward   */
#define TD5_TG_VERGE_REACH    12000.0   /* [R6 item 6] new default verge reach   */
#define TD5_TG_GROUND_DROP       70.0    /* just under the road, avoids z-fight */
/* Seaward skirt: a flat VERGE, then a ramp that carries the terrain THROUGH sea
 * level and keeps going, so land and water actually intersect.
 *
 * The old profile stopped flat at the shoreline 70 units under the road while
 * the sea sat 1200 lower -- an unpainted 1100-unit riser you could see through,
 * which is the feedback item. The numbers here are derived, not chosen: keep the
 * first 3600 units flat (roadside props and Group B's trees are placed at ROAD
 * height with gaps up to ~3200, so they must stand on level ground), then ramp
 * from VERGE to END and require the ramp to cross sea level exactly at the
 * shoreline. That fixes the outer drop as
 *   GROUND_DROP + (road-to-sea) * (END-VERGE)/(BEACH-VERGE)
 * for ANY sea level, and at the nominal 1200-unit drop it works out to a 1:4
 * beach. Where the road runs high above the run's sea level the same formula
 * steepens it into a bank -- steep, but continuous geometry either way, which is
 * the actual bug being fixed. */
#define TD5_TG_SHORE_VERGE     3600.0
#define TD5_TG_SHORE_END       9600.0
/* How far out from the deck edge the far bank of a gorge starts. Inside this,
 * beside a bridge, there is nothing but air and the river below -- which is the
 * point: the bank used to start AT the deck edge at road level and hid the
 * river completely, so under a bridge you saw a grass slope, not water. */
#define TD5_TG_GORGE_INSET     9000.0
int tg_biome_is_snow(const TG_Biome *b);
int tg_ground_page_for_span(int si, const TG_Biome *b);
int tg_r8_median_page_ex(int si, int fallback, int tunnel_exempt);
int tg_r8_median_page(int si, int fallback);
int tg_topo_enabled(void);
int tg_topo_ground_index(int si);
int tg_topo_surface_page(int si);
double tg_ground_branch_clear(const TG_NodeList *nl, int si);
/* (TG_GroundProf and TD5_TG_GROUND_MAXPT moved up above tg_emit_bridge_coast in
 * R9 -- the coastline band now reads the same cross-section the skirt does, and
 * it is emitted earlier in the file. tg_ground_side itself stays here.) */

/* ==================================================================
 * SECTION: [R9 TOPO] TOPOGRAPHIC CONTINUITY AUTHORITY  (items 6, 7, 11)
 *
 * "ALL GEOMETRY HAS TO BE CONNECTED THROUGH THE SAME TOPOGRAPHIC LOGIC."
 * That is the user's own sentence and it is the whole of this section. Items
 * 6, 7 and 11 are one defect seen three ways: two adjacent pieces of world are
 * each individually plausible and mutually inconsistent, because each is built
 * from a query only IT asks.
 *
 * WHY A SHARED AUTHORITY AND NOT THREE PATCHES. This is the same shape as R7's
 * on-road guard, which is the only recurring-class fix in this project's
 * history that held: one rule, consulted by every emitter, so an emitter
 * written NEXT round inherits the constraint instead of re-earning it. Three
 * local patches would each be true at the span they were photographed at.
 *
 * THE RULE. For one span-side there is exactly ONE ground surface: a chain of
 * (outward distance, drop below the road edge) points running from the road
 * edge to wherever the world ends on that side -- the near skirt
 * (tg_ground_side) and the far band (tg_emit_far_band) are two halves of it,
 * not two objects. That chain must satisfy, at every span and both sides:
 *
 *   C1 CONNECTED  no lateral gap: it starts at the road edge and each point
 *                 continues from the last (the far band tucks under the skirt).
 *   C2 CLOSED     it may not simply STOP in mid-air where it can be looked
 *                 down at. Either a wall stands on its outer edge, or it is
 *                 the sea, or it must RUN OUT -- keep going until its terminal
 *                 grade is shallow enough that the edge is on the horizon.
 *   C3 BOUNDED    where another part of the track passes nearby, the chain
 *                 ENDS at that road rather than running past it at this span's
 *                 height. Two carriageways at a U-turn share their ground.
 *   C4 MATERIAL   the surface material is a property of the GROUND RUN, not a
 *                 per-span categorical roll, so neighbouring slabs agree.
 *
 * WHY R8'S HONEST MEASUREMENT DID NOT CLOSE ITEM 6. R8 TERRAIN widened the
 * ground and reported seed 99991 span 549 R going 12000 -> 30000. That number
 * is TRUE. It is also the wrong axis twice over:
 *   - it is a HORIZONTAL extent, and a falling side spends its reach
 *     VERTICALLY. The chain below therefore reports SURFACE distance (arc
 *     length along the profile) as well, which is the distance the ground
 *     actually covers.
 *   - and extent was never the binding constraint anyway. At span 549 the
 *     ground DOES reach 30000 -- but 549 is inside fork 3 (510-631), the fork
 *     gate suppresses the RIDGE on the right (R8 TERRAIN items 5/15 turned the
 *     fork gate from a whole-band gate into a ridge-only gate and kept the
 *     ground), and the ridge was the only thing CLOSING that edge. So the
 *     ground runs 30000 out, sinks toward the global floor on the way, and
 *     then stops dead with sky behind it. "A very inclined slope ... and the
 *     ground is not there any more" is an unterminated edge, not a short one.
 *     C2 is the clause that says so, and the user's own instruction -- "if you
 *     make a sloped side make sure that it goes further than usual" -- is
 *     exactly its run-out: the steeper the drop, the further it must go before
 *     it is allowed to end.
 *
 * TD5RE_R9_TOPO=0 restores round-8 behaviour for an A/B (every clause off).
 * TD5RE_R9_TOPO_LOG=1 sweeps every span-side and reports the violation counts
 * and BOTH extents (horizontal and surface).
 * ================================================================== */

/* Terminal grade a ground edge must run out to before it may end unwalled.
 * 0.06 is ~3.4 degrees: at a 200-unit eye height the edge is then below one
 * degree of depression from 3300 units away, i.e. on the horizon rather than
 * a step you look down at. */
#define TD5_TG_TOPO_RUNOUT     0.06
/* A drop this large at an UNCLOSED outer edge is a C2 violation. Below it the
 * edge is effectively flush with the plain and cannot read as a cliff. */
#define TD5_TG_TOPO_OPEN_DROP  600.0
/* Ceiling on the run-out extension. The extension only lengthens the band's
 * OUTER rings, which the R8 sink pins at the track's global floor, so every
 * band out there is coplanar however many overlap -- the hazard the original
 * 180000 -> 30000 reach cut existed to prevent cannot return through it. The
 * cap is here so a pathological relief cannot make one band cover the map. */
#define TD5_TG_TOPO_MAX_REACH  140000.0
/* C1 tolerance: a lateral step smaller than this is a shared edge, not a gap. */
#define TD5_TG_TOPO_GAP_TOL    1.0
/* C3: clearance kept off a neighbouring carriageway's own edge, and the
 * narrowest verge the cap is ever allowed to leave. */
#define TD5_TG_TOPO_ROAD_MARGIN 600.0
#define TD5_TG_TOPO_MIN_VERGE  1200.0
/* C3 only looks at road that is FAR AWAY ALONG THE TRACK. Nearer than this in
 * span index and the "other" road is just this road a moment later, whose
 * ground is the same surface by construction. */
#define TD5_TG_TOPO_SELF_SPANS 14
/* [R18 EDGE item 4] Fraction of the fold distance the inside-of-a-bend ground
 * skirt is allowed to reach. The skirt slab is a quad swept between the
 * cross-sections at si and si+1; on the INSIDE of a bend the two outward rays
 * converge and the quad folds over itself (and across the carriageway) once the
 * reach passes their intersection. C3's road cap cannot catch this: the road it
 * would lap is THIS road a few spans away, inside TD5_TG_TOPO_SELF_SPANS. Cap the
 * inside reach just short of the geometric fold instead. 0.85 leaves a margin so
 * the skirt stops before self-intersecting; on a straight or the OUTSIDE of a
 * bend the rays never converge forward and the cap is inert. */
#define TD5_TG_R18_BEND_FRAC   0.85
int tg_topo_enabled(void);
double tg_verge_reach(void);
void tg_ground_side(const TG_NodeList *nl, int si, int is_left, double water_side, TG_GroundProf *p);
int tg_emit_ground(const TG_NodeList *nl, int si, TG_Buf *blk, double water_side);
/* ===================== [R8 BRIDGE] MEASUREMENT HARNESS =====================
 *
 * Rounds 6 and 7 both shipped bridge fixes on the strength of a frame and both
 * came back. The reason is structural: everything under a deck is computed from
 * four independent heights (the node, the run-minimum deck, the water surface,
 * the gorge skirt) and a frame shows their DIFFERENCE at one span only. This
 * dumps all four, per span, for every bridge run and every tunnel mouth, so a
 * claim about pier feet or skirt/water ordering is a column of numbers rather
 * than a photograph.
 *
 * Read-only: it re-evaluates the same pure functions the emitters call and
 * writes a CSV-ish line per span. TD5RE_R8_BRIDGE_DIAG=1 (opt-in; off by
 * default so ordinary runs are not slowed by ~200 log lines). */
/* [R12 TUNNEL item 8a] SPAN QUERY -- "what stands at span N at all".
 *
 * Identifying an unknown object in a screenshot starts with the candidate list,
 * and the element inventory already holds it; it just reports the relation the
 * other way round (kind -> spans). Invert it for one span band, and add the two
 * facts that decide which emitters could even have fired there: the BIOME cell
 * and the tunnel kind. TD5RE_R12_SPANQ=407 dumps 407 +/- 3.
 *
 * Lives here rather than inside tg_acct_report because the biome table and
 * tg_tunnel_kind are both defined further down the file. Read-only, opt-in,
 * dev builds only. */
double tg_treeline_height(const TG_Biome *b);   /* [R12 8a] read-only */
double tg_treeline_back(const TG_Biome *b);     /* [R12 8a] read-only */
void tg_r12_spanq(int nspans);
void tg_r8_bridge_diag(const TG_NodeList *nl);
int tg_flora_plant(const TG_NodeList *nl, int si, const TG_Biome *b, double side, double gap, double tw, double *cx, double *cz, double *base_y);
/* GORE / MEDIAN fill for a split-fork span. The main (left) and branch (right)
 * half carriageways only touch at the fork and rejoin; where the branch bows
 * away, the strip between the main's right edge (road centre, lateral 0) and the
 * branch's left edge (branch_shift + width/4) has no road mesh and shows through
 * to the void. Fill it with a ground quad, just below road level so it reads as
 * a sunken median and does not z-fight the carriageway edges. Zero-width (hence
 * invisible) at the fork/rejoin where the two edges meet. `si` is the MAIN span;
 * shift_n/shift_f are the BRANCH lateral offsets at this span's ends.
 *
 * MOUTH HOLE FIX (2026-08-26). Reported: "the beginnings of branches have a
 * small portion of see-through no geometry". Root cause is this quad's geometry
 * at the mouth, not a missing mesh: it met the two carriageways on EXACTLY their
 * edges while sitting a full 20 units below them, so the join was an open
 * vertical slit 20 units tall. Anywhere the gore is wide that slit is hidden by
 * the surrounding tarmac at any sane camera angle, but at the mouth the wedge is
 * only a few hundred units across and the slit is a large fraction of what you
 * can see of it -- you look straight through the split into the void.
 *
 * Two changes, both of which have to hold at once: the drop shrinks to a few
 * units (enough to stay behind the road in depth, not enough to be a visible
 * step) and the quad now UNDERLAPS both carriageways by TD5_TG_GORE_OVERLAP
 * instead of sharing their edges, so there is no seam to see through even where
 * the wedge is narrow. The overlap is why the drop cannot go to zero: with the
 * quads coplanar AND overlapping they would z-fight (the Keswick start-banner
 * lesson). */
#define TD5_TG_GORE_DROP      4.0    /* below road level, world units */
#define TD5_TG_GORE_OVERLAP 240.0    /* underlap into each carriageway */
int tg_fork_gore_page(int fork_index);
int tg_emit_gore(const TG_NodeList *nl, int si, double shift_n, double shift_f, double half_n, double half_f, int ground_page, TG_Buf *blk);
extern long s_r14_outer_faces;
int tg_r14_pave_face(void);
int tg_emit_branch_sidewalk(const TG_NodeList *nl, int mb, int k, int L, int fi, const TG_Biome *b, TG_Buf *blk, size_t *moff, int *nmesh, int acct_si);
int tg_emit_branch_verge(const TG_NodeList *nl, int mb, int k, int L, int fi, double bw, TG_Buf *blk, size_t *moff, int *nmesh, int acct_si);
int tg_emit_branch_flora(const TG_NodeList *nl, int mb, const TG_Biome *b, TG_Buf *blk, size_t *moff, int *nmesh, int acct_si);
int tg_emit_avenue_divider(const TG_NodeList *nl, int si, int fork_index, double sh0, double sh1, double half0, double half1, int br_lanes, TG_Buf *blk, size_t *moff, int *nmesh);
/* ===================== GUARDRAILS =====================
 * The car is already contained by collision WALLS derived from the STRIP rail
 * vertices, but nothing draws them, so the road ends at an invisible boundary.
 * Guardrails do not add a constraint -- they make the existing one legible.
 *
 * PLACEMENT is the whole correctness question, and it is NOT assumed here.
 * td5_track_resolve_wall_contacts builds the left rail from row point 0 and the
 * right rail from row point `lane_count + k_rail_lut_[lr][type]`. For span_type
 * 1 -- the only type this generator emits -- BOTH LUT entries are 0, so the
 * rails are row points 0 and lane_count: the outermost points of the row, which
 * tg_emit_span_range places at -/+ width/2. tg_road_edge returns exactly those
 * two points, so deriving the barrier from it puts the visual where collision
 * actually stops you.
 *
 * Deriving from tg_road_edge rather than hardcoding width/2 also means the
 * barrier tracks width changes for free, and keeps ONE definition of "the road
 * edge" shared with the road mesh and the ground skirts. NOTE for the acute /
 * dual-carriageway work: emitting any span type other than 1 brings the LUT
 * offsets into play (types 2..7 are nonzero) and the right rail stops being the
 * outermost point -- this function would then need the same LUT.
 *
 * A BOX per span was rejected: a box cannot follow a curving or undulating
 * road, which is exactly why the ground moved from slabs to edge-derived
 * strips. This emits a proper 3-quad prism per side (inner face, outer face,
 * top cap) built from the span's own edge points, so it curves and climbs with
 * the road. A prism rather than a flat ribbon because a single quad may be
 * backface-culled from one side, and two opposite-wound coplanar quads would
 * z-fight if culling is off -- the prism is correct either way.
 */
#define TD5_TG_RAIL_HEIGHT     700.0   /* top of the barrier above road level */
#define TD5_TG_RAIL_BASE_DROP   60.0   /* start below the surface: no gap on
                                        * undulating spans */
#define TD5_TG_RAIL_THICK       55.0   /* prism depth, outward */
#define TD5_TG_RAIL_OFFSET      40.0   /* outboard of the rail line, so the
                                        * barrier does not share an edge with
                                        * the road surface and shimmer */
int tg_rail_page(int si);
int tg_rail_vflip_on(void);
/* [R11 CROSS items 9+15] "Is a SIDE STREET carriageway laid at (si, side)?" --
 * the single definition of the crossstreet emitter's placement. It lives with
 * the city block far below, but the ROADSIDE guardrail (right here) has to ask
 * it in order NOT to wall off an intersection mouth, exactly as it already asks
 * tg_rail_kerbfence_here in order to yield a kerbed footway. Forward-declared
 * rather than duplicated so placement and exemption cannot drift. */
int tg_xstreet_here(const TG_NodeList *nl, int si, double side,
                           double *preach);
/* [R11 CROSS items 9+15] ONE root cause, stated once: "a STREET crosses the
 * road at this edge". Items 9 (wooden armco on both sides of a marked
 * crossing) and 15 (a barrier exactly where the streets meet at span 1110)
 * were the same defect seen from two angles -- tg_emit_guardrail has gates for
 * tunnels, decks, forks and kerb footways, and none at all for the two places a
 * road is meant to be OPEN sideways.
 *
 * It is worse than a plain omission, because the R9 railfix made the roadside
 * armco the FALLBACK owner of a city kerb: tg_rail_kerbfence_here breaks the
 * pedestrian railing at a crossing and at a street mouth (`pedestrians cross`,
 * `side-street mouth`), so on exactly those spans the railing stands down, the
 * armco stops yielding, and the gap the railing opened is filled with crash
 * barrier. MEASURED on seed 20260901 before this gate: 50 roadside rails on a
 * zebra span and 78 across a street mouth, every one of them with kerbfence=0.
 * Span 1110 is one of them (right side, xstreet=1) -- item 15 is not a separate
 * mechanism, it is one entry in item 9's list.
 *
 * PAD: a zebra is painted on ONE span, but a barrier that stops dead on the
 * bar line still reads as fencing the crossing in. TD5_TG_R11_XPAD spans of
 * clear air either side let the run end short of the paint.
 *
 * Per SIDE for the street mouth (an intersection opens one kerb at a time, and
 * the far kerb legitimately keeps its barrier); per SPAN for the crossing (it
 * runs kerb to kerb, so a pedestrian must not walk into armco on either end). */
#define TD5_TG_R11_XPAD 1
int tg_r11_street_crosses_here(const TG_NodeList *nl, int si, double sg);
int tg_guardrails_enabled(void);
int tg_span_needs_guardrail(const TG_NodeList *nl, int si, int nspans);
int tg_emit_guardrail(const TG_NodeList *nl, int si, TG_Buf *blk, int *emitted);
/* ===================== [FB] RESERVED SCENERY HOOKS =====================
 * One hook per work area of the 2026-08-26 feedback batch, called from the
 * marked lines in tg_emit_models. They exist so several parallel changes can
 * each add scenery without all editing the same dispatcher: fill in the body of
 * YOUR hook, leave the others alone.
 *
 * Contract, identical to tg_emit_props: append whole meshes to `blk`, and for
 * every mesh appended record its start offset via moff[(*nmesh)++]. Never let
 * *nmesh reach maxmesh. Return 0 only on allocation failure. The span si is
 * always a MAIN-RING span; `b` is its biome. */
typedef struct {
    const TG_NodeList *nl;
    int    si, nspans, lanes;
    const TG_Biome *b;
    TG_Buf *blk;
    size_t *moff;
    int    *nmesh, maxmesh;
} TG_FBHook;
/* ===================== [FB] GROUP A -- CITY STREET FURNITURE =====================
 * Pavements, kerb railings, zebra crossings, real streetlamps and the rows of
 * buildings BEHIND the street wall. Everything here is cosmetic: collision
 * comes from the STRIP, so a kerb is a step you drive over, not a step you hit.
 *
 * Sizes are raw world units (renderer divides by 256); a lane is 1500 raw and a
 * span is TD5_TG_SPAN_LENGTH (1500) raw long.
 *
 * PLACEMENT HOOK for pedestrians (the prop/flora side owns their density and
 * pages, this side owns where the pavement is): tg_city_sidewalk_w(b) > 0 says
 * a span has a walkable pavement, tg_city_kerb_h(b) is the height its surface
 * stands at, and tg_city_edge_frame gives the road edge and outward unit at both
 * ends of the span -- a figure belongs between `back` = 0.2 and 0.8 of the
 * pavement width out from that edge. Nothing here places people.
 */
#define TD5_TG_FENCE_H       520.0   /* pedestrian railing height              */
#define TD5_TG_FENCE_KERB    140.0   /* railing set back from the kerb face    */
#define TD5_TG_CROSS_LIFT     20.0   /* decal lift above the road, see below   */
#define TD5_TG_LAMP_H       2500.0   /* head height == k_prop_pages PP_LAMP y  */
int tg_city_span_paved(const TG_FBHook *h);
void tg_city_edge_frame(const TG_NodeList *nl, int si, double sg, double *out);
void tg_city_push_quad(double *px, double *py, double *pz, double *uu, double *vv, int *pn, const double *xyz, const double *uv);
int tg_r12_pave_stands(const TG_NodeList *nl, int si, int s);
int tg_city_emit_sidewalk(const TG_FBHook *h, double sw);
int tg_city_emit_verge_band(const TG_FBHook *h, double bw);
void tg_r8_city_sidewalk_diag(const TG_FBHook *h);
int tg_rail_kerbfence_here(int si, double sg);
int tg_city_emit_fence(const TG_FBHook *h, double sw);
/* [R7 CROSS item 14] Minimum spacing between KEPT crossings, in spans. Just
 * past a fork on a tight bend (seed 99991, the curve at span 635 immediately
 * after fork 2, 510-631) several side-street mouths open within a few spans of
 * each other, so the first-gap-span rule painted a zebra on nearly every span
 * and the R4/R5 "join" sliver-fill (tg_cross_emit_join_zebra) then merged them
 * into one radial fan of crosswalks -- the "many crossings all together"
 * report. Thinning keeps the first crossing of a cluster and drops any within
 * XMIN_GAP spans of the last KEPT one, so no two crossings crowd a bend. */
#define TD5_TG_XMIN_GAP   8
/* Backward window over which the greedy keep/drop decision is simulated. Larger
 * than any run of bunched mouths, so the simulated "last kept" state at span si
 * is correct wherever the window starts: a run of >= XMIN_GAP base-free spans
 * inside it resets the state, and city gaps that long are common. */
#define TD5_TG_XMIN_LOOK  48
/* [R11 CROSS item 16] "On curves ... intersections near a curve read as
 * confusing." The R7 thinning above is the right mechanism and the wrong
 * MEASURE: its gap is a flat 8 spans and completely curvature-blind, even
 * though its own comment names a tight bend after a fork as the case it exists
 * for. Eight spans of separation reads fine on a straight; the same eight spans
 * of ARC on a bend is much less visual separation, and the mouths foreshorten
 * into one another, which is the fan the user is describing.
 *
 * So the gap scales with how bent the road is, from TD5_TG_XMIN_GAP on a
 * straight up to TD5_TG_R11_XGAP_MAX at TD5_TG_R8_TURN_SIN -- the bend the R8
 * continuation test already calls sharp. The curvature comes from
 * tg_turn_bend, i.e. from tg_turn_map_build's own measurement; there is no
 * second curvature test and no new threshold. Bends thin harder, straights are
 * byte-identical.
 *
 * The gap of a PAIR is decided by the bent-er of its two ends: a crossing on a
 * straight sitting 9 spans from one on a bend is still a crossing you read
 * against a curve. TD5_TG_XMIN_LOOK (48) is still 3x the widest gap, which is
 * what keeps the greedy simulation's backward window valid. */
#define TD5_TG_R11_XGAP_MAX 16
int tg_crossing_base(int si);
int tg_city_crossing_here(int si);
void tg_r11_xcurve_report(int nspans);
int tg_city_emit_crossing(const TG_FBHook *h);
/* REAL STREETLAMPS, on the SHIPPED lamp page.
 *
 * The prop layer emitted PP_LAMP as a bare additive glow at y_off = 2500 with
 * nothing under it, so every light hung in mid-air. The first fix built a
 * fixture out of two grey boxes on the RAIL page, because the only lamp page
 * anyone had found was the additive GLOW (level001 p378, a radial gradient with
 * no post in it) -- that is what "lamp post textures look wrong" reports.
 *
 * [CONFIRMED, re/assets/levels/level001/textures.src/pages/page_356.png]
 * A sweep of every extracted level page for a full-height narrow alpha-keyed
 * silhouette turned up the real article: Keswick page 356 is a whole street
 * lamp -- silver post, arm, dark lantern head -- alpha-keyed on palette index 0,
 * exactly the opcode-4 billboard form the RE notes describe. So the boxes are
 * gone and the fixture is ONE quad carrying that page.
 *
 * The lamp occupies only the left 16 of the page's 64 columns (post at columns
 * 3..5, arm reaching right to column 15), so the quad maps the u SUBRECT
 * 0..TD5_TG_LAMP_U and is sized to that subrect's 1:4 aspect. U runs from the
 * OUTER edge inward, which is what makes the arm lean over the carriageway on
 * both kerbs without a second, mirrored page.
 *
 * 2 meshes per lamp (quad + glow) x 2 kerbs = 4, on 1 span in 7, night only. */
#define TD5_TG_LAMP_U       0.25    /* used width of the page, 16 of 64 cols   */
#define TD5_TG_LAMP_W       (TD5_TG_LAMP_H * TD5_TG_LAMP_U)   /* 1:4 aspect    */
#define TD5_TG_LAMP_POST_U  0.25    /* post's position across the subrect      */
int tg_city_emit_lamp(const TG_FBHook *h, double sw);
/* BUILDINGS SEEN DOWN A CROSS STREET. A city is depth, not a wall, but that
 * depth is only ever VISIBLE where a street opens through the frontage.
 *
 * The first cut emitted these rows on EVERY span, on both sides, whether or not
 * the front row was built. Behind a solid frontage they are hidden, but a taller
 * back building pokes ABOVE a short front roofline and the gaps between front
 * runs let more show through, so the whole city read as "rows of buildings
 * behind buildings" everywhere -- exactly the report. A real street only reveals
 * the block behind it where there is an actual opening to see through.
 *
 * So the rows are now GATED to a side that has a STREET OPENING at this span
 * (the front frontage is a gap there, which is precisely where
 * tg_city_emit_crossstreet lays the cross-street asphalt), and the DEPTH of the
 * reveal follows the street's WIDTH CLASS: a wide vehicular avenue (both kerbs
 * open) shows two rows of taller arterial blocks receding, a narrow pedestrian
 * side street shows a single closer, lower row. Deliberately cheaper than the
 * front row -- one page, no storefront command, no corner returns -- because they
 * are only ever seen DOWN the street, never up close, and set far enough back to
 * clear the cross-street carriageway the crossstreet emitter lays. */
#define TD5_TG_BACKROW_N     2        /* rows revealed down a wide avenue        */
#define TD5_TG_BACKROW_GAP   3200.0   /* clear air behind the row in front       */
int tg_bg_building_box(TG_Buf *blk, size_t *moff, int *nmesh, int maxmesh, double bx, double by, double bz, double ax, double ay, double az, double lx0, double lz0, double lx1, double lz1, double depth, double H, int cols, int rows, int page, int solid, int si);
double tg_xstreet_reach_at(const TG_NodeList *nl, int si, double sg,
                                  double ang, const TG_Biome *b, double sw);
double tg_block_arm_skew(int si, int left);
int tg_block_is_park(int si, int left);
int tg_city_emit_backrows(const TG_FBHook *h, double sw);
double tg_block_arm_skew(int si, int left);
/* A park gap carries a lawn instead of a carriageway, so the crossstreet emitter
 * skips it. Defined with the park code further down; forward-declared here. */
int tg_block_is_park(int si, int left);
/* CROSS STREETS. A facade gap used to be nothing but absent buildings: the
 * corner returns turned inward at each end and between them lay the same verge
 * as open country, so a "side street" was a hole in a wall, not a street. This
 * lays the carriageway of that street -- one quad per open side, running from
 * the kerb straight out past the first back row, on the biome's own road page.
 * With the returns down each flank (tg_facade_push_cap) and the back rows behind
 * them, the gap now reads as a street you could turn into.
 *
 * Two conventions borrowed from the road mesh so the two match without a seam:
 * the page is tg_road_page(si), and UVs are isotropic at the road's own scale --
 * one page per LANE_WIDTH outward, one page per span across -- so the asphalt
 * grain is the same size on both carriageways.
 *
 * Height follows the ground skirt (TD5_TG_GROUND_DROP over TD5_TG_GROUND_WIDTH)
 * rather than staying flat at road level: over the ~7000 raw it reaches, the
 * skirt has already fallen ~20 raw, and a flat strip would lift off it. */
/* [R7 CROSS item 2] The perpendicular street used to stop one BACKROW_GAP past
 * the front facades -- a stub that ended at the first back row, so from the
 * racing line a side street read as a short dead-end apron rather than a road
 * receding into the block. Running it TD5_TG_XSTREET_GAPS back-row gaps deep
 * carries it past the first back row and up to the second, so the carriageway
 * (and the flanking sidewalk arms, which take their reach from here, so item 1
 * pavement lengthens WITH the street) recede convincingly. Purely OUTWARD --
 * the main-road end is unchanged -- so it cannot intrude on the carriageway.
 * DEFAULT ON; TD5RE_AUTOTRACK_XLONG=0 restores the one-gap stub. */
#define TD5_TG_XSTREET_GAPS  2.0
/* [R8 CROSS item 1] REACH MODEL REWRITE, not another increment of the constant.
 *
 * R7 raised TD5_TG_XSTREET_GAPS from 1 to 2 (7100 -> 10300 raw) and the report
 * came back verbatim: "the street should be much longer (in this and all
 * crossings)", now paired with "the buildings are spawning on the edge of the
 * sidewalks near the street". Those are ONE defect, and the constant was never
 * the cause of either half:
 *
 *   - The old expression measured the street against ONE building's depth plus
 *     N slabs of clear air. That is a SETBACK formula (how far behind the kerb
 *     does a wall stand), borrowed to answer a different question (how deep does
 *     a street run). A street's depth is a count of CITY BLOCKS, and a block is
 *     a building depth PLUS the clear air behind it -- the same quantity the
 *     back-row emitter already walks outward in. Expressing the reach in whole
 *     blocks makes "much longer" a number of blocks (3) instead of a magic
 *     distance, and it makes the street commensurate with the massing beside it.
 *   - "Buildings on the edge of the sidewalk near the street" is the reveal ROW:
 *     tg_city_emit_backrows stands its first row at sw + depth + BACKROW_GAP,
 *     which under the old reach was INSIDE the street and lay ACROSS its mouth.
 *     Lengthening the carriageway alone would just have produced a longer
 *     corridor with the same wall across it, which is why R7's increment did not
 *     read as any longer. The row now terminates the vista PAST the reach (see
 *     tg_city_emit_backrows), and new massing lines the street's flanks
 *     (tg_cross_emit_street_flank) instead of blocking it.
 *
 * HEIGHT. The old drop extrapolated GROUND_DROP linearly on a 24000 denominator
 * while the skirt actually falls its full drop over tg_verge_reach() (12000 by
 * default), so the carriageway floated above the ground it was supposed to lie
 * on -- harmless at 10300, a visible lift at 19600. tg_xstreet_drop samples the
 * skirt's real profile and stays FLAT past its outer edge, which is also what
 * makes a reach beyond the skirt safe to ask for.
 *
 * The cap keeps the far end inside the band a fork backdrop is known to render
 * in (14000-21000 out, R7 CITY item 3), so the street never runs off into
 * nothing. Everything is purely OUTWARD; the main-road end is untouched, so a
 * longer street cannot intrude on the carriageway.
 *
 * DEFAULT ON; TD5RE_R8_CROSS_REACH=0 restores the R7 two-gap reach (and with it
 * the R7 drop) for an A/B. */
#define TD5_TG_R8_XSTREET_BLOCKS  3.0      /* city blocks a side street runs   */
#define TD5_TG_R8_XSTREET_MAX 21000.0      /* far end stays on rendered ground */
double tg_xstreet_drop(double d);
/* [R8 CROSS item 1] SELF-LIMITING REACH. A street is a STRAIGHT ray; the main
 * road is not. Doubling the reach therefore introduces a failure the short stub
 * never had: on a bend (and above all around a fork, where tg_carriageway_reach
 * balloons to cover the branch corridor) a long straight street eventually
 * re-enters the carriageway of a span further along the track. Measured on seed
 * 99991 the raw 19500 reach did exactly that at 9 spans, and the R7 on-road
 * guard dutifully dropped the offending meshes -- safe, but it leaves holes in
 * the streets and leans on a backstop for something the model can decide itself.
 *
 * So the reach is clamped per mouth: step outward along the street's own bearing
 * and stop short of the first sample that comes within a margin of ANY nearby
 * span's carriageway. Spans adjacent to the mouth are excluded from the test --
 * the first few hundred units are always beside their own road by construction,
 * and clamping on that would collapse every street to nothing. The result is
 * floored at the R7 reach so a clamp can never ship a street SHORTER than the
 * one already merged.
 *
 * This is also the "cannot box the racing line in" guarantee for both items in
 * one place: nothing this area emits outward can reach back onto drivable
 * tarmac, by construction rather than by cleanup. Default ON;
 * TD5RE_R8_CROSS_CLAMP=0 for an A/B against the raw model reach. */
#define TD5_TG_R8_CLAMP_STEP    600.0   /* outward sampling step, raw       */
#define TD5_TG_R8_CLAMP_MARGIN 1400.0   /* clear air kept off any carriageway */
#define TD5_TG_R8_CLAMP_SKIP       4    /* spans either side excluded        */
#define TD5_TG_R8_CLAMP_WIN       48    /* spans either side tested          */
/* Sampling starts OUT HERE, not at the kerb. Measured trap: a point only a few
 * hundred units off its own kerb is, by construction, within a road width of the
 * spans just up and down the track, so testing from d=0 clamped EVERY street to
 * the floor (mean reach 11577 of a modelled 19500, flank blocks 406 -> 230 --
 * the clamp had quietly cancelled the whole item). Past this distance the ray
 * has cleared the road corridor on a straight, so anything the test then finds
 * is a genuine re-entry further along the track. */
#define TD5_TG_R8_CLAMP_MIN     4200.0
double tg_xstreet_reach_at(const TG_NodeList *nl, int si, double sg, double ang, const TG_Biome *b, double sw);
/* ===================== [R10 SPAN66] SIDE-STREET OCCUPANCY ==================
 * ROUND 10 item 1: "there's people and a phone booth in the middle of the
 * street" at span 66, seed 99991, on a build whose on-road guard reported 70
 * rejects and 0 remaining.
 *
 * MEASURED, not reasoned (TD5RE_R8_GUARD_DIAG=66 on the merged build):
 *   entry@64 mesh 29 kind=prop mark_si=66 verts=20 intr=0    dy=[129,1116]  keep
 *   entry@64 mesh 23 kind=prop mark_si=65 verts=4  intr=-1018 dy=[-0,699]   keep
 * The 20-vertex prop is the phone box (2.40 m * TD5_TG_INFRA_M + the 129 kerb),
 * the 4-vertex ones are the spectator billboards. intr = 0 / negative means the
 * guard measured them as standing OUTSIDE the carriageway -- and by its own
 * definition of carriageway they DO. So the guard was not blind to props, and it
 * was not height-gated or licensed: it answered the question it was asked
 * correctly, and the question was wrong.
 *
 * ROOT CAUSE. tg_carriageway_reach knows about exactly two kinds of drivable
 * tarmac: the main road, and a fork's branch corridor. It does not know about
 * the SIDE STREET that tg_city_emit_crossstreet lays across a frontage gap --
 * asphalt running from the main kerb outward for thousands of units. Every
 * placement helper in this file sets furniture back from the MAIN ROAD EDGE by a
 * pavement-sized gap, so at a gap span the setback lands on that asphalt. Span
 * 66 is a gap-interior span of the 65-68 frontage gap, which is why both the R9
 * INFRA phone box and the PRE-EXISTING spectator layer put something in it.
 *
 * THE FIX IS ONE AUTHORITY WITH TWO CONSUMERS, so placement and enforcement
 * cannot drift apart the way a placement-only fix did four rounds running:
 *   - PLACEMENT: tg_prop_one and tg_infra_place refuse a footprint that overlaps
 *     the street. There is no pavement on a side-street mouth to stand on, so
 *     the honest answer is to place nothing, not to shove it further out.
 *   - ENFORCEMENT: TG_GK_PROP gains its own policy class, TG_GKC_FURNITURE, and
 *     the on-road guard measures that class against main road AND side street.
 *     A future furniture emitter is caught for free, exactly as the R7 guard
 *     catches a future scenery emitter.
 *
 * The gates below are the crossstreet emitter's OWN gates, read in the same
 * order, so this reports the street that is actually laid rather than a second
 * model of it. Default ON; TD5RE_R10_XSTREET_GUARD=0 pins the pre-R10 behaviour
 * for a single-variable A/B. TD5RE_R10_PROP_AUDIT=1 names every furniture
 * placement with its measured laterals. */
#define TD5_TG_R10_XSTREET_MARGIN 300.0   /* clear air kept off side-street tarmac */
#define TD5_TG_R10_AUDIT_MAX      4000
extern int s_r10_audit_n;
/* PER (SPAN, SIDE) MEMO. The guard asks this question once per VERTEX per
 * candidate span, and the answer's expensive half (tg_xstreet_reach_at) marches
 * outward sampling a 48-span window at every step. Unmemoised, a run with the
 * placement half disabled -- the A/B that proves the backstop works -- did not
 * finish generating seed 777 in 900 s. The answer is a pure function of
 * (si, side) for a given track, so the memo is exact, not an approximation.
 * Reset with the rest of the per-build accounting. */
#define TD5_TG_R10_XS_MAX 4096
void tg_r10_xs_memo_reset(void);
int tg_r10_xstreet_guard(void);
int tg_xstreet_here(const TG_NodeList *nl, int si, double side, double *preach);
double tg_footway_reach(const TG_NodeList *nl, int si, double side);
int tg_xstreet_occupies(const TG_NodeList *nl, int si, double side, double inner_d);
void tg_xstreet_audit(const TG_NodeList *nl, int si, double side, double inner_d, const char *what, int placed);
int tg_block_is_park(int si, int left);
/* Fork-back band sizing. Declared here rather than beside tg_city_emit_forkback
 * because the plaza emitter below closes the plaza's ENDS ([R9 CITY item 3]) at
 * the same depth and row cap the backdrop stands at -- one definition, so the
 * end walls can never drift from the band they close. See the rationale for the
 * values at tg_city_emit_forkback ([R5 item 5]: stand it back, halve the
 * footprint, cap the rows). */
#define TD5_TG_FORKBACK_GAP        7000.0 /* new: background air past the corridor */
#define TD5_TG_FORKBACK_GAP_TALL   5000.0 /* old (A/B): pre-R5 gap                  */
#define TD5_TG_FORKBACK_DEPTH      3000.0 /* new: how deep the background reads     */
#define TD5_TG_FORKBACK_DEPTH_TALL 6000.0 /* old (A/B): pre-R5 depth                */
#define TD5_TG_FORKBACK_MAX_ROWS   6      /* a backdrop, not a downtown tower       */
/* [R4 item 6] BACKGROUND massing behind a fork gore. Where a fork clears its
 * RIGHT lateral for the branch corridor, tg_side_geom drops the facade and every
 * verge emitter skips the side (tg_side_blocked), so the whole flank was bare
 * ground tiles out to the horizon -- verbatim "on the right side of the road
 * where there's a lot of tiles there should be a park or buildings in the
 * background depending on whether it's a park or not". This fills that void with
 * a continuous band set BEYOND the corridor: park green where the block
 * classifies as a park, a deep building box otherwise -- exactly the
 * park-or-buildings choice asked for.
 *
 * The setback is pushed out through the shared carriageway authority
 * (tg_carriageway_clear_gap on the right side, which alone knows the bowed
 * branch's current width), so nothing here can land on the corridor -- the same
 * guarantee the facade setback uses, just applied on the side the facade was
 * dropped from. Continuous span to span: consecutive spans share the endpoint
 * node, and height/page key on the SUPERBLOCK not the span, so the band does not
 * saw-tooth. Default ON; TD5RE_AUTOTRACK_FORK_BACKDROP=0 restores the bare flank
 * for an A/B. */
/* [R5 item 5] The fork-back BUILDING massing (span 137 on seed 99991) read as
 * "a way too big skyscraper that takes a lot of space and looks weird". It is a
 * BACKDROP behind the fork gore, only ever seen at a distance past the branch,
 * so it does not need a downtown tower's height or a full building's depth. Three
 * levers, all cut here so item 5 is one reviewable place:
 *   - GAP up (5000 -> 7000): stand it further back so it reads as skyline, not
 *     a wall crowding the branch. Also widens the clear air between it and the
 *     bowed branch road (helps items 6/7, same emitter, same near-branch spans).
 *   - DEPTH down (6000 -> 3000): half the footprint. "takes a lot of space".
 *   - a dedicated ROW CAP (below), well under the street facade's 10, plus
 *     dropping the +2 skyline bump, so it never towers over the street run in
 *     front of it.
 * Default ON; TD5RE_AUTOTRACK_FORKBACK_SMALL=0 restores the old tall/deep/near
 * massing (GAP 5000, DEPTH 6000, cap FACADE_MAX_ROWS, +(hash%5)+2) for a
 * single-build A/B and byte attribution. */
/* (sizes are declared above tg_city_emit_forkback_plaza -- [R9 CITY item 3]
 * needs them to close the plaza's ends at the same depth the backdrop uses.) */
/* [R6 item 3] Per-span variation of the fork-back band. STAGGER pushes each
 * span's block to a different depth so the face is jagged and neighbouring spans
 * no longer share an edge -- distinct buildings with gaps ("breaks in the row"),
 * not one swept curtain. BASE lifts the whole band further back so it reads as a
 * distant skyline rather than a wall hugging the branch. */
#define TD5_TG_FORKBACK_STAGGER_BASE  2500.0
#define TD5_TG_FORKBACK_STAGGER_RANGE 9000u
/* ---- item 3: one corner arm -- a pavement + railing turning off the main kerb
 * to run OUTWARD along the side street. `C` is the kerb corner (a main-road edge
 * point); (ox,oz) the skewed OUTWARD unit down the street; (bx,bz) the BACK unit
 * (away from the gap, onto the built side) so the slab and railing sit clear of
 * the carriageway; `reach` how far the arm runs; `sw` the pavement width. Up to
 * 3 meshes: slab, its road-facing kerb face, and the railing. */
#define TD5_TG_ARM_SET   140.0    /* railing set back from the kerb, raw */
int tg_r11_arm_side(const TG_NodeList *nl, int si, int s);
/* ---- items 5-6: parks & houses ----------------------------------------------
 * A park fills a gap side with a lawn, a hedge border behind the pavement, and
 * the occasional house. The lawn abuts span to span (shared endpoints) into one
 * continuous green, the same no-pop trick the treeline band uses. */
#define TD5_TG_HEDGE_H    520.0    /* park hedge height, raw (~2 wu, see over)  */
#define TD5_TG_HOUSE_W   2600.0    /* individual house footprint, raw           */
#define TD5_TG_HOUSE_D   2200.0
#define TD5_TG_HOUSE_H   2600.0    /* ~two low storeys                          */
int tg_emit_fb_block(const TG_FBHook *h);
/* ==========================================================================
 * [R4 CROSS] REAL INTERSECTIONS  (feedback R4 items 2, 10, 14)
 *
 * This block turns the existing street crossings into fuller intersections. It
 * owns page TD5_TG_PAGE_R4_CROSS (8 slots; +0 is the marked cross-street from
 * item 9) and the TG_ACCT_CROSSFURN inventory kind. Its dispatcher
 * tg_emit_fb_cross is wired into the scenery loop next to tg_emit_fb_block, so
 * none of it edits another area's emitter. Every emitter is behind its own knob
 * (default ON) so each is a single-variable A/B away.
 *
 *   item 2  (R4) a RAISED KERB BREAK at the crossing -- REMOVED in R5 CROSS
 *           item 3 (it was the "elevated sidewalk on crossings" the user then
 *           reported; the wrap is done by tg_block_emit_intersection instead).
 *   item 10 BUILDINGS LINING a through side street (walls on the two along-road
 *           edges of the gap, running outward), so the street reads as going
 *           further with more buildings on its sides. They only ever extend
 *           perpendicular to the main road, so they can never land on the
 *           drivable corridor. (An earlier deep-rows cut emitted but was
 *           invisible from the racing line -- see tg_cross_emit_sidewalls.)
 *   item 14 CONTINUOUS ZEBRA joining two crossings that fall within a couple of
 *           spans of each other on a curve, so a run of closely-spaced side
 *           streets reads as one junction rather than a stutter of crosswalks.
 *           The join is a road-surface decal only (no wall or corridor change).
 * ========================================================================== */
#define TD5_TG_XJOIN_WIN    5       /* max span gap between two joined crossings */
#define TD5_TG_XJOIN_CURVE  0.06    /* min |sin turn| over the sliver to join    */
/* [R8 CROSS item 1] MASSING BESIDE THE STREET -- the other half of the reach
 * rewrite. Lengthening the carriageway alone gives a long strip of asphalt with
 * blank ground down both sides, which does not read as a street either; and the
 * only massing that used to acknowledge a side street at all was the reveal row,
 * which stood ACROSS its mouth (see tg_city_emit_backrows). So a street now gets
 * a proper FRONTAGE: blocks standing back from its pavement arms, running out
 * along the street at block spacing, on the side streets' own flanks.
 *
 * Placement is derived from the pieces that already define the street, not from
 * new constants: the corner point and the outward bearing come from the same
 * tg_city_edge_frame + tg_block_arm_skew the carriageway and the arms use, the
 * lateral setback clears the arm pavement (sw) by a margin, the first block
 * starts past the MAIN-road frontage's own depth so the two never interpenetrate,
 * and the run stops at the street's reach so nothing stands beyond the street it
 * lines. Because the setback is measured from the arm pavement's BACK edge, no
 * block can land "on the edge of the sidewalk".
 *
 * Cheap by construction (tg_bg_building_box, one page, no storefronts, no corner
 * returns): these are only ever seen down a street. About a quarter of slots are
 * left empty as yards so a street is not a solid canyon. Default ON, and paired
 * with the reach model -- TD5RE_R8_CROSS_FLANK=0 (or the reach knob off) removes
 * it for an A/B. */
#define TD5_TG_R8_FLANK_LEN   4200.0   /* one flanking block along the street */
#define TD5_TG_R8_FLANK_ALLEY 1700.0   /* service gap between two of them     */
#define TD5_TG_R8_FLANK_SET    420.0   /* clear air behind the arm pavement   */
/* ============== [R13 FILL item 7b] GAP-INTERIOR INFILL BLOCK ================
 * "The BACK of the buildings of a section after a curve is visible. Fill that
 * zone with streets and buildings so it reads believably."
 *
 * MEASURED CAUSE, not a new city model. A frontage GAP on one side is treated
 * everywhere in this file as A STREET, and three emitters furnish it:
 *   tg_city_emit_crossstreet  lays the carriageway outward from the kerb
 *   tg_cross_emit_street_flank lines the street with blocks -- but ONLY from a
 *                             CORNER span (`if (!near_corner && !far_corner)
 *                             continue;`), i.e. the first and last span of the
 *                             gap
 *   tg_city_emit_backrows     stands the reveal row, which since R8 CROSS item 1
 *                             sits at the street's own REACH (19600-21000 raw)
 *                             so it terminates the vista instead of blocking it
 * All three are correct for a gap that is one street wide (2-4 spans, 6-8 for an
 * avenue). They leave nothing at all on a WIDER gap: its interior spans are
 * neither corner, so no flank is laid, and the only massing anywhere on that
 * side stands a whole street's reach out. That is CASE A below.
 *
 * MEASURED, and it is worth being exact because the first pass at this item got
 * it wrong. On seed 20260901, standing at the reported span 1677, the backs
 * actually on show are spans 1709-1712 on side R, 46000-53000 raw across the
 * chord of the bend. Those sides are BUILT frontage, not gaps -- so CASE A does
 * not touch them, and no gap-interior fill ever could. What is empty behind them
 * is empty BY GATE: tg_city_emit_backrows opens with
 *     if (gate && tg_facade_built(h->si, s)) continue;
 * on the reasoning that "a solid frontage hides whatever is behind it". True on
 * a straight -- the road is on the inward side of every frontage. False the
 * moment the road turns away, which is the whole of the user's viewing
 * condition. So a built run's rear is the one surface in this file that is
 * guaranteed to have nothing behind it at any depth, and on a bend it is
 * broadside to the driver. That is CASE B, and it is the reported zone.
 *
 * The zone is therefore (i) GENUINELY EMPTY, not blank-faced and not fogged: the
 * exposed runs at 1709-1712 report rows=5, so they DO carry a rear sheet
 * (a frontage run's own missing back sheet is r13-faces' item, not this one),
 * and they sit inside the draw range. There is simply no geometry behind them.
 *
 * Both cases get the same answer, because it is the same missing thing -- the
 * block that ought to stand one setback back from a street: one solid mass per
 * span, at the frontage setback plus one service gap, running one span along the
 * road. It is deliberately NOT frontage -- tg_facade_built is untouched, so the
 * run/gap pattern, the crossing furniture, the pavement arms and the reveal row
 * all still read the same opening and none of them moves. The mass is
 * tg_bg_building_box with solid=1, the same six-sided box the reveal rows and
 * the street flanks already use, so it reads from behind as well as in front.
 *
 * CASE B is deliberately NOT "back rows behind every run" -- that is the
 * "rows behind rows everywhere" tg_city_emit_backrows' gate exists to prevent,
 * and it would pay for 993 sides to fix 145. It is gated on the EXPOSURE
 * predicate instead, so only the sides a driver can see the back of are filled.
 *
 * CARRIAGEWAY CLEARANCE. The near face stands at sidewalk + run depth + one
 * service gap outward of the road edge (>= 7000 raw on the city biomes), and
 * grows further outward from there, so it can never reach tg_carriageway_reach
 * and the R7 on-road guard has nothing to drop. Proven by the guard's own reject
 * count, not by looking at a frame.
 *
 * TD5RE_R13_FILL=0 restores the bare interior for an A/B; TD5RE_R13_FILL_REAR=0
 * keeps case A and drops case B. */
#define TD5_TG_R13_GAP_KEEP   2      /* spans of the gap left as pure street  */
#define TD5_TG_R13_SET   1600.0      /* service gap behind the frontage line  */
#define TD5_TG_R13_SCAN      24      /* spans scanned looking for a corner    */
/* EXPOSURE constants, shared by the emitter below and the read-only report much
 * further down. Stated once here because both have to agree on what "the driver
 * can see the back of this run" means: a report measuring a wider cone than the
 * emitter fills would print a residue no setting could ever clear. */
#define TD5_TG_R13_EXPO_BACK  40      /* spans back a camera is tried from   */
#define TD5_TG_R13_EXPO_NEAR   3      /* ignore cameras this close (own span) */
#define TD5_TG_R13_EXPO_RANGE 30000.0 /* beyond this the rear is not readable */
#define TD5_TG_R13_EXPO_COS    0.87   /* forward half-cone, ~30 deg          */
int tg_r14_up_surround(int si, int s);
void tg_r8_cross_report(const TG_NodeList *nl, int nspans);
void tg_r10_cross_report(const TG_NodeList *nl, int nspans);
/* ============ [R13 FILL item 7b] EXPOSED-REAR MEASUREMENT (read-only) =======
 * "The BACK of the buildings of a section after a curve is visible. Fill that
 * zone with streets and buildings."
 *
 * Three different defects would all look like this from the driver's seat, and
 * they need three different answers, so the first job is to tell them apart:
 *   (i)   the zone behind the run is GENUINELY EMPTY -- nothing is emitted there
 *   (ii)  it is filled, but the rear FACES read as blank (that is r13-faces)
 *   (iii) it is filled beyond the draw/fog distance
 * Nothing here decides anything; it prints what the emitters already read.
 *
 * EXPOSURE TEST. A run's rear plane sits one run depth outward of its frontage.
 * Its outward normal is the side's lateral unit. A camera standing on the
 * centreline at span c sees that rear iff it is on the OUTWARD side of the plane
 * (dot(C - P, outward) > 0) while the rear is still AHEAD of it along its own
 * tangent and inside the draw range. On a straight that never happens -- the
 * road is on the inward side of every frontage. It only happens where the road
 * has turned away, which is exactly the viewing condition the user described.
 *
 * TD5RE_R13_FILL_REPORT=1 turns it on; TD5RE_R13_FILL_SPAN=N (+/- _PAD, default
 * 12) windows the per-span dump. Off by default. */
/* The CAM pass is a plain "what is in view" census, so it uses a wider cone and
 * a longer reach than the exposure predicate and prints the front/rear verdict
 * per row instead of filtering on it. */
#define TD5_TG_R13_CAM_RANGE 60000.0
#define TD5_TG_R13_CAM_COS     0.60   /* ~53 deg half-cone                   */
void tg_r13_fill_report(const TG_NodeList *nl, int nspans);
void tg_r11_city_report(const TG_NodeList *nl, int nspans);
/* ================= [R13 JUNCTION] BEND-FOLD MEASUREMENT ====================
 * Item 3 ("double ground and buildings overlapping into the road at an
 * intersection right after a curve", span 709) is the THIRD round on that
 * neighbourhood, so this round measures the GEOMETRY rather than adding a
 * fourth placement predicate.
 *
 * Every lateral this generator lays -- the ground skirt, the facade setback +
 * its massing depth, a side street -- is a STRAIGHT ray off the centreline. A
 * bend of local radius R turns those rays into a pencil that converges at the
 * centre of curvature. Any ray longer than R passes THROUGH that centre and
 * comes out the other side, where it lies on top of the surface the spans
 * around the far limb of the bend laid, and on top of the carriageway itself.
 * That is one mechanism producing both halves of the complaint, and it is a
 * property of (reach, radius) alone -- no junction has to be involved.
 *
 * So this prints, per span: the local turn radius and which side is INSIDE,
 * the inside reach of each of the three lateral families, and -- for the
 * ground slab and the building mass -- the WORST measured intrusion of their
 * actual emitted corners onto some carriageway, found by a brute-force nearest
 * node sweep with NO straddle skip and NO distance bound. That last part is
 * deliberately the on-road guard's test with the guard's two refusals removed,
 * so a mesh the guard declines to judge still shows up here as a number.
 *
 * TD5RE_R13_JUNC_REPORT=1; TD5RE_R13_JUNC_SPAN=N (+/- _PAD, default 12)
 * windows the per-span dump. Read-only, opt-in, decides nothing. */
#define TD5_TG_R13_WIN 64
/* ============ [R14 JUNCTION item 4] MASS PROBE OVER REAL GEOMETRY ==========
 * R13 measured a block's on-road intrusion at its two BACK corners, and said so:
 * "the probe tests only the two back CORNERS, ... so it is a lower bound".
 * R13 FACES had already proved what that class of shortcut costs -- the water
 * audit judged buildings by their bounding box's corners and silently condemned
 * correct ones, because a corner is not a point of the mesh.
 *
 * Here the error runs the other way and HIDES intrusions. A block's footprint is
 * the bilinear quad between the frontage segment (bx,bz)..(bx+ax,bz+az) and the
 * back segment that same segment pushed `depth` along each end's own inward ray.
 * Those two rays are NOT parallel on a bend, so the quad is a trapezium at best
 * and a bowtie past the convergence point -- and a bowtie's deepest incursion is
 * in its INTERIOR, nowhere near either back corner. Sampling two corners of that
 * shape and calling the result "mass on road" understates it by construction.
 *
 * So sample the footprint itself, the same way R13 FACES fixed the water audit:
 * a TD5_TG_R14_GRID x TD5_TG_R14_GRID bilinear lattice over (u,v), which
 * includes all four corners, both back-edge midpoints and the centroid. Same
 * probe, same reach, same penetration threshold as R13 -- only the sample points
 * change, so the two numbers are directly comparable and the report prints BOTH.
 * TD5RE_R14_MASSGEOM=0 restores the two-corner probe for the A/B.
 *
 * WHAT THE 190 IS AND IS NOT. It is NOT a count of buildings visibly parked on
 * the tarmac, and it must not be quoted as one. tg_r13_probe judges a point by
 * the LATERAL it has in some other span's frame and ignores the ALONG-track
 * coordinate entirely, bounded only by TD5_TG_GUARD_NEAR_MAX (12000) and a +/-3
 * span exclusion. On a ring that doubles back on itself, a block standing
 * correctly beside its own kerb can score a small lateral in the frame of a
 * node up to 12000 away that the road has since curved away from -- the exact
 * frame misattribution R13 documented when it explained why the probe takes the
 * NEAREST node rather than the deepest. So:
 *     two corners (12)  = a genuine LOWER bound; it misses the quad interior.
 *     this lattice (190) = an UPPER bound; it inherits the frame error above.
 * The true count is between, and closing the gap needs an along-track
 * containment test this probe does not do. Both numbers are printed rather than
 * one being picked, because picking either would be a claim neither supports. */
#define TD5_TG_R14_GRID 5
void tg_r13_junc_report(const TG_NodeList *nl, int nspans);
/* ============ [R9 CITY] PAVEMENT UNIQUENESS + MOUTH MASSING SWEEP =========
 * Two class-level measurements over the ASSEMBLED geometry, so neither can be
 * satisfied by an emitter merely changing (R9 method rule 1) nor by one frame
 * (rule 2). Both run over every entry of the whole strip, both seeds.
 *
 * (A) PAVEMENT UNIQUENESS -- item 2. Per (main span, side), merge the marked
 *     along-road pavement meshes into SEPARATED lateral bands. One band is
 *     correct however wide it is; TWO separated bands on one span-side is the
 *     user's "double sidewalk". This is a uniqueness test over the shared
 *     resource (this span-side), not a total: emitting more pavement cannot
 *     make it pass, and dropping all pavement would show up as the run-length
 *     going to zero, which the SIDEWALK element inventory already tracks.
 *
 * (B) MOUTH MASSING -- item 1. Per (main span, side), the smallest |lateral| any
 *     UPRIGHT mesh reaches. Compared against the pavement line (road half width
 *     + pavement width), that answers "is there massing standing inside the
 *     sidewalk" directly from geometry. Crucially it is evaluated on EVERY span
 *     of a frontage gap, including the GAP INTERIOR spans -- span 66 is interior
 *     to the 65-68 gap, and the R8 CROSS sweep only ever visited the corner
 *     (mouth) spans, which is why 190 swept mouths left span 66 untouched.
 *
 * A corridor span carries no node of its own, so its pavement is attributed to
 * the MAIN span it runs beside (tg_fork_of_corridor), which is the span the user
 * names when driving the branch.
 *
 * Read-only. TD5RE_R9_CITY_REPORT=1 prints the per-span offender lines; the
 * SUMMARY totals are always logged, since they are the acceptance numbers. */
#define TD5_TG_R9_BANDS_MAX   4      /* separated pavement bands tracked/side  */
#define TD5_TG_R9_BAND_JOIN 150.0    /* closer than this reads as ONE pavement */
#define TD5_TG_R9_UPRIGHT   300.0    /* rises this far above road = massing    */
typedef struct {
    float lo[TD5_TG_R9_BANDS_MAX];   /* inboard |lateral| of the band */
    float hi[TD5_TG_R9_BANDS_MAX];   /* outboard |lateral|            */
    unsigned char src[TD5_TG_R9_BANDS_MAX];
    unsigned char n;
} TG_R9Bands;
/* ============== [R14 BRANCH item 2a] RAW (UNMERGED) PAVEMENT BANDS ==========
 * The R9 store above is the wrong instrument for this item, and the reason is
 * worth stating: tg_r9_band_add MERGES any two bands that come within
 * TD5_TG_R9_BAND_JOIN of each other. That is exactly right for R9's question
 * ("are there two SEPARATED pavements on this span-side?") and it makes the
 * opposite defect invisible -- two pavements laid on the SAME lateral merge
 * into one band and report n=1, i.e. "healthy". A doubled pavement at a fork
 * mouth is precisely that case, which is why DOUBLE_PAVEMENT stayed at its R9
 * number while the user was looking at two slabs stacked on each other.
 *
 * So this keeps every band RAW, tagged with the emitter that laid it, and asks
 * the complementary question: does any pair of bands from DIFFERENT emitters
 * OVERLAP, and by how much? Widths are |lateral| off the main centreline, so
 * the answer is a world-space measurement, not a reading of a screenshot.
 * Read-only; TD5RE_R14_BRANCH_REPORT=1 prints the per-span detail, the SUMMARY
 * totals are always logged because they are this round's acceptance numbers. */
#define TD5_TG_R14_RAW_MAX   6       /* raw bands retained per span-side       */
#define TD5_TG_R14_OV_MIN   50.0     /* thinner than this is a shared edge     */
typedef struct {
    float lo[TD5_TG_R14_RAW_MAX];
    float hi[TD5_TG_R14_RAW_MAX];
    unsigned char src[TD5_TG_R14_RAW_MAX];
    unsigned char n;
} TG_R14Raw;
void tg_r9_city_reset(void);
void tg_r9_city_scan_entry(const TG_NodeList *nl, int ring, int s0, int ns, const TG_Buf *meshes, const size_t *moff, int nmesh);
void tg_r9_city_report(const TG_NodeList *nl, int nspans);
void tg_r14_branch_report(const TG_NodeList *nl, int nspans);
int tg_emit_fb_cross(const TG_FBHook *h);
int tg_emit_fb_city(const TG_FBHook *h);
/* Group B -- flora & figures: tree placement/backdrop, prop scale & density.
 *
 * A CONTINUOUS TREE-LINE band. Individual tree billboards leave the horizon
 * open, so a forest reads as a handful of cut-outs standing on empty ground:
 * there was no backdrop layer at all before 2026-08-26. This lays one
 * alpha-keyed quad per side per span on TD5_TG_PAGE_TREELINE, set back well
 * behind the billboards, so the far side of the verge is closed off.
 *
 * Two properties the report asked for, both from the span's own endpoints:
 *   - NO POP: the band uses nodes si and si+1, and consecutive spans SHARE that
 *     endpoint, so the segments abut into one unbroken wall rather than
 *     appearing and disappearing per span.
 *   - NO SEAM: consecutive quads meet at the same u. As written this was a
 *     per-span 0..1 ALTERNATION (mirroring the page at every shared edge);
 *     [R11 item 1] replaced it with one CUMULATIVE u whose two ends are derived
 *     from the same per-span step, which meets the same requirement without
 *     forcing the whole 0..1 range into one span (see tg_r11_treeline_fit).
 * The band base is sunk below road level so it sits IN the ground skirt rather
 * than floating on it. */
#define TD5_TG_TREELINE_SINK   600.0   /* base below road level, raw */
int tg_r11_treeline_fit(void);
/* ==== [R12 TEX item 1] "the HEIGHT is right now, but it is WAY too pixelated
 * and has rough sloped (stair-stepped) edges" ===============================
 *
 * The follow-on to R11 item 1, which fixed the ASPECT (one tile drawn 1500 wide
 * by the full band tall, a 1:8 vertical smear) by tiling u at the band's own
 * height. That left ONE page tile covering the whole wall, so:
 *
 *   world units per texel = band / 64 = 12000/64 = 187 (FOREST), 219 (ALPINE).
 *
 * The band stands tg_treeline_back = 11000 units out. At a ~60 degree fov and
 * 1920 px across, the world width one pixel covers at 11000 is about 6.6 units,
 * so ONE TEXEL IS ROUGHLY 28 SCREEN PIXELS WIDE. That is the whole report: at
 * 28x magnification the page reads as pixels, and because the crown is a 1-bit
 * cutout resolved by an alpha test (the band is world geometry, so
 * td5_render_apply_page_blend_preset gives it TD5_PRESET_OPAQUE_LINEAR --
 * mag_filter 2, i.e. bilinear is ALREADY on, alpha_ref 1) the cutout edge
 * follows texel diagonals in 28-pixel steps: the "rough sloped edges".
 *
 * FILTERING IS NOT THE LEVER. Magnification is already linear, and mipmaps are
 * a MINIFICATION tool -- irrelevant at 28 px/texel. (Noted while measuring: no
 * page on an auto-track gets a mip chain at all. td5_platform_win32.c routes
 * mips for type-2 pages only, s_foliage_mips and s_opaque_mips both default
 * off, and every trackgen page is type 0 or type 1 -- zero "track mips" lines
 * in engine.log for a full generation. That is a separate question from this
 * item and is left alone.)
 *
 * DENSITY IS THE LEVER: draw the page smaller in world space and repeat it.
 * Two things stop a naive "tile v as well as u":
 *   1. THE PAGE IS NOT VERTICALLY TILEABLE. Measured on the decoded page (47):
 *      rows 0..23 carry the transparent key (the ragged crown), rows 24..63 are
 *      100% opaque foliage mass. Letting v run past 1 would wrap the keyed
 *      crown back into the MIDDLE of the wall -- holes with sky through them.
 *   2. THE PAGE DOES NOT TILE AT x=63 -> 0 either; R11 recorded that, and more
 *      u repeats would make that seam MORE frequent, not less.
 *
 * So the band becomes a STACK of quads inside the same mesh:
 *   - one CROWN quad at the top carrying page rows 0..23 (v 0 .. TL_CROWN_V);
 *   - K BODY quads below it, each carrying rows 24..63 (v TL_CROWN_V .. 1) and
 *     each MIRRORED against its neighbour in v, so consecutive strips meet on a
 *     shared row and the joins are continuous by construction -- no seam and no
 *     wrap into the crown.
 * and u advances half a tile per span through a TRIANGLE wave of period 2
 * tiles, so the horizontal repeat is a mirror about the tile edge and the
 * x=63 -> 0 seam never occurs. A span is exactly half a leg of that wave, so
 * every quad's u interval is linear and no quad straddles a fold.
 *
 * Tile size: TD5_TG_TL_TILE_U (3000) horizontally = 2 spans, exact. Vertically
 * K is chosen to put the tile height nearest the same 3000, which lands at
 * 2909 (FOREST) / 2947 (ALPINE) / 3111 (FIELDS) -- within 4% of square, so the
 * R11 aspect rule survives. Texel size drops from 187 to 45.5 world units, a
 * 4.1x density gain, i.e. ~7 screen pixels per texel instead of 28.
 * TD5RE_R12_TREELINE_DENSITY=0 restores the R11 single-tile band. */
#define TD5_TG_TL_TILE_U   3000.0
#define TD5_TG_TL_CROWN_V  0.375     /* rows 0..23 of 64, measured on page 47 */
#define TD5_TG_TL_MAX_BODY 10
/* [R16 item C 2026-09-07] Vertical tile SCALE for the treeline body stack.
 * Reported (p47 TREELINE): "a lot of vertical repetition ... a better way to
 * represent a taller texture for a lot of forest". The R12 density stack tiles
 * the page body (rows 24..63) every TD5_TG_TL_TILE_U (3000) world units up the
 * wall, so a tall band shows many identical mirrored repeats. Scaling ONLY the
 * VERTICAL tile size up (the horizontal du12 tiling is left at TILE_U so the
 * square-tile aspect and the u wrap are untouched) makes each body tile taller
 * and drops the repeat count. 1.8 keeps ~135 world/texel vertically, still finer
 * than R11's single-tile 187 but with ~1.8x fewer repeats.
 * Gated OFF (factor 1.0) by default via TD5RE_R16_TREELINE_VSCALE. */
#define TD5_TG_R16_TREELINE_VSCALE 1.8
int tg_r12_treeline_density(void);
double tg_r12_tl_fold(double t);
double tg_treeline_height(const TG_Biome *b);
double tg_treeline_back(const TG_Biome *b);
/* [R12 FLORA item 9] "on this curve there was NO tree background for a few
 * spans."
 *
 * MEASURED (TD5RE_R12_FLORA_REPORT=1, seed 20260901; the ALPINE cell 300..449
 * runs into ALPTOWN at 450): the band is ABSENT on spans 432, 440, 441 and 446
 * -- isolated one and two-span holes punched in a 14000-tall wall -- and
 * PRESENT on 450, 451, 452, 455 and 459, i.e. five isolated walls standing
 * inside the town. Every one of those spans reports hard=ALPINE soft=ALPTOWN or
 * the reverse, so the hole is the ~20-span biome DITHER and nothing else:
 * tg_biome_for_span assigns a boundary span to either biome by a per-span hash,
 * ALPTOWN carries no tree line, and a dithered span therefore deletes its
 * segment of the wall. Whole-track: 16 such dither holes.
 *
 * This is R11 GUARD's argument one emitter further out. A backdrop whose own
 * contract is "one unbroken wall" is per-RUN STRUCTURE -- you cannot half-build
 * it, and a missing span is not a thinner forest, it is a hole in the horizon.
 * R11 GUARD hardened the paved road edge for exactly this reason, but it
 * hardens tg_scenery_biome_index, and this emitter reads the RAW dither
 * (hook.b = tg_biome_for_span), so the fix never reached it.
 *
 * WHAT SNAPS AND WHAT RAMPS, the same split R11 GUARD drew: PRESENCE comes from
 * the HARD cell, so the wall runs unbroken to the last span of its own run.
 * HEIGHT and SETBACK are continuous quantities, so they interpolate -- toward
 * the neighbour's own values where the neighbour also carries a band (FOREST
 * 12000 / ALPINE 14000 / FIELDS 7000 at a 22000 setback), and down to zero over
 * the last TD5_TG_R12_BAND_TAPER spans where it does not, so the wall sinks
 * into the ground at the town line rather than ending on a 14000 face.
 * Evaluated per NODE, so consecutive quads share their endpoint height and
 * setback exactly and the wall stays one run -- which also cures the FIELDS
 * boundary, where the dithered 11000/22000 setback zigzagged the wall 11000
 * units in and out span by span.
 *
 * Default ON; TD5RE_R12_FLORA_BAND=0 restores the dithered band for an A/B. */
#define TD5_TG_R12_BAND_TAPER 8
void tg_r12_band_params(int si, double *out_h, double *out_back);
/* ==========================================================================
 * [R12 CROSS item 5] FOREST SIDE ROADS -- "add very occasional street crossings
 * in the FOREST biome. The roads must be long enough not to cut off abruptly,
 * and the tree background must be cut and folded over the road."
 *
 * WHY THE CITY MODEL CANNOT BE REUSED. Every side street on the track today is
 * gated on tg_city_sidewalk_w(b) > 0 -- a raised pavement is the signal "there is
 * a city frontage here to break". FOREST is a billboard biome with cell_w 0, so
 * that helper returns 0 for it (see its definition) and the WHOLE city junction
 * stack -- tg_crossing_base, tg_xstreet_here, tg_city_emit_crossstreet,
 * tg_block_emit_arm -- is a no-op in a forest by construction. Widening that gate
 * would not add a forest lane, it would add pavements, kerbs, zebras, corner arms
 * and flanking massing to a wilderness. So this is a SEPARATE, deliberately
 * minimal element: one carriageway and the tree wall folded around it.
 *
 * VERY OCCASIONAL, AND A PURE FUNCTION OF si. One CANDIDATE per
 * TD5_TG_R12_FCROSS_PERIOD spans, positioned inside its own block by the block's
 * hash, and it only becomes a crossing if every span it touches is forest and
 * clear. No cross-call state and no dependence on span order, because the
 * predicate is asked out of order (the tree-line emitter asks it per span, the
 * verge-tree emitter asks it from a different pass) -- exactly the constraint
 * tg_city_crossing_here documents for itself.
 *
 * LONG ENOUGH, BY CONSTRUCTION. The tree wall stands at tg_treeline_back (11000
 * out in FOREST), so a road that stops short of that reads as a stub cut off by
 * a hedge -- the "cuts off abruptly" complaint. The modelled reach is 16000, PAST
 * the wall, and the reach is clamped by the same outward re-entry test the city
 * street uses (tg_xstreet_reach_at): step out along the bearing and stop short of
 * any nearby span's carriageway. The difference is the FLOOR. The city street
 * floors the clamp and accepts a short street; here a clamped reach below
 * TD5_TG_R12_FCROSS_MIN (13000, i.e. still clear of the wall) REJECTS the whole
 * crossing. "Very occasional" is what makes that affordable: it is cheaper to
 * skip a candidate than to ship a stub, and the result is that nothing this
 * emitter lays can re-enter drivable tarmac and nothing it lays is too short.
 *
 * CUT AND FOLDED. The band is one alpha quad per side per span at lateral
 * `back`. Suppressing it on the crossing spans alone would leave a hole with
 * open sky and far terrain behind it -- a cut, no fold. So the band is cut AND
 * the two cut ends are folded 90 degrees to run OUTWARD along the new road's two
 * along-road edges, from just inside the wall line (TD5_TG_R12_FOLD_LEAD) all the
 * way to the road's far end. From the racing line the mouth reads as a corridor
 * of trees receding to the horizon rather than a notch in a backdrop. Same page,
 * same base sink and the same square-tile u rule as the band it continues, and
 * double-wound so the corridor closes seen from either direction.
 *
 * GUARD. The carriageway is marked TG_GK_ROAD at its own emit site (an authored
 * road surface touching the road edge would otherwise be dropped by
 * tg_guard_validate_entry); the fold walls are marked TG_GK_FLORA, i.e. ordinary
 * SCENERY that must earn its place -- they stand at >= 8400 lateral, so they pass
 * on merit rather than on a licence.
 *
 * Gate TD5RE_R12_FOREST_CROSS (default ON); =0 restores the pre-R12 forest.
 * ========================================================================== */
#define TD5_TG_R12_FCROSS_PERIOD   60    /* one CANDIDATE per this many spans  */
/* Clear of the opening stretch, not merely of the grid. TD5_TG_GRID_SPAN alone
 * put a candidate at span 27 on seed 20260901 -- 40 m past the start line, where
 * the frontage layer already keeps itself out of the way for a full
 * TD5_TG_FACADE_START_RUN. Same 60-span courtesy here. */
#define TD5_TG_R12_FCROSS_CLEAR (TD5_TG_GRID_SPAN + TD5_TG_FACADE_START_RUN)
#define TD5_TG_R12_FCROSS_WIDTH     2    /* street width in spans (3000 raw)   */
#define TD5_TG_R12_FCROSS_WANT  16000.0  /* modelled outward reach, raw        */
#define TD5_TG_R12_FCROSS_MIN   13000.0  /* below this the candidate is dropped */
#define TD5_TG_R12_FCROSS_STEP    600.0  /* outward clamp sampling step        */
#define TD5_TG_R12_FCROSS_MARGIN 1400.0  /* clear air kept off any carriageway */
#define TD5_TG_R12_FCROSS_SKIP      4    /* spans either side excluded         */
#define TD5_TG_R12_FCROSS_WIN      48    /* spans either side tested           */
#define TD5_TG_R12_FCROSS_CMIN   4200.0  /* clamp starts out here, not at kerb */
#define TD5_TG_R12_FOLD_LEAD     2600.0  /* fold starts inside the wall line   */
int tg_r12_fcross_at(const TG_NodeList *nl, int si, double *pside, double *preach);
/* ==========================================================================
 * [R14 FCROSS items 1a/1b/1c/1d] FEEDBACK ON THE R12 FOREST CROSSING
 *
 * Four reports against the element R12 CROSS built, all read off span 88 of
 * seed 20260901. Nothing here re-opens R12's central decision (a forest lane is
 * a SEPARATE minimal element, not the city junction stack widened into a
 * wilderness); three of the four are tuning of this emitter and the fourth is a
 * single new question asked of one shared helper.
 *
 * 1a UP TO TWO LANES. The street is TD5_TG_R12_FCROSS_WIDTH (2) spans wide, and
 *    a span is TD5_TG_SPAN_LENGTH (1500) == TD5_TG_LANE_WIDTH, so on paper it is
 *    already 2.00 lanes. It is not, because the two edges are nodes c and c+2
 *    and the width the driver sees is the CHORD between them: on any curved
 *    forest span that chord is shorter than the arc the span count promises, and
 *    the tighter the bend the narrower the mouth. So the width is not assumed
 *    from the span count, it is MEASURED and then PADDED back up to the two-lane
 *    goal by pushing the street's two along-road edges apart. Capped at the goal
 *    -- "UP TO two lanes" is a ceiling, so a straight-line crossing that already
 *    measures 3000 gets a pad of zero and is bit-identical.
 *
 * 1b RUSTY / WORN SURFACE, MORE OFTEN. Every crossing took the R4 marked-asphalt
 *    page (a painted centre line), because TD5RE_AUTOTRACK_CROSS_MARKINGS
 *    defaults on -- a city cross-street's surface on a forest track. A share of
 *    crossings now take the DIRT road page instead (RS_DIRT: brown, with
 *    down-track ruts and no paint), which is the worn unmade lane the report
 *    asks for and is a page the generator already builds for the dirt biomes, so
 *    this costs no new art. The share is a per-BLOCK hash so both spans of one
 *    street always agree and the same seed always lays the same surface.
 *
 * 1c NOTHING TOUCHES THE LANE. Three placement layers could put something on the
 *    new tarmac and none of them knew about it -- see the three seams declared
 *    next to tg_r12_fcross_at, and the note in tg_infra_place for why the on-road
 *    guard was not going to save us.
 *
 * 1d THE PAVEMENT STOPS AT THE STREET. See tg_pavement_side_width.
 * ========================================================================== */
/* Two lanes is the CEILING the report names, not a target to overshoot. */
#define TD5_TG_R14_FCROSS_LANES   2.0
/* A pad is a correction for chord shortening, not a licence to build a plaza:
 * more than this and the mouth would start eating its own shoulder spans. */
#define TD5_TG_R14_FCROSS_PAD_MAX 500.0
/* Share of crossings taking the worn dirt surface, in 1/256ths. 144/256 = 56%,
 * i.e. the worn lane is the COMMON case in a forest and the made-up marked one
 * is the exception -- which is the direction the report asks for. */
#define TD5_TG_R14_FCROSS_WORN_P  144
/* The crossing asphalt is laid at TD5_TG_VERGE_LIFT, exactly where the verge
 * band lies. Item 1d removes the band from the crossing spans, but the 1a pad
 * pushes the street a little past them, so lift the lane a hair further to win
 * the depth test against the band it overhangs. Well under the 70-raw skirt
 * drop, so it still reads as flat ground rather than a step. */
#define TD5_TG_R14_FCROSS_LIFT    6.0
int tg_r14_fcross_clear_side(const TG_NodeList *nl, int si, double side);
int tg_r14_fcross_occupies(const TG_NodeList *nl, int si, double side, double inner_d);
int tg_r14_fcross_pave_stop(const TG_NodeList *nl, int si, double side);
int tg_emit_fb_forest_cross(const TG_FBHook *h);
void tg_r12_fcross_report(const TG_NodeList *nl, int nspans);
/* ==== [R14 COAST item 5b] "in the background the FOREST ENDS ABRUPTLY -- the
 * tree line should WRAP AROUND THE COASTLINE instead of stopping flat."
 * ============================================================ SECTION:r14wrap
 *
 * MEASURED (TD5RE_R14_COAST_REPORT=1, seed 20260901): of 17 tree-wall run ends
 * on the whole track, SEVENTEEN end on a full-height face and ZERO taper to
 * nothing -- and 10 of the 17 end because the next span is a BRIDGE RUN. The
 * band emitter deletes itself outright over a bridge (TG_R12_BAND_BRIDGE, "see
 * the river from the deck"), with no taper and no setback, so a 12000/14000-tall
 * wall stops dead on a vertical face at one bridge mouth and reappears at full
 * height at the other. At the reported span (1351, inside the 1320-1359 run) the
 * ALPINE cell starts at 1350 but its wall cannot begin until 1360, so what the
 * driver sees off the bridge is exactly that: a 14000 face arriving flat.
 *
 * WHY NOT JUST TAPER IT. R12's taper sinks a wall into the GROUND over its last
 * spans, and it is the right answer at a biome line because there IS ground
 * there. At a bridge there is not: over a run tg_emit_fb_terrain returns before
 * both the far band and the far shore (R11 WATER's finding), so the river is the
 * drawn world all the way out to tg_r11_wet_reach. A wall tapering across a
 * bridge run would be a wall standing in the water.
 *
 * WHAT THE USER ASKED FOR IS A FOLD, and this file already has one:
 * tg_r12_fcross_emit_fold turns the two cut ends of the tree wall to run OUTWARD
 * along a forest side road so the cut never reads as a hole. A bridge cut is the
 * same shape of problem, so it gets the same shape of answer -- the wall turns
 * at the run boundary and runs outward ALONG THE SHORE, tapering to nothing
 * where the ground meets the water. That is "wrap around the coastline"
 * literally: the tree line follows the shoreline away from the bridge instead of
 * ending on a face.
 *
 * IT ADDS GEOMETRY AND CHANGES NO PRESENCE. The wrap starts exactly at the
 * band's own end point at the boundary node, using the same setback, base and
 * height the band computed there, so the two share an edge by construction. It
 * never touches tg_r12_band_params, so tg_r13_band_side -- and therefore R13
 * BAND's per-span/per-side cull decision -- is bit-for-bit what it was.
 *
 * The base follows tg_topo_chain (the whole-width ground authority item 5a gave
 * the coast band) rather than road level, so the wrap descends the bank instead
 * of floating over it, and it STOPS at the first sample where that ground has
 * reached the water surface. TD5RE_R14_COAST_WRAP=0 removes it. */
#define TD5_TG_R14_WRAP_COLS   6        /* columns from the band out to the shore */
/* How far PAST THE BAND'S OWN SETBACK the wrap may run before it gives up
 * looking for the shore. Measured as a length from the band, not as an absolute
 * lateral distance: FIELDS sets its hedgerow line back 22000 (tg_treeline_back)
 * against 11000 everywhere else, so an absolute cap sized for an 11000 band
 * leaves a FIELDS band no room at all and the wrap silently never fires. That
 * is exactly what happened -- 12 wraps on seed 20260901, ZERO on 20260902,
 * whose three water bridges all sit in FIELDS. */
#define TD5_TG_R14_WRAP_LEN 15000.0
int tg_emit_fb_flora(const TG_FBHook *h);
int tg_emit_fb_park_trees(const TG_FBHook *h);
/* ===================== [R9 INFRA] STREET FURNITURE =====================
 * The deferred-backlog item with the best ratio of visible density to risk:
 * twelve breakable street-furniture textures were extracted from TD6 in June
 * (re/assets/props/, extract_td6_prop_meshes.py) and NOTHING in the generator
 * ever placed one. td5_tg_props_tex.h now carries thirteen 64x64 pages mined
 * from those atlases; this places them.
 *
 * FORM. A bin, a crate and a phone box are read from a few metres away at
 * eye level, which is exactly where a camera-facing billboard gives itself
 * away, so these are BOXES: four sides and a lid, in one mesh with three
 * texture segments (ends / long sides / top) so a bench can carry its ornate
 * cast-iron end on the ends and its slats on the seat. Only the genuinely
 * flat-fronted pieces (a road sign, a barrier plank, a rickshaw) stay
 * billboards, where facing the camera is correct rather than a shortcut.
 *
 * PLACEMENT. Furniture stands ON the pavement where a biome has one (city and
 * its relatives) and on the verge where it does not, always outboard of
 * tg_carriageway_clear_gap so a fork's bowed corridor pushes it out with the
 * rest of the scenery. It is NOT exempted from the R7 on-road guard: if the
 * guard eats a bin, the bin was in the road and the guard was right.
 *
 * MENU. What stands beside a road is a property of the place: bins, signs and
 * phone boxes on a city pavement, a rickshaw only in ORIENTAL, crates and
 * cardboard in INDUSTRIAL, a bench on a COAST promenade, roadworks anywhere.
 * Knob TD5RE_R9_INFRA_PROPS (default ON), accounted TG_ACCT_R9_INFRA. */

/* Raw world units per metre: TD5_TG_LANE_WIDTH (1500) over a real 3.65 m lane.
 * Every size below is written as metres * this, so the furniture is at human
 * scale beside the road rather than at whatever looked right in a screenshot --
 * the mistake that made spectators 3.4 m tall until 2026-08-26. */
#define TD5_TG_INFRA_M   411.0
extern long s_r9_infra_props;
extern long s_r9_infra_ponds;
extern long s_r12_bench_form;
extern long s_r12_sign_kept;
extern long s_r12_sign_ctx;
extern long s_r12_sign_rate;
extern long s_r13_bench_end_uv;
extern long s_r13_plank_crop;
extern long s_r13_awn_form;
extern long s_r13_awn_kept;
extern long s_r13_awn_ctx;
/* page_end / page_side / page_top: the three texture segments of the box, or
 * -1 in page_top to mean "billboard, page_side is the page". w = across the
 * road, d = along it, hgt = up; lift raises the piece off its footing (an
 * awning hangs, a sign sits on a post we do not model). */
typedef struct {
    int    page_end, page_side, page_top;
    double w, hgt, d, lift;
} TG_InfraProp;
enum {
    IP_BIN = 0, IP_CRATE, IP_CRATEFRG, IP_CARDBOX, IP_PHONE,
    IP_BENCH, IP_CANOPY, IP_WORKY, IP_REDTAPE, IP_SIGN, IP_RICKSHAW,
    IP_COUNT
};
extern const char *const k_infra_names[IP_COUNT];
#define TD5_TG_INFRA_REPORT_MAX 3000
extern int s_r9_infra_reported;
extern const TG_InfraProp k_infra_props[IP_COUNT];
/* ===== [R12 PROPS item 2] The bench is NOT a box =====
 *
 * MECHANISM, measured not guessed. td6_bench.png q0 (TG_INFRA_BENCH) decodes --
 * verified by decoding the header arrays independently of the game -- as SIX
 * horizontal wooden SLATS with the gaps between them keyed on index 0 (the
 * header's own "type 1, key 29%"), plus one narrow cross-brace bridging them.
 * It is a slatted PANEL cutout, and it was being pasted on the two UPRIGHT LONG
 * SIDES of a solid box, where two things went wrong at once:
 *
 *   - the 29% keyed slat gaps punched see-through holes down the whole side, so
 *     the near face and the far face both showed and the piece read as HALF a
 *     bench rather than a solid object;
 *   - a slat is a line of constant v, so on an upright face the slats came out
 *     as horizontal bars climbing a wall -- "slats in the wrong places" -- and
 *     on the lid they ran ACROSS the 0.50 m depth instead of ALONG the 1.70 m
 *     length.
 *
 * So this is a MESH-FORM defect, not a crop and not a bad sub-rectangle: the
 * page is correct art applied to surfaces that do not exist on a bench. The fix
 * builds the bench a bench is: two ornate END cutouts (TG_INFRA_BENCHEND,
 * unchanged -- its u=1 edge is the backrest post and the box already mapped the
 * ends that way), one horizontal SEAT at TD5_TG_BENCH_SEAT of the height, and
 * one upright BACKREST above the seat on the OUTBOARD edge so the bench faces
 * the road. Seat and backrest both take u ALONG the length, which is the axis
 * the slats run down. There are no long side faces left for the cutout to see
 * through. Knob TD5RE_R12_BENCH_FORM (default ON) falls back to the box. */
#define TD5_TG_BENCH_SEAT  0.53   /* seat plane, fraction of the piece height */
/* ===== [R13 PROPS item 4a] The barrier plank is a CROP, not a wrong object ====
 *
 * "A prop texture CUT IN THE MIDDLE (looks like it needs its other half)."
 * Instrumented first: TD5RE_R9_INFRA_REPORT puts a `barrier-plank` at span 992
 * on an ALPINE verge, which is the red/white piece on snow in the frame, so the
 * page under test is td6_redtape.png q0 (TG_INFRA_REDTAPE).
 *
 * The page was decoded independently of the game, and it does NOT hold one
 * plank filling its cell. It holds TWO unrelated pieces of a 128x128 atlas
 * quadrant plus dead space:
 *
 *      y  0..1    x 0..6 only   grey post cap
 *      y  2..14   x 6..63       the red/white diagonal-banded PLANK
 *      y 15..44   x 0..6 only   a grey POST with a white stripe
 *      y 45..63   nothing (keyed on index 0)
 *
 * 1064 of 4096 texels are opaque and 648 of those are in the left half, which is
 * the post, not the plank. Mapping the whole cell to one billboard therefore
 * squeezed the plank into the TOP FIFTH of the quad, hung the unrelated post
 * down the quad's LEFT EDGE as a stray column, and left the bottom third empty
 * -- "cut in the middle, needs its other half", exactly.
 *
 * So this is a CROP bug and not a context bug. The piece is not rebuilt from
 * imagination either: the leftover column IS the plank's own post, so the honest
 * object is what the page actually contains -- a banded plank carried on two
 * posts, each drawn from its own sub-rectangle. The quad aspect follows the
 * sub-rectangle's texel aspect rather than the old prop box, so the bands stay
 * diagonal instead of being stretched flat.
 *
 * It also stops being camera-facing. A 2.6 m plank that swivels to face the car
 * is a billboard's answer to a problem a barrier does not have; it now lies
 * ALONG the road like the bench does, which additionally keeps its footprint out
 * of the lateral the carriageway authority polices.
 *
 * TD5RE_R13_PLANK_CROP=0 restores the whole-cell billboard. */
#define TD5_TG_PLANK_U0   ( 6.0 / 64.0)   /* plank sub-rect, measured off the  */
#define TD5_TG_PLANK_U1   ( 1.0)          /* decoded page (see the map above)  */
#define TD5_TG_PLANK_V0   ( 2.0 / 64.0)
#define TD5_TG_PLANK_V1   (15.0 / 64.0)
#define TD5_TG_POST_U0    ( 0.0)          /* post sub-rect                     */
#define TD5_TG_POST_U1    ( 7.0 / 64.0)
#define TD5_TG_POST_V0    (16.0 / 64.0)
#define TD5_TG_POST_V1    (45.0 / 64.0)
/* ===== [R13 PROPS item 5a] The awning is not a floating box =================
 *
 * "A prop that looks like an UMBRELLA is FLOATING IN THE AIR, with wrong folding
 * of its textures." The silhouette was not trusted: TD5RE_R9_INFRA_REPORT names
 * `awning` at spans 1145 / 1149 / 1158 on the ALPTOWN pavement around the span
 * in the frame, so the piece is IP_CANOPY, td6_canopy.png q2 -- a shop awning
 * valance. Decoded, the page is a fabric field down to y=21, a broad white
 * scalloped skirt to y=42 and a striped fringe below that: read as one flat
 * quad in mid-air, the scallop is exactly an umbrella dome. So the report is
 * accurate and the object is misidentified by its presentation, not by the user.
 *
 * TWO SEPARATE DEFECTS, diagnosed separately as required:
 *
 *   BASE HEIGHT. IP_CANOPY is the only k_infra_props entry with a non-zero
 *   `lift` (2.20 m) -- the table's own comment says it "hangs off a frontage".
 *   Nothing ever made that true. tg_emit_fb_infra hands every paved piece the
 *   SAME setback, sw*(0.30..0.70), which is the middle of the pavement, and
 *   tg_infra_place then lifts this one 2.20 m off it. There is no wall within a
 *   metre of it in any direction, so the awning hangs in clear air. This is not
 *   the sloping-ground fold hazard the R12 CROSS agent flagged: the footing here
 *   is a flat kerb (base_y = n->y + tg_city_kerb_h) and it is correct. The bug
 *   is that a wall-mounted piece was never given a wall.
 *
 *   FORM. tg_infra_box pastes one page on four upright faces AND the lid, so the
 *   valance -- a page with a top, a skirt and a fringe -- came out wrapped round
 *   a slab and repeated on its roof. That is the "wrong folding".
 *
 * FIX. The awning is CONTEXT-GATED to spans where a facade actually stands on
 * that side (tg_facade_stands + tg_facade_built, the generator's own frontage
 * authority, not a copy of its hash), ANCHORED so its back edge lands on the
 * facade line instead of mid-pavement, and built as an awning: a sheet sloping
 * down from the wall to a free edge over the pavement, plus the valance hanging
 * at that free edge. The sheet takes only the fabric band of the page (v 0 ..
 * TD5_TG_AWN_FABRIC_V) so the fringe cannot appear on a roof; the valance takes
 * the whole page, which is the one surface the art was drawn for.
 *
 * Refused picks are SUBSTITUTED exactly as R12 does for the disc sign, so the
 * furniture total and the r9-infra run list are unchanged and only the awning
 * SHARE moves. TD5RE_R13_AWNING=0 restores the lifted box.
 * ========================================================================== */
#define TD5_TG_AWN_FABRIC_V  (21.0 / 64.0)  /* fabric band of the valance page  */
#define TD5_TG_AWN_VALANCE   (0.35 * TD5_TG_INFRA_M)  /* hanging skirt depth    */
#define TD5_TG_AWN_FALL      (0.30 * TD5_TG_INFRA_M)  /* wall-to-free-edge drop */
int tg_infra_place(const TG_FBHook *h, int kind, double side, double gap, double base_y);
int tg_infra_menu(const TG_Biome *b, int paved, int *out);
/* ===== [R12 PROPS item 3] Where a disc sign belongs, and how often =====
 *
 * IP_SIGN is td6_1bollard.png q1: one specific piece of art, a RED NO-ENTRY
 * DISC, placed as a billboard lifted 1.90 m on a post this generator does not
 * model. Two separate faults, and the frequency one is the smaller:
 *
 *   CONTEXT. A no-entry disc means "you may not drive down there", which is a
 *   statement about a road NETWORK. On a forest or alpine verge with nothing to
 *   forbid, it is not a sparse detail, it is a wrong object -- and no rate makes
 *   a wrong object right. So the disc is now confined to biomes with a street
 *   to speak about: anything with a real pavement, plus ORIENTAL and INDUSTRIAL,
 *   which are billboard-frontage biomes but settled ones with gated yards. The
 *   wild verges are NOT left unsigned: R11 SIGNS owns the direction-arrow
 *   panels (TD5_TG_PAGE_R11_SIGN), which is the signage an open road actually
 *   carries, and that layer is untouched here.
 *
 *   RATE. Even on a street the disc came up on every menu pick that landed on
 *   it -- one in nmenu of ~19% of spans per side, which on the paved menu is a
 *   disc every few hundred metres. Kept at one pick in TD5_TG_SIGN_KEEP_1_IN.
 *
 * A refused pick is SUBSTITUTED, not skipped: total furniture density is
 * deliberately unchanged, so the element inventory and s_r9_infra_props stay
 * comparable across the A/B and the only thing that moved is the sign SHARE.
 * The stand-in is chosen by place -- a bin on a pavement, a bin or a bench on a
 * verge -- rather than "the next menu entry", which on the unpaved menu would
 * have turned every refused disc into a third crate in a wood.
 *
 * Knob TD5RE_R12_SIGN_CTX (default ON) restores the old unconditional pick. */
#define TD5_TG_SIGN_KEEP_1_IN  4u
/* [R15 PROPS item 1] gained (si, side): the disc is now confined to junctions,
 * which needs the span and the kerb it is being placed on. */
int tg_infra_sign_filter(const TG_Biome *b, int si, double side,
                         int paved, int kind, unsigned int hh);
int tg_infra_awning_filter(int si, double side, int paved, int kind, unsigned int hh);
/* ===================== [R9 INFRA] PONDS =====================
 * Asked for in ROUND 5 and unbuilt for four rounds. Small standing water away
 * from the road in the green biomes.
 *
 * IT REUSES THE WATER PLANE, it does not add a second water system: the same
 * TD5_TG_PAGE_WATER, the same world-space UV convention as tg_emit_water, and
 * the same TG_ACCT_WATER kind in the element inventory, so a pond is a water
 * surface as far as every other consumer is concerned.
 *
 * WHY IT IS A SHEET AND NOT A BASIN. There is no terrain-carving pass in this
 * generator -- the ground is a skirt profile (tg_ground_side), authored, not
 * sculpted -- so a pond sunk INTO it would be underneath the ground and
 * invisible, which is the failure mode that looks like a working feature in a
 * mesh count. What is buildable honestly is a sheet of shallow water LYING ON
 * flat ground, so the pond is gated on the terrain under its whole footprint
 * being flat (every sampled corner within TD5_TG_POND_FLAT of the others) and
 * sits TD5_TG_POND_LIFT above it. On a slope it simply does not appear.
 *
 * A basin needs the terrain profile to gain a per-span depression, which is
 * TOPO's authority this round, not INFRA's -- noted here as the design step a
 * later round would take rather than half-built now. */
#define TD5_TG_POND_LIFT    10.0    /* sheet clear of the ground, world units */
#define TD5_TG_POND_FLAT   130.0    /* max ground variation under a pond      */
#define TD5_TG_POND_NEAR  2600.0    /* inner edge, out from the road edge     */
#define TD5_TG_POND_FAR   7200.0    /* outer edge                             */
double tg_infra_ground_dy(const TG_NodeList *nl, int si, double side, double d, double water_side);
int tg_emit_fb_infra(const TG_FBHook *h);
/* How many spans either side of a portal carry mountain massing. Beyond this
 * the camera is inside the bore and the mass is behind the lining, so it is
 * invisible geometry -- 5 spans (~4000 units) is what a driver actually sees
 * from outside the mouth. */
#define TD5_TG_TUNNEL_MASS_SPANS 5
int tg_emit_fb_tunnel(const TG_FBHook *h);
/* Group D -- terrain & water: ledge slopes, longer skirts, hills, snow.
 * ===================== FAR TERRAIN =====================
 * The skirt reaches 24000 units (16 span lengths) and then stops dead, so the
 * background is a thin green band and then sky -- the "grass should be much
 * longer, with small hills and mountains far away" item.
 *
 * The whole design constraint is COST, not looks: this file is written to disk
 * per race and the model buffer is already ~2.3 MB, so distance has to be bought
 * with FEW LARGE polygons. Hence:
 *   - one band mesh per SIDE per GROUP of 4 spans (= one per display-list entry,
 *     so a band always lives in the entry that covers its own spans and cannot
 *     be culled independently of them), not one per span;
 *   - three quads outward per side, spaced geometrically, not a tessellated
 *     grid: 24000 -> ~52000 -> ~94000 -> reach;
 *   - the distant ridge is ONE more quad in the SAME mesh, on its own command,
 *     so a whole side costs 1 mesh / 16 vertices per 4 spans.
 * Measured cost: +0.5 meshes and +8 vertices per span, ~350 bytes/span.
 *
 * Heights come from a smooth function of WORLD POSITION, never of the span
 * index. Adjacent bands share their corner points exactly (tg_road_edge at
 * (si,1.0) and (si+1,0.0) are the same point), so sampling by position makes
 * every seam watertight and the ridge silhouette a continuous polyline rather
 * than a comb of per-group steps. */
#define TD5_TG_FAR_GROUP   TD5_TG_SPANS_PER_ENTRY
#define TD5_TG_FAR_TUCK      2000.0   /* overlap under the skirt's outer edge */
#define TD5_TG_FAR_SINK        150.0   /* and below it, so the skirt wins the seam */
/* Outward reach of the background band, world units.
 *
 * Was 180000. That is a DESIGN limit, not a taste one: each band is a FLAT slab
 * emitted at ITS OWN span's road height, so any two bands whose reaches overlap
 * are two slabs at different heights sharing the same airspace. Measured on
 * seed 1234567: road height across 715 bands runs -195..10467, a relief of
 * 10662 -- while one band reached 180000, seventeen times that. A band emitted
 * up on high ground therefore swept out over the low ground you were driving on
 * and hung ~6400 units above the car, reading in frame as the sky being
 * replaced by a dark slab (log/S1_skyanim_minus1.png).
 *
 * 30000 is the smallest the TD5RE_AUTOTRACK_TERRAIN_REACH knob allows and still
 * comfortably longer than the 24000 this replaced, so the "much longer grass
 * background" the reach was raised for survives. Verified with the band ENABLED
 * at the span that used to be covered: log/H1_reach30k.png.
 *
 * The reach cut was a MITIGATION. The cure is below (TD5_TG_FAR_SINK_AT): the
 * band is no longer a flat slab at all, so overlapping reaches no longer mean
 * overlapping heights. The reach stays at 30000 anyway because nothing has yet
 * looked at a longer one WITH the sink in place, and TD5RE_AUTOTRACK_TERRAIN_
 * REACH raises it without a rebuild once someone has. */
#define TD5_TG_FAR_REACH       30000
#define TD5_TG_RIDGE_BASE      4500.0  /* mean ridge height above the far plain */
double tg_far_reach(void);
double tg_r11_wet_reach(const TG_NodeList *nl, int si);
#define TD5_TG_FAR_SINK_AT   300.0   /* the floor sits this far under the min */
#define TD5_TG_RIDGE_MIN_UP 1200.0   /* crest above the road it is seen from */
/* [R5 item 16] "the height is not following the grass": on a tree-line biome the
 * ridge stands on the OUTERMOST far-ground point (j==3, at TD5_TG_FAR_REACH out).
 * With the full hill amplitude that point plunged up to ~2800 below the near
 * grass seam and jumped ~2000 between adjacent far-groups (measured on seed 99991
 * span 1052..1124, COAST), so the horizon tree line sat in a jagged trough rather
 * than tracking the grass in front of it. For tree-line biomes only, pull the
 * outer points into a shallow, gently-rolling band just under the seam so the
 * band -- and the ridge that stands on it -- follows the near grass. Strictly at
 * or below `base` (the road/grass edge), so the far-terrain "ceiling" the sink
 * cures can never return. TD5RE_R5_FLORA_TREELINE=0 restores the old plunge. */
#define TD5_TG_TREELINE_AMP_SCALE  0.25   /* shrink outer hill roll on tree lines */
#define TD5_TG_TREELINE_BASE_DROP  400.0  /* how far the far grass sits below seam */
int tg_r8_treeline_page(int g0);
/* ============ [R9 BRIDGE item 10] NO BACKGROUND MASSING OVER WATER ============
 * "on span 1039 there's buildings on the background that are over the water."
 *
 * MEASURED CAUSE. tg_far_group_over_bridge already suppresses the band for a
 * group INSIDE a bridge run, and 1036-1039 is such a group, so the offender is
 * not the group the user is standing on -- it is a NEIGHBOURING group. The river
 * rectangle is TD5_TG_BRIDGE_WATER_HALF (32000) either side of the centreline
 * and runs the whole length of the crossing; the far band reaches
 * TD5_TG_FAR_REACH (30000) sideways from a road edge. A group just past the run
 * end (1040-1043 on 99991) therefore lays its apron and stands its ridge
 * STRAIGHT ACROSS the water the crossing flies over, and on a curving approach a
 * group several spans away does the same. Nothing in the band's gating asks what
 * surface is underneath it -- the bridge gate is a SPAN test, and the defect is
 * a SPATIAL one.
 *
 * So this is the same shape of rule as R7's "no trees over water" and R8's "no
 * city backdrop inside a park": a placement-validity predicate over the WORLD
 * POINT, consulted by the emitter, rather than another span-range special case.
 * The band is not dropped -- that would undo R8 TERRAIN's extent work, which is
 * the reason the band now reaches far enough to hit the river at all. Its REACH
 * is CLAMPED to the last dry sample on that side, so the ground still runs out
 * as far as there is ground and stops at the shore. A side whose skirt already
 * ends in water emits nothing.
 *
 * TD5RE_R9_BRIDGE_DRYBAND=0 restores the round-8 unclamped band for an A/B.
 * ========================================================================= */
/* Keep the band this far clear of the river edge, so the clamp lands on dry
 * ground rather than exactly on the waterline. */
#define TD5_TG_R9_WATER_MARGIN 1500.0
/* How far along the strip the river of another span can reach this point. The
 * rectangle is 32000 half-wide and a span is TD5_TG_SPAN_LENGTH long, so 45
 * spans covers any crossing that could possibly overlap a 30000 reach. */
#define TD5_TG_R9_WATER_WINDOW 45
/* ==== [R13 BAND item 1b] NOTHING BEHIND A TREE LINE BUT BUILDINGS ==========
 *
 * "The valley and the SECOND row of background trees are unnecessary.
 * Everything behind a line of trees is invisible to the player."
 *
 * WHAT THE TWO NAMES ARE. Both are tg_emit_far_band, one mesh per side per
 * TD5_TG_FAR_GROUP: three APRON rings running out to tg_far_reach (30000) which
 * the R8 sink descends toward the track's global floor -- the "valley" -- and,
 * standing on the outermost ring, the RIDGE wall drawn on a tree-canopy page
 * (tg_r8_treeline_page) -- the "second row of background trees". The near wall
 * the user is looking at is tg_emit_fb_flora, at tg_treeline_back.
 *
 * THE OCCLUSION ARITHMETIC, which is what decides this rather than taste. The
 * wall top stands TOP above the road at LAT out from the road edge; the ridge
 * crest stands RTOP above the road at D[3] out. Both are seen from an eye
 * TD5_TG_R13_EYE_Y above the road, so the ridge is hidden when
 *
 *     (TOP - eye) / LAT  >=  (RTOP - eye) / D[3]
 *
 * with a TD5_TG_R13_OCCL_MARGIN safety factor on the right. In FOREST that is
 * (12000-600-2000)/11450 = 0.82 against a crest bounded by RIDGE_BASE (4500)
 * plus the hill roll (3600) at 30000, i.e. at most 0.20 -- hidden four times
 * over, and never close. Everything the apron draws sits at or BELOW road level
 * (its base is road - GROUND_DROP - FAR_SINK and the sink only descends), so
 * any apron ring BEYOND the wall is hidden by the same wall for free: the test
 * for it is purely `LAT < D[0]`, no angles needed.
 *
 * THE TWO OUTCOMES, and why they are separate:
 *   - wall in front of the apron's inner ring AND crest hidden -> the whole
 *     mesh is invisible; emit nothing.
 *   - crest hidden but the wall stands OUTSIDE the inner ring -> FIELDS, whose
 *     hedgerow line sits at 22000 with the apron starting near 10000, so the
 *     near half of that apron is in FRONT of the hedge and is the open ground
 *     the FIELDS biome exists to show. Drop the RIDGE only (ridge_ok = 0, the
 *     split R8 TERRAIN already built) and keep the ground.
 *
 * THE BUILDINGS CARVE-OUT the user asked for holds BY CONSTRUCTION, and is
 * asserted rather than assumed: a band only qualifies where an unbroken tree
 * wall stands, tg_treeline_height returns 0 for every urban biome, so a group
 * that qualifies can never be one whose ridge routes to TD5_TG_PAGE_R4_SKYLINE
 * (urbanity >= 2) -- the city skyline behind a frontage is untouched, and so is
 * every facade, forkback and far-shore emitter, none of which this touches.
 * The assert below states it as code so a future biome that carries both a tree
 * line and a skyline cannot silently delete the skyline.
 *
 * PRESENCE IS PER SPAN AND PER SIDE. Towns, bridges and the forest-crossing cut
 * carry no wall, and tg_r13_band_covers demands a whole group of margin either
 * end, so a band is only dropped deep inside a walled run and never where its
 * removal could open a hole in the horizon.
 *
 * TD5RE_R13_BAND_CULL=0 restores the R12 far band for an A/B;
 * TD5RE_R13_BAND_REPORT=1 prints the class-level inventory (meshes, bytes,
 * share of MODELS.DAT) that this item is argued from. */
#define TD5_TG_R13_EYE_Y        2000.0  /* chase-cam eye above the road, raw */
#define TD5_TG_R13_OCCL_MARGIN     1.25 /* safety factor on the crest angle  */
/* [R6 TUNNEL item 8a] Is the far-group owned by span si close enough to a tunnel
 * run that its distant ridge/skyline shows THROUGH the bore or stands beside the
 * portal approach?
 *
 * The far band is never emitted AT a tunnel span (the emit loop takes the tunnel
 * branch there, which calls no far-terrain), so the inventory already gaps over
 * 480-499 / 1520-1539. But the run reported by item 8 -- "geometry crossing the
 * road while inside the tunnel" -- is the far band of the groups just OUTSIDE the
 * mouths (e.g. 476-479 and 500-503 around the 480-499 bore): a long straight bore
 * looks clean through to its far mouth, and the distant ridge standing beyond
 * that mouth reads as a grey slab down the road. The mountain massing
 * (tg_emit_fb_tunnel) only surrounds the portal out to TD5_TG_TUNNEL_MASS_SPANS,
 * so past that the far band is what you see through the hole.
 *
 * Suppress the band for any group within TD5_TG_TUNNEL_FARCLEAR spans of a tunnel
 * span. That clears both the through-bore view and the immediate approach while
 * leaving the ordinary far background everywhere else. Proven by A/B frame at the
 * entrance and by the far-bands inventory widening its gap around each run. */
#define TD5_TG_TUNNEL_FARCLEAR 40
/* [R6 item 4] Does this far-band group cover (or sweep over) a fork's branch?
 * The fork bows the RIGHT corridor out into the plain, so the right far-band
 * skyline -- laid from the main-road edge -- lands ON the branch carriageway as
 * a row of grey slabs "crossing the street" (seed 99991 spans 150/154 on the
 * right branch). The forkback backdrop already closes that flank, so the right
 * band is pure intrusion there. The intrusion is NOT confined to the fork's own
 * spans: the band of a group just PAST the rejoin still sweeps back across the
 * branch on the curve, so the window is padded forward by TD5_TG_FAR_FORK_PAD.
 * Same shape as tg_far_group_over_bridge. */
#define TD5_TG_FAR_FORK_PAD 12
/* [R8 TERRAIN item 15] FAR SHORE -- the seaward horizon.
 *
 * On a water run the seaward side deliberately carries no far band (the plane
 * is 50000 units of sea and a grass apron would float over it), so the horizon
 * out there is bare sky meeting flat water. Measured at the span the user named
 * (777 span 200): left extent 9600 against 30000 on the right.
 *
 * One quad per far-group per seaward side: a wall standing ON the sea surface
 * just inside the water plane's outer edge, textured from the same treeline /
 * skyline pages the inland ridge uses so the far side of a bay reads as land.
 * It is a SILHOUETTE, not terrain -- there is no apron behind it, which is why
 * it costs one quad and cannot recreate the flat-slab-overhead class of bug the
 * far band's reach cut exists to prevent (it never rises above its own base +
 * crest, and its base is the sea, the lowest surface in the run).
 *
 * TD5RE_R8_TERRAIN_SHORE=0 restores the empty seaward horizon for an A/B. */
#define TD5_TG_SHORE_FAR_INSET  4000.0   /* inside the water plane's outer edge */
#define TD5_TG_SHORE_FAR_HIGH   2600.0   /* crest above the sea surface         */
int tg_emit_fb_terrain(const TG_FBHook *h);
extern long s_r13_models_bytes;
void tg_r13_band_report(const TG_NodeList *nl, int nspans);
void tg_r14_band_report(const TG_NodeList *nl, int nspans);
void tg_topo_chain(const TG_NodeList *nl, int si, int is_left, TG_TopoChain *c);
double tg_topo_drop_at(const TG_TopoChain *c, double d);
/* [R9 TOPO item 6] SLOPE FLORA -- "the trees are not following".
 *
 * The near-verge planters put every tree inside the flat skirt, so where a side
 * falls away the tree line stops dead on the ridge and the whole slope below it
 * is bare. That reads as the world ending at the tree line even when the ground
 * carries on, which is half of what the span-549 screenshot shows.
 *
 * This plants a second, sparse rank OUT ON THE FALLING SURFACE: distances drawn
 * past the skirt's outer edge, heights taken from the chain (so a tree sits on
 * the slope, not above it), and only where the chain actually falls. It is a
 * consumer of the authority, not a new placement rule of its own -- it inherits
 * the C3 road cap and the C2 run-out for free because the chain already applies
 * them, which is the entire point of building the authority first.
 *
 * Billboard-tree biomes only (a facade biome has no trees to speak of) and one
 * per span at most. TD5RE_R9_TOPO_FLORA=0 for an A/B without disabling the
 * continuity clauses. */
#define TD5_TG_TOPO_FLORA_MINDROP 700.0   /* a side must fall this much to plant */
int tg_emit_fb_slope_flora(const TG_FBHook *h);
void tg_r9_topo_report(const TG_NodeList *nl, int nspans);
void tg_r14_up_report(const TG_NodeList *nl, int nspans);
/* ===================== [R11 WATER] RIVER SEAM DIAGNOSTIC =====================
 * "Gaps in the water on span 848, on the right" and "at the end of the bridge
 * around span 878 the coastline should reach the end of the drawn area".
 *
 * Both spans sit in a bridge run (800-839 and 840-879 on seed 20260901, two
 * abutting runs over a DRY gorge -- no sea biome anywhere near, so the only wet
 * surface out there is tg_emit_bridge_water's river) and over a bridge group
 * tg_emit_fb_terrain returns before both the far band and the far shore. The
 * river IS therefore the drawn area on those spans, which is why a hole in it
 * reads as a hole in the world rather than as a puddle with a missing corner.
 *
 * MEASURED HERE: the river is one quad per span, and all four of its corners
 * are swept along ONE node's normal (n0's). tg_emit_water -- the sea -- sweeps
 * each corner along its OWN node's normal, so its shared edge at node si+1 is
 * literally the same two points for both spans and the surface is watertight by
 * construction. The river's is not: span si's far edge leaves node si+1 along
 * n0's normal while span si+1's near edge leaves the same node along n1's, so
 * wherever the road turns the two edges diverge, opening a wedge that is zero at
 * the centreline and widest at the rim. That is a per-span number:
 *
 *     seam = BRIDGE_WATER_HALF * |left(n0) - left(n1)|  ~= BW * dheading
 *
 * so a degree of turn between two nodes is ~560 units of open water at the rim.
 * tg_emit_bridge_coast has the same defect one element over -- its LAND corners
 * are swept along the WATER node's normal -- so its outer corners land off the
 * terrain they are supposed to close onto.
 *
 * Read-only; re-evaluates the same pure functions the emitters call.
 * TD5RE_R11_WATER_LOG=1 (opt-in, same idiom as the other trackgen DIAG knobs). */
double tg_r11_sea_outer(const TG_NodeList *nl, int si);
void tg_r11_water_diag(const TG_NodeList *nl, int nspans);
void tg_r8_terrain_extent_report(const TG_NodeList *nl, int nspans);
void tg_r9_bridge_report(const TG_NodeList *nl);
/* ===================== [FB] START / FINISH GANTRY =====================
 * Reported: "add start banner" and "there should be a finish banner".
 *
 * PRIOR ART. Shipped TD5 start gantries are ordinary MODELS.DAT quads on a
 * dedicated texture page (Keswick's is page 338), not a special engine object:
 * two uprights either side of the road and a panel bridging them. Two lessons
 * from that one carry over. (a) It Z-FIGHTS when built from coplanar quads under
 * CULL_NONE, which is how scenery is submitted here -- so the panel is a SLAB
 * with real thickness, front and back separated, never two back-to-back quads
 * on the same plane. (b) It is authored at the road's own scale, so the span is
 * derived from the road edges (tg_road_edge, the same source the guardrails and
 * ground skirts use) rather than from a fixed width.
 *
 * ONE mesh for the whole gantry: legs and panel are the same page and the same
 * opaque dispatch, so splitting them would only cost extra moff slots.
 */
#define TD5_TG_GANTRY_CLEAR    2600.0  /* underside of the panel above the road */
#define TD5_TG_GANTRY_PANEL_H  1000.0  /* panel height                          */
#define TD5_TG_GANTRY_LEG_W     220.0  /* upright half-width, lateral           */
#define TD5_TG_GANTRY_THICK     160.0  /* slab depth, along the road            */
#define TD5_TG_GANTRY_OUT       260.0  /* legs outboard of the road edge         */
/* [R4 item 1] Solid-leg + seam-free dimensions, gated so the fix is a clean
 * single-variable A/B against the shipped round-3 gantry. The leg cross-section
 * at LEG_W 220 x THICK 160 (0.86 x 0.63 wu) under a 3600-raw-tall post reads as
 * a spindly stick, not a solid column ("pillars don't look quite solid"); the
 * fix squares it up to LEG_W/LEG_D and pushes the feet further outboard so the
 * inner face still clears the carriageway. The centre line down the panel is the
 * L/R page boundary sampled at its extreme texel column (same class the facade
 * survey cured with a half-texel inset), so the fixed panel insets its UVs. */
#define TD5_TG_GANTRY_LEG_W2    300.0  /* fixed upright half-width, lateral      */
#define TD5_TG_GANTRY_LEG_D2    300.0  /* fixed leg depth along the road         */
#define TD5_TG_GANTRY_OUT2      360.0  /* fixed foot outboard (inner face clears) */
int tg_emit_fb_track(const TG_FBHook *h);
/* [R3 item 19] Visible backstop wall across the full road at each OPEN end of
 * the strip, so the invisible boundary sentinel (td5_track_bind_boundary_
 * sentinels sets fwd=2 / rev=ring-3 for a custom P2P track) has something to
 * look like. The autotrack centreline is an open path, so those two spans are
 * the real map edges. `at_far` picks which edge of the span to draw on: 0 = the
 * near edge (start end, behind the grid), 1 = the far edge (finish end). One
 * opaque quad on the solid facade page (TD5_TG_PAGE_WALL) -- no new art. */
#define TD5_TG_ENDWALL_H 3200.0   /* raw; ~12.5 wu, over any jump the car makes */
int tg_emit_end_wall(const TG_NodeList *nl, int si, int at_far, size_t *moff, int *nmesh, int maxmesh, TG_Buf *blk);
int tg_fork_of_main(int si);
int tg_fork_of_corridor(int si, int *k);
/* ===================== [R11 SIGNS] CURVE DIRECTION SIGNAGE =================
 * Reported (item 16): "on curves, add street signs indicating where the track
 * goes; intersections near a curve read as confusing."
 *
 * THIS EMITTER BUILDS THE FIRST HALF ONLY. The "confusing intersections" half is
 * a CROSSING PLACEMENT problem, not a signage one -- the R7 min-spacing thinning
 * (tg_city_crossing_here, TD5_TG_XMIN_GAP = 8 spans) is curvature-BLIND, so eight
 * spans of arc separates mouths far less than eight spans of straight and a
 * cluster on a bend still reads as one fan. That belongs to CROSS this round and
 * is deliberately untouched here: nothing below reads or writes a crossing
 * predicate. Annotating a confusing junction is not the same as fixing it.
 *
 * IT READS THE EXISTING BEND MAP, IT DOES NOT MEASURE CURVATURE AGAIN.
 * tg_turn_map_build already classifies bends for the R8 turn continuation, once
 * per generation, with hysteresis: TD5_TG_R8_TURN_SIN (0.34) on the |cross| of
 * the tangents TD5_TG_R8_TURN_BASE (3) spans either side, and
 * TD5_TG_R8_TURN_GAP (16) spans of separation between qualifying turns. It
 * publishes s_turn_side[] (+1 left of travel, -1 right, 0 none) and
 * s_turn_skew[]. A second threshold here would be a second opinion about the
 * same geometry, and the two would drift. The 0.044/0.045 numbers nearby are NOT
 * classifiers -- TD5_TG_BRIDGE_MAX_TURN and TD5_TG_FORK_MAX_TURN are walk-time
 * CEILINGS applied inside the centreline loop; they cap a bend, they never
 * select one, and a threshold built on them would mean nothing.
 *
 * WHICH SIDE, AND WHICH ARROW. s_turn_side is by construction the OUTSIDE of the
 * bend (see tg_turn_map_build: a left turn crosses negative, so sg = -1 and the
 * continuation belongs on the right). The outside of the bend is also where a
 * real direction sign stands -- it is the side you would run off toward and it
 * sits in the driver's forward view through the corner -- so the sign takes that
 * side directly. The same sign then gives the arrow: s_turn_side == -1 is a LEFT
 * turn, +1 a RIGHT turn.
 *
 * LEAD. The sign is emitted TD5_TG_R11_SIGN_LEAD spans BEFORE the bend it
 * describes, because a sign level with the corner is not information. The
 * lookahead direction means the emitter runs at span si and places at span si,
 * which is what lets it consult the element inventory for si (below).
 *
 * SEPARATION FROM EXISTING VERGE FURNITURE, which is the real risk here. The
 * bands already occupied, measured from the road EDGE in raw units: kerb railing
 * <= TD5_TG_FENCE_KERB (140); lamp post at sidewalk*0.35, or 300 unpaved;
 * spectators 400..1700; statue 1500; trees 800..3200; R9 furniture 900..1411 on
 * a verge or sidewalk*(0.30..0.70) on a pavement; animals 2500..6500. Their
 * union leaves no free lateral band wide enough for a sign that is still close
 * enough to the kerb to read, so lateral choice alone cannot solve this.
 *
 * HEIGHT is the axis that does, and it is the same discrimination the on-road
 * guard itself makes (tg_guard_quad_bad separates a lamp arm from a wall by dy,
 * not by width). The panel hangs at 2.20..4.00 m, clear above every ground-level
 * piece in k_infra_props -- the tallest is the phone box at 2.40 m and it stands
 * at a different gap -- and above the spectators, which is what makes the
 * overlap with the 400..1700 spectator band harmless.
 *
 * MEASURED EFFECTIVE BAND, seed 20260901: every sign reports gap = 484, so the
 * panel footprint is 299..669 out from the road edge. That is NOT the nominal
 * TD5_TG_R11_SIGN_GAP: the request below asks the carriageway authority about
 * the footprint's INNER edge (430 - 185 = 245), the authority floors any prop
 * verge at TD5_TG_CARRIAGEWAY_MARGIN = 300, and the half width is added back to
 * recover the centre. So the constant is a MINIMUM the shared authority is
 * allowed to raise, which is the behaviour every other verge emitter gets and
 * should not be fought. 669 leaves 131 of clearance below the tree band.
 *
 * The only things reaching panel height in this lateral band are the lamp head
 * (TD5_TG_LAMP_H, 2500 raw) and tree canopies, so:
 *   - the lamp is excluded by the ELEMENT INVENTORY, not by a copy of its
 *     placement hash: this emitter runs LAST in the per-span sequence, so
 *     TG_ACCT_MASK_TEST(si, TG_ACCT_LAMP) is already populated for si by the
 *     time it is asked. Re-deriving `si % 7` here is exactly the hand-rolled
 *     duplicate that made trees stand on branch carriageways for three rounds.
 *   - trees sit at gap >= 800 and the panel's footprint ends at 615.
 *
 * NOT EXEMPT FROM THE ON-ROAD GUARD. The call site marks these meshes
 * TG_GK_PROP, whose policy class is TG_GKC_FURNITURE -- judged against
 * tg_footway_reach, the wider envelope that includes a side street's asphalt. No
 * byte range is exempted. A sign has no business over the carriageway, and
 * licensing one would forfeit the backstop that is the reason the R7 guard
 * exists. Placement goes through the same three authorities the R9 furniture
 * uses (tg_side_blocked, tg_xstreet_occupies, tg_carriageway_clear_gap) so a
 * corridor bowing into the lateral pushes the sign out rather than the guard
 * silently eating it.
 *
 * Default ON; TD5RE_R11_SIGNS=0 removes the signage for an A/B.
 * ======================================================================== */

/* Spans of warning between the sign and the bend. At TD5_TG_SPAN_LEN this is
 * about 7500 raw of approach -- far enough to be information rather than
 * decoration, close enough that the bend it describes is the next thing to
 * happen. Well inside TD5_TG_R8_TURN_GAP (16), so the lookahead can never reach
 * past one bend into the next. */
#define TD5_TG_R11_SIGN_LEAD    5
/* Lateral setback of the PANEL CENTRE from the road edge. Chosen so the panel's
 * own footprint (centre +/- half its width) spans 245..615: outboard of the kerb
 * railing and the unpaved lamp post, inboard of the tree and R9 furniture bands.
 * See the block comment for the full band map. */
#define TD5_TG_R11_SIGN_GAP     430.0
/* Panel size. The mined page is the sign's 31x62 panel stretched to fill 64x64,
 * so the QUAD has to restore the 1:2 aspect or the arrow comes out fat --
 * height is twice width by construction, not by taste. */
#define TD5_TG_R11_SIGN_W       (0.90 * TD5_TG_INFRA_M)
#define TD5_TG_R11_SIGN_H       (2.0 * TD5_TG_R11_SIGN_W)
/* Underside of the panel. Above the phone box (2.40 m) is not required -- that
 * stands at a different gap -- but above every piece that could share this band
 * is, and 2.20 m is also simply where a real sign panel starts. */
#define TD5_TG_R11_SIGN_LIFT    (2.20 * TD5_TG_INFRA_M)
/* Post: a 6 cm tube, and it overlaps the panel underside slightly so no seam
 * opens between the two at a grazing angle. */
#define TD5_TG_R11_SIGN_POST_W  (0.06 * TD5_TG_INFRA_M)
#define TD5_TG_R11_SIGN_POST_OV (0.10 * TD5_TG_INFRA_M)
extern long s_r11_signs;
extern long s_r11_sign_skip_lamp;
extern long s_r11_sign_skip_street;
extern long s_r11_sign_skip_side;
extern long s_r11_sign_left;
extern long s_r11_sign_right;
int tg_r11_signs_enabled(void);
int tg_emit_r11_sign(const TG_NodeList *nl, int si, int nspans, TG_Buf *blk, size_t *moff, int *nmesh, int maxmesh);
/* Terrain emits at most a couple of meshes per span; this is headroom, and the
 * emitter's own budget check is asserted against it below. */
#define TG_SIDE_MAX_MESH 32
typedef struct {
    TG_Buf buf;                       /* bytes this span emitted */
    size_t moff[TG_SIDE_MAX_MESH];    /* mesh starts, RELATIVE to buf */
    int    nmoff;
    size_t mlo[TG_SIDE_MAX_MESH];     /* guard marks, RELATIVE to buf */
    size_t mhi[TG_SIDE_MAX_MESH];
    unsigned char mkind[TG_SIDE_MAX_MESH];
    int    msi[TG_SIDE_MAX_MESH];
    int    nmark;
    int    ok;
} TG_SideRec;
/* Serial for now. Each span writes only into its OWN record, which is what
 * makes this loop safe to hand to td5_jobs later without further change. */
/* [S2c] One span's terrain, written into that span's own record and nothing
 * else. Split out of tg_side_terrain_build so the same body serves the serial
 * loop and the job-pool worker, which is what makes the two comparable
 * byte-for-byte.
 *
 * The guard-exempt stack it snapshots and unwinds (s_guard_ex_*) is indexed by
 * this thread's slot, so each worker manipulates its own row. Returns 0 to REFUSE the whole pre-pass (the caller then falls back to
 * inline terrain); ctx carries the refusal out because a job cannot return. */
typedef struct {
    const TG_NodeList *nl;
    int   nspans, lanes, ring;
    long  overflow;      /* atomically incremented on a refusal */
    int   failed;        /* set (never cleared) if any span refused */
} TG_SideJob;
/* Per-entry mesh-slot budget. File scope because tg_scenery_begin sizes the
 * offset table with it and tg_scenery_entry enforces it. */
enum { TG_MAX_MESHES_PER_ENTRY = TD5_TG_SPANS_PER_ENTRY * 96 };
/* ===================== SECTION: scenery in three phases ====================
 * tg_emit_models split into begin / entry / end so its per-ENTRY loop can be
 * driven one entry at a time. That is what lets the scenery be STREAMED while
 * the player drives: geometry costs ~300 ms and the race can start on it,
 * while MODELS.DAT (99.4 percent of the build) fills in behind.
 *
 * The three functions are the original function's three regions, unmoved --
 * the pre-loop, the loop body and the post-loop. The body is reproduced
 * VERBATIM and reads its enclosing-scope values through an alias prologue, so
 * the split cannot change behaviour by transcription. The old whole-build path
 * is now begin -> all entries -> end, in the same order, and must stay
 * byte-identical.
 *
 * ENTRIES MUST BE DRIVEN IN ASCENDING ORDER. tg_r12_flora_accept rejects a
 * plant against every accepted plant within +/-3 spans and an entry is 4
 * spans, so acceptance at entry N depends on N-1 and N+1. Any other order
 * changes which trees exist. */
typedef struct {
    const TG_NodeList *nl;
    int nspans, lanes, ring, main_half, br_lanes, nentries, rails;
    int branch_active;
    int nrails, nbudget;
    TG_Buf *blocks;
    int ok, active;
} TG_ScnCtx;
/* Blob headroom per span for a streamed table, which cannot know the real
 * total until the last entry is assembled. Measured ~6 KB/span across three
 * configs (MARATHON: 19,098,312 B over 3187 spans); 8 KB carries the slack. */
enum { TG_STREAM_BYTES_PER_SPAN = 8192 };
int tg_emit_models(const TG_NodeList *nl, int nspans, int lanes, TG_Buf *out);
/* ==========================================================================
 * TEXTURES.DAT -- texture pages for the road mesh
 *
 * Texture pages are PER-LEVEL, so a generated track must ship its own. The
 * loose textures/tex_NNN.png path is gated on g_active_td6_level > 0
 * (td5_asset.c:2864) and custom-track slots force that to 0, so it is not
 * available here -- hence the binary container, written directly like every
 * other level entry.
 *
 * Container (td5_asset.c:3064-3075):
 *   u32 page_count
 *   u32 page_offset[page_count]        -- absolute from file start
 *   per page: u8 pad[3], u8 type, i32 palette_count,
 *             u8 palette[count*3] (BGR), u8 indices[4096]   (64x64, 8-bit)
 * type: 0 opaque, 1 alpha-keyed, 2 semi-transparent, 3 additive.
 * ========================================================================== */
#define TD5_TG_TEX_DIM     64
#define TD5_TG_TEX_TEXELS  (TD5_TG_TEX_DIM * TD5_TG_TEX_DIM)
#define TD5_TG_PAL_COUNT   16
/* ---- tree-line backdrop page ----
 * A canopy band closing off the horizon behind the individual tree billboards.
 * ALPHA-KEYED (type 1, index 0 = key) so the sky shows through a ragged top
 * edge. The page does NOT have to tile horizontally: tg_emit_fb_flora
 * alternates u direction per span, so a shared edge always meets its own texel
 * column.
 *
 * The band is built from a SHIPPED foliage page (real mode) and only falls
 * back to synthetic colour when real textures are switched off. Root cause of
 * the "grey at the bottom, white at the top" report is the synthetic palette
 * below, which desaturated and blue-shifted itself "for haze" and overshot on
 * both counts. MEASURED off its own ramp:
 *   entries 1..7 (the shadowed mass, used from the crown line down to the base
 *     -- around 40 of 64 rows, most of the band's area) run R 47..65 / G 67..97
 *     / B 68..92. Blue tracks green to within 5 at every entry while red sits
 *     20..32 below both, so the hue is TEAL, and saturation never exceeds 0.33.
 *   entries 8..15 (used only in the 8 rows under the crown) mean about
 *     R100 G138 B111 -- 1.7x brighter than the mass below them.
 * A dark near-neutral teal body under a bright cap is exactly "grey at the
 * bottom, white at the top". Real canopy texels have no such ramp to overshoot:
 * the same simulation over the page this now emits gives a mean of R60 G68 B53
 * (blue LOWEST, a leaf green) varying only R46..68 top to bottom.
 *
 * Source window: rows outside a tree page's canopy are sky (keyed) above and
 * trunk below, neither of which belongs in a continuous band, so the densest
 * TG_TREELINE_WIN-row window is used. MEASURED over the ten pages in
 * td5_tg_real_tex.h, the source picked here (tree 1 = L008 p173, big
 * deciduous) peaks at row 15 with 86% of that window's texels non-key -- the
 * densest broadleaf canopy of the set. The window is COMPUTED, not hard-coded,
 * so a re-export of the page data cannot silently slide it onto the trunk.
 *
 * One page slot serves all three banded biomes, so ALPINE gets the same
 * broadleaf mass as FOREST and FIELDS; giving it conifers needs a second page
 * slot, which is shared state this change does not claim. */
#define TG_TREELINE_SRC   1     /* index into k_real_tree_* used for the band */
#define TG_TREELINE_WIN   20    /* rows of canopy sampled out of the source */
/* [R6 item 13] ALPHA-KEYED W-beam guardrail -- the real fix for "guardrails
 * that don't have extrusion should have a transparent background".
 *
 * The R4/R5 pages were opaque: a barrier with no extrusion painted as a solid
 * slab can never read as a guardrail, because real armco is mostly AIR -- a
 * corrugated beam on posts with sky between and below. This page is TYPE 1
 * (index 0 = transparent key), so the thin one-quad rail shows the world through
 * the gaps: a W-beam across the middle, vertical posts at a road pitch, and open
 * air above, below and between them.
 *
 * Same axes as the R4/R5 pages (page-X = barrier HEIGHT 0=deck..63=top, page-Y
 * = along the road) so the rail panel UVs are unchanged. The general rule the
 * user stated applies beyond bridges; the sidewalk/branch rail pages are owned
 * by other areas and should move to this same alpha-keyed shape. */
/* ============ [R13 RAIL item 4b] TWO STACKED RAILS, NOT ONE MIRRORED PANEL ==
 * "The bridge has a DOUBLE GUARDRAIL with a mirrored texture, instead of the
 *  rail being STACKED UP."   -- and R12 item 14c reported this same complaint
 *  fixed, so the first job was to decide whether that reshade LANDED and
 *  whether the mirroring is painted or geometric. Both were measured, not read:
 *
 *  IT LANDED, AND IT DID NOT HELP. Replaying the R12 raster texel for texel and
 *  drawing it at the parapet's own world aspect (barrier 420 units tall, one
 *  page tile per 1500-unit span) gives two IDENTICAL bright ridges at 147u and
 *  307u above the deck, separated by a flat dark plateau at 213-240u. Scored
 *  against its own reflection about the beam's mid-height, that image is 0.7%
 *  asymmetric -- a mirror to within the dither noise. R12's ramp did not reduce
 *  the symmetry, it PERFECTED it: the old hard bands were 1 dark texel at
 *  x = 34, the ramp's `if (v < 3) v = 3` clamp widened that into a 5-texel,
 *  33-unit flat dark band, which is the separating line the user is describing.
 *  So the round-12 diagnosis (contrast) treated the wrong property. The
 *  offending property is SYMMETRY, and contrast was the only thing it changed.
 *
 *  THE MIRRORING IS PAINTED, NOT GEOMETRIC, and no shared fold helper is
 *  implicated. tg_emit_bridge_rail_panel emits ONE quad per deck edge with
 *  u = 0,0,1,1 and v = si,si+1,si+1,si -- monotonic on both axes, no triangle
 *  wave, no second pass; R12's finding that page-X = height agrees with the
 *  panel's u is correct and is confirmed here. The R12 tree-line idiom
 *  (tg_r12_tl_fold) is not called from this file's rail path at all. The
 *  reflection is drawn INTO the page by `d = min(|x-22|, |x-46|)`, which is a
 *  mirror by construction.
 *
 *  WHAT THE USER ASKED FOR IS THE FIX. "Stacked up" is a real barrier: a
 *  crash beam carried on posts with a slimmer hand rail ABOVE it and daylight
 *  in between. That is not a symmetric image, and more importantly the gap
 *  between the two rails is TRANSPARENT rather than dark, so the thing between
 *  them reads as air you see the sky through instead of as a crease down the
 *  middle of one panel. Two sections of DIFFERENT height (a 20-texel W with two
 *  close ridges, an 8-texel round hand rail with one) and posts that run past
 *  both of them: scored the same way, 67.6% asymmetric.
 *
 *  Sizes are in page-X, which this page defines as barrier height, so they are
 *  stated once here and converted by the same 420/63 the panel uses.
 *
 *  TD5RE_R13_RAIL_STACK=0 restores the R12 mirrored profile for an A/B. */
#define TD5_TG_R13_RAIL_LO0   14   /* crash beam, lower lip   ( 93u)  */
#define TD5_TG_R13_RAIL_LOA   19   /* its lower ridge crest   (127u)  */
#define TD5_TG_R13_RAIL_LOB   29   /* its upper ridge crest   (193u)  */
#define TD5_TG_R13_RAIL_LO1   34   /* crash beam, upper lip   (227u)  */
#define TD5_TG_R13_RAIL_HI0   42   /* hand rail, lower lip    (280u)  */
#define TD5_TG_R13_RAIL_HIC   46   /* its single crest        (307u)  */
#define TD5_TG_R13_RAIL_HI1   50   /* hand rail, upper lip    (333u)  */
#define TD5_TG_R13_RAIL_POST  52   /* posts stop just above the hand rail */
int tg_real_textures_enabled(void);
void td5_trackgen_default_spec(TD5_TrackGenSpec *spec);
void td5_trackgen_apply_config(TD5_TrackGenSpec *spec);
int td5_trackgen_build_level(const TD5_TrackGenSpec *spec, int level_num, int *out_spans);
/* ---- [R14 GENPERF 2026-09-03] build stamp: reuse an identical build ------
 * The level directory is rebuilt on every race entry because each race gets a
 * fresh seed. When the seed is NOT fresh -- a pinned TD5RE_AUTOTRACK_SEED, a
 * pause-menu RESTART of the same race, a harness A/B -- the previous build is
 * byte-identical and 20-35 s of generation is wasted. The stamp records what
 * the on-disk level was built FROM: seed, the spec, every TD5RE_* knob (the
 * emitters read dozens of them through getenv), and the identity of the exe
 * that built it (size + mtime: any rebuild of the generator invalidates the
 * cache, so a stale build can never hide a code change). Reuse also needs the
 * few generator statics the runtime asks for after the build, which is why the
 * stamp carries the ring length / finish span / circuit flag. */
#define TG_STAMP_VERSION 1u
typedef struct {
    unsigned int version, seed, spec_hash, env_hash;
    unsigned long long exe_id;
    int spans, ring, finish, circuit, night;
} TG_Stamp;
int td5_trackgen_level_number(void);
unsigned int td5_trackgen_last_seed(void);

#endif /* TD5_TRACKGEN_INTERNAL_H */

